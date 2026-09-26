#ifndef GENESIS_BSD_COMPAT_SYS_SMP_H
#define GENESIS_BSD_COMPAT_SYS_SMP_H

/* Per-CPU identity and count, for uma_core.c's per-CPU caches.
 *
 * Genesis's own header is ksmp.h, not smp.h, for the same reason klock.h is
 * not sys/mutex.h: this file IS <sys/smp.h> for every vendored translation
 * unit, so a Genesis "smp.h" would resolve to this file including itself.
 *
 * Backed by Part 10's real SMP - mp_maxid and curcpu are not fictions here.
 * UMA indexes uz_cpu[] by curcpu and iterates 0..mp_maxid, so both have to
 * be true or the per-CPU cache silently belongs to the wrong core. */

#include "ksmp.h"          /* Genesis's kernel/include/smp.h */

/* HIGHEST valid CPU id, not the count - upstream loops `for (i = 0; i <=
 * mp_maxid; i++)`, so an off-by-one here walks off uz_cpu[]. */
#define mp_maxid     (SMP_MAX_CPUS - 1)
#define mp_ncpus     (smp_cpu_count())
#define curcpu       (smp_this_cpu()->index)

/* No CPU is absent - Genesis only ever counts CPUs that started. */
#define CPU_ABSENT(i)  (0)

#define CPU_FOREACH(i) for ((i) = 0; (i) <= mp_maxid; (i)++)

#define zpcpu_get_cpu(base, cpu)  (&(base)[(cpu)])
#define zpcpu_get(base)           (&(base)[curcpu])

/* UMA's per-CPU slabs are addressed as an OFFSET from a per-CPU base on
 * upstream, so that one pointer serves every CPU. Genesis has no per-CPU
 * segment base for this, so the "offset" IS the address and the two
 * conversions are the identity - correct for a single flat allocation
 * indexed by CPU, which is what pcpu_page_alloc produces here. */
#define zpcpu_base_to_offset(base)   ((void *)(base))
#define zpcpu_offset_to_base(off)    ((void *)(off))
#define zpcpu_sub_protected(p, v)    do { *(p) -= (v); } while (0)
#define zpcpu_add_protected(p, v)    do { *(p) += (v); } while (0)
#define zpcpu_replace_cpu(p, v, c)   (*(p))

#endif
