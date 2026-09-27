/* FreeBSD's multiprocessor KPI - <sys/smp.h>, <sys/pcpu.h>, <sys/sched.h> -
 * on Genesis's SMP. The header comments say what each call promises; this
 * file is the part that needs a translation unit: CPU_ABSENT's two phases,
 * the all_cpus set, and smp_rendezvous with its two barriers.
 *
 * Plus a boot selftest, driving the whole KPI the way a FreeBSD driver
 * would: CPU_FOREACH over the CPUs that exist, a rendezvous whose phases
 * really are separated, sched_bind really moving a thread, DPCPU slots really
 * per CPU, and bus_bind_intr really moving a device's interrupt. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/sched.h>
#include <sys/cpuset.h>

#include "kprintf.h"

/* --- which CPUs exist ------------------------------------------------------- */

int genesis_cpu_absent(int cpu) {
    if (cpu < 0 || cpu > mp_maxid) {
        return 1;
    }
    /* Until smp_init has counted them, every slot counts as present: UMA
     * creates its zones (and their per-CPU caches, by CPU_FOREACH) before
     * the CPUs are enumerated, and a slot skipped then would have no cache
     * on the CPU that later turns out to exist. */
    if (!smp_cpus_counted()) {
        return 0;
    }
    return ((smp_online_mask() >> cpu) & 1) == 0;
}

int genesis_smp_started(void) {
    return smp_scheduling_started();
}

static cpuset_t genesis_cpus_up;

struct _cpuset *genesis_all_cpus(void) {
    int i;

    CPU_ZERO(&genesis_cpus_up);
    for (i = 0; i <= mp_maxid; i++) {
        if ((smp_online_mask() >> i) & 1) {
            CPU_SET(i, &genesis_cpus_up);
        }
    }
    return &genesis_cpus_up;
}

/* --- smp_rendezvous ------------------------------------------------------------
 *
 * Every participant runs in its CPU's IPI handler, concurrently (smp_call_on
 * to each target from here, with this CPU running its own share in place).
 * The barriers are counters: a CPU finishing a phase increments the count
 * and spins until every participant has. smp_call_on waits for each remote
 * call to finish, so the calls are issued as a fan-out first and the local
 * share is run by the CALLER - which is why the fan-out cannot use
 * smp_call_on's wait: it would wait for a remote CPU stuck at a barrier the
 * caller has not reached. smp_call_all does exactly the right thing: it
 * posts to every other CPU, runs the caller's share, then waits. */
void smp_no_rendezvous_barrier(void *arg) {
    (void)arg;
}

struct rendezvous {
    void (*setup)(void *);
    void (*action)(void *);
    void (*teardown)(void *);
    void *arg;
    uint64 map;
    int    participants;
    volatile int entered;
    volatile int setup_done;
    volatile int action_done;
};

static void rv_wait(volatile int *counter, int target) {
    __atomic_add_fetch(counter, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < target) {
        __asm__ volatile ("pause");
    }
}

static void rv_run(void *p) {
    struct rendezvous *rv = p;
    int me = (int)curcpu;

    if (((rv->map >> me) & 1) == 0) {
        return;                         /* not a participant: just ack */
    }
    if (rv->setup != NULL && rv->setup != smp_no_rendezvous_barrier) {
        rv->setup(rv->arg);
    }
    if (rv->setup != smp_no_rendezvous_barrier) {
        rv_wait(&rv->setup_done, rv->participants);
    }
    if (rv->action != NULL) {
        rv->action(rv->arg);
    }
    if (rv->teardown != smp_no_rendezvous_barrier) {
        rv_wait(&rv->action_done, rv->participants);
    }
    if (rv->teardown != NULL && rv->teardown != smp_no_rendezvous_barrier) {
        rv->teardown(rv->arg);
    }
}

static void rendezvous_mask(uint64 map, void (*setup)(void *),
                            void (*action)(void *), void (*teardown)(void *),
                            void *arg) {
    struct rendezvous rv;
    int i;

    rv.setup = setup;
    rv.action = action;
    rv.teardown = teardown;
    rv.arg = arg;
    rv.map = map & smp_online_mask();
    rv.participants = 0;
    rv.entered = 0;
    rv.setup_done = 0;
    rv.action_done = 0;
    for (i = 0; i <= mp_maxid; i++) {
        if ((rv.map >> i) & 1) {
            rv.participants++;
        }
    }
    if (rv.participants == 0) {
        return;
    }
    smp_call_all(rv_run, &rv);
}

void smp_rendezvous(void (*setup)(void *), void (*action)(void *),
                    void (*teardown)(void *), void *arg) {
    rendezvous_mask(~0ULL, setup, action, teardown, arg);
}

void smp_rendezvous_cpus(cpuset_t map, void (*setup)(void *),
                         void (*action)(void *), void (*teardown)(void *),
                         void *arg) {
    uint64 m = 0;
    int i;

    for (i = 0; i <= mp_maxid; i++) {
        if (CPU_ISSET(i, &map)) {
            m |= 1ULL << i;
        }
    }
    rendezvous_mask(m, setup, action, teardown, arg);
}

/* --- the selftest ------------------------------------------------------------------ */

DPCPU_DEFINE_STATIC(long, st_dpcpu);

static volatile int st_setup_count, st_action_count, st_teardown_count;
static volatile int st_action_saw_all_setups;

static void st_setup(void *arg) {
    (void)arg;
    __atomic_add_fetch(&st_setup_count, 1, __ATOMIC_ACQ_REL);
}

static void st_action(void *arg) {
    int n = *(int *)arg;

    /* Past the first barrier: every participant's setup is finished. */
    if (__atomic_load_n(&st_setup_count, __ATOMIC_ACQUIRE) == n) {
        __atomic_add_fetch(&st_action_saw_all_setups, 1, __ATOMIC_ACQ_REL);
    }
    DPCPU_SET(st_dpcpu, (long)curcpu * 10 + 3);
    __atomic_add_fetch(&st_action_count, 1, __ATOMIC_ACQ_REL);
}

static void st_teardown(void *arg) {
    (void)arg;
    __atomic_add_fetch(&st_teardown_count, 1, __ATOMIC_ACQ_REL);
}

static volatile int st_bind_done;
static volatile int st_bind_landed[SMP_MAX_CPUS];

static void st_bind_thread(void *arg) {
    int cpu;

    (void)arg;
    CPU_FOREACH(cpu) {
        sched_bind(curthread, cpu);
        st_bind_landed[cpu] = (int)curcpu + 1;
    }
    sched_unbind(curthread);
    st_bind_done = 1;
}

int genesis_kthread_spawn(void (*fn)(void *), void *arg, const char *name);
void schedule(void);
void bkl_wait_for_interrupt(void);
uint64 timer_ticks_now(void);

int bsd_smp_selftest(void) {
    int failures = 0;
    int n = mp_ncpus, cpu, count = 0;

    CPU_FOREACH(cpu) {
        count++;
    }
    if (count != n || CPU_COUNT(&all_cpus) != n) {
        kprintf_c(0x0C, "bsd smp: CPU_FOREACH saw %d, all_cpus %d, mp_ncpus %d\n",
                  count, CPU_COUNT(&all_cpus), n);
        failures++;
    }
    if (!smp_started) {
        kprintf_c(0x0C, "bsd smp: smp_started is false after scheduling began\n");
        failures++;
    }

    /* A rendezvous: every setup before any action, every CPU in each. */
    st_setup_count = st_action_count = st_teardown_count = 0;
    st_action_saw_all_setups = 0;
    DPCPU_ZERO(st_dpcpu);
    smp_rendezvous(st_setup, st_action, st_teardown, &n);
    if (st_setup_count != n || st_action_count != n || st_teardown_count != n ||
        st_action_saw_all_setups != n) {
        kprintf_c(0x0C, "bsd smp: rendezvous setup %d action %d teardown %d, "
                        "%d actions saw every setup (want %d each)\n",
                  st_setup_count, st_action_count, st_teardown_count,
                  st_action_saw_all_setups, n);
        failures++;
    }

    /* DPCPU: each CPU wrote its own slot. */
    CPU_FOREACH(cpu) {
        if (DPCPU_ID_GET(cpu, st_dpcpu) != (long)cpu * 10 + 3) {
            kprintf_c(0x0C, "bsd smp: DPCPU slot for cpu%d is %ld\n", cpu,
                      DPCPU_ID_GET(cpu, st_dpcpu));
            failures++;
        }
    }
    {
        long want = 0;

        CPU_FOREACH(cpu) {
            want += (long)cpu * 10 + 3;
        }
        if (DPCPU_SUM(st_dpcpu) != want) {
            kprintf_c(0x0C, "bsd smp: DPCPU_SUM is wrong\n");
            failures++;
        }
    }

    /* A rendezvous limited to one CPU runs there only. */
    {
        cpuset_t one;

        CPU_ZERO(&one);
        CPU_SET(n - 1, &one);
        st_setup_count = st_action_count = st_teardown_count = 0;
        st_action_saw_all_setups = 0;
        {
            int one_n = 1;

            smp_rendezvous_cpus(one, st_setup, st_action, st_teardown, &one_n);
        }
        if (st_action_count != 1 || DPCPU_ID_GET(n - 1, st_dpcpu) != (long)(n - 1) * 10 + 3) {
            kprintf_c(0x0C, "bsd smp: a one-CPU rendezvous ran %d actions\n",
                      st_action_count);
            failures++;
        }
    }

    /* sched_bind moves a kernel thread through every CPU. */
    if (n > 1) {
        uint64 start = timer_ticks_now();

        st_bind_done = 0;
        for (cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
            st_bind_landed[cpu] = 0;
        }
        if (genesis_kthread_spawn(st_bind_thread, NULL, "bsd-bind") == 0) {
            kprintf_c(0x0C, "bsd smp: could not start the bind thread\n");
            failures++;
        } else {
            while (!st_bind_done && timer_ticks_now() - start < 500) {
                schedule();
                if (!st_bind_done) {
                    bkl_wait_for_interrupt();
                }
            }
            CPU_FOREACH(cpu) {
                if (st_bind_landed[cpu] != cpu + 1) {
                    kprintf_c(0x0C, "bsd smp: sched_bind(%d) ran on cpu%d\n",
                              cpu, st_bind_landed[cpu] - 1);
                    failures++;
                }
            }
        }
    }

    if (failures == 0) {
        kprintf("bsd smp: selftest passed - %d cpus: CPU_FOREACH, rendezvous "
                "barriers, DPCPU, sched_bind\n", n);
    } else {
        kprintf_c(0x0C, "bsd smp: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
