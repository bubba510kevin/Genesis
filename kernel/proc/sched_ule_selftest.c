#include "kprintf.h"
#include "process.h"
#include "sched.h"
#include "typesk.h"

/* Part 13's check.
 *
 * The scoring is pure arithmetic over two counters, so it can be tested
 * without scheduling anything - which is the point. Driving real processes
 * into the right sleep/run ratios and then observing which gets picked would
 * be testing the timer, the blocker and the switcher at the same time as the
 * policy, and a failure would not say which.
 *
 * So this sets run_ticks and sleep_ticks directly on real process objects
 * and checks the score, the threshold, and the ORDERING the picker uses.
 * That ordering is the property the plan actually asked for: a
 * frequently-blocking process is preferred over a CPU-bound one.
 */

static int check(int cond, const char *what, int *failures) {
    if (!cond) {
        kprintf_c(0x0C, "ule selftest: %s\n", what);
        (*failures)++;
    }
    return cond;
}

int sched_ule_selftest(void) {
    int failures = 0;
    process_t *a = NULL;
    process_t *b = NULL;
    uint64 a_run, a_sleep, b_run, b_sleep;
    int i;

    /* Two real process objects, whichever two exist. Real ones rather than
     * synthesised structs because sched_ule_score takes a process_t * and a
     * fake one would not catch a field being read that a real process has
     * and a stack copy does not. */
    for (i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = proc_at(i);

        if (p == NULL) {
            continue;
        }
        if (a == NULL) {
            a = p;
        } else if (b == NULL) {
            b = p;
            break;
        }
    }
    if (a == NULL) {
        kprintf_c(0x0E, "ule selftest: no processes exist yet - skipped\n");
        return 0;
    }

    a_run = a->run_ticks;
    a_sleep = a->sleep_ticks;

    /* --- 1. the two extremes -------------------------------------------- */
    a->run_ticks = 100;
    a->sleep_ticks = 0;
    check(sched_ule_score(a) == 100, "a process that never slept did not "
                                     "score 100", &failures);
    check(!sched_ule_is_interactive(a), "a process that never slept was "
                                        "called interactive", &failures);

    a->run_ticks = 0;
    a->sleep_ticks = 100;
    check(sched_ule_score(a) == 0, "a process that never ran did not score 0",
          &failures);
    check(sched_ule_is_interactive(a), "a process that never ran was not "
                                       "called interactive", &failures);

    /* --- 2. the direction is upstream's ---------------------------------
     * LOW is interactive. Stated as its own check because it reads
     * backwards, and a re-implementation that "fixed" it would pass every
     * other test here while inverting the whole policy. */
    a->run_ticks   = 10;
    a->sleep_ticks = 90;
    {
        int mostly_sleeping = sched_ule_score(a);

        a->run_ticks   = 90;
        a->sleep_ticks = 10;
        check(sched_ule_score(a) > mostly_sleeping,
              "scoring is inverted - a CPU-bound process scored LOWER than "
              "a sleepy one", &failures);
    }

    /* --- 3. equal time is the midpoint ----------------------------------- */
    a->run_ticks   = 50;
    a->sleep_ticks = 50;
    check(sched_ule_score(a) == 50, "equal run and sleep did not score 50",
          &failures);

    /* --- 4. monotonic ---------------------------------------------------
     * More sleep relative to run must never raise the score. A scoring
     * function with a discontinuity would still pass the extremes. */
    {
        int prev = -1;
        int bad = 0;

        for (i = 1; i <= 20; i++) {
            int score;

            a->run_ticks   = (uint64)i;
            a->sleep_ticks = 20;
            score = sched_ule_score(a);
            if (prev >= 0 && score < prev) {
                bad = 1;
            }
            prev = score;
        }
        check(!bad, "score is not monotonic as run/sleep rises", &failures);
    }

    /* --- 5. the ordering the plan asked for ------------------------------ */
    if (b != NULL) {
        b_run   = b->run_ticks;
        b_sleep = b->sleep_ticks;

        a->run_ticks   = 5;
        a->sleep_ticks = 95;      /* blocks constantly */
        b->run_ticks   = 95;
        b->sleep_ticks = 5;       /* spins */

        check(sched_ule_score(a) < sched_ule_score(b),
              "the frequently-blocking process did not outrank the "
              "CPU-bound one", &failures);
        check(sched_ule_is_interactive(a) && !sched_ule_is_interactive(b),
              "the threshold did not separate the two", &failures);

        b->run_ticks   = b_run;
        b->sleep_ticks = b_sleep;
    } else {
        kprintf_c(0x0E, "ule selftest: only one process - the ordering check "
                        "needs two and was skipped\n");
    }

    a->run_ticks   = a_run;
    a->sleep_ticks = a_sleep;

    if (failures == 0) {
        kprintf("sched: ULE selftest passed\n");
    } else {
        kprintf_c(0x0C, "sched: ULE selftest FAILED (%d)\n", failures);
    }
    return failures;
}
