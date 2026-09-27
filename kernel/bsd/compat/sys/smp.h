#ifndef GENESIS_BSD_COMPAT_SYS_SMP_H
#define GENESIS_BSD_COMPAT_SYS_SMP_H

/* <sys/smp.h> - the machine's CPUs, for FreeBSD code.
 *
 * Backed by Genesis's real SMP (ksmp.h): every CPU runs threads, and
 * mp_ncpus, curcpu and CPU_ABSENT describe the machine that is actually
 * running.
 *
 * CPU_ABSENT has one subtlety. FreeBSD knows which CPUs exist before any
 * allocator runs; Genesis enumerates them later in boot than UMA initialises.
 * So until smp_init has counted them, every slot up to mp_maxid is treated
 * as present - which is what UMA's CPU_FOREACH-at-zone-creation needs, since
 * a slot it skipped then would have no cache later - and from then on only
 * the CPUs that came up are. uz_cpu[] and zpcpu arrays are sized to mp_maxid
 * either way. */

#include "ksmp.h"          /* Genesis's kernel/include/ksmp.h */

#define mp_maxid     (SMP_MAX_CPUS - 1)
#define mp_ncpus     (smp_cpu_count())
#define curcpu       (smp_this_cpu()->index)
#define MAXCPU       SMP_MAX_CPUS

int genesis_cpu_absent(int cpu);
#define CPU_ABSENT(i)  genesis_cpu_absent((int)(i))

/* upstream's own spelling, including the else that keeps a following else
 * from attaching to the hidden if */
#define CPU_FOREACH(i)                                   \
    for ((i) = 0; (i) <= mp_maxid; (i)++)                \
        if (CPU_ABSENT((i))) {} else

/* Set once every CPU is scheduling (smp_start_scheduling). */
int genesis_smp_started(void);
#define smp_started  (genesis_smp_started())
#define smp_cpus     (smp_cpu_count())

/* all_cpus: the set of CPUs that are up. A cpuset_t, as upstream. */
struct _cpuset;
struct _cpuset *genesis_all_cpus(void);
#define all_cpus (*genesis_all_cpus())

/* smp_rendezvous: setup on every CPU, then (once all have finished setup)
 * action on every CPU, then (once all have finished action) teardown on
 * every CPU. NULL skips a phase; smp_no_rendezvous_barrier as setup or
 * teardown skips the barrier too. All CPUs run concurrently, in IPI context,
 * without the big kernel lock - upstream's contract: spin locks only. */
void smp_no_rendezvous_barrier(void *);
void smp_rendezvous(void (*setup)(void *), void (*action)(void *),
                    void (*teardown)(void *), void *arg);
void smp_rendezvous_cpus(struct _cpuset map, void (*setup)(void *),
                         void (*action)(void *), void (*teardown)(void *),
                         void *arg);

#define zpcpu_get_cpu(base, cpu)  (&(base)[(cpu)])
#define zpcpu_get(base)           (&(base)[curcpu])

/* zpcpu_base_to_offset / offset_to_base: the conversion between a pcpu
 * zone's base pointer and the offset form FreeBSD stores in counter_u64_t.
 * Here the two are the same pointer (the per-CPU copies are an ordinary
 * array, not replicated pages at a fixed stride), so both are identity. */
#define zpcpu_base_to_offset(base)   ((void *)(base))
#define zpcpu_offset_to_base(off)    ((void *)(off))
#define zpcpu_sub_protected(p, v)    do { *(p) -= (v); } while (0)
#define zpcpu_add_protected(p, v)    do { *(p) += (v); } while (0)
#define zpcpu_replace_cpu(p, v, c)   (*(p))

#endif
