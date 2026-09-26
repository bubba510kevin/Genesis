#include "keyboard.h"
#include "object.h"
#include "process.h"
#include "screen.h"
#include "tty.h"
#include "typesk.h"

/* Offset is ignored throughout, and that IS the semantics of a terminal: it
 * has no position, seeking it is meaningless, and a program that tries gets
 * the same answer a real system gives - the write lands at the cursor. */

static int64 console_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    (void)obj;
    (void)offset;
    if (n == 0) {
        return 0;
    }
    /* Passed through signed and unchanged: a negative here is -EINTR from an
     * interrupted line read, and flattening it to a byte count would turn an
     * interrupted read back into end of input. */
    return kbd_read_line((char *)buf, n);
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

/* Readable when the line discipline holds a COMPLETED line, writable always.
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
    if (kbd_has_line()) {
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

static object_t *console;

/* The foreground process group. Ctrl-C goes here, and only here - that is the
 * entire point of process groups from the terminal's side. A shell sets it
 * with TIOCSPGRP each time it starts a job, so the signal follows whatever is
 * actually in front of the user rather than going to everything at once. */
static int foreground_pgid;

int tty_foreground_pgid(void) {
    /* Nobody has claimed the terminal yet. That is the normal state right up
     * until a shell with job control calls TIOCSPGRP, and busybox built
     * without ASH_JOB_CONTROL never calls it at all - so leaving this zero
     * meant Ctrl-C had no group to signal and silently did nothing.
     *
     * The process asking is, by definition on a single console, the one in
     * front of the user: it is either the one blocked in read() on the tty or
     * the one asking who owns it. Answering with its own group is what a
     * session with no job control looks like, and it makes TIOCGPGRP return
     * something a shell's job-control probe can accept rather than a group id
     * that belongs to no process. */
    if (foreground_pgid <= 0) {
        process_t *p = proc_current();

        if (p != NULL) {
            return p->pgid > 0 ? p->pgid : p->pid;
        }
    }
    return foreground_pgid;
}

void tty_set_foreground_pgid(int pgid) {
    if (pgid > 0) {
        foreground_pgid = pgid;
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
