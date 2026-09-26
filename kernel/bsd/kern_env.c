/* The kernel environment, and log().
 *
 * --- kern_getenv and the getenv_* family --------------------------------
 * FreeBSD's tunables are read at init through TUNABLE_INT_FETCH and friends,
 * which <sys/kernel.h> expands into getenv_int(). Vendored network source
 * uses them constantly - ip_reass.c sizes its fragment queue that way,
 * tcp_subr.c reads a dozen - and until this file existed the macro expanded
 * to nothing, so every tunable silently kept its compiled-in default.
 *
 * Keeping the default is almost always right. Doing it SILENTLY is what this
 * removes: with a real environment behind it, a tunable that needs setting on
 * this machine can be set, in kernel/driver/hints.c, in upstream's own
 * `name="value"` syntax, next to the device hints that already live there.
 *
 * That the two share a table is upstream's arrangement, not a shortcut -
 * FreeBSD's static environment holds `hint.foo.0.at` and
 * `net.inet.tcp.hostcache.enable` in the same array and kern_getenv()
 * searches all of it.
 *
 * --- what is NOT here ---------------------------------------------------
 * The environment is READ ONLY. Upstream has kern_setenv()/kern_unsetenv()
 * and a dynamic environment that replaces the static one once malloc is up.
 * There is no consumer for that here: nothing sets a tunable at runtime, and
 * a writable environment needs a lock and a lifetime rule for the strings it
 * hands out. kern_setenv() would be the addition, not a rework.
 *
 * freeenv() is therefore a no-op, and that is the one thing about this file a
 * caller could get wrong: upstream's kern_getenv() returns a MALLOCED copy
 * that the caller must freeenv(). Here it returns a pointer into a static
 * buffer that the next kern_getenv() overwrites. Every caller in the vendored
 * tree consumes the value immediately, which is why that is safe - but a new
 * caller that stashes the pointer would be holding a buffer, not a string.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/syslog.h>

#include "hints.h"
#include "syscall.h"   /* user_ptr_ok - see copyin/copyout below */
#include "kprintf.h"

char *kern_getenv(const char *name) {
    /* Cast away const: upstream's signature returns char *, because there it
     * really is a fresh copy the caller owns. See the file comment. */
    return ((char *)(uintptr_t)hint_env(name));
}

void freeenv(char *env) {
    (void)env;      /* nothing was allocated - see the file comment */
}

int testenv(const char *name) {
    return (hint_env(name) != NULL);
}

/* --- string to number ---------------------------------------------------
 *
 * One parser, used by every getenv_* below. Accepts an optional sign, an
 * optional 0x prefix, and upstream's k/m/g suffixes - a tunable written as
 * "16m" is a real thing in a FreeBSD loader.conf and dropping the suffix
 * would turn 16 megabytes into 16.
 *
 * Returns 1 on a completely consumed string, 0 otherwise. Partial success is
 * treated as failure on purpose: "1kk" is a typo, and reading it as 1024 is
 * the wrong kind of forgiving for a value that sizes a buffer.
 */
static int parse_quad(const char *s, int64_t *out) {
    int64_t v = 0;
    int neg = 0;
    int base = 10;
    int digits = 0;

    if (s == NULL) {
        return (0);
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (;;) {
        int d;

        if (*s >= '0' && *s <= '9') {
            d = *s - '0';
        } else if (base == 16 && *s >= 'a' && *s <= 'f') {
            d = *s - 'a' + 10;
        } else if (base == 16 && *s >= 'A' && *s <= 'F') {
            d = *s - 'A' + 10;
        } else {
            break;
        }
        v = v * base + d;
        digits++;
        s++;
    }
    if (digits == 0) {
        return (0);
    }
    switch (*s) {
    case 'k': case 'K': v <<= 10; s++; break;
    case 'm': case 'M': v <<= 20; s++; break;
    case 'g': case 'G': v <<= 30; s++; break;
    case 't': case 'T': v <<= 40; s++; break;
    default: break;
    }
    if (*s != '\0') {
        return (0);
    }
    *out = neg ? -v : v;
    return (1);
}

int getenv_quad(const char *name, quad_t *data) {
    const char *v = hint_env(name);
    int64_t q;

    if (v == NULL || !parse_quad(v, &q)) {
        return (0);
    }
    *data = (quad_t)q;
    return (1);
}

int getenv_int(const char *name, int *data) {
    quad_t q;

    if (!getenv_quad(name, &q)) {
        return (0);
    }
    *data = (int)q;
    return (1);
}

int getenv_uint(const char *name, unsigned int *data) {
    quad_t q;

    if (!getenv_quad(name, &q)) {
        return (0);
    }
    *data = (unsigned int)q;
    return (1);
}

int getenv_long(const char *name, long *data) {
    quad_t q;

    if (!getenv_quad(name, &q)) {
        return (0);
    }
    *data = (long)q;
    return (1);
}

int getenv_ulong(const char *name, unsigned long *data) {
    quad_t q;

    if (!getenv_quad(name, &q)) {
        return (0);
    }
    *data = (unsigned long)q;
    return (1);
}

int getenv_int64(const char *name, int64_t *data) {
    quad_t q;

    if (!getenv_quad(name, &q)) {
        return (0);
    }
    *data = (int64_t)q;
    return (1);
}

int getenv_uint64(const char *name, uint64_t *data) {
    quad_t q;

    if (!getenv_quad(name, &q)) {
        return (0);
    }
    *data = (uint64_t)q;
    return (1);
}

int getenv_string(const char *name, char *data, int size) {
    const char *v = hint_env(name);
    int i;

    if (v == NULL || size <= 0) {
        return (0);
    }
    for (i = 0; i < size - 1 && v[i] != '\0'; i++) {
        data[i] = v[i];
    }
    data[i] = '\0';
    return (1);
}

/* Upstream's spelling of truth: a leading '1'/'t'/'T'/'y'/'Y' is true, '0'/
 * 'f'/'F'/'n'/'N' is false, anything else is not a boolean at all. Matching
 * it matters because a tunable documented as `="YES"` has to work. */
int getenv_bool(const char *name, bool *data) {
    const char *v = hint_env(name);

    if (v == NULL) {
        return (0);
    }
    switch (v[0]) {
    case '1': case 't': case 'T': case 'y': case 'Y':
        *data = true;
        return (1);
    case '0': case 'f': case 'F': case 'n': case 'N':
        *data = false;
        return (1);
    default:
        return (0);
    }
}

bool getenv_is_true(const char *name) {
    bool b = false;

    return (getenv_bool(name, &b) && b);
}

bool getenv_is_false(const char *name) {
    bool b = true;

    return (getenv_bool(name, &b) && !b);
}

/* --- log() --------------------------------------------------------------
 *
 * The kernel's severity-tagged printf. Upstream it goes to the message
 * buffer, is read back by dmesg(8), and reaches the console only if its
 * priority is at or below kern.consmsgbuf level.
 *
 * Here it goes to the console, coloured by severity. There is no message
 * buffer to hold it otherwise, and a log line that goes nowhere is worse than
 * one that is on screen.
 *
 * The severity does real work: an ICMP or ARP message logged at LOG_INFO
 * (a host answering for an address someone else claims) should not look the
 * same as LOG_ERR. Vendored network code logs a lot at LOG_DEBUG, and those
 * are DROPPED rather than printed - upstream's default console level does the
 * same, and printing them would bury the boot log in per-packet noise.
 */
void log(int level, const char *fmt, ...) {
    va_list ap;
    uint8 colour;

    switch (LOG_PRI(level)) {
    case LOG_EMERG:
    case LOG_ALERT:
    case LOG_CRIT:
        colour = 0x4F;      /* white on red */
        break;
    case LOG_ERR:
        colour = 0x0C;      /* red */
        break;
    case LOG_WARNING:
        colour = 0x0E;      /* yellow */
        break;
    case LOG_DEBUG:
        return;             /* below the console level - see above */
    default:
        colour = 0x07;      /* grey */
        break;
    }

    va_start(ap, fmt);
    kvprintf(colour, fmt, ap);
    va_end(ap);
}

void log_console(struct mbuf *m) {
    (void)m;    /* upstream feeds the message buffer from a console mbuf;
                 * there is no message buffer here, so there is nothing to
                 * feed and no caller in the vendored tree */
}

/* --- crossing the user/kernel boundary ----------------------------------
 *
 * net/if.c's ioctl surface (SIOCGIFCONF, SIOCSIFNAME, the capability
 * negotiation) reads and writes userland memory, and these are how.
 *
 * They are REAL, not memcpy under another name: syscall.c's user_ptr_ok()
 * decides whether an address belongs to user space, and that check is the
 * whole point. A kernel that trusts a pointer a user process handed it is
 * a kernel a user process can make write anywhere.
 *
 * What is still missing, and it is worth being precise rather than
 * comfortable: user_ptr_ok() validates the address, not the RANGE. A pointer
 * that is valid at its first byte and runs off the end of the mapping will
 * fault partway through the copy rather than being refused, and there is no
 * fault handler that can recover from it. Upstream solves this with a
 * per-thread pcb_onfault landing pad; Genesis has no equivalent, so the
 * copy would take the fault path in kernel/arch/interrupt.c. That is a real
 * gap and it is recorded in ROADMAP.md rather than papered over here.
 */
int copyin(const void *uaddr, void *kaddr, size_t len) {
    if (uaddr == NULL || !user_ptr_ok((uint64)(uintptr_t)uaddr)) {
        return (EFAULT);
    }
    memcpy(kaddr, uaddr, len);
    return (0);
}

int copyout(const void *kaddr, void *uaddr, size_t len) {
    if (uaddr == NULL || !user_ptr_ok((uint64)(uintptr_t)uaddr)) {
        return (EFAULT);
    }
    memcpy(uaddr, kaddr, len);
    return (0);
}

/* A NUL-terminated string, bounded. `done` receives the length INCLUDING the
 * terminator, which is upstream's contract and the opposite of strlen's. */
int copyinstr(const void *uaddr, void *kaddr, size_t len, size_t *done) {
    const char *s = (const char *)uaddr;
    char *d = (char *)kaddr;
    size_t i;

    if (uaddr == NULL || !user_ptr_ok((uint64)(uintptr_t)uaddr) || len == 0) {
        return (EFAULT);
    }
    for (i = 0; i < len; i++) {
        d[i] = s[i];
        if (s[i] == '\0') {
            if (done != NULL) {
                *done = i + 1;
            }
            return (0);
        }
    }
    /* No terminator inside `len` bytes. ENAMETOOLONG, not a truncated string
     * the caller would then treat as complete. */
    return (ENAMETOOLONG);
}

void vlog(int level, const char *fmt, va_list ap) {
    uint8 colour;

    switch (LOG_PRI(level)) {
    case LOG_EMERG: case LOG_ALERT: case LOG_CRIT: colour = 0x4F; break;
    case LOG_ERR:     colour = 0x0C; break;
    case LOG_WARNING: colour = 0x0E; break;
    case LOG_DEBUG:   return;
    default:          colour = 0x07; break;
    }
    kvprintf(colour, fmt, ap);
}

/* TUNABLE_INT_FETCH's SYSINIT-time counterpart: upstream's kern_environment.c
 * registers a tunable so it is re-read if the environment changes. The
 * environment here is compiled in and never changes, so the fetch at init is
 * the whole story. */
void tunable_int_init(const void *data) {
    const struct tunable_int *t = (const struct tunable_int *)data;

    if (t != NULL) {
        (void)getenv_int(t->path, t->var);
    }
}

void tunable_long_init(const void *data) {
    const struct tunable_long *t = (const struct tunable_long *)data;

    if (t != NULL) {
        (void)getenv_long(t->path, t->var);
    }
}

void tunable_ulong_init(const void *data) {
    const struct tunable_ulong *t = (const struct tunable_ulong *)data;

    if (t != NULL) {
        (void)getenv_ulong(t->path, t->var);
    }
}

void tunable_int64_init(const void *data) {
    const struct tunable_int64 *t = (const struct tunable_int64 *)data;

    if (t != NULL) {
        (void)getenv_int64(t->path, t->var);
    }
}

void tunable_uint64_init(const void *data) {
    const struct tunable_uint64 *t = (const struct tunable_uint64 *)data;

    if (t != NULL) {
        (void)getenv_uint64(t->path, t->var);
    }
}

void tunable_quad_init(const void *data) {
    const struct tunable_quad *t = (const struct tunable_quad *)data;

    if (t != NULL) {
        (void)getenv_quad(t->path, t->var);
    }
}

void tunable_str_init(const void *data) {
    const struct tunable_str *t = (const struct tunable_str *)data;

    if (t != NULL) {
        (void)getenv_string(t->path, t->var, t->size);
    }
}
