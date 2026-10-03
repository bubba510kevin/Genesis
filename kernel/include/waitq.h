#ifndef WAITQ_H
#define WAITQ_H

#include "process.h"
#include "typesk.h"

#define WAITQ_WORDS ((MAX_PROCESSES + 63) / 64)

/* A list of processes blocked on one condition, and the loop that blocks on
 * it correctly.
 *
 * --- Why this is its own file ---------------------------------------------
 * This was inside keyboard.c, where it worked, and the loop around it was
 * three separate subtleties stacked on each other: the `sti; hlt` ordering,
 * the re-test of the condition after waking, and the signal check that is
 * what makes a blocked process killable. Every one of those has to be right
 * in every blocking path in the kernel, and a second copy written from
 * memory gets one of them wrong - usually the signal check, which does not
 * fail visibly, it just makes a process that cannot be killed while it waits.
 *
 * So there is one copy. The keyboard is the first user; a pipe is the second,
 * and it is the second user that this exists for.
 *
 * --- The condition is a callback, not a flag ------------------------------
 * The waiter has to re-test after every wake, because a wake means "the
 * condition MAY now hold", never "it does". Several processes are woken on
 * one event and only one of them gets the byte; the losers must go back to
 * sleep rather than proceed on a stale answer. Passing the test in as a
 * function is what lets that re-test live here instead of being rewritten,
 * and forgotten, at each call site.
 *
 * --- Waking all, not one --------------------------------------------------
 * Every waiter is woken on every event. They then race and whoever loses
 * blocks again, which is what a terminal with two readers actually does - the
 * line goes to exactly one of them and nobody can predict which. Waking one
 * and choosing wrong (a process about to be killed, say) loses the wakeup
 * entirely and hangs the rest. */

typedef struct wait_queue {
    /* One BIT per process-table slot: bit i set means proc_at(i) waits here.
     * Every process can be on the queue at once, add and remove need no
     * allocation on a path that runs with interrupts off, and a queue costs
     * MAX_PROCESSES/8 bytes. A single pointer was the earliest design and it
     * was a silent hang the moment two processes waited: the second
     * overwrote the slot and the first was never woken again.
     *
     * It was an array of MAX_PROCESSES pointers until 2026-10-03, which is
     * the same set - a process was only ever in it once, and the pointer was
     * always &table[slot] - at 64 times the size. That mattered when the
     * table went from 64 slots to 256: every pipe, eventfd, dispatcher object
     * and LinuxKPI task embeds one of these, and at 2KB each they were most
     * of what the larger table cost (ROADMAP 16(k)). A slot recycled while
     * its bit is set gets a spurious wake, exactly as the stale pointer did,
     * and every waiter re-tests its condition after waking. */
    uint64 waiters[WAITQ_WORDS];
} wait_queue_t;

/* Empty the queue. For a fresh queue only - it does not wake anybody. */
void waitq_init(wait_queue_t *q);

/* Block the calling process until `ready(ctx)` returns non-zero.
 *
 * Returns 1 when the condition holds, 0 when a signal arrived instead - and
 * the caller must return -EINTR on 0 rather than looping, because the signal
 * is delivered at the syscall boundary and blocking again here would sit on
 * a pending signal with nothing left to wake it.
 *
 * Callable only from a syscall path on a kernel stack with nothing live
 * below the caller. It turns interrupts on. */
int waitq_wait(wait_queue_t *q, int (*ready)(void *ctx), void *ctx);

/* What waitq_wait_until returns. waitq_wait returns the first two only, and
 * their values are chosen so the existing `if (!waitq_wait(...))` call sites
 * keep meaning exactly what they meant. */
#define WAITQ_READY    1
#define WAITQ_SIGNAL   0
#define WAITQ_TIMEOUT (-1)

/* waitq_wait with a deadline, in absolute ticks. `deadline` of 0 means no
 * deadline and makes this identical to waitq_wait.
 *
 * The deadline is absolute rather than a duration because the loop can go
 * round several times - a signal-free spurious wake, another waiter winning
 * the byte - and a duration would restart on each pass. That is the timeout
 * that never expires under load, which is the worst kind: it works on an idle
 * machine and hangs on a busy one. */
int waitq_wait_until(wait_queue_t *q, int (*ready)(void *ctx), void *ctx,
                     uint64 deadline);

/* The queue every poll(2) parks on.
 *
 * --- Why one shared queue and not one registration per polled object ------
 * poll waits on N objects at once, and a wait queue here records the WAITER,
 * not the wait: process_t has a single `blocked_on` back pointer, so a
 * process can be on exactly one queue. Registering on all N would mean making
 * that pointer a list, capped at some N, and deciding what to do when a poll
 * exceeds the cap. Every answer to that last question is a silent hang or a
 * spurious error on an operation that was perfectly legal.
 *
 * So poll parks here instead, and waitq_wake_all wakes this queue as well as
 * its own - from ONE place, so a wait queue added later gets it without
 * anybody remembering to. A poller is therefore woken by every readiness
 * event in the system, re-tests its own descriptors, and goes back to sleep
 * if none of them was the one. That is a wakeup it did not need, not a wakeup
 * it missed, and the two failure modes are not comparable: a spurious wake
 * costs a scan of at most MAX_HANDLES entries, and a missed wake is a hang.
 *
 * The cost is real and worth writing down: with P processes polling, every
 * pipe write wakes all P. At MAX_PROCESSES = 16 that is nothing. It becomes
 * worth replacing when the process table is not a 16-entry array either, and
 * the replacement is per-object registration with the back pointer widened -
 * at which point this function disappears and nothing else has to change,
 * because no caller outside waitq.c and poll refers to it. */
wait_queue_t *waitq_readiness(void);

/* Make every waiter runnable. Safe from an interrupt handler: sched_wake only
 * marks a process ready and sets the reschedule flag, and the switch happens
 * on the way back out to user mode. */
void waitq_wake_all(wait_queue_t *q);

/* Put a process on the queue without blocking it. waitq_wait does this for
 * you and is what callers should use; it is exported because the list is the
 * part of this file that can be tested off-target - the blocking loop halts
 * the CPU waiting for an interrupt a host process will never receive, and the
 * list is where the single-pointer version was wrong. */
void waitq_add(wait_queue_t *q, process_t *p);

/* Drop a process from the queue without waking it. Callers that know which
 * queue a process is on use this; teardown, which does not, uses waitq_leave
 * below. */
void waitq_remove(wait_queue_t *q, process_t *p);

/* Take a dying process off whatever queue it is on, if any.
 *
 * The case this exists for: a process is blocked in waitq_wait, something
 * else kills it, and its slot is retired without the wait ever returning.
 * waitq_wait removes itself on the way out, so every path that UNWINDS is
 * already clean - it is the paths that do not unwind that leave a pointer to
 * a retired process_t in waiters[], where waitq_wake_all will later hand it
 * to sched_wake.
 *
 * That is survivable today and only by luck: the slot is reused rather than
 * freed, sched_wake ignores anything not in PROC_BLOCKED, and the scheduler
 * scans a table rather than walking a list. Every one of those is a property
 * of the current implementation rather than a guarantee, and the version of
 * this that bites is the one where the slot has been handed to a new process
 * which gets woken out of an unrelated sleep. */
void waitq_leave(process_t *p);

#endif
