#ifndef KTHREAD_H
#define KTHREAD_H

#include "process.h"
#include "typesk.h"

/* Kernel threads - ROADMAP's "THE ONE DEPENDENCY WORTH SEEING FIRST".
 *
 * --- what was missing -----------------------------------------------------
 * kernel/proc/sched.c schedules PROCESSES with user address spaces. Nothing
 * in this kernel was schedulable that was not one. That single absence is
 * what four separate roadmap items were each about to work around:
 *
 *   item 7   ZFS's txg sync thread, ARC reclaim thread and ZIL commit path
 *            are threads that block and are woken. No thread, no write path.
 *   item 14  KeWaitForSingleObject from a driver has nothing to deschedule,
 *            so only the userland half of the dispatcher objects was
 *            buildable.
 *   -        taskqueue(9) runs its task INLINE on the enqueuing CPU
 *            (kernel/bsd/compat/sys/taskqueue.h), which is not a taskqueue.
 *   -        kernel/bsd/kern_synch.c's sleep(9) IDLES the CPU on `hlt`
 *            rather than descheduling, because there was nothing else it
 *            could legally do.
 *
 * --- what a kernel thread is here -----------------------------------------
 * A process_t entry with is_kthread set, running in vmm_kernel_space(), that
 * never returns to ring 3. That is a deliberate choice over a separate
 * thread object with its own table: everything the scheduler already does -
 * the table scan, the readiness states, sched_wake, waitq's queues, the tick
 * accounting ULE scores on - works unchanged on a process_t, and a second
 * schedulable type would have meant a second copy of every one of them.
 *
 * The cost is honest: a kernel thread OCCUPIES A PROCESS SLOT. MAX_PROCESSES
 * is 16, so kernel threads and user processes compete for the same sixteen,
 * and KTHREAD_MAX below reserves a ceiling so a leaking kthread turns into
 * "cannot create kthread" rather than "cannot fork".
 *
 * --- kernel threads are NOT preemptible -----------------------------------
 * This is the one property a caller has to know before writing one.
 *
 * sched_tick sets the reschedule flag for a kernel thread exactly as it does
 * for anything else, but the switch happens in return_to_user(), which does
 * nothing when the interrupt landed while the kernel was already running -
 * and a kernel thread is ALWAYS running in the kernel. So the flag is set,
 * observed, and never acted on until the thread reaches a scheduling point
 * of its own.
 *
 * That is not an oversight to be fixed later by making return_to_user switch
 * on !to_user. This kernel has no locking around the structures a syscall
 * mutates - the heap, the handle tables, the mount table - and preempting
 * kernel code is precisely what turns that into corruption. The kernel is
 * non-preemptive by construction and kernel threads inherit it.
 *
 * The practical rule: A KERNEL THREAD MUST YIELD OR BLOCK. kthread_yield(),
 * kthread_sleep(), tsleep/msleep, or a waitq wait - any of them. A kernel
 * thread that computes in a loop without one hangs the machine, and it hangs
 * it in the way that produces no output at all.
 *
 * Every use the roadmap names is block-driven (a txg sync thread waits for a
 * txg, an ARC reclaimer waits for vm_lowmem, a taskqueue waits for a task),
 * so the rule costs those callers nothing.
 */

/* Ceiling on live kernel threads. See above: they share MAX_PROCESSES with
 * user processes, and this is what keeps one from eating all of it. */
#define KTHREAD_MAX 32   /* per-CPU workers (LinuxKPI workqueues) need room */

/* Set up the kernel-thread layer. Call after proc_init/sched_init and after
 * kheap_init (proc_alloc takes a kernel stack, which needs the VA allocator
 * and the frame allocator both up). */
void kthread_init(void);

/* Create a runnable kernel thread and return it, or NULL if the process
 * table or KTHREAD_MAX is exhausted.
 *
 * `fn` is entered with interrupts ENABLED and a clean 16KB kernel stack, and
 * is passed `arg`. Returning from `fn` is the same as calling kthread_exit().
 *
 * `name` is borrowed, not copied - pass a string literal. It exists so
 * kthread_report() can say which thread is which, and because a hang with a
 * name attached to it is a different debugging problem from a hang without.
 *
 * The thread does NOT run before this returns. It is marked runnable and the
 * caller keeps the CPU until it reaches a scheduling point, which is what
 * makes "create several, then let them run" expressible. */
process_t *kthread_create(void (*fn)(void *), void *arg, const char *name);

/* Give up the CPU. Returns when the scheduler comes back round.
 *
 * Legal from a kernel thread and from any other context that is safe to
 * schedule from; it is a thin wrapper over schedule() that exists so a
 * kernel thread's yield points are greppable. */
void kthread_yield(void);

/* Block for `ticks` timer ticks, or until something calls sched_wake on this
 * thread. Returns early on a wake - the caller loops on its own deadline if
 * it means the full duration, for the same reason sched_sleep_until says so. */
void kthread_sleep(uint64 ticks);

/* End the calling kernel thread. Does not return.
 *
 * The thread cannot free its own stack - it is standing on it - so it marks
 * itself and the next pass through schedule() reclaims the slot. See
 * kthread_reap(). */
void kthread_exit(void) __attribute__((noreturn));

/* Non-zero if the caller is running on a kernel thread.
 *
 * This is the question kernel/bsd/kern_synch.c asks before deciding whether
 * a sleep may deschedule: a kernel thread has somewhere to go, a driver's
 * interrupt handler does not, and the two are otherwise indistinguishable
 * from inside tsleep(). */
int kthread_running(void);

/* The calling kernel thread, or NULL if the caller is not one. */
process_t *kthread_current(void);

/* Free the slot and stack of any kernel thread that has exited.
 *
 * Called from schedule(), which is the only place that can be sure the dead
 * thread is not the one running - a thread cannot unmap the stack it is
 * standing on, so the reclaim has to happen in somebody else's context.
 *
 * A table scan rather than a pending list, for sched.c's reason: a list
 * needs a link field that a freed slot can leave dangling, and scanning
 * sixteen entries reads state that is true by construction. */
void kthread_reap(void);

/* How many kernel threads are live, and a one-line-per-thread boot report. */
int  kthread_count(void);
void kthread_report(uint8 color);

/* Exercise the whole path: creation, the private stack, the interrupt state
 * on entry, a real deschedule through sleep(9), and the exit reclaim.
 * Returns the number of failures. */
int kthread_selftest(void);

#endif
