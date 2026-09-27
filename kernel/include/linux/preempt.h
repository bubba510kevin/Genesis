#ifndef LINUX_PREEMPT_H
#define LINUX_PREEMPT_H

/* <linux/preempt.h> and <linux/irqflags.h>.
 *
 * Preemption, in this kernel, only ever happens on the way back to ring 3 -
 * kernel code is never preempted by the scheduler (see ksmp.h), and a
 * driver's kernel thread keeps its CPU until it blocks or yields. What
 * preempt_disable DOES still have to guarantee is that the thread is not
 * MOVED to another CPU between reading smp_processor_id() and using per-CPU
 * data - and a thread that neither blocks nor yields is not moved. So the
 * count is kept (preempt_count() answers truthfully, and a sleep inside a
 * preempt-disabled region is reported), but it has nothing to switch off.
 *
 * local_irq_* are the real thing: interrupts on this CPU. */
int  linux_preempt_count(void);
void linux_preempt_disable(void);
void linux_preempt_enable(void);

#define preempt_disable()          linux_preempt_disable()
#define preempt_enable()           linux_preempt_enable()
#define preempt_enable_no_resched() linux_preempt_enable()
#define preempt_count()            linux_preempt_count()
#define preemptible()              (linux_preempt_count() == 0)
#define migrate_disable()          preempt_disable()
#define migrate_enable()           preempt_enable()

static inline unsigned long linux_local_save_flags(void) {
    unsigned long f;

    __asm__ __volatile__("pushfq\n\tpopq %0" : "=r"(f) : : "memory");
    return f;
}

#define local_irq_disable() __asm__ __volatile__("cli" ::: "memory")
#define local_irq_enable()  __asm__ __volatile__("sti" ::: "memory")
#define local_irq_save(flags)                                   \
    do {                                                        \
        (flags) = linux_local_save_flags();                     \
        __asm__ __volatile__("cli" ::: "memory");               \
    } while (0)
#define local_irq_restore(flags)                                \
    __asm__ __volatile__("pushq %0\n\tpopfq" : : "r"((unsigned long)(flags)) \
                         : "memory", "cc")
#define local_save_flags(flags) do { (flags) = linux_local_save_flags(); } while (0)
#define irqs_disabled()         ((linux_local_save_flags() & 0x200UL) == 0)
#define irqs_disabled_flags(f)  (((f) & 0x200UL) == 0)

/* in_interrupt(): inside a hardware interrupt handler on this CPU. */
int  linux_in_interrupt(void);
#define in_interrupt() linux_in_interrupt()
#define in_irq()       linux_in_interrupt()
#define in_task()      (!linux_in_interrupt())

#endif
