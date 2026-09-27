#ifndef SCHED_H
#define SCHED_H

#include "process.h"
#include "typesk.h"

/* Scheduling, behind four functions.
 *
 * The policy is separated from the mechanism on purpose. Round-robin is here
 * because it is simple enough to be obviously correct, which matters when the
 * thing underneath it - context switching - cannot be tested off-target and
 * fails silently. ULE goes behind this same interface later; when it does,
 * a hang is attributable to the policy or the switcher but not to both at
 * once.
 *
 * What ULE actually buys on a uniprocessor is its interactivity scoring: the
 * ratio of sleep time to run time over a window, used to promote processes
 * that block often. Its per-CPU run queues and load balancing are inert here.
 * That scoring needs a time source, which is why the timer landed first. */

typedef struct sched_policy {
    const char *name;

    /* A process became runnable. */
    void (*enqueue)(process_t *p);

    /* A process stopped being runnable - blocked, exited, or was reaped. */
    void (*dequeue)(process_t *p);

    /* Which process should CPU `cpu` run now? NULL means nothing is runnable
     * there, and it runs its idle thread. Must return only a process for
     * which sched_eligible(p, cpu) holds. */
    process_t *(*pick_next)(int cpu);

    /* One timer tick has elapsed for `p` on a CPU whose remaining slice is
     * *quantum. Returns non-zero if `p` has used its slice and should be
     * preempted (and refills *quantum for whatever runs next). */
    int (*tick)(process_t *p, int *quantum);

    /* Once per global tick, for statistics that age (ULE's decay). May be
     * NULL. */
    void (*decay)(void);
} sched_policy_t;

/* Can CPU `cpu` run `p` now: READY and allowed by its affinity mask, or
 * already RUNNING on that very CPU. Never a thread running elsewhere, never
 * an idle thread. Every policy's pick_next filters through this. */
int sched_eligible(const process_t *p, int cpu);

/* The two halves of a timer tick: global (BSP, once per tick - sleepers,
 * sleep accounting, decay) and local (every CPU's own timer - charge the
 * current thread, run down its slice). sched_tick() is both, for the BSP. */
void sched_tick_global(void);
void sched_tick_local(void);

/* A CPU's idle thread body; see sched.c. */
void sched_idle_loop(void *arg);

/* Make the CPU running `p` (if any) reschedule soon - after its affinity
 * changed or it was killed. */
void sched_poke(process_t *p);

/* Restrict `p` to the CPUs in `mask` (intersected with those online).
 * -EINVAL if that leaves none. Moves it off a CPU it may no longer use. */
int sched_set_affinity(process_t *p, uint64 mask);

/* After narrowing the CURRENT thread's affinity: move it off this CPU if it
 * may no longer run here. Returns on an allowed CPU. */
void sched_migrate_self(void);

/* Non-zero if CPU `cpu` is running its idle thread. */
int sched_cpu_is_idle(int cpu);

/* Install round-robin and make `first` the running process. */
void sched_init(process_t *first);

/* --- ULE (Part 13) -------------------------------------------------------
 *
 * Implemented in kernel/sched_ule.c, behind this same interface - which is
 * what this file said would happen, and the reason the interface exists.
 *
 * Selection is a boot-time choice rather than a compile-time one, so both
 * policies stay live and a hang can be bisected by switching. */
const sched_policy_t *sched_ule_policy(void);

/* Swap the policy. Safe only before the first schedule() - it does not
 * migrate any state the outgoing policy was holding, and neither policy
 * holds any that matters (readiness lives in the process, not the queue). */
void sched_set_policy(const sched_policy_t *pol);

/* Which policy is installed, for the boot report. */
const char *sched_policy_name(void);

/* The interactivity score, 0..100, LOW meaning interactive. Upstream's
 * direction, kept deliberately even though it reads backwards - see
 * kernel/sched_ule.c. Exposed for the selftest. */
int sched_ule_score(process_t *p);
int sched_ule_is_interactive(process_t *p);
void sched_ule_report(uint8 color);

/* Exercise the scoring and the picker. Returns the number of failures. */
int sched_ule_selftest(void);

void sched_enqueue(process_t *p);
void sched_dequeue(process_t *p);

/* Called from the timer interrupt. Sets the reschedule flag when the running
 * process has used its quantum; does NOT switch, because switching from
 * inside an interrupt handler needs the return path to cooperate. */
void sched_tick(void);

/* Non-zero if a switch is pending. */
int sched_needs_resched(void);

/* Pick another process and switch to it. Safe only on a kernel stack with
 * nothing live below the caller - which means the syscall exit path and the
 * explicit block below, not the middle of an interrupt handler. */
void schedule(void);

/* Called by both kernel entry paths just before returning. `to_user` is
 * non-zero only when the return actually reaches ring 3 - an interrupt taken
 * while already in the kernel passes 0, because switching would abandon the
 * interrupted code's live stack. */
void return_to_user(int to_user);

/* Give up the CPU until something calls sched_wake on this process. The
 * caller must have arranged for that to be possible before calling. */
void sched_block(process_t *p);

/* Make a blocked process runnable again. Safe from an interrupt handler. */
void sched_wake(process_t *p);

/* Block until `tick`, or until something else wakes the process first.
 *
 * The caller loops on its own deadline rather than trusting one return: a
 * signal wakes a sleeper early and so does a spurious wake, and only the
 * caller knows which of those means "stop sleeping". Same shape as
 * waitq_wait's re-test, and for the same reason.
 *
 * May return with the process still blocked, when nothing else was runnable
 * and schedule() came straight back. The caller has to handle that by waiting
 * for the interrupt itself - see sys_nanosleep. */
void sched_sleep_until(process_t *p, uint64 tick);

/* Wake every sleeper whose deadline has passed. Called once per tick.
 *
 * A table scan rather than a sorted queue. With MAX_PROCESSES in the low tens
 * this is a handful of compares at 100Hz, and a delta queue would be a second
 * structure to keep consistent with process states that are already changed
 * from four places. Worth revisiting when the table is not the scheduler's
 * data structure either. */
void sched_wake_sleepers(uint64 now);

#endif
