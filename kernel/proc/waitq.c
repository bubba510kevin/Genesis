#include "bkl.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "timer.h"
#include "typesk.h"
#include "waitq.h"

/* See waitq.h for what this is for. What follows is the loop, and every line
 * of it was already in keyboard.c - this is a move, not a rewrite. The
 * behaviour is meant to be identical, which is why it lands as its own change
 * with the keyboard still the only user: a bisect that ends here should be
 * able to say "the move broke it" without a new feature confusing the answer.
 */

void waitq_init(wait_queue_t *q) {
    int i;

    for (i = 0; i < WAITQ_WORDS; i++) {
        q->waiters[i] = 0;
    }
}

void waitq_add(wait_queue_t *q, process_t *p) {
    int slot;

    if (p == NULL) {
        return;
    }
    slot = proc_index(p);
    if (q->waiters[slot / 64] & (1ULL << (slot % 64))) {
        return;                            /* already queued */
    }
    q->waiters[slot / 64] |= 1ULL << (slot % 64);
    /* The back pointer is set from the same branch that fills the slot,
     * and only from there. Two pieces of state describing one fact can
     * only stay in agreement if one place writes both. */
    p->blocked_on = q;
}

void waitq_remove(wait_queue_t *q, process_t *p) {
    int slot;

    if (p == NULL) {
        return;
    }
    slot = proc_index(p);
    q->waiters[slot / 64] &= ~(1ULL << (slot % 64));
    /* Cleared only if this really is the queue it was on. Clearing
     * unconditionally would let a remove from an unrelated queue erase the
     * record of a wait that is still live, and the process would then be left
     * on a list nothing knows to take it off. */
    if (p->blocked_on == q) {
        p->blocked_on = NULL;
    }
}

void waitq_leave(process_t *p) {
    if (p != NULL && p->blocked_on != NULL) {
        waitq_remove(p->blocked_on, p);
    }
}

/* The queue poll(2) parks on. See waitq.h for why there is exactly one. */
static wait_queue_t readiness_q;

wait_queue_t *waitq_readiness(void) {
    return &readiness_q;
}

static void wake_list(wait_queue_t *q) {
    int w;

    for (w = 0; w < WAITQ_WORDS; w++) {
        uint64 bits = q->waiters[w];     /* a snapshot: wakes may re-queue */

        while (bits != 0) {
            int b = __builtin_ctzll(bits);
            /* NULL for a slot freed since it was queued - the old array
             * would have woken the retired process_t; this skips it. */
            process_t *p = proc_at(w * 64 + b);

            bits &= bits - 1;
            if (p != NULL) {
                sched_wake(p);
            }
        }
    }
}

static uint64 wake_generation;

uint64 waitq_generation(void) {
    return wake_generation;
}

void waitq_wake_all(wait_queue_t *q) {
    wake_generation++;
    wake_list(q);

    /* Every readiness change in the kernel passes through here, which is the
     * only reason one line is enough. Waking the pollers at each individual
     * wake SITE - the pipe's two, the keyboard's one, and every one added
     * later - is the version of this that works until somebody adds a queue
     * and forgets, and then hangs a poll on exactly the new thing.
     *
     * Guarded rather than recursive: waitq_wake_all(waitq_readiness()) is a
     * legal call and must not become infinite. */
    if (q != &readiness_q) {
        wake_list(&readiness_q);
    }
}

int waitq_wait(wait_queue_t *q, int (*ready)(void *ctx), void *ctx) {
    return waitq_wait_until(q, ready, ctx, 0);
}

int waitq_wait_until(wait_queue_t *q, int (*ready)(void *ctx), void *ctx,
                     uint64 deadline) {
    while (!ready(ctx)) {
        process_t *me = proc_current();

        /* Checked at the top of every pass, before blocking and after every
         * wake. A deadline tested only after the block would be one whole
         * wait too late on the pass where something else woke us, and would
         * not be tested at all on a call that arrives already expired -
         * which is poll(fds, n, 0), the non-blocking probe, and the most
         * common timed call there is. */
        if (deadline != 0 && timer_ticks_now() >= deadline) {
            return WAITQ_TIMEOUT;
        }

        /* Halting the CPU was correct with one process and is a bug with two:
         * a background job would stop running whenever the shell was waiting.
         * Block instead and let the producer's interrupt wake us.
         *
         * If nothing else is runnable, schedule() returns immediately and we
         * fall through to the halt below - which is then genuinely the right
         * thing, since there is nothing to do but wait for the interrupt. */
        if (me != NULL) {
            waitq_add(q, me);
            if (deadline != 0) {
                sched_sleep_until(me, deadline);
            } else {
                sched_block(me);
            }
            waitq_remove(q, me);

            /* RUNNING again, whatever the state field says, because this
             * instruction is executing.
             *
             * The two ways back here disagree about it. Woken by sched_wake,
             * the process went READY and then schedule() marked it RUNNING -
             * fine. But when NOTHING ELSE WAS RUNNABLE, schedule() returned
             * immediately and never touched the state, so the process is
             * still PROC_BLOCKED while demonstrably running. If the loop then
             * exits on its deadline, the caller returns to ordinary code
             * marked blocked, and the next schedule() refuses to pick it -
             * for good. That is a process wedged by a TIMEOUT, on an idle
             * machine, which is the machine where "nothing else was runnable"
             * is most likely.
             *
             * poll(2) with a timeout takes this path. So does anything else
             * that waits with a deadline; the dispatcher objects found it
             * because two of their checks time out on purpose and the third
             * then needed the same context to run again.
             *
             * sys_nanosleep already worked around this by writing
             * PROC_RUNNING back by hand before halting - see the comment in
             * sched_tick that mentions it. The workaround belongs here, in
             * the one blocking loop, rather than in each caller that
             * remembers. */
            me->state = PROC_RUNNING;

            /* Re-test before believing the wake. A wake means the condition
             * MAY hold; several waiters are woken by one event and only one
             * of them can consume it. */
            if (ready(ctx)) {
                return 1;
            }
            /* Woken by a signal rather than by the condition. Every blocking
             * path in the kernel needs this check, and forgetting it in one
             * of them is what makes a process unkillable while it waits.
             *
             * Reporting it distinctly matters: the caller used to be unable
             * to tell this apart from a spurious wakeup and looped straight
             * back into sched_block() with the signal still pending and
             * nothing left to wake it. */
            if (signal_pending(me)) {
                return WAITQ_SIGNAL;
            }
            if (deadline != 0 && timer_ticks_now() >= deadline) {
                return WAITQ_TIMEOUT;
            }
        }

        /* `sti; hlt` in that order is load-bearing. sti leaves interrupts
         * disabled for exactly one more instruction, so the hlt cannot be
         * preceded by the wakeup arriving in the gap - which would otherwise
         * halt the CPU with the wakeup already delivered and nothing left to
         * wake it. The cli afterwards restores the state the syscall was
         * entered with: SFMASK clears IF on every SYSCALL, so a wait for an
         * interrupt that did not turn them on waits forever. That is the
         * whole reason this is not simply a while loop. */
        bkl_wait_for_interrupt();
    }
    return WAITQ_READY;
}
