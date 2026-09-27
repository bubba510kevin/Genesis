#ifndef LINUX_KTHREAD_H
#define LINUX_KTHREAD_H

#include "linux/types.h"
#include "linux/cpumask.h"

/* <linux/kthread.h> - kernel threads.
 *
 * A Linux kthread is a Genesis kernel thread (kernel/proc/kthread.c) with
 * the Linux lifecycle on top: created stopped-until-woken (kthread_create)
 * or running (kthread_run), optionally bound to a CPU before it first runs
 * (kthread_bind / kthread_create_on_cpu), and asked to stop by
 * kthread_stop, which the thread notices through kthread_should_stop() and
 * which waits for the thread's function to return its result.
 *
 * The struct task_struct is Genesis's; drivers only ever hold the pointer. */
struct task_struct;

struct task_struct *linux_kthread_create(int (*threadfn)(void *data),
                                         void *data, const char *name);
int  wake_up_process(struct task_struct *p);
void kthread_bind(struct task_struct *p, unsigned int cpu);
void kthread_bind_mask(struct task_struct *p, const struct cpumask *mask);
int  kthread_stop(struct task_struct *k);
int  kthread_should_stop(void);
int  kthread_park(struct task_struct *k);
struct task_struct *kthread_create_on_cpu(int (*threadfn)(void *data),
                                          void *data, unsigned int cpu,
                                          const char *namefmt);

/* The name argument is a printf format in Linux; drivers overwhelmingly
 * pass a literal, and that literal is used as-is. */
#define kthread_create(threadfn, data, namefmt, ...) \
    linux_kthread_create((threadfn), (data), (namefmt))

#define kthread_run(threadfn, data, namefmt, ...)                         \
    ({ struct task_struct *__k = kthread_create(threadfn, data, namefmt); \
       if (__k != (struct task_struct *)0) wake_up_process(__k);          \
       __k; })

struct task_struct *linux_current(void);
#define current linux_current()

int  set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *mask);
int  task_cpu(const struct task_struct *p);

/* The sleep protocol: set_current_state(TASK_INTERRUPTIBLE), test the
 * condition, schedule(); the waker calls wake_up_process. schedule() here
 * really SLEEPS when the state says so - it is not the kernel's yield, which
 * would leave a waiting thread spinning on its CPU (and, holding the big
 * kernel lock, on everybody else's). With TASK_RUNNING it yields. */
#define TASK_RUNNING         0
#define TASK_INTERRUPTIBLE   1
#define TASK_UNINTERRUPTIBLE 2
#define TASK_IDLE            (TASK_UNINTERRUPTIBLE)
#define MAX_SCHEDULE_TIMEOUT 0x7FFFFFFFFFFFFFFFL

void linux_set_current_state(long state);
void linux_schedule(void);
long linux_schedule_timeout(long timeout);
void linux_yield(void);
int  linux_cond_resched(void);
int  linux_signal_pending(struct task_struct *p);

#define set_current_state(s)   linux_set_current_state(s)
#define __set_current_state(s) linux_set_current_state(s)
#define schedule()             linux_schedule()
#define schedule_timeout(t)    linux_schedule_timeout(t)
#define schedule_timeout_interruptible(t)     (linux_set_current_state(TASK_INTERRUPTIBLE), linux_schedule_timeout(t))
#define schedule_timeout_uninterruptible(t)     (linux_set_current_state(TASK_UNINTERRUPTIBLE), linux_schedule_timeout(t))
#define yield()                linux_yield()
#define cond_resched()         linux_cond_resched()
#define signal_pending(p)      linux_signal_pending(p)

/* IS_ERR / PTR_ERR for the kthread_create return, if the driver has not
 * pulled in <linux/err.h>. kthread_create here returns NULL on failure, so
 * IS_ERR is false for every pointer it hands back that is non-NULL. */
#ifndef IS_ERR
#define IS_ERR(p)  ((unsigned long)(p) >= (unsigned long)-4095)
#define PTR_ERR(p) ((long)(p))
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#endif

#endif
