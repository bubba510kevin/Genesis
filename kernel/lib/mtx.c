#include "kprintf.h"
#include "ksmp.h"
#include "ksyms.h"
#include "backtrace.h"
#include "lapic.h"
#include "klock.h"
#include "typesk.h"

static uint32 this_cpu(void);

/* How long a lock may spin before it is treated as a deadlock rather than as
 * contention. Generous - the longest legitimate hold in this kernel is a
 * hash-bucket walk - so reaching it means something is wrong.
 *
 * The reason to have it at all: a spin lock that never gives up turns every
 * lock-order mistake into a machine that stops with no output, which is the
 * single hardest kernel failure to diagnose. kmtx_lock already names its own
 * recursive case above; this is the same idea for the rwlock, and it catches
 * the case that one cannot - a DIFFERENT lock held by the same CPU. */
#define RW_SPIN_LIMIT   200000000L

/* --- which write locks this CPU is holding ------------------------------
 *
 * A tiny per-CPU stack, pushed by krw_wlock and popped by krw_wunlock.
 *
 * It exists because krw_wlock DISABLES INTERRUPTS and krw_wunlock restores
 * them, so a path that takes a write lock and returns without releasing it
 * does not merely leak a lock - it leaves the machine with interrupts off
 * forever. The symptom is that the timer stops, which then looks like a hang
 * absolutely everywhere except where the bug is.
 *
 * That is not hypothetical: it is how the arpresolve path was diagnosed. With
 * this, `krw_report_held()` names the lock that is still down. */
#define RW_HELD_MAX 8

static rwlock_t *rw_held[SMP_MAX_CPUS][RW_HELD_MAX];
static int       rw_held_depth[SMP_MAX_CPUS];

/* The same stack for MUTEXES, and for the same reason: kmtx_lock disables
 * interrupts too. */
static mtx_t    *mtx_held[SMP_MAX_CPUS][RW_HELD_MAX];
static int       mtx_held_depth[SMP_MAX_CPUS];

static void mtx_held_push(mtx_t *m) {
    int c = (int)this_cpu();

    if (c >= 0 && c < SMP_MAX_CPUS) {
        if (mtx_held_depth[c] < RW_HELD_MAX) {
            mtx_held[c][mtx_held_depth[c]] = m;
        }
        mtx_held_depth[c]++;
    }
}

static void mtx_held_pop(void) {
    int c = (int)this_cpu();

    if (c >= 0 && c < SMP_MAX_CPUS && mtx_held_depth[c] > 0) {
        mtx_held_depth[c]--;
    }
}

int kmtx_report_held(uint8 color) {
    int c = (int)this_cpu();
    int i, d;

    if (c < 0 || c >= SMP_MAX_CPUS) {
        return 0;
    }
    d = mtx_held_depth[c];
    if (d == 0) {
        return 0;
    }
    kprintf_c(color, "mtx: cpu %d still holds %d mutex%s:\n",
              c, d, d == 1 ? "" : "es");
    for (i = 0; i < d && i < RW_HELD_MAX; i++) {
        kprintf_c(color, "    %s\n",
                  mtx_held[c][i] != NULL && mtx_held[c][i]->name != NULL
                  ? mtx_held[c][i]->name : "?");
    }
    return d;
}

static void rw_held_push(rwlock_t *rw) {
    int c = (int)this_cpu();

    if (c >= 0 && c < SMP_MAX_CPUS && rw_held_depth[c] < RW_HELD_MAX) {
        rw_held[c][rw_held_depth[c]] = rw;
    }
    if (c >= 0 && c < SMP_MAX_CPUS) {
        rw_held_depth[c]++;
    }
}

static void rw_held_pop(rwlock_t *rw) {
    int c = (int)this_cpu();

    (void)rw;
    if (c >= 0 && c < SMP_MAX_CPUS && rw_held_depth[c] > 0) {
        rw_held_depth[c]--;
    }
}

/* Print every write lock this CPU still holds, innermost last. Returns the
 * depth, so a caller can test it as well as print it. */
int krw_report_held(uint8 color) {
    int c = (int)this_cpu();
    int i, d;

    if (c < 0 || c >= SMP_MAX_CPUS) {
        return 0;
    }
    d = rw_held_depth[c];
    if (d == 0) {
        return 0;
    }
    kprintf_c(color, "rwlock: cpu %d still holds %d write lock%s:\n",
              c, d, d == 1 ? "" : "s");
    for (i = 0; i < d && i < RW_HELD_MAX; i++) {
        kprintf_c(color, "    %s\n",
                  rw_held[c][i] != NULL && rw_held[c][i]->name != NULL
                  ? rw_held[c][i]->name : "?");
    }
    return d;
}

static void rw_deadlock_report(rwlock_t *rw, const char *what) {
    static int reported;
    uint64 off = 0;
    const char *s1;

    if (reported) {
        return;     /* the first one is the useful one; a flood buries it */
    }
    reported = 1;
    s1 = ksym_lookup((uint64)__builtin_return_address(0), &off);
    kprintf_c(0x0C, "rwlock: %s on '%s' did not complete - readers %d, "
                    "caller %s+%x\n",
              what, rw->name != NULL ? rw->name : "?", (int)rw->readers,
              s1 != NULL ? s1 : "?", (uint32)off);
    backtrace_print((uint64)__builtin_return_address(0),
                    (uint64)__builtin_frame_address(0), 0x0C);
}


/* See sys/mutex.h. */

/* Save RFLAGS and disable interrupts; restore. Duplicated from callout.c
 * rather than shared, matching this tree's preference for a trivial helper
 * per file over a header that every file has to know about. */
static uint64 intr_disable(void) {
    uint64 flags;

    __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static void intr_restore(uint64 flags) {
    /* Restores whatever IF was, rather than doing sti. A caller that had
     * interrupts off before taking the lock must still have them off after
     * releasing it - unconditional sti here would silently enable interrupts
     * inside a critical section somebody else established. */
    __asm__ volatile ("pushq %0\n\tpopfq" : : "r"(flags) : "memory", "cc");
}

/* Who am I, for the recursion check. lapic_id() rather than smp_this_cpu()
 * to keep this file independent of smp.c - and because the APIC ID is the
 * identity that matters here, not the array index. */
static uint32 this_cpu(void) {
    return lapic_id();
}

/* --- who owns the interrupt flag ---------------------------------------
 *
 * Every lock in this file disables interrupts while it is held, because every
 * one of them can be taken from an interrupt handler and a spin lock held
 * across the interrupt that wants it is a one-CPU deadlock.
 *
 * The interrupt state used to be saved IN THE LOCK: acquire stashed RFLAGS in
 * lock->saved_flags, release restored it. That is correct only if locks are
 * released in exactly the reverse order they were taken, and it is wrong the
 * moment they are not:
 *
 *     take A   (saves IF=1)
 *     take B   (saves IF=0, because A already disabled them)
 *     drop A   -> restores IF=1  ... with B still held
 *     drop B   -> restores IF=0  ... and interrupts are now off FOREVER
 *
 * Non-LIFO release is not exotic; it is what the vendored network stack does
 * constantly - arpresolve takes the link-layer entry's lock, then the table's,
 * and drops the entry first. The symptom was the timer stopping three ticks
 * after the first ARP request, which presents as a hang in whatever happened
 * to be running, with nothing near the lock code.
 *
 * So the interrupt state belongs to the CPU, not to the lock: the FIRST
 * acquisition on a CPU saves RFLAGS and disables, the LAST release restores.
 * That is exactly what FreeBSD's spinlock_enter()/spinlock_exit() do, and for
 * the same reason.
 *
 * Indexed by APIC ID. That is dense on every machine this runs on (QEMU
 * numbers them 0..n-1) and is bounds-checked below; an ID at or above
 * SMP_MAX_CPUS would be a CPU this kernel does not track anyway. */
static uint64 spin_saved_flags[SMP_MAX_CPUS];
static int    spin_nesting[SMP_MAX_CPUS];

static void spin_enter(void) {
    uint64 flags = intr_disable();
    uint32 c;

    /* Read the CPU id AFTER disabling: with interrupts on there is no
     * migration in this kernel either, but the ordering costs nothing and is
     * the property this depends on. */
    c = this_cpu();
    if (c >= SMP_MAX_CPUS) {
        return;     /* untracked CPU: leave interrupts off, no worse */
    }
    if (spin_nesting[c]++ == 0) {
        spin_saved_flags[c] = flags;
    }
}

static void spin_exit(void) {
    uint32 c = this_cpu();

    if (c >= SMP_MAX_CPUS) {
        return;
    }
    if (spin_nesting[c] > 0 && --spin_nesting[c] == 0) {
        intr_restore(spin_saved_flags[c]);
    }
}

/* How many spin locks this CPU holds. Zero is the only value that should be
 * observable from ordinary kernel context; anything else at a point where
 * interrupts are expected means a lock was leaked. */
int kspin_depth(void) {
    uint32 c = this_cpu();

    return (c < SMP_MAX_CPUS ? spin_nesting[c] : 0);
}

#define MTX_NO_OWNER 0xFFFFFFFFu

void kmtx_init(mtx_t *m, const char *name) {
    m->locked       = 0;
    m->saved_flags  = 0;
    m->name         = name;
    m->owner        = MTX_NO_OWNER;
    m->acquisitions = 0;
    m->contended    = 0;
}

void kmtx_lock(mtx_t *m) {
    spin_enter();
    uint32 me    = this_cpu();
    int    spun  = 0;

    /* Interrupts go off BEFORE the lock is taken, not after.
     *
     * The other order has a window: this CPU owns the lock and interrupts
     * are still on, so an interrupt handler on this same CPU can arrive and
     * try to take it - and spin forever for a lock held by the thing it
     * interrupted. That is a one-CPU deadlock and it does not need SMP to
     * happen. */

    if (m->locked && m->owner == me) {
        /* Recursing on a non-recursive spin lock. The honest thing is to say
         * so, because the alternative is an infinite spin with no output at
         * all - the single hardest kernel failure to diagnose from a hang.
         * Not fatal here: the boot log gets the name, and the lock is taken
         * anyway on the reasoning that a wrong lock is more debuggable than
         * a dead machine. */
        {
            uint64 off = 0;
            const char *s1 = ksym_lookup((uint64)__builtin_return_address(0),
                                         &off);

            /* One frame. Two would name the path that re-entered rather than
             * just the lock wrapper - which is how the kh_calloc_locked bug
             * was found - but __builtin_return_address(1) is documented
             * unsafe and warns, and a diagnostic that can fault is worse
             * than a shallower one. Walk the frame chain with backtrace.h if
             * a deeper answer is needed. */
            kprintf_c(0x0C, "mtx: recursive acquire of %s on cpu %d from "
                            "%s+%x\n",
                      m->name != NULL ? m->name : "?", me,
                      s1 != NULL ? s1 : "?", (uint32)off);
        }
    }

    for (;;) {
        uint32 prev;

        /* xchg is implicitly locked on x86 - no lock prefix needed, and
         * writing one would assemble to the same thing. It is the whole
         * primitive: read the old value and store 1 atomically, so exactly
         * one CPU can observe a 0. */
        __asm__ volatile ("xchgl %0, %1"
                          : "=r"(prev), "+m"(m->locked)
                          : "0"(1u)
                          : "memory");
        if (prev == 0) {
            break;
        }
        spun = 1;

        /* Spin on a plain READ until it looks free, then try the exchange
         * again. Hammering xchg takes the cache line exclusive on every
         * attempt and starves the holder trying to release it - this is the
         * standard test-and-test-and-set shape and the reason for it is
         * performance under exactly the contention the lock exists for. */
        while (m->locked) {
            __asm__ volatile ("pause");
        }
    }

    m->owner       = me;
    m->acquisitions++;
    if (spun) {
        m->contended++;
    }
    mtx_held_push(m);
}

int kmtx_trylock(mtx_t *m) {
    uint32 prev;

    spin_enter();

    __asm__ volatile ("xchgl %0, %1"
                      : "=r"(prev), "+m"(m->locked)
                      : "0"(1u)
                      : "memory");
    if (prev != 0) {
        /* Not taken - unwind the nesting, or a failed trylock silently leaves
         * interrupts disabled for the rest of time. */
        spin_exit();
        return 0;
    }
    m->owner       = this_cpu();
    m->acquisitions++;
    return 1;
}

void kmtx_unlock(mtx_t *m) {
    mtx_held_pop();
    m->owner = MTX_NO_OWNER;

    /* Release, THEN unwind the interrupt nesting. In that order: the instant
     * `locked` goes to zero another CPU may take this lock, and it must not
     * find the releasing CPU still counted as holding one. */
    __asm__ volatile ("" : : : "memory");
    m->locked = 0;

    spin_exit();
}

int kmtx_owned(mtx_t *m) {
    return m->locked && m->owner == this_cpu();
}

/* --- reader/writer ------------------------------------------------------ */

void krw_init(rwlock_t *rw, const char *name) {
    rw->readers     = 0;
    rw->saved_flags = 0;
    rw->name        = name;
    rw->wcpu        = 0xFFFFFFFFu;
}

int krw_wowned(rwlock_t *rw) {
    return rw->readers < 0 && rw->wcpu == this_cpu();
}

void krw_rlock(rwlock_t *rw) {
    long spins = 0;

    /* Read locks disable interrupts too, and that is a change from the
     * original: a reader interrupted by a handler that wants the WRITE lock
     * spins forever on one CPU, because the reader can never run to release
     * it. The vendored network stack takes read locks in exactly such
     * contexts - the routing table is read from the transmit path and written
     * from an interface event. */
    spin_enter();

    for (;;) {
        int32 cur = rw->readers;
        int32 prev;

        if (cur < 0) {
            if (++spins == RW_SPIN_LIMIT) {
                rw_deadlock_report(rw, "read acquire");
            }
            __asm__ volatile ("pause");
            continue;                    /* a writer holds it */
        }
        /* cmpxchg rather than a plain increment: between reading `cur` and
         * writing cur+1, a writer could have taken it to -1, and a blind
         * increment would turn that into 0 - a writer that believes it holds
         * the lock exclusively while readers walk in. */
        __asm__ volatile ("lock cmpxchgl %2, %1"
                          : "=a"(prev), "+m"(rw->readers)
                          : "r"(cur + 1), "0"(cur)
                          : "memory", "cc");
        if (prev == cur) {
            return;
        }
    }
}

void krw_runlock(rwlock_t *rw) {
    __asm__ volatile ("lock decl %0" : "+m"(rw->readers) : : "memory", "cc");
    spin_exit();
}

void krw_wlock(rwlock_t *rw) {
    long spins = 0;

    spin_enter();

    for (;;) {
        int32 prev;

        /* Only from exactly zero: any reader at all, or another writer,
         * must block. */
        __asm__ volatile ("lock cmpxchgl %2, %1"
                          : "=a"(prev), "+m"(rw->readers)
                          : "r"(-1), "0"(0)
                          : "memory", "cc");
        if (prev == 0) {
            break;
        }
        if (++spins == RW_SPIN_LIMIT) {
            rw_deadlock_report(rw, "write acquire");
        }
        __asm__ volatile ("pause");
    }
    rw->wcpu = this_cpu();
    rw_held_push(rw);
}

void krw_wunlock(rwlock_t *rw) {
    rw_held_pop(rw);
    rw->wcpu = 0xFFFFFFFFu;
    __asm__ volatile ("" : : : "memory");
    rw->readers = 0;
    spin_exit();
}

/* One attempt at a READ lock.
 *
 * A real try, and distinct from krw_trywlock - which is what it used to be
 * mapped to, and that was a genuine bug rather than a conservative
 * approximation: taking the WRITE lock sets readers to -1, and the caller
 * then releases with rw_runlock(), which decrements to -2. The lock is
 * corrupt from that moment and the next writer spins forever. It showed up as
 * a hang closing a UDP socket, in netinet/in_pcb.c's INP_TRY_RLOCK path. */
int krw_tryrlock(rwlock_t *rw) {
    int32 cur, prev;

    spin_enter();

    cur = rw->readers;
    if (cur < 0) {
        spin_exit();
        return 0;                       /* a writer holds it */
    }
    __asm__ volatile ("lock cmpxchgl %2, %1"
                      : "=a"(prev), "+m"(rw->readers)
                      : "r"(cur + 1), "0"(cur)
                      : "memory", "cc");
    if (prev != cur) {
        spin_exit();
        return 0;                       /* somebody moved it under us */
    }
    return 1;
}

/* Release without the caller saying which way it holds the lock.
 *
 * Upstream's rw_unlock() works because a FreeBSD rwlock records its owner, so
 * it can tell a reader's release from a writer's. This one reads the counter
 * instead: negative means a writer holds it. netinet/in_pcb.c's INP_UNLOCK is
 * the caller, and it genuinely does not know - the same code path is reached
 * with the pcb read-locked or write-locked. */
void krw_unlock(rwlock_t *rw) {
    if (rw->readers < 0) {
        krw_wunlock(rw);
    } else {
        krw_runlock(rw);
    }
}

/* Turn a read lock into a write lock without releasing in between, so nothing
 * can change the object while the caller reconsiders.
 *
 * Only succeeds when this is the ONLY reader - readers == 1 - which is the
 * same condition upstream requires. Returns 0 otherwise, and the caller is
 * expected to drop and retake, which is what in_pcb.c does. This used to be
 * `(1)`: an unconditional claim of success that left the lock read-held while
 * the caller believed it had exclusive access. */
int krw_tryupgrade(rwlock_t *rw) {
    int32 prev;

    __asm__ volatile ("lock cmpxchgl %2, %1"
                      : "=a"(prev), "+m"(rw->readers)
                      : "r"(-1), "0"(1)
                      : "memory", "cc");
    if (prev == 1) {
        rw->wcpu = this_cpu();
    }
    return (prev == 1);
}

/* And back: exclusive to shared, still without releasing. */
void krw_downgrade(rwlock_t *rw) {
    if (rw->readers < 0) {
        rw_held_pop(rw);
        rw->wcpu = 0xFFFFFFFFu;
        rw->readers = 1;
    }
}

int krw_trywlock(rwlock_t *rw) {
    int32 prev;

    spin_enter();

    /* The same compare-exchange krw_wlock spins on, attempted once. Only from
     * exactly zero: any reader at all, or another writer, means somebody else
     * holds it. */
    __asm__ volatile ("lock cmpxchgl %2, %1"
                      : "=a"(prev), "+m"(rw->readers)
                      : "r"(-1), "0"(0)
                      : "memory", "cc");
    if (prev == 0) {
        rw->wcpu = this_cpu();
        rw_held_push(rw);
        return 1;
    }
    /* Unwind on failure. A held lock keeps interrupts off; a failed attempt
     * must not. */
    spin_exit();
    return 0;
}
