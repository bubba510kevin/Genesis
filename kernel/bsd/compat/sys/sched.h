#ifndef GENESIS_BSD_COMPAT_SYS_SCHED_H
#define GENESIS_BSD_COMPAT_SYS_SCHED_H

/* <sys/sched.h> - binding and pinning, for FreeBSD code.
 *
 * sched_bind(td, cpu) moves the CALLING thread onto `cpu` and keeps it there
 * until sched_unbind - really: the thread's affinity is narrowed and it
 * switches away and resumes on that CPU before sched_bind returns (see
 * sched_migrate_self). UMA's per-CPU cache drain relies on it, and so does a
 * driver that walks CPU_FOREACH doing per-CPU setup. `td` is always
 * curthread in upstream's use and is not consulted: FreeBSD's struct thread
 * here is per CPU, not per Genesis thread (see sys/proc.h), so the
 * scheduler's own notion of "the calling thread" is the one that moves.
 *
 * From a context that cannot block (an interrupt, or before every CPU is
 * scheduling) there is nowhere to go, and the bind is recorded but the
 * thread stays put. Under the big kernel lock that is still correct for
 * everything upstream binds for: no other CPU is in the kernel touching the
 * per-CPU state being drained.
 *
 * sched_pin / sched_unpin keep the thread on its CPU across a short window.
 * Kernel code is never preempted here, and a thread that neither blocks nor
 * yields is never moved, so pinning is a count for sched_is_pinned and has
 * nothing else to do. */

int  genesis_sched_bind(int cpu);
void genesis_sched_unbind(void);
int  genesis_sched_is_bound(void);
void genesis_sched_pin(void);
void genesis_sched_unpin(void);
int  genesis_sched_pinned(void);

#define sched_bind(td, c)   ((void)(td), (void)genesis_sched_bind((int)(c)))
#define sched_unbind(td)    ((void)(td), genesis_sched_unbind())
#define sched_is_bound(td)  ((void)(td), genesis_sched_is_bound())
#define sched_pin()         genesis_sched_pin()
#define sched_unpin()       genesis_sched_unpin()
#define THREAD_CAN_MIGRATE(td) (!genesis_sched_pinned())

#endif
