/* taskqueue(9): a queue of deferred work and a kernel thread that drains it.
 *
 * See <sys/taskqueue.h> for what this replaced (a macro that CALLED the task
 * inline) and why - ROADMAP item 7 names "sleepable taskqueues" as one of the
 * four things ZFS's write path is blocked on.
 *
 * --- the shape ------------------------------------------------------------
 * One queue, one servicing kernel thread, three of upstream's names pointing
 * at it. The thread's whole life is: wait on a condvar until the queue is
 * non-empty, take the head, run it, repeat. That is upstream's taskqueue_run
 * loop with the per-queue thread pool reduced to one.
 *
 * --- the locking, and the one thing that makes it non-obvious -------------
 * A single mutex covers the list, ta_pending, and which task is running. It
 * has to be a SPIN mutex with interrupts disabled while held (kmtx, via
 * <sys/mutex.h>) rather than anything sleepable, because taskqueue_enqueue is
 * called from interrupt handlers - net/if.c's link-state change arrives that
 * way - and a handler that blocked on a lock held by the code it interrupted
 * is a one-CPU deadlock.
 *
 * The service thread therefore must NOT hold that mutex while running a task:
 * a task is allowed to sleep, and sleeping with interrupts disabled would
 * never be woken. So the loop drops the lock around the handler, and the
 * "which task is running" field is what lets a drainer see work that is in
 * flight but no longer on the list. Without that field a drain would return
 * while its task was halfway through touching the softc being freed, which is
 * the exact use-after-free taskqueue_drain exists to prevent.
 *
 * --- the fallback is not a stub -------------------------------------------
 * Before genesis_taskqueue_init runs, and if no kernel thread could be
 * created, taskqueue_enqueue runs the task inline - exactly what this file
 * replaced. That is deliberate: enqueues happen during boot before the thread
 * exists (the network stack's SYSINITs, a driver attaching), and the choices
 * are to run the work at the call site or to drop it. Dropping it would make
 * a missing thread look like a NIC that never reports link state.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/queue.h>
#include <sys/callout.h>
#include <sys/taskqueue.h>
#include <sys/time.h>

#include "kprintf.h"

/* Declared rather than included from kernel/include/sched.h, which reaches
 * process.h and its `struct thread` - the collision <sys/systm.h>'s kthread
 * bridge exists for. */
void kthread_yield(void);

struct taskqueue {
    const char         *tq_name;
    struct mtx          tq_mtx;
    /* The thread sleeps here for work; drainers sleep on tq_drained.
     *
     * Two condvars rather than one, because the two waits are woken by
     * opposite events - one by "a task arrived", the other by "a task
     * finished" - and sharing a channel would wake the service thread on
     * every completion it caused itself. Correct, but a wakeup per task for
     * nothing. */
    struct cv           tq_work;
    struct cv           tq_drained;
    TAILQ_HEAD(, task)  tq_list;
    struct task        *tq_running;
    int                 tq_thread_pid;   /* 0 until a thread services it */
    uint64              tq_enqueued;
    uint64              tq_ran;
    uint64              tq_coalesced;
    uint64              tq_inline;
};

/* The one queue. Statically allocated rather than kmalloc'd: it is a
 * singleton, its lifetime is the machine's, and a static one is usable from
 * genesis_taskqueue_init without ordering it after the heap. */
static struct taskqueue the_queue;
static int              queue_ready;

/* Upstream's three names, all of them this queue. Defined here rather than in
 * the three separate files that each needed one (uma_vendor.c, callout.c,
 * netglue.c) - they were three NULL pointers with three comments explaining
 * that nothing read through them, and now something does. */
struct taskqueue *taskqueue_thread;
struct taskqueue *taskqueue_fast;
struct taskqueue *taskqueue_swi;

/* --- the service thread -------------------------------------------------- */

static void taskqueue_service(void *arg) {
    struct taskqueue *tq = (struct taskqueue *)arg;

    for (;;) {
        struct task *t;
        int pending;

        mtx_lock(&tq->tq_mtx);
        while (TAILQ_EMPTY(&tq->tq_list)) {
            /* Drops the mutex across the sleep and takes it back before
             * returning - see kern_condvar.c. The predicate is re-tested by
             * this `while` rather than trusted, because a wakeup means the
             * queue MAY be non-empty: the wait channels are hashed, so an
             * unrelated wakeup can land here. */
            cv_wait(&tq->tq_work, &tq->tq_mtx);
        }

        t = TAILQ_FIRST(&tq->tq_list);
        TAILQ_REMOVE(&tq->tq_list, t, ta_link);
        pending        = t->ta_pending;
        t->ta_pending  = 0;
        tq->tq_running = t;
        mtx_unlock(&tq->tq_mtx);

        /* Outside the lock, deliberately. A task may sleep, and the lock
         * disables interrupts. See the file comment. */
        t->ta_func(t->ta_context, pending);

        mtx_lock(&tq->tq_mtx);
        tq->tq_running = NULL;
        tq->tq_ran++;
        cv_broadcast(&tq->tq_drained);
        mtx_unlock(&tq->tq_mtx);
    }
}

/* --- init ---------------------------------------------------------------- */

void genesis_taskqueue_init(void) {
    struct taskqueue *tq = &the_queue;

    tq->tq_name = "taskq";
    mtx_init(&tq->tq_mtx, "taskq", NULL, 0);
    cv_init(&tq->tq_work, "taskq-work");
    cv_init(&tq->tq_drained, "taskq-drain");
    TAILQ_INIT(&tq->tq_list);
    tq->tq_running    = NULL;
    tq->tq_thread_pid = 0;

    /* Published BEFORE the thread is spawned. The thread's first act is to
     * take the mutex and look at the list, so everything it touches has to be
     * initialised already - and a thread that started running against a
     * half-built queue would fault inside TAILQ_EMPTY on a NULL head. */
    taskqueue_thread = tq;
    taskqueue_fast   = tq;
    taskqueue_swi    = tq;
    queue_ready      = 1;

    tq->tq_thread_pid = genesis_kthread_spawn(taskqueue_service, tq, "taskq");
    if (tq->tq_thread_pid == 0) {
        kprintf_c(0x0C, "taskqueue: no kernel thread - tasks will run inline "
                        "at their enqueue site\n");
    }
}

/* --- enqueue ------------------------------------------------------------- */

int taskqueue_enqueue(struct taskqueue *queue, struct task *task) {
    struct taskqueue *tq = (queue != NULL) ? queue : &the_queue;

    if (task == NULL || task->ta_func == NULL) {
        return 0;
    }

    /* No thread to hand it to: run it here, which is what this whole file
     * replaced and is still the right answer when there is nowhere to defer
     * to. See the file comment on why this is not a stub. */
    if (!queue_ready || tq->tq_thread_pid == 0) {
        tq->tq_inline++;
        task->ta_pending = 0;
        task->ta_func(task->ta_context, 1);
        return 0;
    }

    mtx_lock(&tq->tq_mtx);
    if (task->ta_pending != 0) {
        /* Already queued and not yet started. Bump the count and DO NOT link
         * it again - linking a TAILQ entry that is already linked corrupts
         * the list rather than queueing twice.
         *
         * The count reaches the handler as its `pending` argument, which is
         * upstream's contract and is what makes coalescing correct rather
         * than lossy: a link-state task that is enqueued twice before it runs
         * must run ONCE and report the current state, and a handler that
         * cares how many events it is answering for is told. */
        if (task->ta_pending < 0xFFFFu) {
            task->ta_pending++;
        }
        tq->tq_coalesced++;
        mtx_unlock(&tq->tq_mtx);
        return 0;
    }

    task->ta_pending = 1;
    TAILQ_INSERT_TAIL(&tq->tq_list, task, ta_link);
    tq->tq_enqueued++;
    /* Signalled with the mutex HELD, which is condvar(9)'s contract and the
     * thing that makes the wait race-free - see kern_condvar.c for the
     * four-step argument. Safe from an interrupt handler: this only bumps a
     * generation counter and marks a thread runnable. */
    cv_signal(&tq->tq_work);
    mtx_unlock(&tq->tq_mtx);
    return 0;
}

/* --- drain --------------------------------------------------------------- */

void taskqueue_drain(struct taskqueue *queue, struct task *task) {
    struct taskqueue *tq = (queue != NULL) ? queue : &the_queue;

    if (task == NULL || !queue_ready || tq->tq_thread_pid == 0) {
        /* Nothing was deferred, so nothing is in flight. */
        return;
    }

    /* A task calling drain on its own queue would be the service thread
     * waiting for the service thread. Refused with a complaint rather than
     * allowed to hang: upstream detects the same case, and a hang here has no
     * symptom other than a machine that stops making progress. */
    if (genesis_kthread_id() == tq->tq_thread_pid) {
        kprintf_c(0x0C, "taskqueue_drain: called from the service thread - "
                        "refusing, it would wait for itself\n");
        return;
    }

    /* --- why there are two loops -----------------------------------------
     *
     * BOTH conditions in each. ta_pending covers "still on the list";
     * tq_running covers "off the list and executing", which is the window the
     * whole function exists for - a drain that only checked the list would
     * return while the handler was halfway through the softc about to be
     * freed.
     *
     * The difference between the loops is how the caller gives up the CPU,
     * and getting it wrong is a hang rather than a slowdown:
     *
     * A KERNEL THREAD waits on the condvar. It deschedules, the service
     * thread runs, and the broadcast at the end of each task wakes it.
     *
     * ANYTHING ELSE - the boot path, a syscall - CANNOT deschedule through
     * sleep(9): kern_synch.c's ksleep_can_block() answers no for it, so
     * cv_wait would fall through to `sti; hlt` and idle the CPU. That is not
     * merely slow here, it is the deadlock: the thing this call is waiting
     * for is a kernel thread that needs the CPU to make progress, and halting
     * is precisely refusing to give it. So this yields instead, which puts
     * the service thread on the CPU and comes back when it blocks or
     * finishes.
     *
     * The yield loop is a spin when the service thread is itself asleep
     * inside a task (nothing else is runnable, so schedule() returns
     * straight back). It is a spin with interrupts ON, so the timer still
     * wakes the sleeper and the loop still terminates - honest busy-waiting,
     * not a hang. The general fix is for a non-thread context to be able to
     * deschedule too, which is a question about interrupt-handler detection
     * rather than about taskqueues; see verif.c's PART B. */
    if (genesis_kthread_id() == 0) {
        for (;;) {
            int busy;

            mtx_lock(&tq->tq_mtx);
            busy = (task->ta_pending != 0 || tq->tq_running == task);
            mtx_unlock(&tq->tq_mtx);
            if (!busy) {
                return;
            }
            kthread_yield();
        }
    }

    mtx_lock(&tq->tq_mtx);
    while (task->ta_pending != 0 || tq->tq_running == task) {
        cv_wait(&tq->tq_drained, &tq->tq_mtx);
    }
    mtx_unlock(&tq->tq_mtx);
}

/* --- the deadline form --------------------------------------------------- */

void genesis_timeout_task_init(struct timeout_task *tt, task_fn_t *fn,
                               void *context) {
    tt->t.ta_pending  = 0;
    tt->t.ta_priority = 0;
    tt->t.ta_func     = fn;
    tt->t.ta_context  = context;
    tt->running       = 0;
    callout_init(&tt->c, 1);
}

/* What the callout wheel calls when the deadline arrives.
 *
 * This runs in TIMER-INTERRUPT context, which is why it enqueues rather than
 * calling the handler: the point of taskqueue_enqueue_timeout is a timer with
 * a THREAD context attached, and running the task from the sweep would give
 * it the timer's context instead - the exact difference <sys/taskqueue.h>
 * used to describe as its limitation. */
static void timeout_task_fired(void *arg) {
    struct timeout_task *tt = (struct timeout_task *)arg;

    tt->running = 0;
    (void)taskqueue_enqueue(&the_queue, &tt->t);
}

int genesis_taskqueue_enqueue_timeout(struct timeout_task *tt, int ticks) {
    if (tt == NULL || tt->t.ta_func == NULL) {
        return 0;
    }
    if (ticks <= 0) {
        /* No delay asked for. Straight onto the queue - which still defers it
         * to the thread, so "zero ticks" means "soon", not "now". */
        return taskqueue_enqueue(&the_queue, &tt->t);
    }
    tt->running = 1;
    callout_reset(&tt->c, ticks, timeout_task_fired, tt);
    return 0;
}

int genesis_taskqueue_cancel_timeout(struct timeout_task *tt) {
    int was_pending;

    if (tt == NULL) {
        return 0;
    }
    /* Non-zero means "it was scheduled and is now not". kern/uipc_socket.c
     * branches on exactly this to decide whether it still owns a reference,
     * so answering 0 for a timer that WAS pending would leak that reference,
     * and answering non-zero for one that was not would drop it twice. */
    was_pending = callout_pending(&tt->c) ? 1 : 0;
    (void)callout_stop(&tt->c);
    tt->running = 0;

    /* A task whose timer already fired is on the QUEUE rather than on the
     * wheel, and cancelling the timer does not unqueue it. Say so by
     * reporting it pending, which is the answer that makes the caller keep
     * its reference until it drains. */
    if (tt->t.ta_pending != 0) {
        was_pending = 1;
    }
    return was_pending;
}

void genesis_taskqueue_drain_timeout(struct timeout_task *tt) {
    if (tt == NULL) {
        return;
    }
    /* The timer first, then the task. In that order and not the other one: a
     * timer that fires after the task has been drained puts the task straight
     * back on the queue, and the drain would have returned while work was
     * still coming. */
    (void)callout_stop(&tt->c);
    tt->running = 0;
    taskqueue_drain(&the_queue, &tt->t);
}

/* --- report -------------------------------------------------------------- */

void genesis_taskqueue_report(unsigned char color) {
    struct taskqueue *tq = &the_queue;

    kprintf_c(color, "taskqueue: %s  thread pid %d  enqueued %u  ran %u  "
                     "coalesced %u  inline %u\n",
              tq->tq_name != NULL ? tq->tq_name : "?", tq->tq_thread_pid,
              (uint32)tq->tq_enqueued, (uint32)tq->tq_ran,
              (uint32)tq->tq_coalesced, (uint32)tq->tq_inline);
}

/* --- module unload: is any queued task inside these bytes? ----------------
 *
 * Asked by kldload.c before it unmaps a module image; see irq.c's twin for
 * why the context is checked as well as the function, and callout.c's for why
 * the struct's own address counts - a struct task is normally a field of the
 * driver's softc, so its TAILQ links are in the module too and the service
 * thread would walk them into unmapped memory.
 *
 * tq_running is checked separately from the queue and is the case a driver
 * cannot fix by draining: the task is not on the list any more because the
 * thread has taken it off and is inside it right now. A module unloaded at
 * that moment gets its code unmapped underneath a running function. It cannot
 * happen through taskqueue_drain (which is what waits for exactly this), but
 * a module that never called drain has no reason to be trusted about it. */
int taskqueue_count_in_range(uint64_t base, uint64_t size) {
    struct taskqueue *tq = &the_queue;
    struct task      *t;
    int               n = 0;

    if (size == 0 || !queue_ready) {
        return 0;
    }
    mtx_lock(&tq->tq_mtx);
    TAILQ_FOREACH(t, &tq->tq_list, ta_link) {
        uint64_t self = (uint64_t)t;
        uint64_t fn   = (uint64_t)t->ta_func;
        uint64_t ctx  = (uint64_t)t->ta_context;

        if ((self >= base && self < base + size) ||
            (fn   >= base && fn   < base + size) ||
            (ctx != 0 && ctx >= base && ctx < base + size)) {
            n++;
        }
    }
    if (tq->tq_running != NULL) {
        uint64_t self = (uint64_t)tq->tq_running;
        uint64_t fn   = (uint64_t)tq->tq_running->ta_func;
        uint64_t ctx  = (uint64_t)tq->tq_running->ta_context;

        if ((self >= base && self < base + size) ||
            (fn   >= base && fn   < base + size) ||
            (ctx != 0 && ctx >= base && ctx < base + size)) {
            n++;
        }
    }
    mtx_unlock(&tq->tq_mtx);
    return n;
}
