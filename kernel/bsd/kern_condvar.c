/* condvar(9): cv_wait/cv_signal/cv_broadcast, over Genesis's sleep(9).
 *
 * --- what this is for ------------------------------------------------------
 * ROADMAP item 7's second blocker, named there as "CONDITION VARIABLES and
 * sleepable taskqueues": cv_wait/cv_broadcast appear throughout OpenZFS's
 * DMU and ARC, and <sys/condvar.h> was vendored with no implementation behind
 * it at all. It was unbuildable until kernel threads existed, because a
 * condvar whose wait cannot deschedule is a spin loop with a nicer name.
 *
 * --- why it is this short --------------------------------------------------
 * A condition variable IS a wait channel. cv_wait(cvp, lock) is "sleep on the
 * address of cvp with the caller's lock dropped across it", and cv_signal is
 * "wake whoever is sleeping on that address" - which is exactly what
 * kern_synch.c's gsleep and wakeup already are. So this file is a mapping,
 * not a mechanism, and that is deliberate: the generation counter, the
 * ten-second ceiling, the kernel-thread-versus-idle decision and the
 * unlock/relock pairing are four separate subtleties, and the argument in
 * kernel/include/waitq.h applies here too - there is one copy of each of
 * them, and a second written from memory gets one wrong.
 *
 * FreeBSD's own condvars are the same mapping onto the same sleep queues.
 * The one real difference is upstream's cv_signal wakes exactly one waiter
 * and this wakes all of them; see cv_signal below.
 *
 * --- the race, and why there is no extra machinery to close it -------------
 * The classic condvar bug is a signal that lands between the waiter's
 * predicate test and its sleep, and is therefore lost. This cannot happen
 * here, and the reason is worth stating because it looks like an omission:
 *
 *   1. A waiter holds the lock when it calls cv_wait.
 *   2. gsleep reads the channel's GENERATION COUNTER before it drops the
 *      lock (see kern_synch.c - the read is above the unlock switch, and
 *      that ordering is the whole point of it).
 *   3. A signaller must hold the same lock to touch the predicate, so it
 *      cannot run between 2 and the unlock.
 *   4. A signal after the unlock has already moved the generation, and the
 *      sleep therefore returns immediately rather than blocking.
 *
 * That is the entire argument. It rests on the caller holding the lock across
 * cv_signal, which is condvar(9)'s documented contract and not an extra
 * requirement Genesis is adding.
 *
 * --- cv_waiters ------------------------------------------------------------
 * The vendored struct carries a waiter count and its header calls it "an
 * optimization to avoid looking up the sleep queue if there are no waiters".
 * It is used here for exactly that, and the direction of its error is what
 * makes that safe:
 *
 *   It can never be WRONG LOW. A waiter increments it while holding the lock,
 *   before dropping it, so any thread that is actually asleep has already
 *   been counted - and a thread that has not incremented yet still holds the
 *   lock and so cannot be asleep. A signaller holding the same lock therefore
 *   cannot see zero while a waiter sleeps.
 *
 *   It can be WRONG HIGH, by one, on the cv_wait_unlock path - that variant
 *   returns without the lock, so its decrement is unprotected. The cost is a
 *   wakeup nobody needed. The cost of the other direction would be a hang.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/time.h>

#include "kprintf.h"

void cv_init(struct cv *cvp, const char *desc) {
    cvp->cv_description = desc;
    cvp->cv_waiters     = 0;
}

/* Upstream asserts the condvar has no waiters left and panics if it does,
 * because destroying one underneath a sleeper leaves that sleeper waiting on
 * an address nothing will ever signal.
 *
 * Reported rather than fatal. A panic in a teardown path takes the machine
 * down over a leak, and the leak is diagnosable from the line: the waiter is
 * still on the channel, will hit kern_synch.c's ten-second ceiling, and will
 * then return EWOULDBLOCK to a caller that re-tests its predicate. A slow
 * path and a printed complaint beat a dead machine. */
void cv_destroy(struct cv *cvp) {
    if (cvp->cv_waiters != 0) {
        kprintf_c(0x0C, "cv_destroy: '%s' destroyed with %d waiter(s) still "
                        "on it\n",
                  cvp->cv_description != NULL ? cvp->cv_description : "?",
                  cvp->cv_waiters);
    }
    cvp->cv_description = NULL;
    cvp->cv_waiters     = 0;
}

/* sbintime to ticks. Zero and negative both become 0, which gsleep reads as
 * "no deadline of my own" - the ceiling still applies. Anything that rounds
 * down to nothing becomes 1 rather than 0, because a caller that asked for a
 * short timeout wants a short timeout, not a ten-second one. */
static int sbt_to_ticks(sbintime_t sbt) {
    int t;

    if (sbt <= 0) {
        return 0;
    }
    t = (int)((sbt * hz) >> 32);
    return t > 0 ? t : 1;
}

void _cv_wait(struct cv *cvp, struct lock_object *lock) {
    cvp->cv_waiters++;
    (void)genesis_lo_sleep(cvp, lock, 0);
    /* The lock is held again here - gsleep reacquires before returning - so
     * this decrement is protected exactly as the increment was. */
    cvp->cv_waiters--;
}

void _cv_wait_unlock(struct cv *cvp, struct lock_object *lock) {
    cvp->cv_waiters++;
    (void)genesis_lo_sleep_unlock(cvp, lock, 0);
    /* Unprotected, deliberately - see the header comment on which direction
     * this can be wrong in and why that direction is the safe one. */
    cvp->cv_waiters--;
}

/* The interruptible forms.
 *
 * They return 0, always, and that is honest rather than lazy: the contexts
 * that can reach a condvar in this kernel are kernel threads and the boot
 * stack, and neither has signals delivered to it. There is no path by which
 * one of these can be interrupted, so reporting EINTR would be reporting
 * something that did not happen.
 *
 * This changes the day a SYSCALL path waits on a condvar. The waiter would
 * then be a user process, waitq_wait_until's signal check would fire, and
 * gsleep would need to distinguish that return from a timeout - which it
 * cannot today, because it collapses WAITQ_SIGNAL and WAITQ_TIMEOUT into
 * "the generation did not move". That is the change to make here, and it is
 * one function deep rather than a redesign. */
int _cv_wait_sig(struct cv *cvp, struct lock_object *lock) {
    _cv_wait(cvp, lock);
    return 0;
}

int _cv_timedwait_sbt(struct cv *cvp, struct lock_object *lock,
                      sbintime_t sbt, sbintime_t pr, int flags) {
    int r;

    (void)pr; (void)flags;
    cvp->cv_waiters++;
    r = genesis_lo_sleep(cvp, lock, sbt_to_ticks(sbt));
    cvp->cv_waiters--;
    /* 0 on a real wakeup, EWOULDBLOCK on the deadline. Upstream's contract,
     * and callers branch on it - a DMU transaction that timed out waiting for
     * a txg must retry rather than believe the txg completed. */
    return r;
}

int _cv_timedwait_sig_sbt(struct cv *cvp, struct lock_object *lock,
                          sbintime_t sbt, sbintime_t pr, int flags) {
    return _cv_timedwait_sbt(cvp, lock, sbt, pr, flags);
}

/* Upstream wakes exactly ONE waiter here. This wakes them all.
 *
 * Same decision kern_synch.c's wakeup_one and kernel/proc/waitq.c both make,
 * and for the same reason: the wait channels are a hash, so the set of
 * threads on a slot is not exactly the set waiting on this condvar, and
 * picking "one" out of it can pick a thread that belongs to a different
 * channel entirely - which loses the wakeup for everybody who wanted it. Every
 * condvar waiter is required to re-test its predicate in a loop, which is
 * what makes waking all of them safe; the cost is that n-1 go back to sleep.
 *
 * The gate on cv_waiters is the optimization the vendored header describes,
 * and it is safe in the only direction that matters - see the file comment. */
void cv_signal(struct cv *cvp) {
    if (cvp->cv_waiters > 0) {
        wakeup_one(cvp);
    }
}

void cv_broadcastpri(struct cv *cvp, int pri) {
    /* pri is a priority to lend the woken thread so it is not starved by
     * whoever woke it. Genesis's ULE scores from the run/sleep ratio and has
     * no settable priority to override it with - sched_prio() in
     * kern_synch.c records nothing for the same reason. */
    (void)pri;
    if (cvp->cv_waiters > 0) {
        wakeup(cvp);
    }
}
