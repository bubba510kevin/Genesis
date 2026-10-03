#include "kprintf.h"
#include "ksmp.h"
#include "process.h"
#include "sched.h"
#include "typesk.h"

/* ULE, behind the sched_policy_t interface sched.h was written to accept.
 *
 * sched.h has said since it was written what ULE is actually for here: "what
 * ULE buys on a uniprocessor is its interactivity scoring: the ratio of sleep
 * time to run time over a window, used to promote processes that block
 * often. Its per-CPU run queues and load balancing are inert." That is still
 * true after Part 10 - the APs do not run user processes - so this
 * implements the half that means something and says so about the half that
 * does not.
 *
 * --- the interactivity score --------------------------------------------
 *
 * Upstream's, from sched_ule.c's sched_interact_score(), with upstream's
 * constants:
 *
 *     SCHED_INTERACT_MAX  = 100
 *     SCHED_INTERACT_HALF = 50
 *     SCHED_INTERACT_THRESH = 30
 *
 *   sleep > run:  score = (MAX / (sleep/run + 1)) ...  approaching 0
 *   run > sleep:  score = (MAX / (run/sleep + 1)) + HALF ... approaching 100
 *
 * So LOW is interactive and HIGH is CPU-bound, and a process scoring under
 * SCHED_INTERACT_THRESH is treated as interactive. Keeping upstream's
 * direction matters even though "low is good" reads backwards: a reader who
 * knows ULE should not have to re-derive which way this one runs.
 *
 * --- what is deliberately not here ---------------------------------------
 *
 * No per-CPU run queues and no load balancer. Processes DO run on every CPU
 * now (see ksmp.h), but they share one table-scan queue under the big
 * kernel lock, so there is no per-CPU queue to balance: an idle CPU takes
 * the best eligible thread directly, and sched_eligible() applies affinity.
 * No ithread or realtime bands.
 */

#define SCHED_INTERACT_MAX     100
#define SCHED_INTERACT_HALF    (SCHED_INTERACT_MAX / 2)
#define SCHED_INTERACT_THRESH  30

/* The window the ratio is measured over. Upstream decays the two counters
 * periodically so the score reflects RECENT behaviour rather than a lifetime
 * average - a process that blocked constantly for its first second and has
 * been spinning ever since must not still look interactive. */
#define SCHED_SLICE_TICKS       5    /* 50ms at 100Hz, the CPU-bound slice */
#define SCHED_SLICE_INTERACTIVE 2    /* 20ms - shorter, so it comes back  */
#define SCHED_DECAY_TICKS       200  /* 2s between halvings               */

static uint64 decay_at;
static uint64 tick_count;

/* Statistics, for the report and the selftest. */
static uint64 stat_interactive_picks;
static uint64 stat_batch_picks;

int sched_ule_score(process_t *p) {
    uint64 sleep, run;

    if (p == NULL) {
        return SCHED_INTERACT_MAX;
    }
    sleep = p->sleep_ticks;
    run   = p->run_ticks;

    /* Division by zero is the whole reason for the two branches, and the
     * +1 in each denominator is upstream's, not a fudge. */
    if (sleep > run) {
        if (run == 0) {
            return 0;                       /* never ran - maximally interactive */
        }
        return (int)(SCHED_INTERACT_MAX / ((sleep / run) + 1));
    }
    if (run > sleep) {
        if (sleep == 0) {
            return SCHED_INTERACT_MAX;      /* never slept - maximally CPU-bound */
        }
        return (int)((SCHED_INTERACT_MAX / ((run / sleep) + 1)) +
                     SCHED_INTERACT_HALF);
    }
    /* Equal. Upstream lands on HALF here too. */
    return SCHED_INTERACT_HALF;
}

int sched_ule_is_interactive(process_t *p) {
    return sched_ule_score(p) < SCHED_INTERACT_THRESH;
}

static void ule_enqueue(process_t *p) {
    if (p != NULL && p->state == PROC_BLOCKED) {
        p->state = PROC_READY;
    }
}

static void ule_dequeue(process_t *p) {
    (void)p;   /* readiness is the process's own state, as in round-robin */
}

/* Halve both counters on every process.
 *
 * Halving rather than resetting, and both rather than one: the RATIO is what
 * the score reads, so scaling both by the same factor leaves a steady-state
 * process where it was while letting a change in behaviour move it within a
 * couple of windows. Resetting to zero would make every process look
 * maximally interactive for one window, which is a periodic fairness glitch
 * rather than a decay. */
static void ule_decay(void) {
    int i;

    for (i = 0; i < proc_slots_used(); i++) {
        process_t *t = proc_at(i);

        if (t == NULL) {
            continue;
        }
        t->run_ticks   >>= 1;
        t->sleep_ticks >>= 1;
    }
}

static process_t *ule_pick_next(int cpu) {
    process_t *start = smp_cpu(cpu)->current;
    process_t *best = NULL;
    int best_score = SCHED_INTERACT_MAX + 1;
    int i;

    /* Walk from AFTER the current process, exactly as round-robin does, and
     * keep the lowest score seen. Starting after the current one is what
     * breaks ties round-robin-style: among processes with equal scores - and
     * at boot they all score the same - the one after the current wins, so
     * this degenerates to round-robin rather than to "always run process 0".
     *
     * That degeneration is a feature and is the reason the existing
     * round-robin behaviour test still passes against this policy. */
    int n = proc_slots_used();

    for (i = 1; i <= n; i++) {
        process_t *p = proc_at((proc_index(start) + i) % n);
        int score;

        if (!sched_eligible(p, cpu)) {
            continue;
        }
        score = sched_ule_score(p);
        if (score < best_score) {
            best_score = score;
            best       = p;
        }
    }

    if (best == NULL) {
        return NULL;
    }

    if (best_score < SCHED_INTERACT_THRESH) {
        stat_interactive_picks++;
    } else {
        stat_batch_picks++;
    }
    return best;
}

/* Aging, once per global tick - on the BSP only, so N CPUs do not decay the
 * counters N times as fast. */
static void ule_decay_tick(void) {
    tick_count++;
    if (tick_count >= decay_at) {
        ule_decay();
        decay_at = tick_count + SCHED_DECAY_TICKS;
    }
}

static int ule_tick(process_t *p, int *quantum) {
    if (*quantum > 0) {
        (*quantum)--;
    }
    if (*quantum != 0) {
        return 0;
    }

    /* The slice is chosen when it RUNS OUT, for the process that just used
     * it - so an interactive process gets a short one and comes back to the
     * picker sooner, which is how a low score turns into more frequent
     * scheduling rather than just into being picked first once. */
    *quantum = sched_ule_is_interactive(p) ? SCHED_SLICE_INTERACTIVE
                                           : SCHED_SLICE_TICKS;
    return 1;
}

static const sched_policy_t ule_policy = {
    "ULE", ule_enqueue, ule_dequeue, ule_pick_next, ule_tick, ule_decay_tick
};

const sched_policy_t *sched_ule_policy(void) {
    decay_at     = SCHED_DECAY_TICKS;
    tick_count   = 0;
    return &ule_policy;
}

void sched_ule_report(uint8 color) {
    kprintf_c(color, "sched: ULE picks - interactive %lx, batch %lx\n",
              stat_interactive_picks, stat_batch_picks);
}
