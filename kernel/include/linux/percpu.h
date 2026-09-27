#ifndef LINUX_PERCPU_H
#define LINUX_PERCPU_H

#include "linux/types.h"
#include "linux/smp.h"

/* <linux/percpu.h> - one copy of a variable per CPU, Linux's way.
 *
 * Every per-CPU object has a CPU-0 address, and CPU n's copy is at that
 * address plus __per_cpu_offset[n] - the same offset for every object, which
 * is what lets one pointer name "this object, on whichever CPU" and lets a
 * dynamically allocated object and a static one be used the same way.
 *
 *   static     DEFINE_PER_CPU puts the variable in the `lkpi_percpu`
 *              section. In the kernel image that section sits in .data and
 *              IS CPU 0's copy; kernel/driver/lkpi_smp.c allocates a block
 *              per other CPU, copies the section into it, and records the
 *              offsets. A loaded module's section is placed by kldload into
 *              the same area (lkpi_percpu_module_alloc), so its variables
 *              obey the same offsets.
 *   dynamic    alloc_percpu carves from a reserve inside that section, so a
 *              dynamic object's CPU-0 address is also in it and the same
 *              offsets apply. Zeroed on every CPU.
 *
 * `__percpu` is an annotation (sparse's address space) and nothing else. */
#define __percpu

extern unsigned long __per_cpu_offset[NR_CPUS];

#define DEFINE_PER_CPU(type, name) \
    __attribute__((section("lkpi_percpu"), aligned(16))) __typeof__(type) name
#define DEFINE_PER_CPU_ALIGNED(type, name) \
    __attribute__((section("lkpi_percpu"), aligned(64))) __typeof__(type) name
#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name) DEFINE_PER_CPU_ALIGNED(type, name)
#define DECLARE_PER_CPU(type, name) extern __typeof__(type) name

#define per_cpu_ptr(ptr, cpu) \
    ((__typeof__(ptr))((unsigned long)(ptr) + __per_cpu_offset[(cpu)]))
#define per_cpu(var, cpu)      (*per_cpu_ptr(&(var), (cpu)))
#define this_cpu_ptr(ptr)      per_cpu_ptr((ptr), smp_processor_id())
#define raw_cpu_ptr(ptr)       this_cpu_ptr(ptr)
#define get_cpu_ptr(ptr)       ({ preempt_disable(); this_cpu_ptr(ptr); })
#define put_cpu_ptr(ptr)       do { (void)(ptr); preempt_enable(); } while (0)
#define get_cpu_var(var)       (*({ preempt_disable(); this_cpu_ptr(&(var)); }))
#define put_cpu_var(var)       do { (void)&(var); preempt_enable(); } while (0)

/* The this_cpu_* operations: read-modify-write of THIS CPU's copy, safe
 * against an interrupt on the same CPU (interrupts are held off across it),
 * which is Linux's guarantee for them. */
#define __lkpi_this_cpu_op(var, op, val)                                \
    ({ unsigned long __f;                                               \
       __typeof__(var) *__p;                                            \
       __asm__ __volatile__("pushfq\n\tpopq %0\n\tcli" : "=r"(__f) : : "memory"); \
       __p = this_cpu_ptr(&(var));                                      \
       *__p op (val);                                                   \
       __asm__ __volatile__("pushq %0\n\tpopfq" : : "r"(__f) : "memory", "cc"); \
    })
#define this_cpu_read(var)       (*this_cpu_ptr(&(var)))
#define this_cpu_write(var, val) __lkpi_this_cpu_op(var, =, val)
#define this_cpu_add(var, val)   __lkpi_this_cpu_op(var, +=, val)
#define this_cpu_sub(var, val)   __lkpi_this_cpu_op(var, -=, val)
#define this_cpu_inc(var)        this_cpu_add(var, 1)
#define this_cpu_dec(var)        this_cpu_sub(var, 1)
#define this_cpu_or(var, val)    __lkpi_this_cpu_op(var, |=, val)
#define this_cpu_and(var, val)   __lkpi_this_cpu_op(var, &=, val)
#define __this_cpu_read(var)       this_cpu_read(var)
#define __this_cpu_write(var, val) this_cpu_write(var, val)
#define __this_cpu_add(var, val)   this_cpu_add(var, val)
#define __this_cpu_inc(var)        this_cpu_inc(var)
#define __this_cpu_dec(var)        this_cpu_dec(var)

void *linux_alloc_percpu(size_t size, size_t align);
void  linux_free_percpu(void *ptr);

#define alloc_percpu(type) \
    ((__typeof__(type) *)linux_alloc_percpu(sizeof(type), __alignof__(type)))
#define __alloc_percpu(size, align) linux_alloc_percpu((size), (align))
#define alloc_percpu_gfp(type, gfp) alloc_percpu(type)
#define free_percpu(ptr) linux_free_percpu((void *)(ptr))

/* For kldload: a loaded module's lkpi_percpu section. Returns the CPU-0
 * address to relocate the section's symbols against (0 if the reserve is
 * exhausted), and replicate copies its initial bytes onto every CPU once the
 * module's relocations are done. */
unsigned long lkpi_percpu_module_alloc(size_t size, size_t align);
void          lkpi_percpu_replicate(unsigned long cpu0_addr, size_t size);

#endif
