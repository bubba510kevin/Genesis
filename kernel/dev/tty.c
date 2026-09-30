#include "keyboard.h"
#include "object.h"
#include "process.h"
#include "screen.h"
#include "signal.h"
#include "timer.h"
#include "tty.h"
#include "typesk.h"
#include "waitq.h"

/* --- the console terminal and its line discipline (ROADMAP item 15(k)) ---
 *
 * One terminal - the console, fed by the PS/2 keyboard and the serial line
 * through tty_input() - with a real termios behind it. Until this file grew
 * one, the line discipline lived in keyboard.c as kbd_read_line: always
 * canonical, always echoing, and TCSETS was "accepted and ignored". That was
 * enough for BusyBox built without line editing and not for anything else:
 * readline asks for raw mode, gets cooked input anyway, and a line typed at
 * bash shows up twice.
 *
 * WHAT CHANGED IN SHAPE, not just in features: input is processed WHEN IT
 * ARRIVES, in the interrupt, as Linux's n_tty does - not when somebody
 * reads. That is what makes three things work that could not before:
 *
 *   Ctrl-C reaches a program that is not reading the terminal. The old
 *   discipline only saw the byte inside a read(), so `sleep 100` could not
 *   be interrupted at all - nothing was reading.
 *
 *   Type-ahead is echoed as it is typed, not later when a read consumes it.
 *
 *   Non-canonical mode has somewhere to come from: bytes are queued already
 *   processed (ICRNL applied, signals taken out), and a read in raw mode
 *   just takes them.
 *
 * The interrupt holds the big kernel lock (interrupt.c takes it), which is
 * what makes signal_send and waking readers from here safe.
 *
 * THE QUEUE holds processed input as 16-bit cells. A byte is itself; a cell
 * of TTY_EOF_MARK is an end-of-file typed in canonical mode (VEOF on an
 * empty line, or VEOF ending a partial one). A line is counted as ready when
 * its terminator (a newline, VEOL, or an EOF mark) is queued, and canonical
 * reads return at most one line. The partial line being edited is NOT in
 * the queue - it lives in edit[], where erase and kill act on it - and
 * moves to the queue when it is terminated.
 *
 * What is deliberately not here: IXON/IXOFF flow control (there is no
 * output queue to stop), parity and baud handling (c_cflag is stored and
 * returned, nothing interprets it), and output post-processing beyond what
 * the console already does (a '\n' written reaches the serial line as CRLF
 * whatever OPOST says). */

/* Linux's termios bits, the ones this discipline acts on. */
#define T_IGNBRK  0000001
#define T_ISTRIP  0000040
#define T_INLCR   0000100
#define T_IGNCR   0000200
#define T_ICRNL   0000400
#define T_IXON    0002000
#define T_IUTF8   0040000

#define T_OPOST   0000001
#define T_ONLCR   0000004

#define T_ISIG    0000001
#define T_ICANON  0000002
#define T_ECHO    0000010
#define T_ECHOE   0000020
#define T_ECHOK   0000040
#define T_ECHONL  0000100
#define T_NOFLSH  0000200
#define T_TOSTOP  0000400
#define T_ECHOCTL 0001000
#define T_ECHOKE  0004000
#define T_IEXTEN  0100000

/* c_cc indices. */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP   10
#define VEOL    11
#define VREPRINT 12
#define VWERASE 14
#define VLNEXT  15
#define VEOL2   16
#define NCCS    19

/* The kernel's struct termios (TCGETS/TCSETS): 36 bytes. Not the 60-byte
 * glibc one - libc translates. */
struct ktermios {
    uint32 c_iflag;
    uint32 c_oflag;
    uint32 c_cflag;
    uint32 c_lflag;
    uint8  c_line;
    uint8  c_cc[NCCS];
};
typedef char ktermios_size_assert[(sizeof(struct ktermios) == 36) ? 1 : -1];

/* ioctl requests handled here. */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCSBRK      0x5409
#define TCXONC      0x540A
#define TCFLSH      0x540B
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCOUTQ    0x5411
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define FIONREAD    0x541B
#define TIOCNOTTY   0x5422
#define TIOCGSID    0x5429
#define TCSBRKP     0x5425

#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

#define TTY_EOF_MARK 0x100
#define QUEUE_SIZE   4096          /* cells; a power of two */
#define EDIT_MAX     4095          /* Linux's N_TTY_BUF_SIZE - 1 */

#define ECHO_COLOR   0x0F

static struct ktermios termios;

static uint16 queue[QUEUE_SIZE];
static uint32 q_head;              /* next cell to write */
static uint32 q_tail;              /* next cell to read  */
static uint32 lines_ready;         /* terminators queued and not yet read */

static char   edit[EDIT_MAX];
static uint32 edit_len;
static int    lnext_pending;       /* the previous byte was VLNEXT */

static int    foreground_pgid;
static int    tty_sid;             /* session this is the controlling tty of */
static uint16 win_rows, win_cols, win_xpix, win_ypix;
static int    win_set;             /* TIOCSWINSZ has overridden the screen */

static wait_queue_t readers;

static object_t *console;

/* --- defaults -------------------------------------------------------------
 * What Linux's console starts with (`stty sane`): canonical, echoing with
 * the visual erase, signals on, CR mapped to NL on input and NL to CRLF on
 * output. VERASE is DEL, which is what a serial terminal sends for
 * Backspace; the PS/2 keymap produces '\b' for it, and both are treated as
 * erase (see is_erase). */
void tty_init(void) {
    int i;

    termios.c_iflag = T_ICRNL | T_IXON | T_IUTF8;
    termios.c_oflag = T_OPOST | T_ONLCR;
    termios.c_cflag = 0x00BF;          /* B38400 | CS8 | CREAD */
    termios.c_lflag = T_ISIG | T_ICANON | T_ECHO | T_ECHOE | T_ECHOK |
                      T_ECHOCTL | T_ECHOKE | T_IEXTEN;
    termios.c_line  = 0;
    for (i = 0; i < NCCS; i++) {
        termios.c_cc[i] = 0;
    }
    termios.c_cc[VINTR]   = 3;         /* ^C */
    termios.c_cc[VQUIT]   = 28;        /* ^\ */
    termios.c_cc[VERASE]  = 0x7F;      /* DEL */
    termios.c_cc[VKILL]   = 21;        /* ^U */
    termios.c_cc[VEOF]    = 4;         /* ^D */
    termios.c_cc[VTIME]   = 0;
    termios.c_cc[VMIN]    = 1;
    termios.c_cc[VSTART]  = 17;        /* ^Q */
    termios.c_cc[VSTOP]   = 19;        /* ^S */
    termios.c_cc[VSUSP]   = 26;        /* ^Z */
    termios.c_cc[VREPRINT] = 18;       /* ^R */
    termios.c_cc[VWERASE] = 23;        /* ^W */
    termios.c_cc[VLNEXT]  = 22;        /* ^V */
    q_head = q_tail = lines_ready = 0;
    edit_len = 0;
    lnext_pending = 0;
    foreground_pgid = 0;
    tty_sid = 0;
    win_set = 0;
    waitq_init(&readers);
}

/* --- the queue ------------------------------------------------------------ */

static uint32 q_count(void) {
    return q_head - q_tail;
}

static int q_push(uint16 cell) {
    if (q_count() >= QUEUE_SIZE) {
        return 0;              /* full: dropped, as n_tty drops past its buffer */
    }
    queue[q_head % QUEUE_SIZE] = cell;
    q_head++;
    return 1;
}

static void flush_input(void) {
    q_tail = q_head;
    lines_ready = 0;
    edit_len = 0;
    lnext_pending = 0;
}

static int lflag(uint32 bit) {
    return (termios.c_lflag & bit) != 0;
}

/* A _POSIX_VDISABLE (0) entry matches nothing. */
static int is_cc(uint8 c, int idx) {
    return termios.c_cc[idx] != 0 && c == termios.c_cc[idx];
}

/* VERASE, and '\b' too: the PS/2 keymap sends '\b' for Backspace while a
 * serial terminal sends DEL, and a console that ignored one of its two
 * keyboards' Backspace keys would be worse than this small deviation. */
static int is_erase(uint8 c) {
    return is_cc(c, VERASE) || c == '\b';
}

/* --- echo -----------------------------------------------------------------
 * ECHOCTL shows a control character as ^X; tab and newline are themselves. */
static void echo_char(uint8 c) {
    if (c < 0x20 && c != '\t' && c != '\n' && lflag(T_ECHOCTL)) {
        print_char('^', ECHO_COLOR);
        print_char((char)(c + '@'), ECHO_COLOR);
    } else if (c == 0x7F && lflag(T_ECHOCTL)) {
        print_char('^', ECHO_COLOR);
        print_char('?', ECHO_COLOR);
    } else {
        print_char((char)c, ECHO_COLOR);
    }
}

/* How many columns the echo of c took, so erase can take it back. */
static int echo_width(uint8 c) {
    if ((c < 0x20 && c != '\t' && c != '\n') || c == 0x7F) {
        return lflag(T_ECHOCTL) ? 2 : 0;
    }
    return 1;
}

static void erase_one(void) {
    uint8 c;
    int w;

    if (edit_len == 0) {
        return;
    }
    c = (uint8)edit[--edit_len];
    if (lflag(T_ECHO) && lflag(T_ECHOE)) {
        for (w = echo_width(c); w > 0; w--) {
            print_backspace(ECHO_COLOR);
        }
    }
}

/* --- input ----------------------------------------------------------------
 * One byte from a keyboard, in interrupt context (or from the serial poll,
 * which is the same thing with the lock already held). */

static void wake_readers(void) {
    waitq_wake_all(&readers);
}

/* The foreground group, or 0 when nobody has claimed the terminal or the
 * group that did has no members left - a job-control shell that died
 * without handing the terminal back must not leave it owned by nobody. */
static int fg_group_live(void) {
    int i;

    if (foreground_pgid <= 0) {
        return 0;
    }
    for (i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = proc_at(i);

        if (p != NULL && p->state != PROC_UNUSED && p->state != PROC_ZOMBIE &&
            p->pgid == foreground_pgid) {
            return foreground_pgid;
        }
    }
    return 0;
}

static void input_signal(int signo, uint8 c) {
    if (!lflag(T_NOFLSH)) {
        flush_input();
    }
    if (lflag(T_ECHO)) {
        echo_char(c);
        print_char('\n', ECHO_COLOR);
    }
    signal_send_group(tty_foreground_pgid(), signo);
    /* A reader of this terminal in the foreground group is interrupted by
     * the signal it was just sent; waking it is what lets it see that. */
    wake_readers();
}

static void commit_edit(uint16 terminator) {
    uint32 i;

    for (i = 0; i < edit_len; i++) {
        q_push((uint8)edit[i]);
    }
    edit_len = 0;
    if (q_push(terminator)) {
        lines_ready++;
    }
}

void tty_input(char ch) {
    uint8 c = (uint8)ch;

    /* Input mapping (c_iflag). */
    if (termios.c_iflag & T_ISTRIP) {
        c &= 0x7F;
    }
    if (!lnext_pending) {
        if (c == '\r') {
            if (termios.c_iflag & T_IGNCR) {
                return;
            }
            if (termios.c_iflag & T_ICRNL) {
                c = '\n';
            }
        } else if (c == '\n' && (termios.c_iflag & T_INLCR)) {
            c = '\r';
        }
    }

    /* The literal-next escape: in canonical mode the byte after ^V is data,
     * whatever it is (n_tty only honours VLNEXT under ICANON; in raw mode ^V
     * is itself just data, and readline implements its own quoted-insert). */
    if (lnext_pending) {
        lnext_pending = 0;
        if (lflag(T_ECHO)) {
            print_backspace(ECHO_COLOR);   /* the "^" shown as a placeholder */
        }
        goto ordinary;
    }
    if (lflag(T_ICANON) && lflag(T_IEXTEN) && is_cc(c, VLNEXT)) {
        lnext_pending = 1;
        if (lflag(T_ECHO)) {
            print_char('^', ECHO_COLOR);
        }
        return;
    }

    /* Signal characters (c_lflag ISIG). */
    if (lflag(T_ISIG)) {
        if (is_cc(c, VINTR)) {
            input_signal(SIGINT, c);
            return;
        }
        if (is_cc(c, VQUIT)) {
            input_signal(SIGQUIT, c);
            return;
        }
        if (is_cc(c, VSUSP)) {
            input_signal(SIGTSTP, c);
            return;
        }
    }

    if (!lflag(T_ICANON)) {
        goto raw;
    }

    /* Canonical editing. */
    if (is_erase(c)) {
        erase_one();
        return;
    }
    if (is_cc(c, VKILL)) {
        if (lflag(T_ECHO) && lflag(T_ECHOK) && !lflag(T_ECHOKE)) {
            echo_char(c);
            print_char('\n', ECHO_COLOR);
            edit_len = 0;
        } else {
            while (edit_len > 0) {
                erase_one();
            }
        }
        return;
    }
    if (lflag(T_IEXTEN) && is_cc(c, VWERASE)) {
        while (edit_len > 0 && (edit[edit_len - 1] == ' ' ||
                                edit[edit_len - 1] == '\t')) {
            erase_one();
        }
        while (edit_len > 0 && edit[edit_len - 1] != ' ' &&
               edit[edit_len - 1] != '\t') {
            erase_one();
        }
        return;
    }
    if (lflag(T_IEXTEN) && is_cc(c, VREPRINT)) {
        uint32 i;

        if (lflag(T_ECHO)) {
            echo_char(c);
            print_char('\n', ECHO_COLOR);
            for (i = 0; i < edit_len; i++) {
                echo_char((uint8)edit[i]);
            }
        }
        return;
    }
    if (is_cc(c, VEOF)) {
        /* Ends the line WITHOUT a newline: on an empty line that is a read
         * of zero bytes, end of file; mid-line it hands over what was typed
         * so `read` sees a partial last line. Not echoed. */
        commit_edit(TTY_EOF_MARK);
        wake_readers();
        return;
    }
    if (c == '\n' || is_cc(c, VEOL) || is_cc(c, VEOL2)) {
        if (lflag(T_ECHO) || (c == '\n' && lflag(T_ECHONL))) {
            echo_char(c);
        }
        commit_edit(c);
        wake_readers();
        return;
    }

ordinary:
    if (edit_len >= EDIT_MAX) {
        return;        /* the line is full; n_tty drops what does not fit */
    }
    edit[edit_len++] = (char)c;
    if (lflag(T_ECHO)) {
        echo_char(c);
    }
    return;

raw:
    if (q_push(c)) {
        if (lflag(T_ECHO)) {
            echo_char(c);
        } else if (c == '\n' && lflag(T_ECHONL)) {
            echo_char(c);
        }
        wake_readers();
    }
}

/* --- reading ---------------------------------------------------------------
 *
 * Canonical: block until a line is ready, return at most that line. A
 * buffer too small for it gets the front and the rest stays for the next
 * read. An EOF mark ends the read without being returned, so an EOF on an
 * empty line reads as 0.
 *
 * Non-canonical, VMIN/VTIME exactly as termios(3) defines them:
 *   MIN>0 TIME=0  block until MIN bytes (or the buffer is full)
 *   MIN=0 TIME>0  wait up to TIME tenths for one byte; 0 on timeout
 *   MIN>0 TIME>0  block for the first byte, then an INTER-byte timer
 *   MIN=0 TIME=0  whatever is there, possibly nothing, never blocking */

static int ready_canon(void *ctx) {
    (void)ctx;
    return lines_ready > 0 || !lflag(T_ICANON);
}

static int ready_any(void *ctx) {
    (void)ctx;
    return q_count() > 0 || lflag(T_ICANON);
}

/* A read from a background process group. Linux's job-control rule: the
 * group is sent SIGTTIN and the read fails, unless SIGTTIN is ignored or
 * blocked, in which case it fails with EIO. Only for a process in the
 * terminal's session, and only while some live group owns the terminal. */
static int background_read_check(void) {
    process_t *p = proc_current();
    int fg = fg_group_live();
    int sid;

    if (p == NULL || fg == 0 || p->pgid == fg) {
        return 0;
    }
    sid = p->sid != 0 ? p->sid : p->pgid;
    if (tty_sid != 0 && sid != tty_sid) {
        return 0;
    }
    if (p->sig_handlers[SIGTTIN].handler == SIG_IGN_HANDLER ||
        (p->sig_blocked & sigmask_of(SIGTTIN))) {
        return -5;                     /* -EIO */
    }
    signal_send_group(p->pgid, SIGTTIN);
    return -4;                         /* -EINTR (Linux: ERESTARTSYS) */
}

static int64 read_canonical(uint8 *buf, uint64 n) {
    uint64 got = 0;

    for (;;) {
        int rc;

        if (lines_ready > 0) {
            break;
        }
        rc = waitq_wait(&readers, ready_canon, NULL);
        if (rc != WAITQ_READY) {
            return -4;                 /* -EINTR */
        }
        if (!lflag(T_ICANON)) {
            return -11;                /* mode changed under us: retry (EAGAIN) */
        }
    }
    while (got < n && q_count() > 0) {
        uint16 cell = queue[q_tail % QUEUE_SIZE];

        if (cell == TTY_EOF_MARK) {
            q_tail++;
            lines_ready--;
            break;
        }
        q_tail++;
        buf[got++] = (uint8)cell;
        if (cell == '\n' || is_cc((uint8)cell, VEOL) || is_cc((uint8)cell, VEOL2)) {
            lines_ready--;
            break;
        }
    }
    return (int64)got;
}

static uint64 take_raw(uint8 *buf, uint64 got, uint64 n) {
    while (got < n && q_count() > 0) {
        uint16 cell = queue[q_tail % QUEUE_SIZE];

        q_tail++;
        if (cell == TTY_EOF_MARK) {
            /* An EOF queued before the switch to raw mode: Linux would have
             * delivered it already; here it simply ends this read. */
            if (lines_ready > 0) {
                lines_ready--;
            }
            break;
        }
        if (cell == '\n' && lines_ready > 0) {
            lines_ready--;
        }
        buf[got++] = (uint8)cell;
    }
    return got;
}

static int64 read_raw(uint8 *buf, uint64 n) {
    uint64 vmin  = termios.c_cc[VMIN];
    uint64 vtime = termios.c_cc[VTIME];
    uint64 got = 0;
    uint64 tick_per_decisec = timer_hz() / 10 ? timer_hz() / 10 : 1;

    if (vmin > n) {
        vmin = n;
    }
    if (vmin == 0 && vtime == 0) {
        return (int64)take_raw(buf, 0, n);
    }
    for (;;) {
        uint64 deadline = 0;
        int rc;

        got = take_raw(buf, got, n);
        if (got >= n || (vmin > 0 && got >= vmin)) {
            return (int64)got;
        }
        if (vmin == 0 && got > 0) {
            return (int64)got;
        }
        /* The timer: overall for MIN=0, between bytes once one has arrived
         * for MIN>0. Before the first byte, MIN>0 waits forever. */
        if (vtime > 0 && (vmin == 0 || got > 0)) {
            deadline = timer_ticks_now() + vtime * tick_per_decisec + 1;
        }
        rc = waitq_wait_until(&readers, ready_any, NULL, deadline);
        if (rc == WAITQ_SIGNAL) {
            return got > 0 ? (int64)got : -4;
        }
        if (rc == WAITQ_TIMEOUT) {
            return (int64)got;
        }
        if (lflag(T_ICANON)) {
            return got > 0 ? (int64)got : -11;
        }
    }
}

static int64 console_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    int rc;

    (void)obj;
    (void)offset;
    if (n == 0) {
        return 0;
    }
    rc = background_read_check();
    if (rc != 0) {
        return rc;
    }
    for (;;) {
        int64 got = lflag(T_ICANON) ? read_canonical((uint8 *)buf, n)
                                    : read_raw((uint8 *)buf, n);

        /* -EAGAIN from inside means the mode changed while waiting; read
         * again under the new one. */
        if (got != -11) {
            return got;
        }
    }
}

static int64 console_write(object_t *obj, const void *buf, uint64 n, uint64 *offset) {
    const char *s = (const char *)buf;
    uint64 done = 0;

    (void)obj;
    (void)offset;

    /* Copied through a bounded stack buffer rather than printed directly:
     * print_string wants a terminator and the caller's bytes have none, and
     * a user pointer must never be handed to something that scans for a NUL
     * of its own. */
    while (done < n) {
        char chunk[65];
        uint64 i, take = n - done;

        if (take > sizeof(chunk) - 1) {
            take = sizeof(chunk) - 1;
        }
        for (i = 0; i < take; i++) {
            chunk[i] = s[done + i];
        }
        chunk[take] = '\0';
        print_string(chunk, 0x0A);
        done += take;
    }
    return (int64)n;
}

/* Readable when a read would not block: a whole line in canonical mode, any
 * byte in raw mode. Writable always.
 *
 * There is no POLLHUP: a console has no far end to hang up. Ctrl-D produces
 * an end-of-file from one read and the terminal is readable again straight
 * afterwards, which is why it is not reported as a hangup - a shell that read
 * POLLHUP from its terminal would stop polling it for good after the first
 * Ctrl-D, and never see the next command. */
static int console_poll(object_t *obj, int events) {
    int ready = OB_POLLOUT;

    (void)obj;
    (void)events;
    if (lflag(T_ICANON) ? lines_ready > 0 : q_count() > 0) {
        ready |= OB_POLLIN;
    }
    return ready;
}

static const object_type_t console_type = {
    .name    = "console",
    .klass   = OBJ_CONSOLE,
    .read    = console_read,
    .write   = console_write,
    .poll    = console_poll,
    .destroy = NULL   /* never destroyed - it outlives every process */
};

/* --- the foreground process group -----------------------------------------
 * Ctrl-C goes here, and only here - that is the entire point of process
 * groups from the terminal's side. A shell sets it with TIOCSPGRP each time
 * it starts a job, so the signal follows whatever is actually in front of
 * the user rather than going to everything at once. */
int tty_foreground_pgid(void) {
    /* Nobody has claimed the terminal yet (or the group that did is gone).
     * That is the normal state right up until a shell with job control
     * calls TIOCSPGRP, and busybox built without ASH_JOB_CONTROL never calls
     * it at all - so leaving this zero meant Ctrl-C had no group to signal
     * and silently did nothing.
     *
     * The process asking is, by definition on a single console, the one in
     * front of the user: it is either the one blocked in read() on the tty or
     * the one asking who owns it. Answering with its own group is what a
     * session with no job control looks like, and it makes TIOCGPGRP return
     * something a shell's job-control probe can accept rather than a group id
     * that belongs to no process. */
    int fg = fg_group_live();

    if (fg <= 0) {
        process_t *p = proc_current();

        if (p != NULL) {
            return p->pgid > 0 ? p->pgid : p->pid;
        }
    }
    return fg;
}

void tty_set_foreground_pgid(int pgid) {
    if (pgid > 0) {
        foreground_pgid = pgid;
    }
}

/* --- ioctl ------------------------------------------------------------------
 * The terminal half of sys_ioctl. Returns -ENOTTY for anything it does not
 * know, so the caller can try its own (KDSETMODE and friends). `arg` has
 * been range-checked by the caller when non-zero. */
static void get_winsize(uint16 *ws) {
    if (win_set) {
        ws[0] = win_rows;
        ws[1] = win_cols;
        ws[2] = win_xpix;
        ws[3] = win_ypix;
    } else {
        /* The console's real grid: 80x25 in text mode, larger once it
         * draws into a framebuffer. */
        ws[0] = (uint16)screen_height_chars();
        ws[1] = (uint16)screen_width_chars();
        ws[2] = 0;
        ws[3] = 0;
    }
}

static int set_termios(const struct ktermios *t, int flush) {
    int was_canon = lflag(T_ICANON);

    if (flush) {
        flush_input();
    }
    termios = *t;
    if (was_canon && !lflag(T_ICANON) && edit_len > 0) {
        /* Leaving canonical mode: what was typed of an unfinished line
         * becomes readable at once, as n_tty does. */
        uint32 i;

        for (i = 0; i < edit_len; i++) {
            q_push((uint8)edit[i]);
        }
        edit_len = 0;
    }
    wake_readers();
    return 0;
}

int64 tty_ioctl(uint32 request, uint64 arg) {
    process_t *p = proc_current();
    uint8 *ptr = (uint8 *)arg;

    switch (request) {
        case TCGETS:
            if (arg == 0) {
                return -14;
            }
            *(struct ktermios *)ptr = termios;
            return 0;

        case TCSETS:
        case TCSETSW:           /* output is never queued, so "drain" is a no-op */
        case TCSETSF:
            if (arg == 0) {
                return -14;
            }
            return set_termios((const struct ktermios *)ptr, request == TCSETSF);

        case TCFLSH:
            if (arg == TCIFLUSH || arg == TCIOFLUSH) {
                flush_input();
                return 0;
            }
            return arg == TCOFLUSH ? 0 : -22;

        case TCXONC:
        case TCSBRK:
        case TCSBRKP:
            return 0;           /* no flow control, no line to break */

        case TIOCOUTQ:
            if (arg == 0) {
                return -14;
            }
            *(int *)ptr = 0;    /* nothing is ever waiting to go out */
            return 0;

        case FIONREAD:
            if (arg == 0) {
                return -14;
            }
            /* In canonical mode, only what a read could return now. */
            *(int *)ptr = (lflag(T_ICANON) && lines_ready == 0)
                        ? 0 : (int)q_count();
            return 0;

        case TIOCGPGRP:
            if (arg == 0) {
                return -14;
            }
            *(int *)ptr = tty_foreground_pgid();
            return 0;

        case TIOCSPGRP: {
            int pgid;

            if (arg == 0) {
                return -14;
            }
            pgid = *(const int *)ptr;
            if (pgid <= 0) {
                return -22;
            }
            /* The shell claiming the terminal for a job. Everything typed at
             * the keyboard - Ctrl-C in particular - goes to this group from
             * here on. */
            tty_set_foreground_pgid(pgid);
            return 0;
        }

        case TIOCGSID:
            if (arg == 0) {
                return -14;
            }
            if (tty_sid == 0) {
                return -25;     /* -ENOTTY: not anyone's controlling tty */
            }
            *(int *)ptr = tty_sid;
            return 0;

        case TIOCSCTTY:
            /* Make this the caller's controlling terminal. The caller must be
             * a session leader; stealing it from another session needs root
             * and arg 1, as on Linux. */
            if (p == NULL || p->sid != p->pid) {
                return -1;      /* -EPERM */
            }
            if (tty_sid != 0 && tty_sid != p->sid) {
                if (!(arg == 1 && p->uid == 0)) {
                    return -1;
                }
            }
            tty_sid = p->sid;
            foreground_pgid = p->pgid;
            return 0;

        case TIOCNOTTY:
            if (p != NULL && tty_sid != 0 && p->sid == tty_sid &&
                p->sid == p->pid) {
                /* The session leader giving it up: the foreground group is
                 * hung up, as Linux does. */
                signal_send_group(fg_group_live(), SIGHUP);
                signal_send_group(fg_group_live(), SIGCONT);
                tty_sid = 0;
                foreground_pgid = 0;
            }
            return 0;

        case TIOCGWINSZ:
            if (arg == 0) {
                return -14;
            }
            get_winsize((uint16 *)ptr);
            return 0;

        case TIOCSWINSZ: {
            const uint16 *ws = (const uint16 *)ptr;
            uint16 old[4];

            if (arg == 0) {
                return -14;
            }
            get_winsize(old);
            win_rows = ws[0];
            win_cols = ws[1];
            win_xpix = ws[2];
            win_ypix = ws[3];
            win_set = 1;
            if (old[0] != ws[0] || old[1] != ws[1] ||
                old[2] != ws[2] || old[3] != ws[3]) {
                signal_send_group(fg_group_live(), SIGWINCH);
            }
            return 0;
        }

        default:
            return -25;         /* -ENOTTY */
    }
}

object_t *tty_console(void) {
    if (console == NULL) {
        console = ob_create(&console_type, NULL);
        /* Held forever, so the refcount never reaches zero and the shared
         * instance survives the last process closing its descriptors. */
        ob_ref(console);
    }
    return console;
}
