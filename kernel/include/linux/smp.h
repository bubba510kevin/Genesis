#ifndef LINUX_SMP_H
#define LINUX_SMP_H

#include "linux/types.h"
#include "linux/cpumask.h"
#include "linux/preempt.h"

/* <linux/smp.h> - which CPU, and running a function on others.
 *
 * The cross-CPU calls are Genesis's smp_call_on / smp_call_all (ksmp.h):
 * the function runs in the target CPU's IPI handler, with interrupts off and
 * WITHOUT the big kernel lock - which is Linux's own contract for these (no
 * sleeping, no locks but spinlocks). `wait` is honoured by always waiting:
 * returning before the function has run would let the caller free `info`
 * under it, and a caller that asked not to wait loses nothing but time. */
typedef void (*smp_call_func_t)(void *info);

int  linux_smp_processor_id(void);
#define smp_processor_id()     linux_smp_processor_id()
#define raw_smp_processor_id() linux_smp_processor_id()

/* get_cpu pins the caller to its CPU until put_cpu (preempt_disable); the
 * CPU it returns stays the caller's for that long. */
#define get_cpu()  ({ preempt_disable(); smp_processor_id(); })
#define put_cpu()  preempt_enable()

int  smp_call_function_single(int cpu, smp_call_func_t func, void *info,
                              int wait);
/* Every online CPU EXCEPT the caller's. */
void smp_call_function(smp_call_func_t func, void *info, int wait);
void smp_call_function_many(const struct cpumask *mask, smp_call_func_t func,
                            void *info, int wait);
/* Every online CPU INCLUDING the caller's. */
void on_each_cpu(smp_call_func_t func, void *info, int wait);
void on_each_cpu_mask(const struct cpumask *mask, smp_call_func_t func,
                      void *info, int wait);
/* Run on some online CPU in `mask` - the caller's if it is in it. */
int  smp_call_function_any(const struct cpumask *mask, smp_call_func_t func,
                           void *info, int wait);

/* Rendezvous: ask each CPU in `cond`'s yes-set to run `func`. */
void on_each_cpu_cond(int (*cond)(int cpu, void *info), smp_call_func_t func,
                      void *info, int wait);

#endif
