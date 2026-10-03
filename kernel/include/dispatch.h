#ifndef DISPATCH_H
#define DISPATCH_H

#include "object.h"
#include "typesk.h"

/* NT dispatcher objects: events, semaphores and mutants.
 *
 * ROADMAP item 14 calls these "the biggest gap, and the half of the object
 * manager that has no substitute anywhere else in the kernel". The argument
 * is worth keeping, because it is not about hardware: NT's namespace exists
 * at least as much to let two unrelated processes agree on a SYNCHRONISATION
 * OBJECT BY NAME - \BaseNamedObjects\SomeMutex - as it does to name devices.
 * A Win32 program's first kernel call is very often CreateMutexW rather than
 * CreateFileW.
 *
 * --- three types, and why they cannot be one ------------------------------
 * The differences are visible to a WAITER, which is what makes them types
 * rather than flags on one type:
 *
 *   EVENT       a flag. A NOTIFICATION event stays set once signalled and
 *               releases every waiter; a SYNCHRONISATION event releases
 *               exactly one and auto-resets. Same object, opposite answer to
 *               "is it still signalled after I woke up".
 *   SEMAPHORE   a count with a limit. A release past the limit is REFUSED,
 *               not clamped: a caller that over-releases has a counting bug,
 *               and clamping hides it forever.
 *   MUTANT      ownership, and RECURSIVE - the owner may take it again and
 *               must release it as many times. That is exactly why it is not
 *               a semaphore of one, and why release by a non-owner is an
 *               error rather than a no-op.
 *
 * --- what a caller has to know about waiting ------------------------------
 * ob_wait CONSUMES. A synchronisation event resets, a semaphore decrements, a
 * mutant takes ownership. ob_poll does not - it answers "would this block".
 * See object_type_t::wait: the second cannot be built out of the first, and
 * calling wait speculatively takes something.
 *
 * --- who may wait ---------------------------------------------------------
 * Anything with a process context: a user process inside a syscall, a kernel
 * thread, the boot path. NOT an interrupt handler, which has nothing to
 * deschedule - the same boundary kernel/bsd/kern_synch.c draws for sleep(9),
 * and the reason KeWaitForSingleObject at raised IRQL has to refuse rather
 * than block when the driver-facing half of this is written.
 */

/* Create an unnamed event. `manual` non-zero for a notification event (stays
 * set, releases everybody), zero for a synchronisation event (releases one,
 * auto-resets). `initial` non-zero to start signalled.
 *
 * The caller owns the returned reference. NULL if the object pool is full. */
object_t *event_create(int manual, int initial);

/* Create an unnamed semaphore with `initial` permits and a ceiling of
 * `limit`. NULL if the pool is full or the arguments are inconsistent
 * (initial above limit, limit below one). */
object_t *semaphore_create(int64 initial, int64 limit);

/* Create an unnamed mutant, owned by the caller if `owned` is non-zero -
 * which is CreateMutex(bInitialOwner=TRUE), and is not the same as creating
 * it and then taking it, because the second has a window. */
object_t *mutant_create(int owned);

/* Create a dispatcher object and NAME it under \BaseNamedObjects.
 *
 * The naming is the point of the item rather than a convenience: two
 * unrelated processes agreeing on an object by name is what the namespace is
 * for. `name` is the leaf, not a path.
 *
 * Returns 0 and stores the object (caller owns the reference), -EEXIST if the
 * name is taken, or another negative errno. */
/* An NT thread's waitable half - see OBJ_THREAD in object.h. Created
 * unsignalled for thread `tid`; thread_object_exited signals it for good
 * with the thread's 32-bit exit code (the first call wins). query returns
 * 1 if the thread has exited, 0 if it is still running, or -EINVAL if `obj`
 * is not a thread; *exit_code is only meaningful after an exit. */
object_t *thread_object_create(int tid);
void thread_object_exited(object_t *obj, uint32 exit_code);
int thread_object_query(object_t *obj, int *tid, uint32 *exit_code);
/* The CPU time (ticks) a thread had used at exit, kept on its object so the
 * times of a finished thread stay answerable. record: the exit paths. */
void thread_object_record_cpu(object_t *obj, uint64 cpu_ticks);
int  thread_object_cpu(object_t *obj, uint64 *cpu_ticks);

/* An NT process's waitable half - see OBJ_PROCESS in object.h. Created
 * unsignalled for process `pid` (its group leader's pid); exited signals it
 * for good with the 32-bit exit code and the process's CPU time (the first
 * call wins); query returns 1 once exited, 0 while running, -EINVAL when
 * obj is not a process. */
object_t *process_object_create(int pid);
void process_object_exited(object_t *obj, uint32 exit_code, uint64 cpu_ticks);
int process_object_query(object_t *obj, int *pid, uint32 *exit_code,
                         uint64 *cpu_ticks);

int dispatch_create_named(const char *name, object_t *obj);

/* Thread `pid` has died: every mutant it still owns is released and marked
 * ABANDONED, and its waiters are woken. The next take of such a mutant
 * returns 1 from ob_wait rather than 0 - NT's WAIT_ABANDONED - and clears
 * the mark. Idempotent: called from every exit path (proc_nt_thread_exit,
 * kthread_exit), some of which run twice for one thread. */
void dispatch_owner_exited(int pid);

/* Wait on up to DISPATCH_WAIT_MAX dispatcher objects at once -
 * NtWaitForMultipleObjects. With `wait_all` zero, returns the index of the
 * object taken (the LOWEST ready one); with it set, takes every object in
 * one indivisible step once all are ready, never some of them first, and
 * returns 0. Either way DISPATCH_WAIT_ABANDONED + i instead when object i
 * was an abandoned mutant (the first such, for wait-all).
 *
 * -EINVAL when any object is not a dispatcher object or n is out of range,
 * DISPATCH_WAIT_DUPLICATE for the same object twice in a wait-all,
 * -ETIMEDOUT at the deadline (absolute ticks, 0 for none), -EINTR on a
 * signal. */
#define DISPATCH_WAIT_MAX        64          /* MAXIMUM_WAIT_OBJECTS */
#define DISPATCH_WAIT_ABANDONED  0x80        /* + index */
#define DISPATCH_WAIT_DUPLICATE  (-33)
/* An ALERTABLE wait (alertable non-zero) also ends - taking nothing - when
 * the calling thread has a user APC queued: DISPATCH_WAIT_APC. */
#define DISPATCH_WAIT_APC        (-1000)
int dispatch_wait_multiple(object_t **objs, int n, int wait_all,
                           int alertable, uint64 deadline);

/* Look up a named dispatcher object. Returns it with a reference taken, or
 * NULL. `name` is the leaf under \BaseNamedObjects. */
object_t *dispatch_open_named(const char *name);

/* Create the standard directories this item lists as missing:
 * \BaseNamedObjects, \KernelObjects, \ObjectTypes and \Sessions, and publish
 * the registered object types into the third. After ns_init(). */
void dispatch_init(void);

/* Exercise all three types, the naming, and a wait that really blocks.
 * Returns the number of failures. */
int dispatch_selftest(void);

#endif
