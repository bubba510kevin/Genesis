#ifndef LINUX_WORKQUEUE_H
#define LINUX_WORKQUEUE_H

#include "linux/types.h"

/* <linux/workqueue.h> - deferred work, run by kernel threads.
 *
 * Every workqueue has one worker thread per CPU, bound to it, created when
 * the queue is. queue_work puts an item on the CALLER's CPU's worker (Linux's
 * locality default); queue_work_on names the CPU. A work item is on at most
 * one queue at a time - queueing a pending item again is a no-op returning
 * false, which is Linux's rule and what makes "kick the worker" from an
 * interrupt handler idempotent.
 *
 * system_wq (and its aliases) is created at boot. Delayed work runs its
 * item on the target queue once the delay has passed. */
struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
    work_func_t          func;
    struct work_struct  *next;        /* on a worker's list                  */
    volatile int         pending;     /* queued and not yet started          */
    volatile int         running;     /* the function is executing           */
    int                  cpu;         /* where it was last queued            */
    void                *wq;          /* the workqueue it was last queued to */
};

struct delayed_work {
    struct work_struct   work;
    unsigned long        expires;     /* jiffies                             */
    int                  armed;
    int                  cpu;
    struct delayed_work *next_timer;
};

struct workqueue_struct;

extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_highpri_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_unbound_wq;

#define WORK_CPU_UNBOUND (-1)
#define WQ_UNBOUND       0x0002
#define WQ_HIGHPRI       0x0010
#define WQ_MEM_RECLAIM   0x0008
#define WQ_FREEZABLE     0x0004

#define INIT_WORK(w, f)                                               \
    do { (w)->func = (f); (w)->next = 0; (w)->pending = 0;            \
         (w)->running = 0; (w)->cpu = -1; (w)->wq = 0; } while (0)
#define INIT_DELAYED_WORK(dw, f)                                      \
    do { INIT_WORK(&(dw)->work, (f)); (dw)->expires = 0;             \
         (dw)->armed = 0; (dw)->cpu = -1; (dw)->next_timer = 0; } while (0)
#define DECLARE_WORK(n, f) \
    struct work_struct n = { (f), 0, 0, 0, -1, 0 }
#define to_delayed_work(w) \
    ((struct delayed_work *)((char *)(w) - __builtin_offsetof(struct delayed_work, work)))

struct workqueue_struct *linux_alloc_workqueue(const char *name,
                                               unsigned int flags,
                                               int max_active);
#define alloc_workqueue(fmt, flags, max_active, ...) \
    linux_alloc_workqueue((fmt), (flags), (max_active))
#define create_workqueue(name)        linux_alloc_workqueue((name), 0, 1)
#define create_singlethread_workqueue(name) \
    linux_alloc_workqueue((name), WQ_UNBOUND, 1)
#define alloc_ordered_workqueue(fmt, flags, ...) \
    linux_alloc_workqueue((fmt), (flags) | WQ_UNBOUND, 1)
void destroy_workqueue(struct workqueue_struct *wq);

int  queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work);
int  queue_work(struct workqueue_struct *wq, struct work_struct *work);
int  schedule_work(struct work_struct *work);
int  schedule_work_on(int cpu, struct work_struct *work);
int  queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
                           struct delayed_work *dw, unsigned long delay);
int  queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
                        unsigned long delay);
int  schedule_delayed_work(struct delayed_work *dw, unsigned long delay);
int  mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
                      unsigned long delay);

int  flush_work(struct work_struct *work);
int  flush_delayed_work(struct delayed_work *dw);
void flush_workqueue(struct workqueue_struct *wq);
void flush_scheduled_work(void);
int  cancel_work_sync(struct work_struct *work);
int  cancel_delayed_work(struct delayed_work *dw);
int  cancel_delayed_work_sync(struct delayed_work *dw);
int  work_pending(struct work_struct *work);
#define delayed_work_pending(dw) work_pending(&(dw)->work)

#endif
