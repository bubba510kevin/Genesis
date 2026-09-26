#ifndef GENESIS_BSD_COMPAT_SYS_TASKQUEUE_H
#define GENESIS_BSD_COMPAT_SYS_TASKQUEUE_H

/* Deferred work, onto a KERNEL THREAD.
 *
 * --- what this used to be, and why it changed ----------------------------
 * `taskqueue_enqueue` was a macro that CALLED the task, immediately, on the
 * enqueuing CPU. That was correct for what used it - UMA's per-CPU cache
 * drain and netinet/in_mcast.c's inm-free task, neither of which sleeps and
 * both of which are safe to run at the call site - and it was not a
 * taskqueue. ROADMAP item 7 names it as one of the four things ZFS's write
 * path is blocked on, because the DMU enqueues work that must run LATER, in
 * a context that may block.
 *
 * It is real now: kernel/bsd/kern_taskqueue.c keeps a queue and a kernel
 * thread that drains it, so a task runs after its enqueuer has returned and
 * may sleep while it runs. The thread is what made this possible and did not
 * exist before kernel/include/kthread.h.
 *
 * --- what a caller has to know now that did not matter before -------------
 * A task's function no longer runs before taskqueue_enqueue returns. Code
 * that relied on that - anything reading state the task produces right after
 * enqueuing it - has to wait, and taskqueue_drain is how. Upstream's
 * semantics, and the reason drain exists upstream at all.
 *
 * Enqueuing is still safe from an interrupt handler: the queue's lock
 * disables interrupts on this CPU while held, and waking the thread only
 * marks it runnable.
 *
 * ONE queue and ONE thread serve all three of upstream's names
 * (taskqueue_thread, taskqueue_swi, taskqueue_fast). Upstream separates them
 * so that a task which blocks cannot delay one which must not - that
 * distinction is real and is NOT made here, so a slow task delays every task
 * behind it. It is a latency cost rather than a correctness one, and the fix
 * is a second thread on a second queue rather than a redesign. */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/_callout.h>

struct taskqueue;
typedef void task_fn_t(void *context, int pending);
struct task {
    /* The TAILQ link is upstream's and has to be present by name: UMA's
     * timeout_task is embedded in a struct that other code walks. */
    TAILQ_ENTRY(task)  ta_link;
    unsigned short     ta_pending;
    unsigned short     ta_priority;
    void (*ta_func)(void *, int);
    void  *ta_context;
};
struct timeout_task {
    struct task t;
    int         running;
    /* Genesis: the timer behind the deadline form.
     *
     * Upstream keeps this in the queue rather than in the task, because its
     * queues have a callout each. Here it is per-task, which is the cheaper
     * arrangement when there is one queue: the alternative is a single timer
     * plus a sorted list of deadlines, which is what the callout wheel
     * already is - so this delegates to it instead of building a second one.
     *
     * <sys/_callout.h> exists for exactly this: embedding a struct callout
     * without pulling in the callout API. */
    struct callout c;
};

extern struct taskqueue *taskqueue_thread;

/* ta_pending is zeroed here, and that is not cosmetic any more.
 *
 * It used to be left alone, which was harmless while taskqueue_enqueue called
 * the task and never looked at the field. Coalescing reads it: a non-zero
 * ta_pending means "already on the queue", so a task living in freshly
 * kmalloc'd memory whose ta_pending happened to be non-zero would be counted
 * as queued and never run at all. Upstream sets it here for the same reason. */
#define TASK_INIT(t, p, f, c) do {                                  \
        (t)->ta_pending = 0; (t)->ta_priority = (unsigned short)(p);\
        (t)->ta_func = (f); (t)->ta_context = (c);                  \
} while (0)
/* The callout has to be initialised too now, and TIMEOUT_TASK_INIT is the
 * only place upstream gives to do it. genesis_timeout_task_init does both
 * halves so a caller cannot get one and forget the other - which would be a
 * callout_reset on an uninitialised callout, i.e. a wheel insertion through a
 * garbage link. */
void genesis_timeout_task_init(struct timeout_task *tt, task_fn_t *fn,
                               void *context);
#define TIMEOUT_TASK_INIT(q, tt, p, f, c) \
        genesis_timeout_task_init((tt), (f), (c))

/* TASK_INITIALIZER - the same fields as TASK_INIT but as a static
 * initialiser, upstream's spelling. netinet/in_mcast.c uses one for the
 * deferred inm-free task. */
#define TASK_INITIALIZER(priority, func, context)       \
        { .ta_priority = (priority),                    \
          .ta_func = (func),                            \
          .ta_context = (context) }

/* TASKQUEUE_DEFINE_THREAD declares a NAMED global taskqueue with its own
 * thread. There is one taskqueue here, shared under all of upstream's names
 * (see the file comment), so this defines the pointer and
 * genesis_taskqueue_init points it at that one. */
#define TASKQUEUE_DEFINE_THREAD(name)   struct taskqueue *taskqueue_##name

int  genesis_taskqueue_enqueue_timeout(struct timeout_task *tt, int ticks);
int  genesis_taskqueue_cancel_timeout(struct timeout_task *tt);

/* The sbintime-deadline form, and cancellation. kern/uipc_socket.c uses both
 * for its socket-splice idle timer. The sbintime is reduced to ticks because
 * that is the resolution behind it either way (see kernel/bsd/kern_time.c). */
#define taskqueue_enqueue_timeout_sbt(q, tt, sbt, pr, fl) \
        genesis_taskqueue_enqueue_timeout((tt), \
            (sbt) > 0 ? (int)(((sbt) * hz) >> 32) : 0)
#define taskqueue_cancel_timeout(q, tt, pendp) \
        genesis_taskqueue_cancel_timeout((tt))
void genesis_taskqueue_drain_timeout(struct timeout_task *tt);

#define taskqueue_enqueue_timeout(q, tt, t) genesis_taskqueue_enqueue_timeout((tt), (t))
#define taskqueue_drain_timeout(q, tt)      genesis_taskqueue_drain_timeout((tt))

/* A real function now, not a macro that calls the task.
 *
 * Returns 0 like upstream. A task already queued and not yet started is
 * COALESCED - ta_pending is bumped and the task is not linked twice, and the
 * handler is told how many enqueues it is answering for through its
 * `pending` argument. That is upstream's contract and it is load-bearing for
 * a link-state task: two state changes before the thread runs must produce
 * one run that reports the CURRENT state, not two runs reporting a stale one
 * and then the current one. */
int taskqueue_enqueue(struct taskqueue *queue, struct task *task);

/* NET_TASK_INIT is TASK_INIT with the network epoch entered around the
 * handler. There is no epoch here (see net/vnet.h for the same reasoning
 * about VIMAGE), so it is the plain form. */
#ifndef NET_TASK_INIT
#define NET_TASK_INIT(t, p, f, c) TASK_INIT((t), (p), (f), (c))
#endif

/* taskqueue_fast - upstream's queue for handlers that must not sleep, which
 * is what a driver's interrupt task uses. Genesis has one taskqueue, so this
 * names it - which is SAFER than upstream in one direction (a fast task runs
 * on a sleepable thread, so it may block even though it need not) and worse
 * in the other (it queues behind a task that does). */
extern struct taskqueue *taskqueue_fast;

/* Wait until `task` is neither queued nor running. A driver calls this on
 * detach so a task cannot still be running against a softc about to be
 * freed - which is the specific use-after-free this exists to prevent, and
 * which became possible for the first time when tasks stopped running inline.
 *
 * Blocks, so it must not be called from an interrupt handler; and it refuses
 * rather than deadlocks if called from inside a task on the queue it is
 * draining, which would otherwise be a thread waiting for itself. */
void taskqueue_drain(struct taskqueue *queue, struct task *task);

/* The system's general-purpose software-interrupt task queue. net/if.c
 * enqueues if_link_task on it so a link-state change is reported outside the
 * driver's interrupt handler.
 *
 * Genesis has one queue serviced by one kernel thread (see the file comment
 * and kernel/bsd/kern_taskqueue.c), so this names it. Declared as a pointer
 * because that is what upstream's callers
 * pass around, and defined in kernel/bsd/netglue.c. */
extern struct taskqueue *taskqueue_swi;

/* Genesis: create the one queue and start its thread. Called once, from the
 * boot path, after kthread_init() and kheap_init(). Everything above works
 * before this runs - taskqueue_enqueue falls back to running the task inline,
 * which is what it always did - so the ordering is about WHEN work starts
 * being deferred, not about correctness. */
void genesis_taskqueue_init(void);

/* How many queued (or currently running) tasks have any pointer into
 * [base, base+size)? Asked by kldload.c before unmapping a module image -
 * see kern_taskqueue.c. */
int  taskqueue_count_in_range(uint64_t base, uint64_t size);

void genesis_taskqueue_report(unsigned char color);
int  genesis_taskqueue_selftest(void);

#endif
