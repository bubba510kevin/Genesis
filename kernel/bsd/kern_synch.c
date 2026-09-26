/* sleep(9): tsleep/msleep/wakeup, over Genesis's tick and interrupts.
 *
 * --- what upstream does -------------------------------------------------
 * FreeBSD's _sleep() puts the CURRENT THREAD on a hashed sleep queue keyed by
 * a wait channel, marks it not-runnable, and calls the scheduler. Some other
 * context calls wakeup(chan), which makes every thread on that queue runnable
 * again. The sleeping thread's CPU goes and runs something else in the
 * meantime.
 *
 * --- what happens here, and why there are two answers --------------------
 * There are two, because there are two kinds of caller and only one of them
 * has anywhere to go.
 *
 * A KERNEL THREAD (kernel/include/kthread.h) really deschedules. It goes on
 * the channel's wait queue and blocks, the CPU runs something else, and the
 * waker - a timer or a NIC interrupt calling wakeup() - makes it runnable
 * again. That is upstream's behaviour, reached through ksleep_wait() rather
 * than by calling waitq_wait_until() directly, because <sys/proc.h> and
 * kernel/include/process.h both define `struct thread` and cannot appear in
 * one translation unit. See kernel/include/ksleep.h.
 *
 * ANYTHING ELSE still sleeps by IDLING: it drops the caller's lock, enables
 * interrupts, and `hlt`s until either the channel's generation counter moves
 * or the deadline passes, then reacquires the lock. That path is not legacy
 * and is not going away - a driver's interrupt handler has nothing to
 * deschedule, and a sleep that blocked one would take the interrupt's own
 * wakeup down with it. The distinction is made per call, by
 * ksleep_can_block(), because the same tsleep() in the same driver is
 * reached both ways.
 *
 * The cost of the idling path is honest and worth restating: the CPU is not
 * available to anything else while such a sleep is in progress. Every sleep
 * here is therefore given a DEFAULT TIMEOUT even when the caller passed
 * none, so a lost wakeup degrades into a slow path rather than a hung
 * machine - and that ceiling still applies to the blocking path, where it
 * costs a wakeup every ten seconds and buys the same protection.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/sx.h>
#include <sys/proc.h>
#include <sys/sleepqueue.h>
#include <sys/time.h>

#include "kprintf.h"
#include "ksleep.h"
#include "timer.h"

/* Non-zero until sleeping is safe. See <sys/systm.h>.
 *
 * Still 1, and still 1 on purpose now that kernel threads exist. `cold` is a
 * property of the MACHINE, and the vendored code reads it as "may I block
 * here" - but whether a block is legal in this kernel is a property of the
 * CALLER, since a kernel thread can be descheduled and a driver's interrupt
 * handler cannot. A global that answered yes would be answering for the
 * interrupt handler too.
 *
 * The per-caller question is ksleep_can_block(), asked at each sleep. This
 * can become 0 only when every context that reaches sleep(9) is a kernel
 * thread, which is a statement about the drivers, not about this file. */
int cold = 1;

/* --- the wait channels ---------------------------------------------------
 *
 * A fixed table, keyed by the channel pointer. Each entry holds a generation
 * counter that wakeup() bumps; a sleeper records the value it saw and waits
 * for it to change.
 *
 * A generation counter rather than a flag, and that is the whole correctness
 * argument: a wakeup that arrives BETWEEN the caller's condition test and its
 * call to sleep must not be lost. With a flag it would be - the flag is
 * cleared by whoever consumes it. With a generation, the sleeper compares
 * against a value it read before it dropped the lock, so a wakeup in that
 * window has already moved the counter and the sleep returns immediately.
 *
 * The table is small and channels are hashed into it, so two unrelated
 * channels can share a slot. That produces SPURIOUS wakeups, never lost ones,
 * and every caller of tsleep is already required to re-test its condition in
 * a loop - which is exactly what makes sharing safe.
 */
/* One shared size with kernel/include/ksleep.h, which holds the matching
 * wait queues. Defined from it rather than beside it: two 64s that have to
 * agree are two 64s that will not. */
#define SLEEP_HASH_SIZE KSLEEP_SLOTS

static volatile uint32 sleep_gen[SLEEP_HASH_SIZE];

static unsigned int chan_slot(const void *chan) {
    uintptr_t v = (uintptr_t)chan;

    /* Fibonacci hashing: multiply by 2^64 / phi and take the high bits. The
     * low bits of a pointer are mostly allocator alignment and would collide
     * far more often than this does. */
    v *= 0x9E3779B97F4A7C15ULL;
    return ((unsigned int)(v >> 58) & (SLEEP_HASH_SIZE - 1));
}

void wakeup(const void *chan) {
    unsigned int slot = chan_slot(chan);

    /* Bump the generation FIRST, then wake.
     *
     * A woken thread's first act is to re-read the generation, so waking
     * before bumping would hand it the old value and put it straight back to
     * sleep - on a wakeup that has now been consumed and will not repeat.
     * The order is the whole reason the counter is useful to a sleeper that
     * genuinely blocks rather than one that polls in a loop. */
    sleep_gen[slot]++;
    ksleep_wake(slot);
}

/* Upstream wakes exactly one waiter. This wakes them all, for the same reason
 * kernel/proc/waitq.c does: choosing one and choosing wrong loses the wakeup
 * entirely, and every waiter re-tests its condition anyway. The cost is that
 * n-1 of them go back to sleep. */
void wakeup_one(const void *chan) {
    wakeup(chan);
}

void wakeup_any(const void *chan) {
    wakeup(chan);
}

/* --- the sleep itself ---------------------------------------------------
 *
 * The lock argument is upstream's: msleep/mtx_sleep pass a mutex, sx_sleep an
 * sx, rw_sleep an rwlock, and tsleep none. In every case the lock is dropped
 * before sleeping and reacquired before returning, which is what makes the
 * "test the condition, then sleep" sequence race-free.
 *
 * The type is carried in `flags` the way upstream does not - upstream reaches
 * the right unlock through the lock class in struct lock_object. There is no
 * lock class machinery here, so the four entry points below each know what
 * they were handed and pass it in.
 */
#define GSLEEP_NOLOCK   0
#define GSLEEP_MTX      1
#define GSLEEP_SX       2
#define GSLEEP_RW       3

/* The ceiling on any sleep, in ticks. Ten seconds at 100Hz.
 *
 * Applied even when the caller asked to sleep forever, because a lost wakeup
 * with no timeout is an unrecoverable hang on a kernel with no other runnable
 * work, and a ten-second stall is a bug that can be seen and fixed. A caller
 * that genuinely waits longer than this loops, because it re-tests its
 * condition - so the ceiling costs a wakeup every ten seconds and nothing
 * else. */
#define GSLEEP_MAX_TICKS (10 * 100)

static int gsleep(const void *chan, void *lock, int locktype, int timo) {
    unsigned int slot = chan_slot(chan);
    uint32 seen;
    int deadline;
    int woken = 0;

    if (timo <= 0 || timo > GSLEEP_MAX_TICKS) {
        timo = GSLEEP_MAX_TICKS;
    }
    /* Read the generation BEFORE dropping the lock. This is the window the
     * counter exists to close. */
    seen = sleep_gen[slot];
    deadline = ticks + timo;

    switch (locktype) {
    case GSLEEP_MTX: mtx_unlock((struct mtx *)lock);      break;
    case GSLEEP_SX:  sx_xunlock((struct sx *)lock);       break;
    case GSLEEP_RW:  rw_wunlock((struct rwlock *)lock);   break;
    default: break;
    }

    for (;;) {
        if (sleep_gen[slot] != seen) {
            woken = 1;
            break;
        }
        if ((int)(ticks - deadline) >= 0) {
            break;
        }
        if (ksleep_can_block()) {
            /* A kernel thread: really deschedule. The remaining time is
             * recomputed on every pass rather than passed once, because the
             * loop can go round several times - a spurious wake from a
             * channel sharing this slot, another sleeper winning the event -
             * and a duration that restarted each pass would be a timeout
             * that never expires under load. Same argument as
             * waitq_wait_until's absolute deadline; the conversion to
             * absolute happens inside ksleep_wait. */
            int remaining = deadline - ticks;

            if (remaining <= 0) {
                break;
            }
            ksleep_wait(slot, &sleep_gen[slot], seen, (uint32)remaining);
        } else {
            /* Not descheduleable - an interrupt handler, or the boot stack
             * before there are kernel threads. Idle instead.
             *
             * sti before hlt, in that order and with no instruction between:
             * the CPU guarantees that an interrupt taken by the sti is
             * delivered AFTER the hlt begins, which is what stops the wakeup
             * from arriving in the window between the two and leaving this
             * halted forever. */
            __asm__ volatile ("sti; hlt");
        }
    }

    switch (locktype) {
    case GSLEEP_MTX: mtx_lock((struct mtx *)lock);      break;
    case GSLEEP_SX:  sx_xlock((struct sx *)lock);       break;
    case GSLEEP_RW:  rw_wlock((struct rwlock *)lock);   break;
    default: break;
    }

    /* EWOULDBLOCK on timeout, 0 on a real wakeup. Upstream's contract, and
     * callers branch on it - a socket receive that times out must return
     * EAGAIN rather than claiming it has data. */
    return (woken ? 0 : EWOULDBLOCK);
}

/* Which of the three unlock/relock pairs a lock_object needs.
 *
 * This used to be `lock != NULL ? GSLEEP_MTX : GSLEEP_NOLOCK` - a guess that
 * would have called mtx_unlock on an sx. See the lo_class comment in
 * kernel/bsd/compat/sys/lock.h for why the field exists and why an
 * unclassified lock is treated as a mutex rather than refused.
 *
 * The complaint is LATCHED. This runs on paths reached from interrupt
 * handlers, and a per-call kprintf on a lock that is wrong every time would
 * bury the boot log in the one situation where the log is what you need. */
static int lo_complained;

static int locktype_of(struct lock_object *lock) {
    if (lock == NULL) {
        return GSLEEP_NOLOCK;
    }
    switch (lock->lo_class) {
    case LO_CLASS_MTX: return GSLEEP_MTX;
    case LO_CLASS_SX:  return GSLEEP_SX;
    case LO_CLASS_RW:  return GSLEEP_RW;
    default:
        if (!lo_complained) {
            lo_complained = 1;
            kprintf_c(0x0E, "sleep: lock '%s' has no class - it never went "
                            "through its init; assuming a mutex\n",
                      lock->lo_name != NULL ? lock->lo_name : "?");
        }
        return GSLEEP_MTX;
    }
}

int _sleep(const void *chan, struct lock_object *lock, int pri,
           const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags) {
    (void)pri; (void)wmesg; (void)pr; (void)flags;
    /* sbt is an absolute or relative deadline in sbintime; converted to ticks
     * because that is what the loop above counts in. */
    return (gsleep(chan, lock, locktype_of(lock),
                   sbt > 0 ? (int)((sbt * hz) >> 32) : 0));
}

/* --- the entry point kernel/bsd/kern_condvar.c uses ----------------------
 *
 * A condvar wait is a sleep on the condvar's own address with the caller's
 * lock dropped across it, which is what gsleep already is. Exported rather
 * than reimplemented for waitq.h's reason: the generation counter, the
 * ten-second ceiling, the kernel-thread-versus-idle decision and the
 * unlock/relock pairing are four separate subtleties, and a second copy
 * written from memory gets one of them wrong.
 *
 * `timo` is in ticks, 0 for "no deadline of my own" (the ceiling still
 * applies). Returns 0 on a real wakeup and EWOULDBLOCK on the deadline,
 * which is what cv_timedwait's contract is written in. */
int genesis_lo_sleep(const void *chan, struct lock_object *lock, int timo) {
    return (gsleep(chan, lock, locktype_of(lock), timo));
}

/* gsleep with the lock dropped and NOT reacquired - cv_wait_unlock's shape.
 *
 * Written as its own entry point rather than a flag on the one above because
 * the asymmetry is the whole point of the call: a caller that will not touch
 * the protected state again wants the lock gone, and reacquiring it just to
 * have the caller drop it would be a second acquisition that can block. */
int genesis_lo_sleep_unlock(const void *chan, struct lock_object *lock,
                            int timo) {
    int r = gsleep(chan, lock, locktype_of(lock), timo);

    switch (locktype_of(lock)) {
    case GSLEEP_MTX: mtx_unlock((struct mtx *)lock);      break;
    case GSLEEP_SX:  sx_xunlock((struct sx *)lock);       break;
    case GSLEEP_RW:  rw_wunlock((struct rwlock *)lock);   break;
    default: break;
    }
    return r;
}

int genesis_tsleep(const void *chan, int pri, const char *wmesg, int timo) {
    (void)pri; (void)wmesg;
    return (gsleep(chan, NULL, GSLEEP_NOLOCK, timo));
}

int genesis_mtx_sleep(const void *chan, struct mtx *mtx, int pri,
                      const char *wmesg, int timo) {
    (void)pri; (void)wmesg;
    return (gsleep(chan, mtx, GSLEEP_MTX, timo));
}

int genesis_sx_sleep(const void *chan, struct sx *sx, int pri,
                     const char *wmesg, int timo) {
    (void)pri; (void)wmesg;
    return (gsleep(chan, sx, GSLEEP_SX, timo));
}

int genesis_rw_sleep(const void *chan, struct rwlock *rw, int pri,
                     const char *wmesg, int timo) {
    (void)pri; (void)wmesg;
    return (gsleep(chan, rw, GSLEEP_RW, timo));
}

/* pause(9): sleep for a fixed time on a channel nobody will signal. Used by
 * retry loops. Implemented as a sleep on its own address, which no wakeup can
 * name - so it always runs to the timeout, which is the point. */
int pause(const char *wmesg, int timo) {
    static char pause_chan;

    (void)wmesg;
    return (gsleep(&pause_chan, NULL, GSLEEP_NOLOCK, timo));
}

/* --- scheduler hooks -----------------------------------------------------
 *
 * sched_prio() raises or lowers a thread's priority, which upstream's socket
 * splice code does so a helper does not starve. There are kernel threads now,
 * but ULE scores them from the run/sleep ratio sched_tick measures (see
 * kernel/proc/sched_ule.c) and has no settable priority to override it with,
 * so this still records nothing. Named rather than macro'd away so that its absence is
 * visible at the call site in a debugger. */
void sched_prio(struct thread *td, u_char prio) {
    (void)td; (void)prio;
}
