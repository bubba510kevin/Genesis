/* LinuxKPI's multiprocessor surface, exercised the way a Linux driver uses
 * it - written against the <linux/...> headers only, so what is tested is the
 * driver-facing API and not the implementation's internals. */

#include "linux/types.h"
#include "linux/kernel.h"
#include "linux/smp.h"
#include "linux/cpumask.h"
#include "linux/percpu.h"
#include "linux/preempt.h"
#include "linux/kthread.h"
#include "linux/completion.h"
#include "linux/workqueue.h"
#include "linux/atomic.h"
#include "linux/jiffies.h"

static int failures;

#define CHECK(cond, what)                                             \
    do {                                                              \
        if (!(cond)) {                                                \
            printk("lkpi smp selftest: FAILED - %s\n", what);         \
            failures++;                                               \
        }                                                             \
    } while (0)

/* --- cross-CPU calls ------------------------------------------------------------ */
static atomic_t call_hits[NR_CPUS];

static void record_cpu(void *info) {
    int *out = info;

    *out = smp_processor_id();
}

static void count_cpu(void *info) {
    (void)info;
    atomic_inc(&call_hits[smp_processor_id()]);
}

/* --- per-CPU data ------------------------------------------------------------------ */
static DEFINE_PER_CPU(long, lkpi_test_pcpu);

struct pair { long a; long b; };

static void stamp_pcpu(void *info) {
    struct pair *dyn = info;
    int me = smp_processor_id();

    this_cpu_write(lkpi_test_pcpu, (long)(me + 1) * 100);
    this_cpu_inc(lkpi_test_pcpu);
    this_cpu_ptr(dyn)->a = me + 7;
    this_cpu_ptr(dyn)->b = -(long)me;
}

/* --- kthreads ------------------------------------------------------------------------ */
struct kt_arg {
    struct completion ran;
    volatile int      cpu;
    volatile int      flag;
};

static int bound_fn(void *data) {
    struct kt_arg *a = data;

    a->cpu = smp_processor_id();
    complete(&a->ran);
    while (!kthread_should_stop()) {
        set_current_state(TASK_INTERRUPTIBLE);
        if (!kthread_should_stop()) {
            schedule();
        }
        __set_current_state(TASK_RUNNING);
    }
    return 42;
}

static int sleeper_fn(void *data) {
    struct kt_arg *a = data;

    /* The Linux sleep protocol: state, test, schedule, and the waker's
     * wake_up_process. */
    for (;;) {
        set_current_state(TASK_INTERRUPTIBLE);
        if (a->flag) {
            break;
        }
        schedule();
    }
    __set_current_state(TASK_RUNNING);
    complete(&a->ran);
    while (!kthread_should_stop()) {
        set_current_state(TASK_INTERRUPTIBLE);
        if (!kthread_should_stop()) {
            schedule();
        }
        __set_current_state(TASK_RUNNING);
    }
    return 0;
}

/* --- workqueues ------------------------------------------------------------------------ */
struct wq_item {
    struct work_struct work;
    volatile int       ran_on;
    volatile int       runs;
};

static void wq_fn(struct work_struct *w) {
    struct wq_item *it = (struct wq_item *)w;

    it->ran_on = smp_processor_id();
    it->runs++;
}

static struct delayed_work dwork;
static volatile unsigned long dwork_ran_at;

static void dwork_fn(struct work_struct *w) {
    (void)w;
    dwork_ran_at = jiffies;
}

int lkpi_smp_selftest(void) {
    int n = (int)num_online_cpus();
    int cpu, count;

    failures = 0;

    /* Counting. */
    count = 0;
    for_each_online_cpu(cpu) {
        count++;
    }
    CHECK(count == n && n >= 1, "for_each_online_cpu visits num_online_cpus()");
    CHECK(cpumask_weight(cpu_online_mask) == (unsigned int)n,
          "cpu_online_mask has one bit per CPU");
    CHECK(cpumask_test_cpu(smp_processor_id(), cpu_online_mask),
          "this CPU is online");
    CHECK(cpumask_first(cpumask_of(n - 1)) == (unsigned int)(n - 1),
          "cpumask_of(n) names n");

    /* smp_call_function_single lands where it is aimed. */
    for_each_online_cpu(cpu) {
        int got = -1;

        CHECK(smp_call_function_single(cpu, record_cpu, &got, 1) == 0,
              "smp_call_function_single succeeds");
        CHECK(got == cpu, "and the function ran on the CPU it named");
    }

    /* on_each_cpu: every CPU, once; smp_call_function: every OTHER CPU. */
    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        atomic_set(&call_hits[cpu], 0);
    }
    on_each_cpu(count_cpu, NULL, 1);
    smp_call_function(count_cpu, NULL, 1);
    for_each_online_cpu(cpu) {
        int want = cpu == smp_processor_id() ? 1 : 2;

        CHECK(atomic_read(&call_hits[cpu]) == want,
              "on_each_cpu hits every CPU once, smp_call_function all but me");
    }

    /* get_cpu pins, put_cpu releases. */
    {
        int c = get_cpu();

        CHECK(preempt_count() > 0, "get_cpu disables preemption");
        CHECK(c == smp_processor_id(), "and returns this CPU");
        put_cpu();
        CHECK(preempt_count() == 0, "put_cpu undoes it");
    }

    /* Per-CPU variables: static and dynamic, one copy each. */
    {
        struct pair *dyn = alloc_percpu(struct pair);

        CHECK(dyn != NULL, "alloc_percpu succeeds");
        if (dyn != NULL) {
            for_each_online_cpu(cpu) {
                CHECK(per_cpu_ptr(dyn, cpu)->a == 0 && per_cpu_ptr(dyn, cpu)->b == 0,
                      "alloc_percpu memory starts zeroed on every CPU");
            }
            on_each_cpu(stamp_pcpu, dyn, 1);
            for_each_online_cpu(cpu) {
                CHECK(per_cpu(lkpi_test_pcpu, cpu) == (long)(cpu + 1) * 100 + 1,
                      "each CPU's copy of a DEFINE_PER_CPU holds its own value");
                CHECK(per_cpu_ptr(dyn, cpu)->a == cpu + 7 &&
                      per_cpu_ptr(dyn, cpu)->b == -(long)cpu,
                      "each CPU's copy of an alloc_percpu object holds its own value");
                if (cpu > 0) {
                    CHECK(per_cpu_ptr(dyn, cpu) != per_cpu_ptr(dyn, 0),
                          "and the copies are distinct memory");
                }
            }
            free_percpu(dyn);
        }
    }

    /* A kthread bound to each CPU runs there, and stops on request. */
    for_each_online_cpu(cpu) {
        struct kt_arg a;
        struct task_struct *k;

        init_completion(&a.ran);
        a.cpu = -1;
        a.flag = 0;
        k = kthread_create_on_cpu(bound_fn, &a, (unsigned int)cpu, "lkpi-bound");
        CHECK(k != NULL, "kthread_create_on_cpu");
        if (k == NULL) {
            continue;
        }
        CHECK(!completion_done(&a.ran), "a created kthread waits to be woken");
        wake_up_process(k);
        wait_for_completion(&a.ran);
        CHECK(a.cpu == cpu, "a bound kthread runs on its CPU");
        CHECK(kthread_stop(k) == 42, "kthread_stop returns the thread's result");
    }

    /* The sleep protocol: a thread asleep in schedule() until woken. */
    {
        struct kt_arg a;
        struct task_struct *k;

        init_completion(&a.ran);
        a.flag = 0;
        k = kthread_run(sleeper_fn, &a, "lkpi-sleeper");
        CHECK(k != NULL, "kthread_run");
        if (k != NULL) {
            CHECK(wait_for_completion_timeout(&a.ran, 5) == 0,
                  "a thread in schedule() stays asleep until woken");
            a.flag = 1;
            wake_up_process(k);
            CHECK(wait_for_completion_timeout(&a.ran, 200) != 0,
                  "wake_up_process wakes it");
            kthread_stop(k);
        }
    }

    /* A bound workqueue runs each item on the CPU it was queued to. */
    {
        struct workqueue_struct *wq = alloc_workqueue("lkpi-test", 0, 0);
        struct wq_item items[NR_CPUS];

        CHECK(wq != NULL, "alloc_workqueue");
        if (wq != NULL) {
            for_each_online_cpu(cpu) {
                INIT_WORK(&items[cpu].work, wq_fn);
                items[cpu].ran_on = -1;
                items[cpu].runs = 0;
                CHECK(queue_work_on(cpu, wq, &items[cpu].work),
                      "queue_work_on queues an idle item");
            }
            flush_workqueue(wq);
            for_each_online_cpu(cpu) {
                CHECK(items[cpu].runs == 1 && items[cpu].ran_on == cpu,
                      "each work item ran once, on its CPU");
                CHECK(!work_pending(&items[cpu].work), "and is no longer pending");
            }
            /* Queueing an item that is still pending is a no-op. */
            INIT_WORK(&items[0].work, wq_fn);
            items[0].runs = 0;
            CHECK(schedule_work(&items[0].work), "schedule_work");
            CHECK(!schedule_work(&items[0].work) || items[0].runs == 1,
                  "a pending item is not queued twice");
            flush_work(&items[0].work);
            CHECK(items[0].runs == 1, "it ran exactly once");
            destroy_workqueue(wq);
        }
    }

    /* Delayed work waits its delay. */
    {
        unsigned long start = jiffies;

        INIT_DELAYED_WORK(&dwork, dwork_fn);
        dwork_ran_at = 0;
        CHECK(schedule_delayed_work(&dwork, 3), "schedule_delayed_work");
        {
            unsigned long spin_until = jiffies + 200;

            while (dwork_ran_at == 0 && time_before(jiffies, spin_until)) {
                schedule_timeout_interruptible(1);
            }
        }
        CHECK(dwork_ran_at != 0, "delayed work ran");
        CHECK(dwork_ran_at - start >= 3, "and not before its delay");
    }

    if (failures == 0) {
        printk("lkpi smp: selftest passed - %d cpus: calls, per-CPU data, bound "
               "kthreads, sleep/wake, workqueues\n", n);
    }
    return failures;
}
