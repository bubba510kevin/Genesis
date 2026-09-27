#ifndef GENESIS_BSD_COMPAT_SYS_PROC_H
#define GENESIS_BSD_COMPAT_SYS_PROC_H

/* Just enough of a thread for uma_core.c.
 *
 * It uses curthread for two things: as a sleep/wakeup channel identity, and
 * to hold td_domain (a NUMA hint). With one domain and Genesis's own
 * scheduler underneath, the pointer only has to be a stable per-CPU token,
 * so it is the per-CPU block. That is honest: nothing here dereferences it
 * as a FreeBSD thread. */

#include <sys/types.h>
#include <sys/smp.h>
#include <sys/resource.h>
#include "ksmp.h"    /* struct cpu_local, smp_this_cpu, SMP_MAX_CPUS */

/* A process, as much of one as the vendored network code reads.
 *
 * p_fibnum is the only field with a real consumer: it selects which routing
 * table this process's sockets use, and net/route/route_tables.c reads it
 * through curthread->td_proc. One fib here, so it is always 0 - but it has to
 * BE somewhere, because setfib(2)'s code path is compiled whether or not the
 * syscall is reachable. */
struct proc {
    int  p_fibnum;
    pid_t p_pid;
};

/* <sys/types.h> already forward-declares `struct thread`, so this is the
 * definition of that same tag rather than a second one. */
struct thread {
    int td_dummy;

    /* The thread's credential. Genesis has no credentials, so this is
     * always NULL and every consumer of it (getcredhostuuid, the jail
     * checks in sys/jail.h) is written to accept that - which is the same
     * shape as a kernel thread upstream, whose td_ucred is the unrestricted
     * one. */
    struct ucred *td_ucred;

    /* The process this thread belongs to. All of them share one here. */
    struct proc *td_proc;

    int td_tid;
    int td_domain;      /* NUMA hint; one domain, so always 0 */

    /* Resource usage. kern/uipc_socket.c charges message counts to
     * td_ru.ru_msgsnd / ru_msgrcv on every send and receive. Nothing reads it
     * back - there is no getrusage(2) - but the field has to exist for the
     * accounting to compile, and having it real means it is countable the day
     * something does read it. The struct is the VENDORED one from
     * <sys/resource.h>. */
    struct rusage td_ru;

    /* The resource limits this thread is charged against. NULL, and every
     * reader is written for that: lim_cur() in <sys/resourcevar.h> returns
     * RLIM_INFINITY when there is no limit structure, which is the correct
     * answer for a kernel with no resource accounting. The socket layer reads
     * RLIMIT_SBSIZE through it when sizing a socket buffer. */
    struct plimit *td_limit;
};

/* curthread used to be `(struct thread *)smp_this_cpu()` - the per-CPU block
 * reinterpreted as a thread, with a comment saying nothing dereferenced it as
 * a FreeBSD thread.
 *
 * That stopped being true and the failure would have been silent. struct
 * cpu_local's first two fields are kernel_rsp and user_rsp (their offsets are
 * hardcoded in the syscall entry assembly), so under the old definition
 * `curthread->td_ucred` READ THE PARKED USER STACK POINTER AND USED IT AS A
 * CREDENTIAL POINTER. Nothing dereferenced it while the only reader was UMA's
 * NUMA hint; netinet/in.c's privilege checks and route_tables.c's fib lookup
 * both do.
 *
 * So there is a real per-CPU thread array now. It keeps the property the old
 * definition was chosen for - curthread is a distinct, stable token per CPU,
 * which is what the mutex owner field needs - and adds the one the old one
 * silently did not have: the fields mean what they say. Defined in
 * kernel/bsd/netglue.c. */
extern struct thread genesis_threads[SMP_MAX_CPUS];
extern struct proc   genesis_proc0;

#define curthread   (&genesis_threads[smp_this_cpu()->index])
#define curproc     (&genesis_proc0)

#define critical_enter()   genesis_critical_enter()
#define critical_exit()    genesis_critical_exit()

void genesis_critical_enter(void);
void genesis_critical_exit(void);

/* thread_lock/thread_unlock protect a thread's scheduler state. Genesis does
 * not migrate kernel threads between CPUs (Part 10 is mechanism only), so
 * there is no scheduler state here for another CPU to change under us. */
#define thread_lock(td)      do { } while (0)
#define thread_unlock(td)    do { } while (0)

/* Per-CPU field access. UMA reads PCPU_GET(domain) to pick a NUMA domain,
 * and there is one. */
#define PCPU_GET(field)      genesis_pcpu_get_##field()
static __inline int genesis_pcpu_get_domain(void) { return 0; }
static __inline int genesis_pcpu_get_cpuid(void)  { return (int)curcpu; }

/* thread0 - upstream's first thread, whose credential is the unrestricted
 * one. The TCP syncache connects a new pcb with thread0's credential.
 * Defined in kernel/bsd/netglue.c. */
extern struct thread thread0;

/* "May cred a see objects owned by cred b" - there is one credential in the
 * network stack (see net_absences.c), so the answer is always yes. */
struct ucred;
static __inline int cr_cansee(struct ucred *a, struct ucred *b) {
    (void)a;
    (void)b;
    return 0;
}

#define PROC_LOCK(p)     do { } while (0)
#define PROC_UNLOCK(p)   do { } while (0)

#endif
