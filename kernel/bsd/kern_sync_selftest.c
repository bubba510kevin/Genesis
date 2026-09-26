/* The checks for condvar(9) and taskqueue(9) - ROADMAP item 7's second
 * blocker, "CONDITION VARIABLES and sleepable taskqueues".
 *
 * --- why these are boot-time tests ---------------------------------------
 * Same reason kthread_selftest is: nothing in ring 3 can reach either of
 * them, and nothing in the tree USES either of them yet - the first real
 * caller is ZFS's DMU, which is not vendored. So this is the only thing that
 * has ever executed this code, and it says so rather than implying more.
 *
 * --- what makes these tests rather than smoke checks ----------------------
 * Both facilities are the kind that a completely broken implementation
 * passes: a cv_wait that returned immediately and a taskqueue_enqueue that
 * called the task inline would both leave every "did it happen" flag set.
 * The check that separates them is the same in both halves and it is a
 * CONTROL, not an assertion about the happy path:
 *
 *   cv_wait     the waiter must be observed BLOCKED, by another context that
 *               is running - and must not have proceeded until the signal.
 *   taskqueue   the task must NOT have run when taskqueue_enqueue returns.
 *               That single check is the entire difference between what this
 *               file replaced and what it is now.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/callout.h>
#include <sys/time.h>

#include "kprintf.h"

/* Yield without seeing kernel/include/sched.h, which reaches process.h and
 * its colliding `struct thread`. Declared rather than included, the same way
 * <sys/systm.h> declares the kthread bridge. */
void kthread_yield(void);

/* Bounded by TICKS, not by iterations.
 *
 * It was an iteration count, and that reported failures in the code under
 * test rather than in itself: a yield loop spins through five hundred passes
 * in well under one 10ms tick, so a check waiting on a five-tick cv_timedwait
 * gave up before the deadline could possibly arrive and then reported that
 * the timed wait never returned. Anything waiting on a timer has to be
 * bounded by the clock that timer runs on.
 *
 * `ticks` is the hz-rate counter <sys/kernel.h> publishes; see
 * kernel/bsd/kern_time.c. Compared with a wraparound-safe subtraction because
 * it is an int and upstream's idiom is written that way. */
#define WAIT_TICKS 300    /* three seconds at 100Hz */

#define WAIT_UNTIL(cond) do {                                   \
        int _deadline = ticks + WAIT_TICKS;                     \
        while (!(cond) && (int)(ticks - _deadline) < 0) {        \
            kthread_yield();                                    \
        }                                                       \
} while (0)

static int check(int cond, const char *what, int *failures) {
    if (!cond) {
        kprintf_c(0x0C, "sync selftest: %s\n", what);
        (*failures)++;
    }
    return cond;
}

/* ======================================================================
 * condvar
 * =================================================================== */

static struct mtx cv_test_mtx;
static struct cv  cv_test_cv;
static volatile int cv_predicate;
static volatile int cv_stage;        /* 0 -> 1 (about to wait) -> 2 (woke) */
static volatile int cv_timed_ret;
static volatile int cv_timed_done;

/* cv_waiters as seen by the waiter itself, immediately after its wait
 * returned and while it still holds the lock.
 *
 * Sampled here rather than by the creator, because the creator cannot sample
 * it at the right moment: the kernel is non-preemptive, so by the time the
 * creator runs again the waiter has already gone on to its SECOND wait and
 * incremented the count back to one. Checking it from outside was a test
 * asserting something that was never true rather than something that was
 * wrong. */
static volatile int cv_waiters_after;

/* Non-zero once the waiter thread is genuinely descheduled inside cv_wait.
 * Read by the creator, which can only be running if the waiter is not. */
static int cv_waiter_pid;

static void cv_waiter(void *arg) {
    (void)arg;

    mtx_lock(&cv_test_mtx);
    cv_stage = 1;
    /* The loop is the contract, not decoration: a wakeup means the predicate
     * MAY hold. Testing it once would pass here and fail against the first
     * hashed-channel collision. */
    while (!cv_predicate) {
        cv_wait(&cv_test_cv, &cv_test_mtx);
    }
    cv_waiters_after = cv_test_cv.cv_waiters;
    cv_stage = 2;
    mtx_unlock(&cv_test_mtx);

    /* Second half: a timed wait nobody will ever signal. It must come back
     * with EWOULDBLOCK rather than 0, or "it was woken" and "it gave up"
     * are indistinguishable and the timeout half of the API is untested. */
    mtx_lock(&cv_test_mtx);
    cv_timed_ret  = cv_timedwait(&cv_test_cv, &cv_test_mtx, 5);
    cv_timed_done = 1;
    mtx_unlock(&cv_test_mtx);
}

int genesis_condvar_selftest(void) {
    int failures = 0;

    mtx_init(&cv_test_mtx, "cvtest", NULL, 0);
    cv_init(&cv_test_cv, "cvtest");
    cv_predicate  = 0;
    cv_stage      = 0;
    cv_timed_done = 0;
    cv_timed_ret  = -1;

    check(cv_test_cv.cv_waiters == 0, "a fresh condvar had waiters", &failures);

    /* Signalling an empty condvar must be a no-op rather than anything. This
     * is the cheap half of the pair below - it proves cv_signal does not
     * depend on there being a waiter, so the later signal is doing work. */
    cv_signal(&cv_test_cv);
    cv_broadcast(&cv_test_cv);
    check(cv_test_cv.cv_waiters == 0,
          "signalling an empty condvar changed the waiter count", &failures);

    cv_waiter_pid = genesis_kthread_spawn(cv_waiter, NULL, "cv-waiter");
    if (!check(cv_waiter_pid != 0, "could not spawn the condvar waiter",
               &failures)) {
        return failures;
    }

    WAIT_UNTIL(cv_stage >= 1);
    if (!check(cv_stage >= 1, "the condvar waiter never ran", &failures)) {
        return failures;
    }

    /* THE CONTROL. Reaching this line means this context is running, and the
     * waiter can only have let that happen by descheduling inside cv_wait.
     * A cv_wait that spun, or that returned immediately, would either never
     * give the CPU back or would already have set cv_stage to 2. */
    check(cv_stage == 1, "the waiter passed cv_wait without the predicate "
                         "ever being set", &failures);
    check(cv_test_cv.cv_waiters == 1,
          "cv_waiters did not count the sleeping waiter - cv_signal would "
          "then skip the wakeup and hang it", &failures);

    /* Set the predicate under the lock and signal under it too, which is
     * condvar(9)'s contract and the thing the race argument in
     * kern_condvar.c rests on. */
    mtx_lock(&cv_test_mtx);
    cv_predicate = 1;
    cv_signal(&cv_test_cv);
    mtx_unlock(&cv_test_mtx);

    WAIT_UNTIL(cv_stage >= 2);
    check(cv_stage == 2, "cv_signal did not release the waiter", &failures);
    check(cv_waiters_after == 0,
          "cv_waiters was not decremented after the wait returned", &failures);

    /* The timed wait. Bounded by yields rather than by a tick count so a
     * broken timeout fails the test instead of hanging the boot. */
    WAIT_UNTIL(cv_timed_done);
    if (check(cv_timed_done, "the timed cv wait never returned", &failures)) {
        check(cv_timed_ret == EWOULDBLOCK,
              "an unsignalled cv_timedwait returned success - a timeout and "
              "a wakeup are indistinguishable", &failures);
    }

    /* The waiter has returned from its function, so it is exiting. Let it,
     * before cv_destroy complains about a waiter that is really gone. */
    WAIT_UNTIL(cv_test_cv.cv_waiters == 0);
    cv_destroy(&cv_test_cv);

    if (failures == 0) {
        kprintf("condvar: selftest passed\n");
    } else {
        kprintf_c(0x0C, "condvar: selftest FAILED (%d)\n", failures);
    }
    return failures;
}

/* ======================================================================
 * taskqueue
 * =================================================================== */

static struct task  tq_simple;
static struct task  tq_sleeper;
static struct timeout_task tq_timed;

static volatile int tq_simple_runs;
static volatile int tq_simple_pending;
static volatile int tq_sleeper_started;
static volatile int tq_sleeper_done;
static volatile int tq_timed_runs;

static void simple_task(void *ctx, int pending) {
    (void)ctx;
    tq_simple_pending = pending;
    tq_simple_runs++;
}

static void sleeper_task(void *ctx, int pending) {
    (void)ctx; (void)pending;
    tq_sleeper_started = 1;
    /* A task that BLOCKS. The whole point of a sleepable taskqueue, and
     * something the inline version could not have survived - it would have
     * blocked whoever enqueued it, which in the network stack is an
     * interrupt handler. */
    (void)tsleep(&tq_sleeper, 0, "tqslp", 3);
    tq_sleeper_done = 1;
}

static void timed_task(void *ctx, int pending) {
    (void)ctx; (void)pending;
    tq_timed_runs++;
}

int genesis_taskqueue_selftest(void) {
    int failures = 0;

    tq_simple_runs     = 0;
    tq_simple_pending  = 0;
    tq_sleeper_started = 0;
    tq_sleeper_done    = 0;
    tq_timed_runs      = 0;

    TASK_INIT(&tq_simple, 0, simple_task, NULL);
    TASK_INIT(&tq_sleeper, 0, sleeper_task, NULL);

    /* --- 1. the task is DEFERRED --------------------------------------
     * The one check this entire file exists for. Everything else here would
     * also pass against the macro that called the task inline; this would
     * not. */
    taskqueue_enqueue(taskqueue_thread, &tq_simple);
    if (!check(tq_simple_runs == 0,
               "the task ran inside taskqueue_enqueue - it is still inline, "
               "not deferred", &failures)) {
        return failures;
    }
    check(tq_simple.ta_pending == 1, "an enqueued task was not marked pending",
          &failures);

    /* --- 2. coalescing --------------------------------------------------
     * A second enqueue before the thread has run must NOT link the task
     * twice - that corrupts the list - and must reach the handler as
     * pending == 2. Deterministic here because the kernel is non-preemptive:
     * the service thread cannot run until this context yields. */
    taskqueue_enqueue(taskqueue_thread, &tq_simple);
    check(tq_simple.ta_pending == 2,
          "a second enqueue of a pending task did not coalesce", &failures);

    WAIT_UNTIL(tq_simple_runs != 0);
    if (!check(tq_simple_runs == 1,
               "the deferred task did not run exactly once", &failures)) {
        return failures;
    }
    check(tq_simple_pending == 2,
          "the handler was not told how many enqueues it was answering for",
          &failures);
    check(tq_simple.ta_pending == 0,
          "ta_pending was not cleared when the task ran", &failures);

    /* --- 3. a task that sleeps, and a drain that waits for it ----------
     * taskqueue_drain must not return while the handler is still executing.
     * The sleeper makes that window wide enough to be a real test rather
     * than a race the scheduler happens to win. */
    taskqueue_enqueue(taskqueue_thread, &tq_sleeper);
    taskqueue_drain(taskqueue_thread, &tq_sleeper);
    check(tq_sleeper_started,
          "taskqueue_drain returned before the task had even started",
          &failures);
    check(tq_sleeper_done,
          "taskqueue_drain returned while the task was still running - this "
          "is the use-after-free it exists to prevent", &failures);

    /* --- 4. the deadline form ------------------------------------------ */
    TIMEOUT_TASK_INIT(taskqueue_thread, &tq_timed, 0, timed_task, NULL);
    genesis_taskqueue_enqueue_timeout(&tq_timed, 3);
    check(tq_timed_runs == 0, "a timeout task ran before its deadline",
          &failures);

    WAIT_UNTIL(tq_timed_runs != 0);
    check(tq_timed_runs == 1, "a timeout task never ran", &failures);

    /* And cancellation, which has to be distinguishable from "it ran". */
    tq_timed_runs = 0;
    genesis_taskqueue_enqueue_timeout(&tq_timed, 200);
    check(genesis_taskqueue_cancel_timeout(&tq_timed) != 0,
          "cancelling a pending timeout task reported it was not scheduled",
          &failures);
    /* Waited out in TICKS, and for longer than a couple of them: the
     * cancelled deadline was 200 ticks away, so the only way to distinguish
     * "cancelled" from "not due yet" is to wait past when it would have been
     * due. A loop of fifty yields finishes in under a tick and proves
     * nothing. */
    {
        int quiet = ticks + 250;

        while ((int)(ticks - quiet) < 0) {
            kthread_yield();
        }
    }
    check(tq_timed_runs == 0, "a cancelled timeout task ran anyway",
          &failures);

    if (failures == 0) {
        kprintf("taskqueue: selftest passed\n");
    } else {
        kprintf_c(0x0C, "taskqueue: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
