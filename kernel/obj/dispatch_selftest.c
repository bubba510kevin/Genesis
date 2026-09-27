#include "dispatch.h"
#include "kprintf.h"
#include "kthread.h"
#include "ns.h"
#include "object.h"
#include "process.h"
#include "sched.h"
#include "timer.h"
#include "tty.h"
#include "typesk.h"

/* The check ROADMAP item 14 writes down for the dispatcher objects:
 *
 *   "a named event created by one process and opened by name from another,
 *    with the second one BLOCKING until the first signals - a test that only
 *    creates and signals in one process cannot tell a real wait from a
 *    function that returns immediately. Plus the negative: waiting on a
 *    console must answer -EINVAL rather than hanging, and that has to be
 *    asserted or the 'not waitable' path is indistinguishable from a type
 *    nobody has got to yet."
 *
 * Both halves are here. One honest difference from the letter of it: the
 * second context is a KERNEL THREAD, not a second process. Nothing in ring 3
 * can reach these yet - the NT syscalls for them are not written - so a
 * second process would have to be a PE binary and a whole test harness. What
 * is under test is name -> object -> blocking wait, and a kernel thread
 * exercises every step of that. Said plainly rather than left to look like
 * the process version.
 *
 * --- what makes the blocking claim real -----------------------------------
 * Not "the waiter eventually returned". A wait implemented as `return 0`
 * would also do that. The claim is a PAIR, both halves observed from the
 * other context while the waiter is in it:
 *
 *   the waiter is in PROC_BLOCKED, and
 *   this context is running, which it could not be if the waiter had spun
 *
 * and then the waiter has NOT passed its wait until the signal is sent.
 */

#define WAIT_TICKS  300      /* three seconds: a bound, not a delay */
#define SHORT_TICKS 3        /* how long a "this should time out" wait costs */

static int check(int cond, const char *what, int *failures) {
    if (!cond) {
        kprintf_c(0x0C, "dispatch selftest: %s\n", what);
        (*failures)++;
    }
    return cond;
}

/* Yield until `cond` or the deadline. Ticks, not iterations: a yield loop
 * spins through hundreds of passes inside one 10ms tick, so anything waiting
 * on a real timer has to be bounded by the clock that timer runs on. */
#define WAIT_UNTIL(cond) do {                                       \
        uint64 _dl = timer_ticks_now() + WAIT_TICKS;                \
        while (!(cond) && timer_ticks_now() < _dl) {                \
            kthread_yield();                                        \
        }                                                           \
} while (0)

/* --- a thread that dies holding a mutant --------------------------------- */

static object_t     *abandon_mutant;
static volatile int  abandon_taken;

static void abandoner_thread(void *arg) {
    (void)arg;
    /* Taken twice, so the abandonment has a recursion depth to discard. */
    (void)ob_wait(abandon_mutant, 0);
    (void)ob_wait(abandon_mutant, 0);
    abandon_taken = 1;
    /* ...and returns without releasing either: kthread_exit. */
}

/* --- the second context -------------------------------------------------- */

static volatile int  opener_stage;      /* 0 -> 1 (about to wait) -> 2 (woke) */
static volatile int  opener_rc;
static volatile int  opener_found_by_name;
static volatile int  opener_mutant_rc;
static int           opener_pid;

static object_t     *shared_mutant;

static void opener_thread(void *arg) {
    object_t *ev;

    (void)arg;

    /* BY NAME. Not the pointer the creator has - that would test nothing
     * about the namespace, which is the half of this item that exists so two
     * unrelated contexts can agree on an object without sharing anything. */
    ev = dispatch_open_named("GenesisTestEvent");
    opener_found_by_name = (ev != NULL);
    if (ev == NULL) {
        opener_stage = 2;
        return;
    }

    /* Releasing a mutant this thread does not own. Done here rather than in
     * the creator because ownership is per-caller, and a test that "releases
     * something it does not own" from the owning context is not testing
     * anything. */
    if (shared_mutant != NULL) {
        opener_mutant_rc = ob_signal(shared_mutant, OB_SIG_SET, 1, NULL);
    }

    opener_stage = 1;
    opener_rc    = ob_wait(ev, 0);
    opener_stage = 2;
    ob_deref(ev);
}

/* --- the test ------------------------------------------------------------ */

int dispatch_selftest(void) {
    int failures = 0;
    object_t *ev;

    /* --- 1. the NEGATIVE, and its positive half ------------------------
     *
     * A console has no wait slot and must say so. On its own that check
     * cannot tell "correctly refused" from "nothing is waitable yet", which
     * is why the event immediately below it is part of the same assertion. */
    check(ob_wait(tty_console(), 0) == -22,
          "waiting on a console is -EINVAL, not a hang", &failures);
    check(ob_signal(tty_console(), OB_SIG_SET, 1, NULL) == -22,
          "and signalling one is -EINVAL too", &failures);

    ev = event_create(1, 1);               /* notification, already set */
    if (!check(ev != NULL, "an event can be created", &failures)) {
        return failures;
    }
    check(ob_wait(ev, 0) == 0,
          "and waiting on an already-signalled event succeeds - so the "
          "refusal above is a property of the console, not of waiting",
          &failures);

    /* --- 2. notification vs synchronisation ----------------------------
     *
     * The difference is only visible to a SECOND waiter, which is why one
     * wait cannot test it. */
    check(ob_wait(ev, 0) == 0,
          "a NOTIFICATION event stays set - a second wait also succeeds",
          &failures);
    check(ob_signal(ev, OB_SIG_RESET, 1, NULL) == 0, "and can be reset",
          &failures);
    check(ob_wait(ev, timer_ticks_now() + SHORT_TICKS) == -110,
          "after which a wait times out rather than succeeding", &failures);
    ob_deref(ev);

    ev = event_create(0, 1);               /* synchronisation, already set */
    if (ev != NULL) {
        check(ob_wait(ev, 0) == 0,
              "a SYNCHRONISATION event releases one waiter", &failures);
        check(ob_wait(ev, timer_ticks_now() + SHORT_TICKS) == -110,
              "and auto-resets, so the second wait blocks - which is the "
              "whole difference between the two kinds", &failures);
        ob_deref(ev);
    }

    /* --- 3. semaphore: the count, and the refused over-release ---------- */
    {
        object_t *sem = semaphore_create(2, 2);
        int64 prev = -1;

        if (check(sem != NULL, "a semaphore can be created", &failures)) {
            check(ob_wait(sem, 0) == 0, "it hands out its first permit",
                  &failures);
            check(ob_wait(sem, 0) == 0, "and its second", &failures);
            check(ob_wait(sem, timer_ticks_now() + SHORT_TICKS) == -110,
                  "and then it is empty", &failures);

            check(ob_signal(sem, OB_SIG_SET, 1, &prev) == 0,
                  "a permit can be returned", &failures);
            check(prev == 0, "and the previous count is reported", &failures);

            /* THE REFUSAL, and then proof the count did not move. A clamp
             * would pass the first line and fail the second, which is the
             * whole reason both are here: a caller that over-releases has a
             * counting bug, and clamping hides it forever. */
            check(ob_signal(sem, OB_SIG_SET, 5, NULL) == -22,
                  "releasing past the limit is REFUSED, not clamped",
                  &failures);
            check(ob_wait(sem, 0) == 0,
                  "and the refused release left the count alone - one permit",
                  &failures);
            check(ob_wait(sem, timer_ticks_now() + SHORT_TICKS) == -110,
                  "and only one", &failures);

            check(ob_signal(sem, OB_SIG_RESET, 1, NULL) == -22,
                  "and ResetEvent on a semaphore is -EINVAL, not ignored",
                  &failures);
            ob_deref(sem);
        }
    }

    /* --- 4. mutant: recursive, and owned -------------------------------- */
    {
        object_t *m = mutant_create(0);

        if (check(m != NULL, "a mutant can be created", &failures)) {
            shared_mutant = m;

            check(ob_wait(m, 0) == 0, "and taken", &failures);
            /* RECURSION. A semaphore of one would deadlock here, which is
             * exactly why a mutant is not one. */
            check(ob_wait(m, 0) == 0,
                  "and taken AGAIN by its owner - it is recursive",
                  &failures);
            check(ob_signal(m, OB_SIG_SET, 1, NULL) == 0,
                  "released once", &failures);
            check(ob_signal(m, OB_SIG_SET, 1, NULL) == 0,
                  "and once more, which is what the second take requires",
                  &failures);
            check(ob_signal(m, OB_SIG_SET, 1, NULL) == -1,
                  "a third release is -EPERM - it is not held any more",
                  &failures);
        }
    }

    /* --- 4b. mutant abandoned by a thread that died holding it --------- */
    {
        object_t *m = mutant_create(0);

        if (check(m != NULL, "a mutant for the abandonment case", &failures)) {
            process_t *t;

            abandon_mutant = m;
            abandon_taken  = 0;
            t = kthread_create(abandoner_thread, NULL, "disp-abandon");
            check(t != NULL, "a thread to abandon it can be started",
                  &failures);
            WAIT_UNTIL(abandon_taken);
            check(abandon_taken, "which takes it twice", &failures);
            /* Blocking, not a poll: the owner may still be between its
             * second take and its exit. The exit is what releases this. */
            /* Every wait from here on is bounded. If abandonment is broken
             * the dead thread owns this mutant forever, and an unbounded
             * wait would turn a failed check into a boot that never ends. */
            if (check(ob_wait(m, timer_ticks_now() + WAIT_TICKS) == 1,
                      "its death hands the mutant on as ABANDONED (1, not 0)",
                      &failures)) {
                check(ob_wait(m, timer_ticks_now() + SHORT_TICKS) == 0,
                      "and the new owner's recursive take is an ordinary one "
                      "- the mark is reported once", &failures);
                check(ob_signal(m, OB_SIG_SET, 1, NULL) == 0 &&
                      ob_signal(m, OB_SIG_SET, 1, NULL) == 0 &&
                      ob_signal(m, OB_SIG_SET, 1, NULL) == -1,
                      "released twice by the new owner, and no more - the "
                      "dead owner's depth of two went with it", &failures);
            }
            ob_deref(m);
        }
    }

    /* --- 5. the named event, opened from another context ---------------- */
    opener_stage         = 0;
    opener_rc            = -1;
    opener_found_by_name = 0;
    opener_mutant_rc     = 0;

    ev = event_create(0, 0);               /* synchronisation, not signalled */
    if (!check(ev != NULL, "the named event can be created", &failures)) {
        return failures;
    }
    check(dispatch_create_named("GenesisTestEvent", ev) == 0,
          "and named under \\BaseNamedObjects", &failures);
    check(dispatch_create_named("GenesisTestEvent", ev) == -17,
          "and naming it twice is -EEXIST - two contexts racing to create "
          "one named object get one object", &failures);

    {
        /* kthread_create, not the genesis_kthread_spawn bridge: that one
         * exists so files compiled against the vendored FreeBSD headers can
         * reach a kernel thread without seeing process_t, and this file is on
         * the side of the split that can see it. */
        process_t *t = kthread_create(opener_thread, NULL, "disp-open");

        opener_pid = (t != NULL) ? t->pid : 0;
    }
    if (!check(opener_pid != 0, "a second context can be started", &failures)) {
        ob_deref(ev);
        return failures;
    }

    WAIT_UNTIL(opener_stage >= 1);
    if (!check(opener_stage >= 1, "which runs", &failures)) {
        ob_deref(ev);
        return failures;
    }
    check(opener_found_by_name,
          "and finds the event BY NAME, having never been handed a pointer",
          &failures);

    /* The mutant it tried to release is owned by nobody now, but it was never
     * owned by THAT thread - so the refusal is about ownership rather than
     * about the mutant being free. */
    check(opener_mutant_rc == -1,
          "releasing a mutant it does not own is -EPERM from the other "
          "context too", &failures);

    /* THE PAIR. Reaching this line means this context is running, which it
     * could not be if the waiter were spinning; and the waiter's state says
     * it is genuinely descheduled rather than looping. */
    {
        process_t *w = proc_find(opener_pid);

        check(w != NULL && w->state == PROC_BLOCKED,
              "and is BLOCKED in the wait - not spinning, not returned",
              &failures);
        check(opener_stage == 1,
              "and has not passed the wait, because nothing has signalled yet",
              &failures);
    }

    check(ob_signal(ev, OB_SIG_SET, 1, NULL) == 0, "the event is signalled",
          &failures);
    WAIT_UNTIL(opener_stage >= 2);
    check(opener_stage == 2, "which releases the waiter", &failures);
    check(opener_rc == 0,
          "with success rather than a timeout - it was woken, not given up "
          "on", &failures);

    ns_remove("\\BaseNamedObjects\\GenesisTestEvent");
    ob_deref(ev);

    /* --- 6. the namespace describes itself ------------------------------ */
    check(ns_lookup_entry("\\BaseNamedObjects") != NULL,
          "\\BaseNamedObjects exists", &failures);
    check(ns_lookup_entry("\\ObjectTypes\\Event") != NULL,
          "and \\ObjectTypes\\Event - the type list is published", &failures);
    check(ns_lookup_entry("\\ObjectTypes\\Semaphore") != NULL,
          "and Semaphore", &failures);
    check(ns_lookup_entry("\\ObjectTypes\\Mutant") != NULL,
          "and Mutant", &failures);
    check(ns_lookup_entry("\\ObjectTypes\\Type") != NULL,
          "and Type itself, so the list describes its own membership",
          &failures);

    if (failures == 0) {
        kprintf("dispatch: selftest passed\n");
    } else {
        kprintf_c(0x0C, "dispatch: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
