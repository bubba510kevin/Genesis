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

static int shift_down;
static int ctrl_down;
static int caps_lock;
static int extended;

void kbd_init(void) {
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

static void send_seq(const char *s) {
    while (*s != '\0') {
        tty_input(*s++);
    }
}

/* The 0xE0-prefixed keys, as the escape sequences a Linux console sends for
 * them. Right Ctrl shares Left Ctrl's second byte and is a modifier; the
 * rest of the extended set (right Alt, the Windows keys, keypad Enter and /)
 * produces nothing yet. */
static void extended_key(uint8 code) {
    switch (code) {
        case 0x1D: ctrl_down = 1;       return;   /* right Ctrl */
        case 0x48: send_seq("\033[A");  return;   /* Up    */
        case 0x50: send_seq("\033[B");  return;   /* Down  */
        case 0x4D: send_seq("\033[C");  return;   /* Right */
        case 0x4B: send_seq("\033[D");  return;   /* Left  */
        case 0x47: send_seq("\033[1~"); return;   /* Home  */
        case 0x4F: send_seq("\033[4~"); return;   /* End   */
        case 0x52: send_seq("\033[2~"); return;   /* Insert */
        case 0x53: send_seq("\033[3~"); return;   /* Delete */
        case 0x49: send_seq("\033[5~"); return;   /* PgUp  */
        case 0x51: send_seq("\033[6~"); return;   /* PgDn  */
        case 0x1C: tty_input('\r');     return;   /* keypad Enter */
        case 0x35: tty_input('/');      return;   /* keypad /     */
        default:                        return;
    }
}

void kbd_scancode(uint8 code) {
    char c;

    /* Extended keys announce themselves with a prefix byte and then a second
     * scancode that collides with an ordinary one. */
    if (code == KEY_EXTENDED) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        if (code == (KEY_CTRL | KEY_RELEASE)) {
            ctrl_down = 0;                  /* right Ctrl released */
        } else if (!(code & KEY_RELEASE)) {
            extended_key(code);
        }
        return;
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

    /* Ctrl folds a letter to its control code (Ctrl-D is 4, Ctrl-C is 3),
     * and the few punctuation keys that have one: Ctrl-\ is 28 (VQUIT),
     * Ctrl-[ is ESC. */
    if (ctrl_down) {
        if (is_letter(c)) {
            c = (char)(c & 0x1F);
        } else if (c == '\\' || c == '[' || c == ']') {
            c = (char)(c & 0x1F);
        }
    }

    /* Enter is CR, as on a real terminal; the terminal's ICRNL makes it a
     * newline for canonical readers, and readline in raw mode takes either. */
    if (c == '\n') {
        c = '\r';
    }
    kbd_inject(c);
}

/* An already-decoded character, from the PS/2 path above or from the serial
 * console. One entry point for both, so the two keyboards cannot come to
 * disagree about anything - they reach the same line discipline through the
 * same door. */
void kbd_inject(char c) {
    /* Ctrl-T: the task dump, consumed here rather than delivered - see
     * proc_dump. Nothing reading the console expects the byte (readline's
     * transpose-chars is the one casualty, and a debugging key that works
     * while the machine is wedged is worth more). */
    if (c == 0x14) {
        proc_dump();
        return;
    }
    tty_input(c);
}

void kbd_irq(void) {
    kbd_scancode(inb(0x60));
}
