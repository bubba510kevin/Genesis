/* Host tests for keyboard.c and the terminal line discipline in tty.c.
 *
 * The line discipline is the half of a terminal that is actually easy to get
 * wrong and hard to debug on target: an off-by-one in the erase path or a
 * mode switch that loses a half-typed line shows up as a shell that behaves
 * strangely three keystrokes in, with nothing to inspect. None of it touches
 * hardware - kbd_scancode takes a byte and the console's read fills a buffer
 * - so all of it runs here.
 *
 * What is NOT covered: kbd_irq (one inb) and the blocking waits, whose whole
 * content is the sti/hlt sequence the harness strips. Every test here queues
 * its input BEFORE reading, and the end of the run asserts that nothing ever
 * reached the scheduler stubs. */

#include <stdio.h>
#include <string.h>

#include "keyboard.h"
#include "tty.h"
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

int screen_width_chars(void)  { return 80; }
int screen_height_chars(void) { return 25; }
uint32 timer_hz(void)         { return 100; }

/* A process table of two, live only while test_waitq runs: a wait queue is a
 * bitmap of table slots, so proc_index and proc_at have to agree on where
 * its processes are. Everywhere else the harness has no processes at all. */
static process_t stub_table[2];
static int       stub_table_live;

process_t *proc_at(int index) {
    if (!stub_table_live || index < 0 || index >= 2) {
        return NULL;
    }
    return &stub_table[index];
}

int proc_index(const process_t *p) {
    return (int)(p - stub_table);
}

/* The terminal blocks through the scheduler. The harness has no processes,
 * and every test queues its input before reading, so these stubs are never
 * reached by the line-discipline tests - they exist to link, and the run
 * asserts that. test_waitq below calls sched_wake deliberately, and runs
 * after that assertion for exactly that reason.
 *
 * Real signatures rather than void *: waitq.h pulls in process.h, and a stub
 * that disagrees with the declaration it is standing in for is a link-time
 * coin flip. */
int  sched_stub_calls;
void sched_block(process_t *p) { (void)p; sched_stub_calls++; }
void sched_wake(process_t *p)  { (void)p; sched_stub_calls++; }
process_t *proc_current(void)  { return NULL; }

/* The signal characters send real signals, at input time. The harness has
 * no processes, so this records the call and goes no further. */
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

/* --- the terminal from a program's side ---------------------------------- */

static int64 rd(char *buf, uint64 n) {
    object_t *o = tty_console();

    return o->type->read(o, buf, n, NULL);
}

/* How much a read could take right now - FIONREAD. Asked before every read
 * in the tests below, because reading an empty terminal would block, and
 * blocking here is a spin on the stubs. */
static int avail(void) {
    int n = -1;

    tty_ioctl(0x541B, (uint64)(uintptr)&n);
    return n;
}

/* Linux's kernel termios, as the ioctls move it. */
struct kt {
    uint32 iflag, oflag, cflag, lflag;
    uint8  line;
    uint8  cc[19];
};

#define L_ISIG   0000001
#define L_ICANON 0000002
#define L_ECHO   0000010
#define I_ICRNL  0000400
#define CC_VTIME 5
#define CC_VMIN  6

static void get_mode(struct kt *t) {
    tty_ioctl(0x5401, (uint64)(uintptr)t);          /* TCGETS */
}

static void set_mode(const struct kt *t) {
    tty_ioctl(0x5402, (uint64)(uintptr)t);          /* TCSETS */
}

/* readline's mode: no ICANON, no ECHO, signals still on, one byte at a time. */
static void raw_mode(void) {
    struct kt t;

    get_mode(&t);
    t.lflag &= ~(uint32)(L_ICANON | L_ECHO);
    t.cc[CC_VMIN] = 1;
    t.cc[CC_VTIME] = 0;
    set_mode(&t);
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

static void ctrl(char letter) {
    kbd_scancode(SC_CTRL);
    press(scancode_for(letter));
    kbd_scancode(SC_CTRL | SC_RELEASE);
}

/* A fresh terminal for each test: the defaults, nothing queued. */
static void fresh(void) {
    tty_init();
    echo_reset();
    signal_stub_sends = 0;
}

/* --- canonical mode ------------------------------------------------------- */

static void test_plain_line(void) {
    char buf[64];
    int64 n;

    printf("\ntty: a typed line comes back intact\n");
    fresh();

    type("ls -l");
    check(avail() == 0, "an unfinished line is not readable yet");
    type_enter();
    check(avail() == 6, "Enter (CR, mapped by ICRNL) completes it");
    n = rd(buf, sizeof(buf));

    check(n == 6, "returned the byte count including the newline");
    check(memcmp(buf, "ls -l\n", 6) == 0, "buffer holds exactly what was typed");
    check(strcmp(echo_text(), "ls -l\n") == 0,
          "every character was echoed once, AS IT WAS TYPED");
}

static void test_backspace(void) {
    char buf[64];
    int64 n;

    printf("\ntty: backspace removes from the line and the screen\n");
    fresh();

    type("lsx");
    press(SC_BACKSPACE);
    type(" -l");
    type_enter();
    n = rd(buf, sizeof(buf));

    check(n == 6, "the erased character is not counted");
    check(memcmp(buf, "ls -l\n", 6) == 0, "the erased character is not delivered");
    check(strcmp(echo_text(), "ls -l\n") == 0, "the screen matches the buffer");

    /* DEL (what a serial terminal sends) is VERASE and erases too. */
    fresh();
    kbd_inject('a');
    kbd_inject('b');
    kbd_inject(0x7F);
    kbd_inject('\r');
    n = rd(buf, sizeof(buf));
    check(n == 2 && memcmp(buf, "a\n", 2) == 0, "DEL from the serial line erases");
}

static void test_backspace_on_empty_line(void) {
    char buf[64];
    int64 n;

    printf("\ntty: backspace at the start of a line does nothing\n");
    fresh();

    press(SC_BACKSPACE);
    press(SC_BACKSPACE);
    type("ok");
    type_enter();
    n = rd(buf, sizeof(buf));

    /* The interesting failure here is an underflow: len-- at len == 0 wraps
     * and the next store writes far outside the buffer. */
    check(n == 3, "the line is unaffected");
    check(memcmp(buf, "ok\n", 3) == 0, "no underflow past the start of the buffer");
}

static void test_kill_and_werase(void) {
    char buf[64];
    int64 n;

    printf("\ntty: ^U kills the line, ^W erases a word\n");
    fresh();
    type("wrong words");
    ctrl('u');
    type("ls");
    type_enter();
    n = rd(buf, sizeof(buf));
    check(n == 3 && memcmp(buf, "ls\n", 3) == 0, "^U discards everything typed");
    check(strcmp(echo_text(), "ls\n") == 0, "and takes it off the screen (ECHOKE)");

    fresh();
    type("echo two words");
    ctrl('w');
    type_enter();
    n = rd(buf, sizeof(buf));
    check(n == 10 && memcmp(buf, "echo two \n", 10) == 0,
          "^W takes back the last word and nothing before it");
}

static void test_shift_and_caps(void) {
    char buf[64];

    printf("\ntty: shift and caps lock\n");
    fresh();

    kbd_scancode(SC_LSHIFT);
    type("ab");
    kbd_scancode(SC_LSHIFT | SC_RELEASE);
    type("c");
    type_enter();
    rd(buf, sizeof(buf));
    check(memcmp(buf, "ABc\n", 4) == 0, "shift capitalises only while held");

    fresh();
    press(SC_CAPS);
    type("ab");
    kbd_scancode(SC_LSHIFT);
    type("c");
    kbd_scancode(SC_LSHIFT | SC_RELEASE);
    type_enter();
    rd(buf, sizeof(buf));
    check(memcmp(buf, "ABc\n", 4) == 0, "caps lock XORs with shift rather than overriding it");
    press(SC_CAPS);   /* back off for later tests */
}

static void test_shifted_symbols(void) {
    char buf[64];

    printf("\ntty: shifted number row gives symbols\n");
    fresh();

    kbd_scancode(SC_LSHIFT);
    type("12");
    kbd_scancode(SC_LSHIFT | SC_RELEASE);
    type_enter();
    rd(buf, sizeof(buf));
    check(memcmp(buf, "!@\n", 3) == 0, "shift+1 and shift+2 are ! and @");
}

static void test_ctrl_c(void) {
    char buf[64];
    int64 n;

    printf("\ntty: Ctrl-C raises SIGINT when it is TYPED\n");
    fresh();

    type("half typed");
    ctrl('c');
    /* Nobody is reading. The old discipline only looked at ^C inside a
     * read, so a program not reading the terminal could not be interrupted
     * at all - `sleep 100` ran its full hundred seconds. */
    check(signal_stub_sends == 1, "one signal was sent, with no reader");
    check(signal_stub_last == 2, "and it was SIGINT");
    check(strcmp(echo_text(), "half typed^C\n") == 0, "echoed as ^C (ECHOCTL)");
    check(avail() == 0, "nothing became readable");
    type("x");
    type_enter();
    n = rd(buf, sizeof(buf));
    check(n == 2 && memcmp(buf, "x\n", 2) == 0,
          "the half-typed line was discarded, not prefixed to the next one");

    /* Type-ahead queued before the ^C belonged to the interrupted command
     * too - delivering it would run a command nobody typed at this prompt. */
    fresh();
    type("rm");
    type_enter();
    ctrl('c');
    check(avail() == 0, "a whole line typed ahead of the ^C is flushed too");
    type("ok");
    type_enter();
    n = rd(buf, sizeof(buf));
    check(n == 3 && memcmp(buf, "ok\n", 3) == 0, "and the next line reads normally");

    fresh();
    ctrl('z');
    check(signal_stub_sends == 1 && signal_stub_last == 20, "^Z is SIGTSTP");
    fresh();
    kbd_inject(28);                   /* ^\ */
    check(signal_stub_sends == 1 && signal_stub_last == 3, "^\\ is SIGQUIT");
}

static void test_ctrl_d(void) {
    char buf[64];
    int64 n;

    printf("\ntty: Ctrl-D\n");
    fresh();

    ctrl('d');
    check(avail() >= 0, "(queued)");
    n = rd(buf, sizeof(buf));
    check(n == 0, "on an empty line it is end of input - the shell will exit");

    fresh();
    type("hi");
    ctrl('d');
    n = rd(buf, sizeof(buf));
    check(n == 2 && memcmp(buf, "hi", 2) == 0,
          "mid-line it submits what was typed, with no newline");
}

static void test_line_longer_than_buffer(void) {
    char buf[8];
    int64 n;

    printf("\ntty: a line longer than the buffer\n");
    fresh();

    type("0123456789");
    type_enter();

    n = rd(buf, sizeof(buf));
    check(n == 8, "the first read fills the buffer exactly");
    check(memcmp(buf, "01234567", 8) == 0, "and holds the first 8 bytes");

    n = rd(buf, sizeof(buf));
    check(n == 3 && memcmp(buf, "89\n", 3) == 0,
          "the rest arrives on the next read instead of being discarded");
}

static void test_one_line_per_read(void) {
    char buf[64];
    int64 n;

    printf("\ntty: canonical reads stop at the end of a line\n");
    fresh();
    type("one");
    type_enter();
    type("two");
    type_enter();
    n = rd(buf, sizeof(buf));
    check(n == 4 && memcmp(buf, "one\n", 4) == 0, "the first read gets one line");
    n = rd(buf, sizeof(buf));
    check(n == 4 && memcmp(buf, "two\n", 4) == 0, "the second read gets the next");
}

/* --- non-canonical (raw) mode --------------------------------------------- */

static void test_raw_mode(void) {
    char buf[64];
    int64 n;
    struct kt t;

    printf("\ntty: raw mode, as readline sets it\n");
    fresh();
    raw_mode();

    type("ab");
    check(avail() == 2, "bytes are readable at once, no Enter needed");
    check(echo_len == 0, "and nothing was echoed - readline echoes itself");
    n = rd(buf, 1);
    check(n == 1 && buf[0] == 'a', "VMIN=1 returns the first byte alone");
    n = rd(buf, sizeof(buf));
    check(n == 1 && buf[0] == 'b', "and the next read the rest");

    type_enter();
    n = rd(buf, sizeof(buf));
    check(n == 1 && buf[0] == '\n', "Enter still reads as NL under ICRNL");

    /* A PS/2 arrow key must reach readline as the sequence a VT100 sends -
     * this used to be swallowed, so history recall was impossible. */
    kbd_scancode(0xE0);
    kbd_scancode(0x48);
    kbd_scancode(0xE0);
    kbd_scancode(0x48 | SC_RELEASE);
    n = rd(buf, sizeof(buf));
    check(n == 3 && memcmp(buf, "\033[A", 3) == 0, "Up arrow reads as ESC [ A");

    ctrl('c');
    check(signal_stub_sends == 1 && signal_stub_last == 2,
          "^C still signals in raw mode while ISIG is on");

    get_mode(&t);
    t.lflag &= ~(uint32)L_ISIG;
    set_mode(&t);
    signal_stub_sends = 0;
    ctrl('c');
    n = rd(buf, sizeof(buf));
    check(signal_stub_sends == 0 && n == 1 && buf[0] == 3,
          "with ISIG off, ^C is just byte 3");

    /* VMIN=0 VTIME=0 is a poll: an empty terminal returns 0 at once. */
    get_mode(&t);
    t.cc[CC_VMIN] = 0;
    t.cc[CC_VTIME] = 0;
    set_mode(&t);
    n = rd(buf, sizeof(buf));
    check(n == 0, "VMIN=0 VTIME=0 on an empty queue returns 0 without blocking");
}

static void test_mode_switch(void) {
    char buf[64];
    int64 n;
    struct kt t, back;

    printf("\ntty: switching modes keeps what was typed\n");
    fresh();
    get_mode(&back);
    type("par");
    raw_mode();
    check(avail() == 3, "a half line becomes readable when ICANON goes off");
    n = rd(buf, sizeof(buf));
    check(n == 3 && memcmp(buf, "par", 3) == 0, "with its bytes intact");

    set_mode(&back);
    get_mode(&t);
    check(t.lflag == back.lflag && t.iflag == back.iflag &&
          t.cc[CC_VMIN] == back.cc[CC_VMIN],
          "TCGETS returns exactly what TCSETS stored");
    check((t.lflag & L_ICANON) && (t.lflag & L_ECHO) && (t.iflag & I_ICRNL),
          "and the default is canonical, echoing, CR->NL");

    /* TCSETSF flushes pending input; TCSETS does not. */
    type("gone");
    type_enter();
    tty_ioctl(0x5404, (uint64)(uintptr)&t);
    check(avail() == 0, "TCSETSF discards unread input");
}

static void test_winsize(void) {
    uint16 ws[4] = { 0, 0, 0, 0 };
    uint16 set[4] = { 50, 132, 0, 0 };

    printf("\ntty: window size\n");
    fresh();
    tty_ioctl(0x5413, (uint64)(uintptr)ws);
    check(ws[0] == 25 && ws[1] == 80, "TIOCGWINSZ reports the console grid");
    tty_ioctl(0x5414, (uint64)(uintptr)set);
    check(signal_stub_sends == 1 && signal_stub_last == 28,
          "TIOCSWINSZ with a new size sends SIGWINCH");
    tty_ioctl(0x5413, (uint64)(uintptr)ws);
    check(ws[0] == 50 && ws[1] == 132, "and the new size is what is reported");
    signal_stub_sends = 0;
    tty_ioctl(0x5414, (uint64)(uintptr)set);
    check(signal_stub_sends == 0, "setting the same size again signals nobody");
    check(tty_ioctl(0x1234, 0) == -25, "an unknown request is -ENOTTY");
}

static void test_extended_and_release(void) {
    char buf[64];
    int64 n;

    printf("\ntty: release codes and unmapped extended keys type nothing\n");
    fresh();

    kbd_scancode(0xE0);              /* right Alt: prefix... */
    kbd_scancode(0x38);              /* ...then a code that is plain Alt */
    kbd_scancode(scancode_for('a') | SC_RELEASE);   /* a bare release */
    type("z");
    type_enter();

    n = rd(buf, sizeof(buf));
    check(n == 2 && memcmp(buf, "z\n", 2) == 0,
          "neither the extended key nor the stray release typed a character");
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
 * calls sched_wake deliberately, and it must not be able to hide a terminal
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
    process_t *pa = &stub_table[0], *pb = &stub_table[1];
#define a (*pa)
#define b (*pb)

    printf("\nwaitq: the list keyboard.c used to own\n");
    stub_table_live = 1;

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

    /* A slot freed while still queued (a process killed mid-wait whose
     * teardown missed the queue) is skipped, not woken as a dead entry. */
    waitq_add(&q, &a);
    stub_table_live = 0;
    before = sched_stub_calls;
    waitq_wake_all(&q);
    check(sched_stub_calls == before,
          "a slot that is no longer in use is not woken");
    waitq_init(&q);
#undef a
#undef b
}

int kbd_run_tests(void) {
    test_plain_line();
    test_backspace();
    test_backspace_on_empty_line();
    test_kill_and_werase();
    test_shift_and_caps();
    test_shifted_symbols();
    test_ctrl_c();
    test_ctrl_d();
    test_line_longer_than_buffer();
    test_one_line_per_read();
    test_raw_mode();
    test_mode_switch();
    test_winsize();
    test_extended_and_release();

    printf("\ntty: the blocking path was never entered\n");
    /* Every test above queues its input before reading, so no read ever had
     * to block. If this ever fires, a test changed shape and the stubs above
     * started standing in for real scheduling. */
    check(sched_stub_calls == 0, "no test relied on the scheduler stubs");

    test_waitq();
    return kbd_failures;
}
