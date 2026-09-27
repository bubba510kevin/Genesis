#ifndef LINUX_CPUMASK_H
#define LINUX_CPUMASK_H

#include "linux/types.h"

/* <linux/cpumask.h> - sets of CPUs.
 *
 * NR_CPUS is this kernel's SMP_MAX_CPUS, so a mask is one unsigned long and
 * every operation is a bit operation on it. The struct wrapper is Linux's
 * own shape, kept so driver code that takes `const struct cpumask *` and
 * passes `cpu_online_mask` compiles unchanged. */
#define NR_CPUS 8
#define nr_cpu_ids NR_CPUS

struct cpumask {
    unsigned long bits[1];
};
typedef struct cpumask cpumask_t;
typedef struct cpumask cpumask_var_t[1];

/* Live views, maintained by the kernel as CPUs come online. */
extern const struct cpumask *const cpu_online_mask;
extern const struct cpumask *const cpu_possible_mask;
extern const struct cpumask *const cpu_present_mask;
extern const struct cpumask *const cpu_active_mask;

static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *m) {
    __atomic_fetch_or(&m->bits[0], 1UL << cpu, __ATOMIC_RELAXED);
}
static inline void cpumask_clear_cpu(unsigned int cpu, struct cpumask *m) {
    __atomic_fetch_and(&m->bits[0], ~(1UL << cpu), __ATOMIC_RELAXED);
}
static inline int cpumask_test_cpu(int cpu, const struct cpumask *m) {
    return cpu >= 0 && cpu < NR_CPUS && ((m->bits[0] >> cpu) & 1UL) != 0;
}
static inline int cpumask_test_and_set_cpu(int cpu, struct cpumask *m) {
    return (__atomic_fetch_or(&m->bits[0], 1UL << cpu, __ATOMIC_ACQ_REL) >> cpu) & 1UL;
}
static inline void cpumask_clear(struct cpumask *m) { m->bits[0] = 0; }
static inline void cpumask_setall(struct cpumask *m) {
    m->bits[0] = (NR_CPUS >= 64) ? ~0UL : ((1UL << NR_CPUS) - 1);
}
static inline void cpumask_copy(struct cpumask *d, const struct cpumask *s) {
    d->bits[0] = s->bits[0];
}
static inline int cpumask_empty(const struct cpumask *m) { return m->bits[0] == 0; }
static inline int cpumask_equal(const struct cpumask *a, const struct cpumask *b) {
    return a->bits[0] == b->bits[0];
}
static inline unsigned int cpumask_weight(const struct cpumask *m) {
    /* By hand: __builtin_popcountl is a libgcc call without -mpopcnt, and
     * the kernel links no libgcc. */
    unsigned long v = m->bits[0];
    unsigned int n = 0;

    while (v != 0) {
        v &= v - 1;
        n++;
    }
    return n;
}
static inline int cpumask_and(struct cpumask *d, const struct cpumask *a,
                              const struct cpumask *b) {
    d->bits[0] = a->bits[0] & b->bits[0];
    return d->bits[0] != 0;
}
static inline void cpumask_or(struct cpumask *d, const struct cpumask *a,
                              const struct cpumask *b) {
    d->bits[0] = a->bits[0] | b->bits[0];
}
static inline int cpumask_andnot(struct cpumask *d, const struct cpumask *a,
                                 const struct cpumask *b) {
    d->bits[0] = a->bits[0] & ~b->bits[0];
    return d->bits[0] != 0;
}
static inline int cpumask_subset(const struct cpumask *a, const struct cpumask *b) {
    return (a->bits[0] & ~b->bits[0]) == 0;
}
static inline int cpumask_intersects(const struct cpumask *a, const struct cpumask *b) {
    return (a->bits[0] & b->bits[0]) != 0;
}
/* First set CPU at or after n+1 (cpumask_next) / at or after 0 (first);
 * nr_cpu_ids when there is none - Linux's "not found" value, which is what
 * for_each_cpu's loop condition tests. */
static inline unsigned int cpumask_next(int n, const struct cpumask *m) {
    unsigned int i;

    for (i = (unsigned int)(n + 1); i < NR_CPUS; i++) {
        if ((m->bits[0] >> i) & 1UL) {
            return i;
        }
    }
    return nr_cpu_ids;
}
static inline unsigned int cpumask_first(const struct cpumask *m) {
    return cpumask_next(-1, m);
}
static inline unsigned int cpumask_any(const struct cpumask *m) {
    return cpumask_first(m);
}
static inline unsigned int cpumask_first_and(const struct cpumask *a,
                                             const struct cpumask *b) {
    struct cpumask t;

    t.bits[0] = a->bits[0] & b->bits[0];
    return cpumask_first(&t);
}

/* A mask with exactly `cpu` set, as a pointer to constant storage. */
const struct cpumask *linux_cpumask_of(unsigned int cpu);
#define cpumask_of(cpu) linux_cpumask_of(cpu)

static inline int zalloc_cpumask_var(cpumask_var_t *m, unsigned int flags) {
    (void)flags;
    (*m)->bits[0] = 0;
    return 1;
}
static inline int alloc_cpumask_var(cpumask_var_t *m, unsigned int flags) {
    return zalloc_cpumask_var(m, flags);
}
static inline void free_cpumask_var(cpumask_var_t m) { (void)m; }

#define cpumask_bits(m) ((m)->bits)

#define for_each_cpu(cpu, mask)                                   \
    for ((cpu) = (int)cpumask_first(mask); (cpu) < (int)nr_cpu_ids; \
         (cpu) = (int)cpumask_next((cpu), (mask)))

#define for_each_online_cpu(cpu)   for_each_cpu((cpu), cpu_online_mask)
#define for_each_possible_cpu(cpu) for_each_cpu((cpu), cpu_possible_mask)
#define for_each_present_cpu(cpu)  for_each_cpu((cpu), cpu_present_mask)

#define cpu_online(cpu)   cpumask_test_cpu((cpu), cpu_online_mask)
#define cpu_possible(cpu) cpumask_test_cpu((cpu), cpu_possible_mask)
#define cpu_present(cpu)  cpumask_test_cpu((cpu), cpu_present_mask)
#define cpu_active(cpu)   cpumask_test_cpu((cpu), cpu_active_mask)

#define num_online_cpus()   cpumask_weight(cpu_online_mask)
#define num_possible_cpus() cpumask_weight(cpu_possible_mask)
#define num_present_cpus()  cpumask_weight(cpu_present_mask)
#define num_active_cpus()   cpumask_weight(cpu_active_mask)

#endif
