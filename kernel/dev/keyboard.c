#include "io.h"
#include "keyboard.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "tty.h"
#include "waitq.h"
#include "screen.h"
#include "typesk.h"

/* --- scancode set 1 -------------------------------------------------------
 * What a PS/2 controller emits by default. A make code on press, the same
 * value with bit 7 set on release, and a 0xE0 prefix for the keys that were
 * added after the original 84-key layout - arrows, right ctrl/alt, the
 * navigation cluster.
 *
 * Only the first 0x3B entries produce characters; everything above is a
 * function key or a modifier, and stays zero. US layout, because that is what
 * the tables below encode and there is nothing here that consults a locale. */

#define KEY_ESC        0x01
#define KEY_BACKSPACE  0x0E
#define KEY_ENTER      0x1C
#define KEY_CTRL       0x1D
#define KEY_LSHIFT     0x2A
#define KEY_RSHIFT     0x36
#define KEY_ALT        0x38
#define KEY_CAPS       0x3A
#define KEY_RELEASE    0x80
#define KEY_EXTENDED   0xE0

static const char keymap[128] = {
    /* 0x00 */  0,   27, '1', '2', '3', '4', '5', '6',
    /* 0x08 */ '7', '8', '9', '0', '-', '=','\b','\t',
    /* 0x10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    /* 0x18 */ 'o', 'p', '[', ']','\n',  0, 'a', 's',
    /* 0x20 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /* 0x28 */'\'', '`',   0,'\\', 'z', 'x', 'c', 'v',
    /* 0x30 */ 'b', 'n', 'm', ',', '.', '/',   0, '*',
    /* 0x38 */   0, ' ',   0,   0,   0,   0,   0,   0,
};

static const char keymap_shift[128] = {
    /* 0x00 */  0,   27, '!', '@', '#', '$', '%', '^',
    /* 0x08 */ '&', '*', '(', ')', '_', '+','\b','\t',
    /* 0x10 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /* 0x18 */ 'O', 'P', '{', '}','\n',  0, 'A', 'S',
    /* 0x20 */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    /* 0x28 */ '"', '~',   0, '|', 'Z', 'X', 'C', 'V',
    /* 0x30 */ 'B', 'N', 'M', '<', '>', '?',   0, '*',
    /* 0x38 */   0, ' ',   0,   0,   0,   0,   0,   0,
};

/* --- the ring ------------------------------------------------------------
 * Single producer (the IRQ), single consumer (the read syscall). head is
 * written only by the producer, tail only by the consumer, so neither can
 * observe a half-updated index and no lock is needed. Both are volatile
 * because the compiler cannot see that an interrupt modifies one of them.
 *
 * Full is dropped rather than overwritten: losing the newest keystroke is
 * confusing, but losing the oldest silently reorders what the user typed. */
#define RING_SIZE 256

static volatile uint8  ring[RING_SIZE];
static volatile uint32 ring_head;
static volatile uint32 ring_tail;

/* Who is blocked on a keystroke.
 *
 * The list and the blocking loop moved to waitq.c - see waitq.h for why.
 * Nothing about the behaviour changed: same one-slot-per-process list, same
 * wake-everybody policy, same `sti; hlt` ordering and the same signal check.
 * What changed is that a pipe can now block correctly without a second copy
 * of any of it. */
static wait_queue_t kbd_waiters;

static int shift_down;
static int ctrl_down;
static int caps_lock;
static int extended;

static void ring_push(char c) {
    uint32 next = (ring_head + 1) % RING_SIZE;

    if (next == ring_tail) {
        return;   /* full - drop it */
    }
    ring[ring_head] = (uint8)c;
    ring_head = next;
}

static int ring_pop(void) {
    int c;

    if (ring_tail == ring_head) {
        return -1;
    }
    c = ring[ring_tail];
    ring_tail = (ring_tail + 1) % RING_SIZE;
    return c;
}

/* Throw away everything queued. Ctrl-C discards type-ahead as well as the
 * line in progress: both were typed at the command being interrupted, and
 * handing them to whatever runs next is how a stray keystroke becomes a
 * command nobody meant to run. */
static void ring_flush(void) {
    ring_tail = ring_head;
}

int kbd_has_input(void) {
    return ring_tail != ring_head;
}

int kbd_has_line(void) {
    uint32 i = ring_tail;
    uint32 head = ring_head;

    /* Read once into a local. head is written by the IRQ and this is the
     * consumer side, so it can grow underneath the scan; taking a snapshot
     * means the loop terminates on a fixed bound rather than chasing a moving
     * one. Characters that arrive during the scan are missed here and caught
     * by the next wake, which the same interrupt sends. */
    while (i != head) {
        uint8 c = ring[i];

        /* The four bytes kbd_read_line returns on. Ctrl-C is in the list
         * because -EINTR is a return: a poller told "not readable" while a
         * SIGINT is sitting in the ring waits for input that has already
         * arrived and will never be re-sent. */
        if (c == '\n' || c == '\r' || c == 4 || c == 3) {
            return 1;
        }
        i = (i + 1) % RING_SIZE;
    }
    return 0;
}

void kbd_init(void) {
    ring_head = 0;
    ring_tail = 0;
    waitq_init(&kbd_waiters);
    shift_down = 0;
    ctrl_down = 0;
    caps_lock = 0;
    extended = 0;

    /* Drain whatever the BIOS left in the controller. Without this the first
     * read returns a keystroke from before the kernel started. */
    while (inb(0x64) & 0x01) {
        (void)inb(0x60);
    }
}

static int is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

void kbd_scancode(uint8 code) {
    char c;

    /* Extended keys announce themselves with a prefix byte and then a second
     * scancode that collides with an ordinary one. Swallowing both is what
     * keeps the arrow keys from typing letters. */
    if (code == KEY_EXTENDED) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        return;   /* nothing above the base layout produces a character yet */
    }

    if (code & KEY_RELEASE) {
        uint8 made = code & ~KEY_RELEASE;
        if (made == KEY_LSHIFT || made == KEY_RSHIFT) {
            shift_down = 0;
        } else if (made == KEY_CTRL) {
            ctrl_down = 0;
        }
        return;
    }

    switch (code) {
        case KEY_LSHIFT:
        case KEY_RSHIFT: shift_down = 1;        return;
        case KEY_CTRL:   ctrl_down = 1;         return;
        case KEY_CAPS:   caps_lock = !caps_lock; return;
        case KEY_ALT:                            return;
        default: break;
    }

    if (code >= 128) {
        return;
    }
    c = shift_down ? keymap_shift[code] : keymap[code];
    if (c == 0) {
        return;
    }

    /* Caps lock applies to letters only, and XORs with shift rather than
     * overriding it - shift+letter with caps on gives lowercase, which is
     * what every other terminal does. */
    if (caps_lock && is_letter(c)) {
        c = shift_down ? (char)(c + 32) : (char)(c - 32);
    }

    /* Ctrl folds a letter to its control code: Ctrl-D is 4, Ctrl-C is 3.
     * The line reader below gives 4 its end-of-input meaning. */
    if (ctrl_down && is_letter(c)) {
        c = (char)(c & 0x1F);
    }

    ring_push(c);
}

/* Push an already-decoded character into the input ring, from something that
 * is not the PS/2 keyboard.
 *
 * The serial console is the caller. It matters that this is the same ring
 * rather than a second one: the line discipline, the Ctrl-C handling, the
 * echo and every reader above sit on this ring, so a serial console behaves
 * identically to the keyboard instead of being a parallel input path that
 * drifts. A second ring would need all of that duplicated, and the copies
 * would disagree about something eventually - most likely about what ends a
 * line.
 *
 * Takes a CHARACTER, not a scancode. kbd_scancode is the PS/2 translation
 * layer and has no meaning for a byte that arrived over a wire already
 * decoded; routing serial input through it would try to interpret 'a' as a
 * make/break code.
 */
void kbd_inject(char c) {
    ring_push(c);
    /* Woken here rather than left to the caller, because forgetting it is a
     * character that sits in the ring until the next unrelated keystroke -
     * which on a machine with no keyboard is forever. */
    if (kbd_has_input()) {
        waitq_wake_all(&kbd_waiters);
    }
}

void kbd_irq(void) {
    kbd_scancode(inb(0x60));
    /* Waking from the interrupt rather than switching in it: sched_wake only
     * marks the process ready and sets the reschedule flag. The switch
     * happens on the way back out to user mode. */
    if (kbd_has_input()) {
        waitq_wake_all(&kbd_waiters);
    }
}

/* Block until the ring has something.
 *
 * The condition, and nothing else. Every subtlety that used to be written out
 * here - the `sti; hlt` ordering, the re-test after waking, the signal check
 * that makes the wait killable - is in waitq_wait now, in one copy that the
 * pipe code will use too. */
static int kbd_ready(void *ctx) {
    (void)ctx;
    return kbd_has_input();
}

static int kbd_wait(void) {
    return waitq_wait(&kbd_waiters, kbd_ready, NULL);
}

#define ERASE_COLOR 0x07
#define ECHO_COLOR  0x0F

int64 kbd_read_line(char *buf, uint64 max) {
    uint64 len = 0;

    if (max == 0) {
        return 0;
    }

    for (;;) {
        int c;

        if (!kbd_wait()) {
            /* A signal arrived instead of a keystroke. Return so the syscall
             * can unwind and delivery can happen at the boundary; blocking
             * again here would sit on a pending signal forever. */
            return -4;                   /* -EINTR */
        }
        c = ring_pop();
        if (c < 0) {
            continue;
        }

        if (c == 4) {                    /* Ctrl-D */
            /* Only end-of-input on an empty line. Mid-line it submits what
             * has been typed without a newline, which is what a terminal
             * does and what lets `read` see a partial last line. */
            if (len == 0) {
                return 0;
            }
            return (int64)len;
        }

        if (c == '\b' || c == 0x7F) {    /* backspace or delete */
            if (len > 0) {
                len--;
                print_backspace(ERASE_COLOR);
            }
            continue;
        }

        if (c == 3) {                    /* Ctrl-C */
            /* A real SIGINT, to the foreground process group only. The line
             * in progress is discarded, which is what a terminal does:
             * whatever was half-typed belonged to the command being
             * interrupted.
             *
             * -EINTR, never 0. Zero means end of input, and a shell that
             * reads it exits - which is exactly what made Ctrl-C on an empty
             * line kill the shell, because with no signal actually posted
             * sys_read had no reason to translate the zero into anything
             * else. Whether the signal turns out to be deliverable is not
             * this function's business: an interrupted read is interrupted
             * even when the target has SIGINT blocked, ignored, or is a
             * process group that no longer has members. */
            print_string("^C\n", ECHO_COLOR);
            ring_flush();
            signal_send_group(tty_foreground_pgid(), SIGINT);
            return -4;                   /* -EINTR */
        }

        if (c == '\n' || c == '\r') {
            print_char('\n', ECHO_COLOR);
            buf[len++] = '\n';
            return (int64)len;
        }

        if (c < 0x20) {
            continue;                    /* other control codes: ignore */
        }

        /* Reserve the last byte for the newline. A line that fills the buffer
         * is delivered as-is and the rest arrives on the next read, which is
         * how a real tty behaves - the alternative is silently discarding
         * what the user typed. */
        if (len + 1 >= max) {
            buf[len++] = (char)c;
            return (int64)len;
        }

        buf[len++] = (char)c;
        print_char((char)c, ECHO_COLOR);
    }
}
