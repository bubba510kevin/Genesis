/* Host tests for keyboard.c.
 *
 * The line discipline is the half of a keyboard driver that is actually easy
 * to get wrong and hard to debug on target: an off-by-one in the backspace
 * path or the buffer-full path shows up as a shell that behaves strangely
 * three keystrokes in, with nothing to inspect. None of it touches hardware -
 * kbd_scancode takes a byte and kbd_read_line fills a buffer - so all of it
 * runs here.
 *
 * What is NOT covered: kbd_irq (one inb) and the blocking wait in kbd_wait,
 * whose whole content is the sti/hlt sequence the harness strips. Those two
 * are only testable on the machine. */

#include <stdio.h>
#include <string.h>

#include "keyboard.h"
#include "waitq.h"
#include "typesk.h"

/* Echo goes to the screen driver; capture it so the tests can assert on what
 * the user would have seen, not just on what the buffer ended up holding. */
static char echoed[512];
static int  echo_len;

void print_char(char c, uint8 color) {
    (void)color;
    if (echo_len < (int)sizeof(echoed) - 1) {
        echoed[echo_len++] = c;
    }
}

void print_backspace(uint8 color) {
    (void)color;
    if (echo_len > 0) {
        echo_len--;   /* destructive, exactly like the real one */
    }
}

/* The keyboard blocks through the scheduler now. The harness has no
 * processes, and every test feeds a complete line before reading, so these
 * stubs are never reached by the line-discipline tests - they exist to link,
 * and asserting that keeps a future change from silently depending on them.
 * test_waitq below calls sched_wake deliberately, and runs after that
 * assertion for exactly that reason.
 *
 * Real signatures rather than void *: waitq.h pulls in process.h, and a stub
 * that disagrees with the declaration it is standing in for is a link-time
 * coin flip. */
int  sched_stub_calls;
void sched_block(process_t *p) { (void)p; sched_stub_calls++; }
void sched_wake(process_t *p)  { (void)p; sched_stub_calls++; }
process_t *proc_current(void)  { return NULL; }

/* Ctrl-C sends a real signal now. The harness has no processes and no tty
 * ownership, so these record the call and go no further - which lets the
 * Ctrl-C test assert that the signal was raised without needing a scheduler. */
int  signal_stub_sends;
int  signal_stub_last;
void signal_send_group(int pgid, int signo) {
    (void)pgid;
    signal_stub_sends++;
    signal_stub_last = signo;
}
int  signal_pending(process_t *p) { (void)p; return 0; }

/* pipe.c raises SIGPIPE on the writer itself, so the single-process form is
 * needed too. The counters live in pipe_test.c, which asserts on them. */
extern int pipe_sigs;
extern int pipe_last_sig;
void signal_send(process_t *p, int signo) {
    (void)p;
    pipe_sigs++;
    pipe_last_sig = signo;
}
int  tty_foreground_pgid(void) { return 42; }

static void echo_reset(void) {
    echo_len = 0;
    echoed[0] = '\0';
}

static const char *echo_text(void) {
    echoed[echo_len] = '\0';
    return echoed;
}

extern int kbd_failures;
int kbd_failures;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL  %s\n", what);
        kbd_failures++;
    } else {
        printf("  ok    %s\n", what);
    }
}

/* --- scancode helpers ---------------------------------------------------- */

#define SC_BACKSPACE 0x0E
#define SC_ENTER     0x1C
#define SC_CTRL      0x1D
#define SC_LSHIFT    0x2A
#define SC_CAPS      0x3A
#define SC_SPACE     0x39
#define SC_RELEASE   0x80

/* Just enough of the US layout for the strings these tests type. */
static uint8 scancode_for(char c) {
    static const char *row1 = "1234567890-=";
    static const char *row2 = "qwertyuiop[]";
    static const char *row3 = "asdfghjkl;'";
    static const char *row4 = "zxcvbnm,./";
    const char *p;

    if (c == ' ') return SC_SPACE;
    if ((p = strchr(row1, c)) != NULL) return (uint8)(0x02 + (p - row1));
    if ((p = strchr(row2, c)) != NULL) return (uint8)(0x10 + (p - row2));
    if ((p = strchr(row3, c)) != NULL) return (uint8)(0x1E + (p - row3));
    if ((p = strchr(row4, c)) != NULL) return (uint8)(0x2C + (p - row4));
    return 0;
}

static void press(uint8 sc) {
    kbd_scancode(sc);
    kbd_scancode((uint8)(sc | SC_RELEASE));
}

static void type(const char *s) {
    while (*s) {
        press(scancode_for(*s));
        s++;
    }
}

static void type_enter(void) { press(SC_ENTER); }

/* --- tests --------------------------------------------------------------- */

static void test_plain_line(void) {
    char buf[64];
    int64 n;

    printf("\nkbd: a typed line comes back intact\n");
    echo_reset();

    type("ls -l");
    type_enter();
    n = kbd_read_line(buf, sizeof(buf));

    check(n == 6, "returned the byte count including the newline");
    check(memcmp(buf, "ls -l\n", 6) == 0, "buffer holds exactly what was typed");
    check(strcmp(echo_text(), "ls -l\n") == 0, "every character was echoed once");
}

static void test_backspace(void) {
    char buf[64];
    int64 n;

    printf("\nkbd: backspace removes from the buffer and the screen\n");
    echo_reset();

    type("lsx");
    press(SC_BACKSPACE);
    type(" -l");
    type_enter();
    n = kbd_read_line(buf, sizeof(buf));

    check(n == 6, "the erased character is not counted");
    check(memcmp(buf, "ls -l\n", 6) == 0, "the erased character is not delivered");
    check(strcmp(echo_text(), "ls -l\n") == 0, "the screen matches the buffer");
}

static void test_backspace_on_empty_line(void) {
    char buf[64];
    int64 n;

    printf("\nkbd: backspace at the start of a line does nothing\n");
    echo_reset();

    press(SC_BACKSPACE);
    press(SC_BACKSPACE);
    type("ok");
    type_enter();
    n = kbd_read_line(buf, sizeof(buf));

    /* The interesting failure here is an underflow: len-- at len == 0 wraps a
     * uint64 to 0xFFFF... and the next store writes far outside the buffer. */
    check(n == 3, "the line is unaffected");
    check(memcmp(buf, "ok\n", 3) == 0, "no underflow past the start of the buffer");
}

static void test_shift_and_caps(void) {
    char buf[64];

    printf("\nkbd: shift and caps lock\n");
    echo_reset();

    kbd_scancode(SC_LSHIFT);
    type("ab");
    kbd_scancode(SC_LSHIFT | SC_RELEASE);
    type("c");
    type_enter();
    kbd_read_line(buf, sizeof(buf));
    check(memcmp(buf, "ABc\n", 4) == 0, "shift capitalises only while held");

    echo_reset();
    press(SC_CAPS);
    type("ab");
    kbd_scancode(SC_LSHIFT);
    type("c");
    kbd_scancode(SC_LSHIFT | SC_RELEASE);
    type_enter();
    kbd_read_line(buf, sizeof(buf));
    check(memcmp(buf, "ABc\n", 4) == 0, "caps lock XORs with shift rather than overriding it");
    press(SC_CAPS);   /* back off for later tests */
}

static void test_shifted_symbols(void) {
    char buf[64];

    printf("\nkbd: shifted number row gives symbols\n");
    echo_reset();

    kbd_scancode(SC_LSHIFT);
    type("12");
    kbd_scancode(SC_LSHIFT | SC_RELEASE);
    type_enter();
    kbd_read_line(buf, sizeof(buf));
    check(memcmp(buf, "!@\n", 3) == 0, "shift+1 and shift+2 are ! and @");
}

static void test_ctrl_c(void) {
    char buf[64];
    int64 n;

    printf("\nkbd: Ctrl-C raises SIGINT rather than faking a newline\n");
    echo_reset();
    signal_stub_sends = 0;

    type("half typed");
    kbd_scancode(SC_CTRL);
    press(scancode_for('c'));
    kbd_scancode(SC_CTRL | SC_RELEASE);

    n = kbd_read_line(buf, sizeof(buf));
    check(signal_stub_sends == 1, "one signal was sent");
    check(signal_stub_last == 2, "and it was SIGINT");
    /* The half-typed line is discarded: it belonged to the command being
     * interrupted, not to whatever runs next. */
    check(n == -4, "the interrupted read returns -EINTR, not end of input");

    /* The distinction that keeps a shell alive. Zero is Ctrl-D and only
     * Ctrl-D; if Ctrl-C ever returns it again, ash exits on an empty line. */
    echo_reset();
    signal_stub_sends = 0;
    kbd_scancode(SC_CTRL);
    press(scancode_for('c'));
    kbd_scancode(SC_CTRL | SC_RELEASE);
    n = kbd_read_line(buf, sizeof(buf));
    check(n != 0, "on an empty line it is still not end of input");
    check(signal_stub_sends == 1, "and it still raises SIGINT");

    /* Type-ahead queued behind the Ctrl-C belonged to the interrupted
     * command too - delivering it would run a command nobody typed at this
     * prompt. */
    echo_reset();
    signal_stub_sends = 0;
    kbd_scancode(SC_CTRL);
    press(scancode_for('c'));
    kbd_scancode(SC_CTRL | SC_RELEASE);
    type("rm\n");
    n = kbd_read_line(buf, sizeof(buf));
    check(n == -4, "the read still ends at the Ctrl-C");
    check(!kbd_has_input(), "and the type-ahead behind it was flushed");
}

static void test_ctrl_d(void) {
    char buf[64];
    int64 n;

    printf("\nkbd: Ctrl-D\n");
    echo_reset();

    kbd_scancode(SC_CTRL);
    press(scancode_for('d'));
    kbd_scancode(SC_CTRL | SC_RELEASE);
    n = kbd_read_line(buf, sizeof(buf));
    check(n == 0, "on an empty line it is end of input - the shell will exit");

    echo_reset();
    type("hi");
    kbd_scancode(SC_CTRL);
    press(scancode_for('d'));
    kbd_scancode(SC_CTRL | SC_RELEASE);
    n = kbd_read_line(buf, sizeof(buf));
    check(n == 2 && memcmp(buf, "hi", 2) == 0,
          "mid-line it submits what was typed, with no newline");
}

static void test_line_longer_than_buffer(void) {
    char buf[8];
    int64 n;

    printf("\nkbd: a line longer than the buffer\n");
    echo_reset();

    type("0123456789");
    type_enter();

    n = kbd_read_line(buf, sizeof(buf));
    check(n == 8, "the first read fills the buffer exactly");
    check(memcmp(buf, "01234567", 8) == 0, "and holds the first 8 bytes");

    n = kbd_read_line(buf, sizeof(buf));
    check(n == 3 && memcmp(buf, "89\n", 3) == 0,
          "the rest arrives on the next read instead of being discarded");
}

static void test_extended_and_release(void) {
    char buf[64];
    int64 n;

    printf("\nkbd: prefixed and release codes produce nothing\n");
    echo_reset();

    kbd_scancode(0xE0);              /* an arrow key: prefix... */
    kbd_scancode(0x48);              /* ...then a code that collides with 'b' */
    kbd_scancode(scancode_for('a') | SC_RELEASE);   /* a bare release */
    type("z");
    type_enter();

    n = kbd_read_line(buf, sizeof(buf));
    check(n == 2 && memcmp(buf, "z\n", 2) == 0,
          "neither the arrow key nor the stray release typed a character");
}


/* The wait queue's bookkeeping, on its own.
 *
 * The blocking loop itself cannot run here - it halts the CPU waiting for an
 * interrupt that a host process will never get - but the list around it can,
 * and the list is where the old single-pointer version was wrong: a second
 * waiter overwrote the first and the first was never woken again. That is a
 * silent hang on the machine and four lines to check here.
 *
 * Runs after the "no scheduler stubs" assertion above on purpose: this test
 * calls sched_wake deliberately, and it must not be able to hide a keyboard
 * test that started blocking by accident. */
static void test_waitq(void) {
    wait_queue_t q, other;
    int          before;
    /* Real process_t objects, not stand-in bytes.
     *
     * These used to be two `static char`s cast to process_t *, on the stated
     * grounds that waitq only ever compares and stores the pointers. That
     * stopped being true when waitq_add started writing p->blocked_on: the
     * cast turned a one-byte object into a structure and the store went off
     * the end of it. Nothing failed - the bytes after a static char belong to
     * something, and that something was not being checked. */
    static process_t a, b;

    printf("\nwaitq: the list keyboard.c used to own\n");

    waitq_init(&q);
    waitq_init(&other);
    a.blocked_on = NULL;
    b.blocked_on = NULL;

    before = sched_stub_calls;
    waitq_wake_all(&q);
    check(sched_stub_calls == before, "an empty queue wakes nobody");

    waitq_add(&q, &a);
    waitq_add(&q, &b);
    before = sched_stub_calls;
    waitq_wake_all(&q);
    check(sched_stub_calls - before == 2,
          "two waiters are BOTH woken - one pointer used to lose the first");
    check(a.blocked_on == &q && b.blocked_on == &q,
          "and each one records the queue it is on");

    waitq_add(&q, &a);
    before = sched_stub_calls;
    waitq_wake_all(&q);
    check(sched_stub_calls - before == 2,
          "adding a process already queued does not queue it twice");

    waitq_remove(&q, &a);
    before = sched_stub_calls;
    waitq_wake_all(&q);
    check(sched_stub_calls - before == 1, "removal takes exactly one off");
    check(a.blocked_on == NULL, "and clears its record of the queue");

    /* A remove naming a queue the process is not on must not clear the
     * record of the one it IS on - that would leave it listed with nothing
     * left able to find it. */
    waitq_remove(&other, &b);
    check(b.blocked_on == &q,
          "removing from an unrelated queue leaves the real one recorded");

    /* Teardown's route: it does not know which queue, only which process. */
    waitq_leave(&b);
    check(b.blocked_on == NULL, "waitq_leave finds the queue for you");
    before = sched_stub_calls;
    waitq_wake_all(&q);
    check(sched_stub_calls == before,
          "and really took it off - a dying process is woken by nobody");

    waitq_leave(&b);
    check(b.blocked_on == NULL, "leaving twice is harmless");
    waitq_leave(NULL);
}

int kbd_run_tests(void) {
    test_plain_line();
    test_backspace();
    test_backspace_on_empty_line();
    test_shift_and_caps();
    test_shifted_symbols();
    test_ctrl_c();
    test_ctrl_d();
    test_line_longer_than_buffer();
    test_extended_and_release();

    printf("\nkbd: the blocking path was never entered\n");
    /* Every test above supplies a full line before reading, so kbd_wait
     * never had to block. If this ever fires, a test changed shape and the
     * stubs above started standing in for real scheduling. */
    check(sched_stub_calls == 0, "no test relied on the scheduler stubs");

    test_waitq();
    return kbd_failures;
}
