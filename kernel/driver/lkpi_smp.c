/* LinuxKPI's multiprocessor surface: CPU identity and masks, cross-CPU
 * calls, preemption and interrupt state, per-CPU variables, kernel threads
 * with the Linux lifecycle, completions and workqueues. See the headers in
 * kernel/include/linux/ for each contract; this file is how each is built on
 * Genesis.
 *
 * Everything here that touches shared state - the task table, workqueue
 * lists, completion counts - runs under the big kernel lock, like all kernel
 * code (ksmp.h). The exceptions are the cross-CPU call trampolines, which run
 * in IPI context without it and touch only their own argument.
 *
 * The Genesis headers are included, and NOT the driver-side <linux/kthread.h>
 * whose `current` and `schedule()` macros would rename things this file means
 * literally; its prototypes are restated where needed. */

#include "bkl.h"
#include "kheap.h"
#include "kprintf.h"
#include "ksmp.h"
#include "kthread.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "timer.h"
#include "typesk.h"
#include "waitq.h"

#include "linux/cpumask.h"
#include "linux/percpu.h"
#include "linux/completion.h"
#include "linux/workqueue.h"

typedef char lkpi_nr_cpus_matches[(NR_CPUS == SMP_MAX_CPUS) ? 1 : -1];
typedef char lkpi_wq_fits[(sizeof(((struct completion *)0)->wq_opaque) >=
                           sizeof(wait_queue_t)) ? 1 : -1];

/* ============================================================================
 * CPUs
 * ========================================================================== */

static struct cpumask online_mask, possible_mask;
const struct cpumask *const cpu_online_mask   = &online_mask;
const struct cpumask *const cpu_possible_mask = &possible_mask;
const struct cpumask *const cpu_present_mask  = &online_mask;
const struct cpumask *const cpu_active_mask   = &online_mask;

static struct cpumask single_masks[NR_CPUS];

const struct cpumask *linux_cpumask_of(unsigned int cpu) {
    return cpu < NR_CPUS ? &single_masks[cpu] : &single_masks[0];
}

int linux_smp_processor_id(void) {
    return smp_cpu_index();
}

static volatile int preempt_counts[SMP_MAX_CPUS];

int linux_preempt_count(void) {
    return preempt_counts[smp_cpu_index()];
}

void linux_preempt_disable(void) {
    preempt_counts[smp_cpu_index()]++;
}

void linux_preempt_enable(void) {
    int c = smp_cpu_index();

    if (preempt_counts[c] > 0) {
        preempt_counts[c]--;
    }
}

int linux_in_interrupt(void) {
    return smp_this_cpu()->irq_depth > 0;
}

/* --- cross-CPU calls ---------------------------------------------------------
 *
 * A Linux smp_call_func_t is an ordinary SysV function, the same ABI as
 * smp_call_on's, so it is handed straight through. */
int smp_call_function_single(int cpu, void (*func)(void *), void *info, int wait) {
    (void)wait;
    if (cpu < 0 || cpu >= NR_CPUS || !cpumask_test_cpu(cpu, &online_mask)) {
        return -6;                                      /* -ENXIO */
    }
    return smp_call_on(cpu, func, info) == 0 ? 0 : -6;
}

void smp_call_function_many(const struct cpumask *mask, void (*func)(void *),
                            void *info, int wait) {
    int cpu, me = smp_cpu_index();

    (void)wait;
    for_each_cpu(cpu, mask) {
        if (cpu != me && cpumask_test_cpu(cpu, &online_mask)) {
            (void)smp_call_on(cpu, func, info);
        }
    }
}

void smp_call_function(void (*func)(void *), void *info, int wait) {
    smp_call_function_many(&online_mask, func, info, wait);
}

void on_each_cpu(void (*func)(void *), void *info, int wait) {
    (void)wait;
    smp_call_all(func, info);
}

void on_each_cpu_mask(const struct cpumask *mask, void (*func)(void *),
                      void *info, int wait) {
    int me = smp_cpu_index();

    smp_call_function_many(mask, func, info, wait);
    if (cpumask_test_cpu(me, mask)) {
        (void)smp_call_on(me, func, info);
    }
}

int smp_call_function_any(const struct cpumask *mask, void (*func)(void *),
                          void *info, int wait) {
    int me = smp_cpu_index();
    unsigned int cpu;

    if (cpumask_test_cpu(me, mask) && cpumask_test_cpu(me, &online_mask)) {
        return smp_call_function_single(me, func, info, wait);
    }
    cpu = cpumask_first_and(mask, &online_mask);
    if (cpu >= nr_cpu_ids) {
        return -6;
    }
    return smp_call_function_single((int)cpu, func, info, wait);
}

void on_each_cpu_cond(int (*cond)(int cpu, void *info), void (*func)(void *),
                      void *info, int wait) {
    struct cpumask m;
    int cpu;

    cpumask_clear(&m);
    for_each_online_cpu(cpu) {
        if (cond == NULL || cond(cpu, info)) {
            cpumask_set_cpu((unsigned int)cpu, &m);
        }
    }
    on_each_cpu_mask(&m, func, info, wait);
}

/* ============================================================================
 * per-CPU variables
 * ========================================================================== */

extern char __start_lkpi_percpu[];
extern char __stop_lkpi_percpu[];

unsigned long __per_cpu_offset[NR_CPUS];

/* The reserve alloc_percpu and loaded modules carve from - IN the section,
 * so everything carved has a CPU-0 address the offsets apply to. 32KB in
 * 16-byte granules, tracked by a bitmap and a per-allocation length. */
#define PCPU_RESERVE   (32 * 1024)
#define PCPU_GRANULE   16
#define PCPU_GRANULES  (PCPU_RESERVE / PCPU_GRANULE)

__attribute__((section("lkpi_percpu"), aligned(64)))
static char pcpu_reserve[PCPU_RESERVE];

static uint8  pcpu_used[PCPU_GRANULES / 8];
static uint16 pcpu_len[PCPU_GRANULES];       /* granules, at the first one */
static char  *pcpu_copies[NR_CPUS];          /* CPU n's block (n > 0)      */

static int granule_used(int g) {
    return (pcpu_used[g / 8] >> (g % 8)) & 1;
}

static void granule_mark(int g, int used) {
    if (used) {
        pcpu_used[g / 8] |= (uint8)(1u << (g % 8));
    } else {
        pcpu_used[g / 8] &= (uint8)~(1u << (g % 8));
    }
}

/* Zero (or copy CPU 0's bytes into) the same range on every other CPU. */
static void pcpu_sync_range(unsigned long cpu0_addr, size_t size, int zero) {
    int cpu;

    for (cpu = 1; cpu < NR_CPUS; cpu++) {
        char *dst;
        size_t b;

        if (pcpu_copies[cpu] == NULL) {
            continue;
        }
        dst = (char *)(cpu0_addr + __per_cpu_offset[cpu]);
        for (b = 0; b < size; b++) {
            dst[b] = zero ? 0 : ((const char *)cpu0_addr)[b];
        }
    }
}

static unsigned long pcpu_carve(size_t size, size_t align) {
    int need, g, k, step;

    if (size == 0) {
        size = 1;
    }
    if (align < PCPU_GRANULE) {
        align = PCPU_GRANULE;
    }
    need = (int)((size + PCPU_GRANULE - 1) / PCPU_GRANULE);
    step = (int)(align / PCPU_GRANULE);
    for (g = 0; g + need <= PCPU_GRANULES; g += step) {
        for (k = 0; k < need && !granule_used(g + k); k++) {
        }
        if (k == need) {
            for (k = 0; k < need; k++) {
                granule_mark(g + k, 1);
            }
            pcpu_len[g] = (uint16)need;
            return (unsigned long)&pcpu_reserve[g * PCPU_GRANULE];
        }
    }
    return 0;
}

void *linux_alloc_percpu(size_t size, size_t align) {
    unsigned long a = pcpu_carve(size, align);
    size_t b;

    if (a == 0) {
        kprintf_c(0x0C, "lkpi: alloc_percpu(%d) - reserve exhausted\n", (int)size);
        return NULL;
    }
    for (b = 0; b < size; b++) {
        ((char *)a)[b] = 0;
    }
    pcpu_sync_range(a, size, 1);
    return (void *)a;
}

void linux_free_percpu(void *ptr) {
    long g;
    int k;

    if (ptr == NULL) {
        return;
    }
    g = ((char *)ptr - pcpu_reserve) / PCPU_GRANULE;
    if (g < 0 || g >= PCPU_GRANULES || pcpu_len[g] == 0) {
        kprintf_c(0x0C, "lkpi: free_percpu of a pointer it did not allocate\n");
        return;
    }
    for (k = 0; k < pcpu_len[g]; k++) {
        granule_mark((int)g + k, 0);
    }
    pcpu_len[g] = 0;
}

unsigned long lkpi_percpu_module_alloc(uint64 size, uint64 align) {
    return pcpu_carve((size_t)size, (size_t)align);
}

void lkpi_percpu_replicate(unsigned long cpu0_addr, uint64 size) {
    pcpu_sync_range(cpu0_addr, (size_t)size, 0);
}

/* One block per CPU beyond 0, holding a copy of the whole section; the
 * offset from the section to the block is that CPU's __per_cpu_offset. */
static void percpu_setup(void) {
    size_t len = (size_t)(__stop_lkpi_percpu - __start_lkpi_percpu);
    int cpu;

    __per_cpu_offset[0] = 0;
    for (cpu = 1; cpu < NR_CPUS; cpu++) {
        char *blk;
        size_t b;

        if (!cpumask_test_cpu(cpu, &online_mask)) {
            __per_cpu_offset[cpu] = 0;       /* never used: not a CPU */
            continue;
        }
        blk = (char *)kmalloc_a(len);
        if (blk == NULL) {
            kprintf_c(0x0C, "lkpi: no memory for cpu%d's per-CPU area\n", cpu);
            __per_cpu_offset[cpu] = 0;
            continue;
        }
        for (b = 0; b < len; b++) {
            blk[b] = __start_lkpi_percpu[b];
        }
        pcpu_copies[cpu] = blk;
        __per_cpu_offset[cpu] = (unsigned long)blk -
                                (unsigned long)__start_lkpi_percpu;
    }
}

/* ============================================================================
 * tasks: a Linux view of every thread
 * ========================================================================== */

struct task_struct {
    process_t        *p;
    int               pid;
    volatile long     state;          /* TASK_RUNNING / INTERRUPTIBLE / ...  */
    volatile int      woken;          /* a wake_up_process since set_state   */

    /* A LinuxKPI kthread's lifecycle; unused for any other thread. */
    int             (*threadfn)(void *data);
    void             *data;
    volatile int      started;
    volatile int      should_stop;
    volatile int      exited;
    int               result;
    wait_queue_t      gate;           /* waits for wake_up_process          */
    wait_queue_t      exitq;          /* kthread_stop waits here            */
};

static struct task_struct tasks[MAX_PROCESSES];

static struct task_struct *task_of(process_t *p) {
    struct task_struct *t;

    if (p == NULL) {
        return NULL;
    }
    t = &tasks[proc_index(p)];
    if (t->p != p || t->pid != p->pid) {
        /* A slot seen for the first time, or recycled for a new thread:
         * reset the Linux view to "running, nothing pending". */
        t->p = p;
        t->pid = p->pid;
        t->state = 0;
        t->woken = 0;
        t->threadfn = NULL;
        t->data = NULL;
        t->started = 1;
        t->should_stop = 0;
        t->exited = 0;
        t->result = 0;
        waitq_init(&t->gate);
        waitq_init(&t->exitq);
    }
    return t;
}

struct task_struct *linux_current(void) {
    return task_of(proc_current());
}

void linux_set_current_state(long state) {
    struct task_struct *t = linux_current();

    if (t != NULL) {
        t->state = state;
        if (state != 0) {
            t->woken = 0;
        }
    }
}

static int woken_ready(void *ctx) {
    struct task_struct *t = ctx;

    return t->woken || t->state == 0;
}

/* schedule(): sleep if the state says so, until wake_up_process. With
 * TASK_RUNNING it is a yield. The wait is waitq-based, so a wakeup that
 * lands between set_current_state and here is not lost: woken is checked
 * under the big kernel lock before blocking. */
static long do_schedule(uint64 deadline) {
    struct task_struct *t = linux_current();
    int r = WAITQ_READY;

    if (t == NULL) {
        return 0;
    }
    if (t->state == 0) {
        schedule();
        return 0;
    }
    if (preempt_counts[smp_cpu_index()] > 0) {
        kprintf_c(0x0C, "lkpi: schedule() with preemption disabled\n");
    }
    if (!woken_ready(t)) {
        r = deadline == 0 ? waitq_wait(&t->gate, woken_ready, t)
                          : waitq_wait_until(&t->gate, woken_ready, t, deadline);
    }
    t->state = 0;
    t->woken = 0;
    return r;
}

void linux_schedule(void) {
    (void)do_schedule(0);
}

long linux_schedule_timeout(long timeout) {
    uint64 now = timer_ticks_now();
    uint64 deadline;
    long r;

    if (timeout <= 0) {
        linux_set_current_state(0);
        return 0;
    }
    if (timeout == 0x7FFFFFFFFFFFFFFFL) {
        linux_schedule();
        return timeout;
    }
    deadline = now + (uint64)timeout;
    r = do_schedule(deadline);
    if (r == WAITQ_TIMEOUT) {
        return 0;
    }
    now = timer_ticks_now();
    return now < deadline ? (long)(deadline - now) : 1;
}

void linux_yield(void) {
    schedule();
}

int linux_cond_resched(void) {
    if (sched_needs_resched()) {
        schedule();
        return 1;
    }
    return 0;
}

int linux_signal_pending(struct task_struct *p) {
    return p != NULL && p->p != NULL && signal_pending(p->p);
}

int wake_up_process(struct task_struct *t) {
    if (t == NULL || t->p == NULL) {
        return 0;
    }
    if (!t->started) {
        t->started = 1;
        waitq_wake_all(&t->gate);
        return 1;
    }
    {
        int was_sleeping = t->state != 0;

        /* Recorded even for a thread that has not gone to sleep yet: one
         * between set_current_state and schedule() must not sleep through a
         * wakeup that was aimed at it. */
        t->woken = 1;
        waitq_wake_all(&t->gate);
        return was_sleeping;
    }
}

/* --- LinuxKPI kthreads ---------------------------------------------------------- */

static int started_ready(void *ctx) {
    struct task_struct *t = ctx;

    return t->started;
}

static void lkpi_kthread_trampoline(void *arg) {
    struct task_struct *t = arg;

    /* Created stopped: Linux's kthread_create does not run the function
     * until wake_up_process (or kthread_stop, which then skips it). */
    while (!t->started) {
        (void)waitq_wait(&t->gate, started_ready, t);
    }
    if (!t->should_stop && t->threadfn != NULL) {
        t->result = t->threadfn(t->data);
    } else {
        t->result = -4;                      /* -EINTR: stopped before start */
    }
    t->exited = 1;
    waitq_wake_all(&t->exitq);
    kthread_exit();
}

struct task_struct *linux_kthread_create(int (*threadfn)(void *data), void *data,
                                         const char *name) {
    process_t *p;
    struct task_struct *t;

    p = kthread_create(lkpi_kthread_trampoline, NULL, name);
    if (p == NULL) {
        return NULL;
    }
    t = task_of(p);
    t->threadfn = threadfn;
    t->data     = data;
    t->started  = 0;
    p->karg     = t;
    return t;
}

void kthread_bind(struct task_struct *t, unsigned int cpu) {
    if (t != NULL && t->p != NULL && cpu < NR_CPUS) {
        (void)sched_set_affinity(t->p, 1ULL << cpu);
    }
}

void kthread_bind_mask(struct task_struct *t, const struct cpumask *mask) {
    if (t != NULL && t->p != NULL && mask != NULL) {
        (void)sched_set_affinity(t->p, mask->bits[0]);
    }
}

struct task_struct *kthread_create_on_cpu(int (*threadfn)(void *data), void *data,
                                          unsigned int cpu, const char *namefmt) {
    struct task_struct *t = linux_kthread_create(threadfn, data, namefmt);

    if (t != NULL) {
        kthread_bind(t, cpu);
    }
    return t;
}

int kthread_should_stop(void) {
    struct task_struct *t = linux_current();

    return t != NULL && t->should_stop;
}

static int exited_ready(void *ctx) {
    struct task_struct *t = ctx;

    return t->exited;
}

int kthread_stop(struct task_struct *t) {
    if (t == NULL) {
        return -22;
    }
    t->should_stop = 1;
    if (!t->started) {
        t->started = 1;
    }
    t->woken = 1;
    waitq_wake_all(&t->gate);
    while (!t->exited) {
        (void)waitq_wait(&t->exitq, exited_ready, t);
    }
    return t->result;
}

int kthread_park(struct task_struct *t) {
    (void)t;
    return -38;                               /* -ENOSYS: no parking */
}

int set_cpus_allowed_ptr(struct task_struct *t, const struct cpumask *mask) {
    int rc;

    if (t == NULL || t->p == NULL || mask == NULL) {
        return -22;
    }
    rc = sched_set_affinity(t->p, mask->bits[0]);
    if (rc == 0 && t->p == proc_current()) {
        sched_migrate_self();
    }
    return rc;
}

int task_cpu(const struct task_struct *t) {
    return (t != NULL && t->p != NULL) ? t->p->cpu : 0;
}

/* ============================================================================
 * completions
 * ========================================================================== */

static wait_queue_t *cq(struct completion *x) {
    return (wait_queue_t *)x->wq_opaque;
}

void init_completion(struct completion *x) {
    x->done = 0;
    waitq_init(cq(x));
}

void reinit_completion(struct completion *x) {
    x->done = 0;
}

void complete(struct completion *x) {
    if (x->done != COMPLETION_ALL_DONE) {
        x->done++;
    }
    waitq_wake_all(cq(x));
}

void complete_all(struct completion *x) {
    x->done = COMPLETION_ALL_DONE;
    waitq_wake_all(cq(x));
}

static int done_ready(void *ctx) {
    return ((struct completion *)ctx)->done != 0;
}

static void consume(struct completion *x) {
    if (x->done != COMPLETION_ALL_DONE) {
        x->done--;
    }
}

void wait_for_completion(struct completion *x) {
    while (x->done == 0) {
        (void)waitq_wait(cq(x), done_ready, x);
    }
    consume(x);
}

unsigned long wait_for_completion_timeout(struct completion *x,
                                          unsigned long timeout) {
    uint64 deadline = timer_ticks_now() + timeout;

    while (x->done == 0) {
        if (waitq_wait_until(cq(x), done_ready, x, deadline) == WAITQ_TIMEOUT &&
            x->done == 0) {
            return 0;
        }
    }
    consume(x);
    {
        uint64 now = timer_ticks_now();
        return now < deadline ? (unsigned long)(deadline - now) : 1;
    }
}

int wait_for_completion_interruptible(struct completion *x) {
    while (x->done == 0) {
        if (waitq_wait(cq(x), done_ready, x) == WAITQ_SIGNAL && x->done == 0) {
            return -512;                               /* -ERESTARTSYS */
        }
    }
    consume(x);
    return 0;
}

int try_wait_for_completion(struct completion *x) {
    if (x->done == 0) {
        return 0;
    }
    consume(x);
    return 1;
}

int completion_done(struct completion *x) {
    return x->done != 0;
}

/* ============================================================================
 * workqueues
 * ========================================================================== */

#define WQ_MAX 8

struct wq_worker {
    struct workqueue_struct *wq;
    int                      cpu;          /* -1: unbound                     */
    struct work_struct      *head, *tail;
    wait_queue_t             waitq;        /* the worker sleeps here          */
    process_t               *thread;
    uint64                   ran;
};

struct workqueue_struct {
    const char       *name;
    unsigned int      flags;
    int               in_use;
    volatile int      dying;
    int               nworkers;
    struct wq_worker  workers[NR_CPUS];
};

static struct workqueue_struct wq_pool[WQ_MAX];
static wait_queue_t            wq_flushq;       /* flush_work waits here */
static int                     wq_ready_flag;

struct workqueue_struct *system_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;

static int worker_ready(void *ctx) {
    struct wq_worker *w = ctx;

    return w->head != NULL || w->wq->dying;
}

static void worker_main(void *arg) {
    struct wq_worker *w = arg;

    for (;;) {
        struct work_struct *work;

        while (w->head == NULL && !w->wq->dying) {
            (void)waitq_wait(&w->waitq, worker_ready, w);
        }
        if (w->head == NULL && w->wq->dying) {
            break;
        }
        work = w->head;
        w->head = work->next;
        if (w->head == NULL) {
            w->tail = NULL;
        }
        work->next = NULL;
        /* pending drops BEFORE the call, so the function may requeue its own
         * item - the self-rearming idiom - and a queue_work from anywhere
         * while it runs queues it once more rather than being dropped. */
        work->pending = 0;
        work->running = 1;
        work->func(work);
        work->running = 0;
        w->ran++;
        waitq_wake_all(&wq_flushq);
    }
    kthread_exit();
}

static void wq_init_once(void) {
    if (!wq_ready_flag) {
        wq_ready_flag = 1;
        waitq_init(&wq_flushq);
    }
}

struct workqueue_struct *linux_alloc_workqueue(const char *name, unsigned int flags,
                                               int max_active) {
    struct workqueue_struct *wq = NULL;
    int i, cpu;

    (void)max_active;
    wq_init_once();
    for (i = 0; i < WQ_MAX; i++) {
        if (!wq_pool[i].in_use) {
            wq = &wq_pool[i];
            break;
        }
    }
    if (wq == NULL) {
        kprintf_c(0x0C, "lkpi: no room for workqueue '%s'\n",
                  name != NULL ? name : "?");
        return NULL;
    }
    wq->name     = name != NULL ? name : "workqueue";
    wq->flags    = flags;
    wq->in_use   = 1;
    wq->dying    = 0;
    wq->nworkers = 0;
    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        struct wq_worker *w = &wq->workers[cpu];

        w->wq     = wq;
        w->cpu    = -1;
        w->head   = w->tail = NULL;
        w->thread = NULL;
        w->ran    = 0;
        waitq_init(&w->waitq);
    }
    /* Unbound: one worker that may run anywhere. Bound: one per online CPU,
     * pinned there - which is what makes queue_work_on mean "on that CPU". */
    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        struct wq_worker *w = &wq->workers[cpu];

        if ((flags & WQ_UNBOUND) ? cpu != 0 : !cpumask_test_cpu(cpu, &online_mask)) {
            continue;
        }
        w->cpu    = (flags & WQ_UNBOUND) ? -1 : cpu;
        w->thread = kthread_create(worker_main, w, wq->name);
        if (w->thread == NULL) {
            continue;
        }
        if (w->cpu >= 0) {
            (void)sched_set_affinity(w->thread, 1ULL << cpu);
        }
        wq->nworkers++;
    }
    if (wq->nworkers == 0) {
        wq->in_use = 0;
        return NULL;
    }
    return wq;
}

static struct wq_worker *worker_for(struct workqueue_struct *wq, int cpu) {
    if (wq->flags & WQ_UNBOUND) {
        return &wq->workers[0];
    }
    if (cpu < 0 || cpu >= NR_CPUS || wq->workers[cpu].thread == NULL) {
        cpu = smp_cpu_index();
    }
    if (wq->workers[cpu].thread == NULL) {
        int c;
        for (c = 0; c < NR_CPUS; c++) {
            if (wq->workers[c].thread != NULL) {
                return &wq->workers[c];
            }
        }
    }
    return &wq->workers[cpu];
}

static void ensure_system_wq(void) {
    if (system_wq == NULL) {
        system_wq = linux_alloc_workqueue("events", 0, 0);
        system_highpri_wq = system_wq;
        system_long_wq    = system_wq;
        system_unbound_wq = system_wq;
    }
}

int queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work) {
    struct wq_worker *w;

    if (wq == NULL || work == NULL || work->func == NULL) {
        return 0;
    }
    if (work->pending) {
        return 0;
    }
    w = worker_for(wq, cpu);
    work->pending = 1;
    work->next    = NULL;
    work->cpu     = w->cpu;
    work->wq      = wq;
    if (w->tail != NULL) {
        w->tail->next = work;
    } else {
        w->head = work;
    }
    w->tail = work;
    waitq_wake_all(&w->waitq);
    return 1;
}

int queue_work(struct workqueue_struct *wq, struct work_struct *work) {
    return queue_work_on(smp_cpu_index(), wq, work);
}

int schedule_work(struct work_struct *work) {
    ensure_system_wq();
    return queue_work(system_wq, work);
}

int schedule_work_on(int cpu, struct work_struct *work) {
    ensure_system_wq();
    return queue_work_on(cpu, system_wq, work);
}

int work_pending(struct work_struct *work) {
    return work != NULL && work->pending;
}

static int work_idle(void *ctx) {
    struct work_struct *work = ctx;

    return !work->pending && !work->running;
}

int flush_work(struct work_struct *work) {
    int waited = 0;

    if (work == NULL) {
        return 0;
    }
    wq_init_once();
    while (!work_idle(work)) {
        waited = 1;
        (void)waitq_wait(&wq_flushq, work_idle, work);
    }
    return waited;
}

/* Remove a pending item from its worker's list; 1 if it was there. */
static int unqueue(struct work_struct *work) {
    struct workqueue_struct *wq = work->wq;
    int cpu;

    if (!work->pending || wq == NULL) {
        return 0;
    }
    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        struct wq_worker *w = &wq->workers[cpu];
        struct work_struct *prev = NULL, *it = w->head;

        while (it != NULL && it != work) {
            prev = it;
            it = it->next;
        }
        if (it == work) {
            if (prev == NULL) {
                w->head = work->next;
            } else {
                prev->next = work->next;
            }
            if (w->tail == work) {
                w->tail = prev;
            }
            work->next = NULL;
            work->pending = 0;
            return 1;
        }
    }
    return 0;
}

int cancel_work_sync(struct work_struct *work) {
    int was;

    if (work == NULL) {
        return 0;
    }
    was = unqueue(work);
    while (work->running) {
        (void)waitq_wait(&wq_flushq, work_idle, work);
    }
    return was;
}

struct flush_marker {
    struct work_struct work;
    struct completion  done;
};

static void flush_marker_fn(struct work_struct *work) {
    struct flush_marker *m = (struct flush_marker *)work;

    complete(&m->done);
}

/* Every item queued before the call has run: a marker per worker, queued
 * behind them, and a wait for each marker. */
void flush_workqueue(struct workqueue_struct *wq) {
    struct flush_marker *markers;
    int cpu, n = 0;

    if (wq == NULL) {
        return;
    }
    markers = (struct flush_marker *)kmalloc(sizeof(*markers) * NR_CPUS);
    if (markers == NULL) {
        return;
    }
    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        if (wq->workers[cpu].thread == NULL) {
            continue;
        }
        INIT_WORK(&markers[cpu].work, flush_marker_fn);
        init_completion(&markers[cpu].done);
        (void)queue_work_on(cpu, wq, &markers[cpu].work);
        n++;
    }
    for (cpu = 0; cpu < NR_CPUS && n > 0; cpu++) {
        if (wq->workers[cpu].thread != NULL) {
            wait_for_completion(&markers[cpu].done);
        }
    }
    kfree(markers);
}

void flush_scheduled_work(void) {
    flush_workqueue(system_wq);
}

void destroy_workqueue(struct workqueue_struct *wq) {
    int cpu;

    if (wq == NULL || !wq->in_use) {
        return;
    }
    flush_workqueue(wq);
    wq->dying = 1;
    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        waitq_wake_all(&wq->workers[cpu].waitq);
    }
    /* The workers exit on their own and kthread_reap collects them; the
     * slot is reusable once none of them can touch it, which is after they
     * have seen `dying` - they only look at their own worker struct. */
    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        while (wq->workers[cpu].thread != NULL &&
               wq->workers[cpu].thread->state != PROC_ZOMBIE &&
               wq->workers[cpu].thread->state != PROC_UNUSED) {
            schedule();
            bkl_wait_for_interrupt();
        }
        wq->workers[cpu].thread = NULL;
    }
    wq->in_use = 0;
}

/* --- delayed work ------------------------------------------------------------------
 *
 * One timer thread for all of them: it sleeps until the earliest expiry
 * (or until a new earlier one is armed) and queues each due item on the
 * queue and CPU it was scheduled for. */
static struct delayed_work *timer_head;
static wait_queue_t timerq;
static process_t *timer_thread;

static struct workqueue_struct *dw_target(struct delayed_work *dw) {
    return (struct workqueue_struct *)dw->work.wq;
}

static int timer_ready(void *ctx) {
    (void)ctx;
    return timer_head != NULL && timer_head->expires <= timer_ticks_now();
}

static void timer_main(void *arg) {
    (void)arg;
    for (;;) {
        struct delayed_work *dw;

        if (timer_head == NULL) {
            (void)waitq_wait(&timerq, timer_ready, NULL);
            continue;
        }
        if (timer_head->expires > timer_ticks_now()) {
            (void)waitq_wait_until(&timerq, timer_ready, NULL, timer_head->expires);
            continue;
        }
        dw = timer_head;
        timer_head = dw->next_timer;
        dw->next_timer = NULL;
        dw->armed = 0;
        dw->work.pending = 0;
        (void)queue_work_on(dw->cpu, dw_target(dw), &dw->work);
    }
}

static void arm(struct delayed_work *dw) {
    struct delayed_work **pp = &timer_head;

    while (*pp != NULL && (*pp)->expires <= dw->expires) {
        pp = &(*pp)->next_timer;
    }
    dw->next_timer = *pp;
    *pp = dw;
    dw->armed = 1;
    if (timer_thread == NULL) {
        waitq_init(&timerq);
        timer_thread = kthread_create(timer_main, NULL, "lkpi-timer");
    }
    waitq_wake_all(&timerq);
}

static int disarm(struct delayed_work *dw) {
    struct delayed_work **pp = &timer_head;

    while (*pp != NULL && *pp != dw) {
        pp = &(*pp)->next_timer;
    }
    if (*pp == dw) {
        *pp = dw->next_timer;
        dw->next_timer = NULL;
        dw->armed = 0;
        dw->work.pending = 0;
        return 1;
    }
    return 0;
}

int queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
                          struct delayed_work *dw, unsigned long delay) {
    if (wq == NULL || dw == NULL || dw->work.pending) {
        return 0;
    }
    if (delay == 0) {
        return queue_work_on(cpu, wq, &dw->work);
    }
    dw->work.pending = 1;
    dw->work.wq = wq;
    dw->cpu = cpu;
    dw->expires = timer_ticks_now() + delay;
    arm(dw);
    return 1;
}

int queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
                       unsigned long delay) {
    return queue_delayed_work_on(smp_cpu_index(), wq, dw, delay);
}

int schedule_delayed_work(struct delayed_work *dw, unsigned long delay) {
    ensure_system_wq();
    return queue_delayed_work(system_wq, dw, delay);
}

int mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw,
                     unsigned long delay) {
    int was = 0;

    if (dw->armed) {
        was = disarm(dw);
    } else if (dw->work.pending) {
        was = unqueue(&dw->work);
    }
    (void)queue_delayed_work(wq, dw, delay);
    return was;
}

int cancel_delayed_work(struct delayed_work *dw) {
    if (dw == NULL) {
        return 0;
    }
    if (dw->armed) {
        return disarm(dw);
    }
    return unqueue(&dw->work);
}

int cancel_delayed_work_sync(struct delayed_work *dw) {
    int was = cancel_delayed_work(dw);

    while (dw->work.running) {
        (void)waitq_wait(&wq_flushq, work_idle, &dw->work);
    }
    return was;
}

int flush_delayed_work(struct delayed_work *dw) {
    if (dw->armed && disarm(dw)) {
        (void)queue_work_on(dw->cpu, dw_target(dw), &dw->work);
    }
    return flush_work(&dw->work);
}

/* ============================================================================
 * startup
 * ========================================================================== */

void lkpi_smp_started(void) {
    uint64 m = smp_online_mask();
    int cpu;

    online_mask.bits[0]   = (unsigned long)m;
    possible_mask.bits[0] = (unsigned long)m;
    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        single_masks[cpu].bits[0] = 1UL << cpu;
    }
    wq_init_once();
    percpu_setup();
}

/* --- late_initcall ---------------------------------------------------------------
 *
 * Queued until the kernel is fully up (lkpi_run_late_initcalls, called at the
 * end of smp_start_scheduling), run at once after that. */
#define LATE_MAX 16
static int (*late_calls[LATE_MAX])(void);
static int late_count;
static int late_done;

int linux_register_late_initcall(int (*fn)(void)) {
    if (fn == NULL) {
        return 0;
    }
    if (late_done) {
        return fn();
    }
    if (late_count >= LATE_MAX) {
        kprintf_c(0x0C, "lkpi: too many late_initcalls\n");
        return -12;                                 /* -ENOMEM */
    }
    late_calls[late_count++] = fn;
    return 0;
}

void lkpi_run_late_initcalls(void) {
    int i;

    late_done = 1;
    for (i = 0; i < late_count; i++) {
        int rc = late_calls[i]();

        if (rc != 0) {
            kprintf_c(0x0C, "lkpi: late_initcall %d returned %d\n", i, rc);
        }
    }
    late_count = 0;
}
