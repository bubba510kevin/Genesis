#include "cpu.h"
#include "kthread.h"
#include "process.h"
#include "sched.h"
#include "screen.h"
#include "syscall.h"
#include "timer.h"
#include "typesk.h"

/* --- round-robin ---------------------------------------------------------
 * A circular scan over the process table rather than a linked list.
 *
 * The table is 16 entries, so a scan is cheaper than the pointer chasing a
 * list would need, and it cannot develop the failure a list can: a process
 * freed while still enqueued leaves a dangling next pointer, which corrupts
 * the run queue rather than the process. Scanning reads state that is always
 * true by construction.
 *
 * ULE will want a real queue, because its interactivity scoring means the
 * order is not simply "whoever is next in the table". That is a reason to
 * replace this implementation, not this interface. */

#define QUANTUM_TICKS 5     /* 50ms at 100Hz */

static process_t *run_current;
static int quantum_left;
static volatile int resched;

static void rr_enqueue(process_t *p) {
    if (p != NULL && p->state == PROC_BLOCKED) {
        p->state = PROC_READY;
    }
}

static void rr_dequeue(process_t *p) {
    (void)p;   /* nothing to unlink: readiness is the process's own state */
}

static process_t *rr_pick_next(void) {
    process_t *start = proc_current();
    process_t *p;
    int i;

    /* Begin AFTER the current process so a runnable peer is preferred over
     * running the same one again - that is the whole of round-robin's
     * fairness, and starting at index 0 would starve everything behind a
     * process that never blocks. */
    for (i = 1; i <= MAX_PROCESSES; i++) {
        p = proc_at((proc_index(start) + i) % MAX_PROCESSES);
        if (p != NULL && (p->state == PROC_READY || p->state == PROC_RUNNING)) {
            return p;
        }
    }
    if (start != NULL && start->state == PROC_RUNNING) {
        return start;
    }
    return NULL;
}

static int rr_tick(process_t *p) {
    (void)p;
    if (quantum_left > 0) {
        quantum_left--;
    }
    return quantum_left == 0;
}

static const sched_policy_t round_robin = {
    "round-robin", rr_enqueue, rr_dequeue, rr_pick_next, rr_tick
};

static const sched_policy_t *policy = &round_robin;

/* --- mechanism ----------------------------------------------------------- */

void sched_init(process_t *first) {
    policy       = &round_robin;
    run_current  = first;
    quantum_left = QUANTUM_TICKS;
    resched      = 0;
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

void sched_tick(void) {
    process_t *p = proc_current();

    /* Before the null check, not after. A sleeping process is exactly one
     * that is NOT current, so making the wakeups conditional on there being a
     * current process would be a deadlock on the one path where it matters
     * least obviously. */
    sched_wake_sleepers(timer_ticks_now());

    /* Charge the tick.
     *
     * The CURRENT process is charged unconditionally, with no test on its
     * state, and that is the whole correctness argument: being current when
     * the timer fires IS what consumed the tick. p->state is derived
     * bookkeeping, and this kernel already has paths where it disagrees with
     * reality - waitq_wait leaves a process marked PROC_BLOCKED while it goes
     * round its own `sti; hlt; cli` loop, which is exactly why sys_nanosleep
     * has to write PROC_RUNNING back by hand before halting. Gating on
     * `state == PROC_RUNNING` meant those ticks were either dropped or, worse,
     * charged to sleep_ticks for a process that was demonstrably running.
     *
     * This is the only place in the kernel that knows a tick has elapsed AND
     * which process it elapsed for, which is why the accounting lives here
     * rather than in the policy - a second policy would otherwise have to
     * remember to do it, and the one that forgot would score every process
     * identically and look like it was working.
     *
     * Sleep is charged separately, to every OTHER process that is blocked. A
     * blocked process accrues sleep precisely because it is not running, so
     * charging only the current one would leave sleep_ticks at zero for
     * exactly the processes an interactivity score exists to promote. The
     * current process is excluded from that scan whatever its state says,
     * for the reason above: it cannot be both.
     *
     * PROC_READY is charged to NEITHER. A process that is runnable and
     * waiting for a CPU is not sleeping - it has work to do - and counting
     * that as sleep would make a compute-bound process on a busy machine look
     * interactive, which is precisely backwards. It is not running either.
     * The time is real and belongs to a third counter nothing needs yet.
     *
     * A table scan at 100Hz over MAX_PROCESSES entries, for the same reason
     * sched_wake_sleepers is one. */
    if (p != NULL) {
        /* Two counters, one event. run_ticks is ULE's scoring input and gets
         * halved every couple of seconds; cpu_ticks is the accounting total
         * and nothing may touch it. See process.h on why they are separate -
         * they were one field, and the CPU-time clock every process reads
         * halved with the scheduler's heuristic. */
        p->run_ticks++;
        p->cpu_ticks++;
    }
    {
        int i;

        for (i = 0; i < MAX_PROCESSES; i++) {
            process_t *t = proc_at(i);

            if (t == NULL || t == p) {
                continue;
            }
            if (t->state == PROC_BLOCKED) {
                t->sleep_ticks++;
            }
        }
    }

    if (p == NULL) {
        return;
    }
    if (policy->tick(p)) {
        resched = 1;
    }
}

int sched_needs_resched(void) {
    return resched;
}

void schedule(void) {
    process_t *prev = proc_current();
    process_t *next;

    resched = 0;
    quantum_left = QUANTUM_TICKS;

    /* Reclaim any kernel thread that has exited.
     *
     * Here rather than in kthread_exit, because a thread cannot unmap the
     * stack it is standing on: the exiting thread marks itself PROC_ZOMBIE
     * and the reclaim has to happen in somebody else's context. This is the
     * first point in the kernel that is guaranteed to be somebody else - the
     * dead thread is by definition not proc_current() here, and kthread_reap
     * skips proc_current() anyway so the guarantee is asserted rather than
     * assumed.
     *
     * Before pick_next, not after, so a freed slot is not a candidate on the
     * same pass that freed it. */
    kthread_reap();

    next = policy->pick_next();
    if (next == NULL || next == prev) {
        /* Nothing else to run - but the process that carries on running must
         * still be MARKED as running, and for a long time it was not.
         *
         * The path that made this matter: a process blocks (sched_block ->
         * PROC_BLOCKED), something wakes it (sched_wake -> PROC_READY), and
         * it resumes as the only runnable process. schedule() then took the
         * early return above, so the `next->state = PROC_RUNNING` below was
         * never reached, and the process ran on marked PROC_READY - for the
         * rest of its life, since nothing else ever writes that field for a
         * process that does not block again.
         *
         * It stayed invisible because the only consumer was rr_pick_next,
         * which accepts PROC_READY and PROC_RUNNING alike and so could not
         * tell the difference. The first thing to actually READ the state -
         * per-process CPU accounting - measured zero for every process that
         * had ever called wait4, which is every shell and every program that
         * forks.
         *
         * A state field that only one caller reads, and that caller treats
         * two values as equivalent, is a field with no test behind it. */
        if (next != NULL) {
            next->state = PROC_RUNNING;
        }
        return;
    }

    /* The user stack pointer lives in the per-CPU block, which the next
     * thread is about to overwrite. Saving it here rather than in the entry
     * stub keeps the stub free of any knowledge that processes exist.
     *
     * This read is only correct because BOTH entry paths fill that slot: the
     * SYSCALL stub parks it directly, and interrupt_dispatch parks the
     * frame's RSP when the interrupt came from ring 3. Without the second
     * one, a thread preempted by the timer parked whatever thread last made
     * a syscall - see the comment there for why that was invisible. */
    if (prev != NULL) {
        /* A kernel thread has no user RSP to park, and the per-CPU slot it
         * would read holds whatever the last user process left there. Parking
         * that into the kernel thread and handing it back on the way out
         * would be harmless today - the value round-trips - and is skipped
         * anyway, because "the kernel thread is carrying a user process's
         * stack pointer" is exactly the kind of state that stops being
         * harmless the first time something reads it for a different reason.
         *
         * The slot itself is left alone rather than zeroed, so the user
         * process that resumes after this kernel thread gets its own value
         * back from its own saved_user_rsp below. */
        if (!prev->is_kthread) {
            prev->saved_user_rsp = syscall_get_user_rsp();
        }
        if (prev->state == PROC_RUNNING) {
            prev->state = PROC_READY;
        }
    }

    next->state = PROC_RUNNING;
    proc_set_current_raw(next);
    proc_activate_stack(next);
    if (!next->is_kthread) {
        syscall_set_user_rsp(next->saved_user_rsp);
    }

    /* prev is NULL-checked: pick_next can hand back a runnable process when
     * there is no current one at all, and reading prev->space to decide
     * whether CR3 needs to move faulted on exactly that path. */
    if (next->space != NULL && (prev == NULL || next->space != prev->space)) {
        vmm_switch_to(next->space);
    }

    /* The FPU is context too. switch_context saves the callee-saved integer
     * registers and nothing else, so without this the outgoing thread's x87
     * stack, XMM registers, rounding mode and exception mask simply become
     * the incoming thread's - and the failure is not a fault but a wrong
     * number, appearing in a process that did nothing wrong.
     *
     * Safe to do here rather than lazily on first use because the kernel
     * builds with -mno-sse: nothing between these two instructions and the
     * return to ring 3 touches the registers being moved. */
    if (prev != NULL) {
        fpu_save(prev->thread.fpu_state);
    }
    fpu_restore(next->thread.fpu_state);

    /* Everything above is bookkeeping; this is the switch. When it returns,
     * `prev` is running again and every local above is stale - which is why
     * nothing is read after it. */
    {
        /* prev is checked here for the same reason it is checked three times
         * above: pick_next can hand back a runnable process when there is no
         * current one at all. &prev->thread.saved_rsp would then be an
         * address computed off a null pointer, and switch_context WRITES
         * through it - a silent corruption at a fixed low address rather than
         * a fault you could read. There is no outgoing thread to park, so it
         * goes somewhere nobody reads. */
        static uint64 no_outgoing_thread;
        uint64 *park = (prev != NULL) ? &prev->thread.saved_rsp
                                      : &no_outgoing_thread;

        switch_context(park, next->thread.saved_rsp);
    }
}

/* The single place the kernel decides what to do on its way back to user
 * mode.
 *
 * Both entry paths - SYSCALL and an IDT gate - end here before returning, so
 * anything that must happen "just before user code resumes" has exactly one
 * home. Preemption is the only tenant today. Signal delivery is the next one,
 * and it belongs here for the same reason: a pending signal must be delivered
 * on the way out regardless of which way the kernel was entered, and two
 * copies of that check would drift.
 *
 * `to_user` is false when an interrupt landed while the kernel was already
 * running. Switching then would abandon whatever the interrupted kernel code
 * had live on its stack, so the flag stays set and the switch happens on the
 * outermost return instead. */
void return_to_user(int to_user) {
    process_t *me;

    if (!to_user) {
        return;
    }

    /* A process that died while inside the kernel must not be returned to.
     *
     * This is the convergence point for both entry paths, which is the only
     * reason one check is enough. Every way a process can die without
     * unwinding ends up here: a signal whose default action is to terminate,
     * a corrupt signal frame caught by rt_sigreturn, a fault that could not
     * be resolved. Each of those used to mark the process a zombie and then
     * let the syscall or interrupt return anyway, and a zombie returning to
     * ring 3 resumes at whatever the unrestored frame happens to say - which
     * is a second, unrelated-looking fault a few instructions later.
     *
     * A zombie is not runnable, so schedule() never comes back for it. */
    me = proc_current();
    if (me != NULL && me->state == PROC_ZOMBIE) {
        schedule();

        /* Returned, so nothing else was runnable. */
        print_string("\n[nothing left to run]\n", 0x4F);
        __asm__ volatile ("cli");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

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
    /* Returns when something called sched_wake and the scheduler came back
     * around. If schedule() found nothing else runnable, this returns
     * immediately with the process still blocked - which is a deadlock, and
     * the caller has to be written so it cannot happen. kbd_wait handles it
     * by leaving interrupts on and halting instead. */
}

void sched_sleep_until(process_t *p, uint64 tick) {
    if (p == NULL) {
        return;
    }
    p->wake_tick = tick;
    sched_block(p);
    /* Cleared on the way out whatever woke it - a deadline that is still set
     * on a running process is a wakeup waiting to be delivered to something
     * that is not asleep. */
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

void sched_wake(process_t *p) {
    if (p == NULL || p->state != PROC_BLOCKED) {
        return;
    }
    p->wake_tick = 0;
    p->state = PROC_READY;
    policy->enqueue(p);
    resched = 1;
}
