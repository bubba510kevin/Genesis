#include "bkl.h"
#include "cpu.h"
#include "ksmp.h"
#include "kthread.h"
#include "ksleep.h"
#include "process.h"
#include "sched.h"
#include "screen.h"
#include "syscall.h"
#include "timer.h"
#include "typesk.h"

/* --- the scheduler, on every CPU --------------------------------------------
 *
 * One run "queue" - the process table, scanned - shared by all CPUs, and
 * everything about the CURRENT run per CPU: which thread each CPU is
 * running, its slice, its resched flag, its idle thread (see ksmp.h's
 * struct cpu_local). A shared queue rather than per-CPU queues because the
 * table is small and a scan reads state that is true by construction; ULE's
 * per-CPU queues exist to avoid lock contention on a queue lock, and the
 * big kernel lock already serialises every reader and writer of this one.
 *
 * What makes a table entry runnable ON A GIVEN CPU is sched_eligible(), and
 * every policy goes through it: READY and allowed by affinity, or already
 * RUNNING on this very CPU. A thread RUNNING on another CPU is never picked -
 * that would be two CPUs executing one thread's kernel stack.
 *
 * --- round-robin ---------------------------------------------------------
 * A circular scan over the table from after the CPU's current thread. The
 * table is small, so a scan is cheaper than the pointer chasing a list would
 * need, and it cannot develop the failure a list can: a process freed while
 * still enqueued leaves a dangling next pointer. */

#define QUANTUM_TICKS 5     /* 50ms at 100Hz */

int sched_eligible(const process_t *p, int cpu) {
    if (p == NULL || p->is_idle) {
        return 0;
    }
    if ((p->affinity & (1ULL << cpu)) == 0) {
        return 0;
    }
    if (p->state == PROC_READY) {
        return !p->oncpu;
    }
    if (p->state == PROC_RUNNING) {
        return p == smp_cpu(cpu)->current;
    }
    return 0;
}

static void rr_enqueue(process_t *p) {
    if (p != NULL && p->state == PROC_BLOCKED) {
        p->state = PROC_READY;
    }
}

static void rr_dequeue(process_t *p) {
    (void)p;   /* nothing to unlink: readiness is the process's own state */
}

static process_t *rr_pick_next(int cpu) {
    process_t *start = smp_cpu(cpu)->current;
    process_t *p;
    int i;

    /* Begin AFTER the current process so a runnable peer is preferred over
     * running the same one again - that is the whole of round-robin's
     * fairness. The current one is reached last (i == MAX_PROCESSES), so it
     * keeps the CPU only when nothing else here can have it. */
    for (i = 1; i <= MAX_PROCESSES; i++) {
        p = proc_at((proc_index(start) + i) % MAX_PROCESSES);
        if (sched_eligible(p, cpu)) {
            return p;
        }
    }
    return NULL;
}

static int rr_tick(process_t *p, int *quantum) {
    (void)p;
    if (*quantum > 0) {
        (*quantum)--;
    }
    if (*quantum == 0) {
        *quantum = QUANTUM_TICKS;
        return 1;
    }
    return 0;
}

static const sched_policy_t round_robin = {
    "round-robin", rr_enqueue, rr_dequeue, rr_pick_next, rr_tick, NULL
};

static const sched_policy_t *policy = &round_robin;

/* --- mechanism ----------------------------------------------------------- */

void sched_init(process_t *first) {
    struct cpu_local *c = smp_this_cpu();

    policy          = &round_robin;
    c->current      = first;
    c->quantum_left = QUANTUM_TICKS;
    c->resched      = 0;
    if (first != NULL) {
        first->cpu   = (int)c->index;
        first->oncpu = 1;
    }
}

void sched_set_policy(const sched_policy_t *pol) {
    if (pol != NULL) {
        policy = pol;
    }
}

const char *sched_policy_name(void) {
    return policy->name;
}

void sched_enqueue(process_t *p) { policy->enqueue(p); }
void sched_dequeue(process_t *p) { policy->dequeue(p); }

/* The half of a tick that belongs to the MACHINE: wake sleepers whose
 * deadline passed, charge sleep time to every blocked thread, and let the
 * policy age its statistics. Once per tick, on the BSP, whose PIT is the one
 * clock every deadline is written in.
 *
 * Sleep is charged to every thread that is blocked; PROC_READY is charged
 * to NEITHER counter. A process that is runnable and waiting for a CPU is
 * not sleeping - counting that as sleep would make a compute-bound process
 * on a busy machine look interactive, which is precisely backwards. */
void sched_tick_global(void) {
    int i;

    sched_wake_sleepers(timer_ticks_now());
    for (i = 0; i < MAX_PROCESSES; i++) {
        process_t *t = proc_at(i);

        if (t != NULL && t->state == PROC_BLOCKED && !t->is_idle) {
            t->sleep_ticks++;
        }
    }
    if (policy->decay != NULL) {
        policy->decay();
    }
}

/* The half that belongs to THIS CPU: charge the tick to the thread it was
 * running, and ask the policy whether that thread's slice is spent.
 *
 * The current thread is charged unconditionally, with no test on its state:
 * being current when the timer fires IS what consumed the tick. Two
 * counters, one event - run_ticks is ULE's scoring input and gets halved
 * every couple of seconds; cpu_ticks is the accounting total and nothing may
 * touch it. An idle thread is charged nothing: its time is the CPU's idle
 * time, counted separately. */
void sched_tick_local(void) {
    struct cpu_local *c = smp_this_cpu();
    process_t *p = c->current;

    c->ticks++;
    if (p == NULL) {
        return;
    }
    if (p->is_idle) {
        c->idle_ticks++;
        return;
    }
    p->run_ticks++;
    p->cpu_ticks++;
    if (policy->tick(p, &c->quantum_left)) {
        c->resched = 1;
    }
}

void sched_tick(void) {
    sched_tick_global();
    sched_tick_local();
}

int sched_needs_resched(void) {
    return smp_this_cpu()->resched;
}

/* Wake anyone waiting to reap `z`, which has just stopped running here.
 *
 * A zombie is not reapable while it is still on a CPU (see process.h's
 * oncpu), so a parent that looked while it was - and found nothing to reap -
 * went back to sleep. Nothing else would wake it: the child's exit already
 * sent its SIGCHLD and wakeup. So the switch away is the event, and it wakes
 * every thread in wait4. Spurious wakeups are cheap - they re-scan - and a
 * missed one is a shell hung on a finished job. */
static void zombie_left_cpu(process_t *z) {
    int i;

    (void)z;
    for (i = 0; i < MAX_PROCESSES; i++) {
        process_t *t = proc_at(i);

        if (t != NULL && t->waiting_for_child && t->state == PROC_BLOCKED) {
            sched_wake(t);
        }
    }
}

void schedule(void) {
    struct cpu_local *c = smp_this_cpu();
    process_t *prev = c->current;
    process_t *next;

    c->resched = 0;

    /* Reclaim exited kernel threads and user threads here: this is the
     * first point guaranteed to be running on somebody else's stack. */
    kthread_reap();
    proc_reap_threads();

    next = policy->pick_next((int)c->index);
    if (next == NULL) {
        /* Nothing runnable on this CPU. The idle thread, if there is one yet;
         * before smp_start_scheduling there is not, and the caller falls
         * back to halting in its own context as it always did. */
        next = c->idle;
        if (next == NULL) {
            if (prev != NULL && prev->state == PROC_READY) {
                prev->state = PROC_RUNNING;
            }
            return;
        }
    }
    /* A fresh slice for whatever runs next under round-robin. ULE sizes its
     * own slices by score when one runs out, so it is left alone. */
    if (policy == &round_robin) {
        c->quantum_left = QUANTUM_TICKS;
    }

    if (next == prev) {
        /* Carrying on. The state must still SAY running: a process woken
         * (BLOCKED -> READY) that resumes as the only runnable one would
         * otherwise run on marked READY for the rest of its life. */
        next->state = PROC_RUNNING;
        return;
    }

    /* The user stack pointer lives in the per-CPU block, which the next
     * thread is about to overwrite. A kernel thread has none to park. */
    if (prev != NULL) {
        if (!prev->is_kthread) {
            prev->saved_user_rsp = syscall_get_user_rsp();
        }
        if (prev->state == PROC_RUNNING) {
            prev->state = PROC_READY;
        }
        /* Off this CPU. Safe to publish before the switch below has saved
         * prev's registers because the big kernel lock is held, and passes
         * to `next` rather than being released: no other CPU can look at
         * prev until the switch is long complete. */
        prev->oncpu = 0;
        if (prev->state == PROC_ZOMBIE) {
            zombie_left_cpu(prev);
        }
    }

    next->state = PROC_RUNNING;
    next->oncpu = 1;
    next->cpu   = (int)c->index;
    c->current  = next;
    c->switches++;
    proc_activate_stack(next);
    if (!next->is_kthread) {
        syscall_set_user_rsp(next->saved_user_rsp);
    }

    if (next->space != NULL && (prev == NULL || next->space != prev->space)) {
        vmm_switch_to(next->space);
    }

    /* The FPU is context too. switch_context saves the callee-saved integer
     * registers and nothing else. Safe here rather than lazily because the
     * kernel builds with -mno-sse. */
    if (prev != NULL) {
        fpu_save(prev->thread.fpu_state);
    }
    fpu_restore(next->thread.fpu_state);

    {
        static uint64 no_outgoing_thread[SMP_MAX_CPUS];
        uint64 *park = (prev != NULL) ? &prev->thread.saved_rsp
                                      : &no_outgoing_thread[c->index];

        switch_context(park, next->thread.saved_rsp);
    }
}

/* The single place the kernel decides what to do on its way back to user
 * mode. Both entry paths end here before returning. `to_user` is false when
 * an interrupt landed while the kernel was already running; switching then
 * would abandon whatever the interrupted kernel code had live on its stack. */
void return_to_user(int to_user) {
    process_t *me;

    if (!to_user) {
        return;
    }

    /* A process that died while inside the kernel - or was killed while it
     * ran on this CPU, by a thread on another - must not be returned to. A
     * zombie is not runnable, so schedule() never comes back for it. */
    me = proc_current();
    if (me != NULL && me->state == PROC_ZOMBIE) {
        schedule();

        /* Returned, so nothing else was runnable and there is no idle
         * thread - only possible before the scheduler is fully up. */
        print_string("\n[nothing left to run]\n", 0x4F);
        __asm__ volatile ("cli");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    /* Suspended (NtSuspendThread) - by itself in the syscall now ending, or
     * by another thread that kicked this CPU to get here. Parked rather than
     * returned: the next instruction in ring 3 is exactly what a suspend
     * promises will not run. A loop, because anything else that wakes a
     * blocked thread is a spurious wake here; only NtResumeThread taking the
     * count to 0 lets it out. A kill does not come back through here at
     * all - a zombie's schedule() never returns. */
    /*
     * A job-control stop (SIGSTOP, ^Z's SIGTSTP...) parks the same way and
     * for the same reason: until SIGCONT, not one more ring-3 instruction.
     * nt_parked is what keeps an ordinary signal from waking it (see
     * signal_send); SIGCONT and SIGKILL clear job_stopped and wake it. */
    {
        int was_stopped = 0;

        while (me != NULL && (me->nt_suspend_count > 0 || me->job_stopped) &&
               me->state != PROC_ZOMBIE) {
            if (me->job_stopped) {
                was_stopped = 1;
            }
            me->nt_parked = 1;
            sched_block(me);
        }
        if (me != NULL) {
            me->nt_parked = 0;
        }
        /* Let out by SIGKILL: die now, not after one more trip to ring 3. */
        if (me != NULL && was_stopped && signal_kill_if_fatal(me)) {
            schedule();
            print_string("\n[nothing left to run]\n", 0x4F);
            __asm__ volatile ("cli");
            for (;;) {
                __asm__ volatile ("hlt");
            }
        }
    }
    /* A GS base changed from another thread (nt_attach giving this one a
     * TEB): loaded now, so the next instruction in ring 3 sees it. */
    if (me != NULL && me->gs_reload) {
        me->gs_reload = 0;
        syscall_set_user_gs_base(me->thread.gs_base);
    }

    smp_run_deferred();
    if (sched_needs_resched()) {
        schedule();
    }
}

void sched_block(process_t *p) {
    if (p == NULL) {
        return;
    }
    p->state = PROC_BLOCKED;
    policy->dequeue(p);
    schedule();
    /* Returns when something called sched_wake and a CPU picked this thread
     * again. Only before smp_start_scheduling (no idle thread yet) can it
     * return with the thread still blocked; every caller is written to
     * re-test its condition, so that is a spurious wakeup and not a hang. */
}

void sched_sleep_until(process_t *p, uint64 tick) {
    if (p == NULL) {
        return;
    }
    p->wake_tick = tick;
    sched_block(p);
    p->wake_tick = 0;
}

void sched_wake_sleepers(uint64 now) {
    int i;

    for (i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = proc_at(i);

        if (p != NULL && p->state == PROC_BLOCKED && p->wake_tick != 0 &&
            now >= p->wake_tick) {
            sched_wake(p);
        }
    }
}

/* Is this CPU idle - running its idle thread - and allowed to take `p`? */
static int cpu_idle_for(int cpu, const process_t *p) {
    struct cpu_local *c = smp_cpu(cpu);

    return c != NULL && c->online && c->scheduling && c->idle != NULL &&
           c->current == c->idle && (p->affinity & (1ULL << cpu)) != 0;
}

/* Make sure SOME CPU notices that `p` is runnable.
 *
 * An idle CPU that may run it is best, and it is kicked out of its halt: its
 * last run of `p` first (warm cache), then NT's ideal processor, then any.
 * With none idle, this CPU reschedules at its next return to ring 3 if `p`
 * may run here - which is what preempting the current thread in favour of a
 * woken, interactive one always did on one CPU - and otherwise the CPU `p`
 * last ran on is asked to. */
static void wake_some_cpu(process_t *p) {
    struct cpu_local *me = smp_this_cpu();
    int n = smp_cpu_count();
    int i;

    if (p->cpu >= 0 && p->cpu < n && cpu_idle_for(p->cpu, p)) {
        if (p->cpu == (int)me->index) {
            me->resched = 1;
        } else {
            smp_kick(p->cpu);
        }
        return;
    }
    if (p->ideal_cpu >= 0 && p->ideal_cpu < n && cpu_idle_for(p->ideal_cpu, p)) {
        smp_kick(p->ideal_cpu);
        return;
    }
    for (i = 0; i < n; i++) {
        if (i != (int)me->index && cpu_idle_for(i, p)) {
            smp_kick(i);
            return;
        }
    }
    if (p->affinity & (1ULL << me->index)) {
        me->resched = 1;
    } else if (p->cpu >= 0 && p->cpu < n && p->cpu != (int)me->index) {
        smp_kick(p->cpu);
    } else {
        for (i = 0; i < n; i++) {
            if (p->affinity & (1ULL << i)) {
                smp_kick(i);
                return;
            }
        }
    }
}

void sched_wake(process_t *p) {
    if (p == NULL || p->state != PROC_BLOCKED) {
        return;
    }
    p->wake_tick = 0;
    p->state = PROC_READY;
    policy->enqueue(p);
    wake_some_cpu(p);
}

/* Somewhere to run `p` changed - its affinity was narrowed, or it was
 * killed while running elsewhere. If it is on a CPU that must give it up,
 * that CPU is made to enter the kernel and reschedule. */
void sched_poke(process_t *p) {
    struct cpu_local *me = smp_this_cpu();

    if (p == NULL || !p->oncpu) {
        return;
    }
    if (p->cpu == (int)me->index) {
        me->resched = 1;
    } else {
        smp_kick(p->cpu);
    }
}

/* Leave this CPU now if the current thread may no longer run on it - the
 * second half of narrowing its own affinity, which KeSetSystemAffinityThread
 * and sched_bind promise has taken effect by the time they return. The
 * thread is made visible to a CPU that may run it, and this one switches
 * away; it resumes, still inside this call, on an allowed CPU. Before
 * smp_start_scheduling there is nowhere to go, and it returns in place. */
void sched_migrate_self(void) {
    struct cpu_local *c = smp_this_cpu();
    process_t *me = c->current;

    if (me == NULL || me->is_idle || c->idle == NULL) {
        return;
    }
    while ((me->affinity & (1ULL << smp_cpu_index())) == 0) {
        wake_some_cpu(me);
        smp_this_cpu()->resched = 0;
        schedule();
    }
}

int sched_set_affinity(process_t *p, uint64 mask) {
    mask &= smp_online_mask();
    if (p == NULL || mask == 0) {
        return -22;                         /* -EINVAL: no CPU left */
    }
    p->affinity = mask;
    if (p->oncpu && (mask & (1ULL << p->cpu)) == 0) {
        sched_poke(p);
    }
    return 0;
}

/* Any thread this CPU could run, other than the one it is running? */
static int cpu_has_work(int cpu) {
    process_t *cur = smp_cpu(cpu)->current;
    int i;

    for (i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = proc_at(i);

        if (p != NULL && p != cur && sched_eligible(p, cpu)) {
            return 1;
        }
    }
    return 0;
}

/* --- the idle thread ---------------------------------------------------------
 *
 * Entered with the big kernel lock held (it arrived by a context switch, and
 * the lock travels with those). Loops: under the lock, look for work and
 * switch to it if there is any; otherwise release the lock, halt until an
 * interrupt, and take the lock back.
 *
 * The check and the halt are ordered so a wakeup cannot fall between them:
 * the check runs with interrupts off, the lock is released with them still
 * off, and `sti; hlt` enables them as one pair - so a wake IPI sent after
 * the check is pending at the sti and wakes the hlt rather than being lost. */
void sched_idle_loop(void *arg) {
    struct cpu_local *c = smp_this_cpu();

    (void)arg;
    for (;;) {
        __asm__ volatile ("cli");
        smp_run_deferred();
        if (c->resched || cpu_has_work((int)c->index)) {
            schedule();
            continue;
        }
        bkl_release_all();
        smp_idle_poll_work();
        if (!c->resched) {
            __asm__ volatile ("sti; hlt; cli" : : : "memory");
        }
        /* And again after the wake, before queueing for the lock: work
         * posted by smp_run_on_aps comes from a CPU that is HOLDING the lock
         * and waiting for this one to finish it. */
        smp_idle_poll_work();
        bkl_acquire();
    }
}

int sched_cpu_is_idle(int cpu) {
    struct cpu_local *c = smp_cpu(cpu);

    return c != NULL && c->current != NULL && c->current == c->idle;
}

/* --- FreeBSD's sched_bind / sched_pin (see kernel/bsd/compat/sys/sched.h) ---- */
static uint64 bind_saved[MAX_PROCESSES];
static uint8  bind_on[MAX_PROCESSES];
static int    pin_depth[SMP_MAX_CPUS];

int genesis_sched_bind(int cpu) {
    process_t *me = proc_current();
    int slot;

    if (me == NULL || cpu < 0 || cpu >= smp_cpu_count() ||
        !smp_cpu(cpu)->online) {
        return -1;
    }
    slot = proc_index(me);
    if (!bind_on[slot]) {
        bind_saved[slot] = me->affinity;
        bind_on[slot] = 1;
    }
    /* Only a context that can switch away can move. From an interrupt the
     * current thread is whoever was interrupted, and narrowing ITS affinity
     * would pin a stranger. */
    if (!ksleep_can_block()) {
        return 0;
    }
    if (sched_set_affinity(me, 1ULL << cpu) != 0) {
        return -1;
    }
    sched_migrate_self();
    return 0;
}

void genesis_sched_unbind(void) {
    process_t *me = proc_current();
    int slot;

    if (me == NULL) {
        return;
    }
    slot = proc_index(me);
    if (bind_on[slot]) {
        me->affinity = bind_saved[slot] != 0 ? bind_saved[slot] : ~0ULL;
        bind_on[slot] = 0;
    }
}

int genesis_sched_is_bound(void) {
    process_t *me = proc_current();

    return me != NULL && bind_on[proc_index(me)];
}

void genesis_sched_pin(void) {
    pin_depth[smp_cpu_index()]++;
}

void genesis_sched_unpin(void) {
    int c = smp_cpu_index();

    if (pin_depth[c] > 0) {
        pin_depth[c]--;
    }
}

int genesis_sched_pinned(void) {
    return pin_depth[smp_cpu_index()] > 0;
}
