/* Clock and scheduler stubs for the host suite.
 *
 * waitq.c gained timer_ticks_now and sched_sleep_until when ppoll and
 * nanosleep landed, and run.sh was never told - so the host suite has not
 * linked since. It failed at the LINK step, which is why it went unnoticed:
 * a suite that cannot build prints no failures, and "no failures" and "did not
 * run" look identical from outside.
 *
 * The stubs make the blocking wait a spin, which is correct here for the same
 * reason run.sh already strips the "sti; hlt; cli" out of that loop: every
 * host test feeds a complete input before reading, so nothing ever actually
 * blocks. A test that DID need to block would hang rather than pass, which is
 * the right failure - it is visible.
 */

#include "typesk.h"

struct process;

/* A monotonically advancing tick, one per call.
 *
 * Not a constant. A constant would make any "has the deadline passed" loop in
 * waitq.c spin forever if it ever ran, turning a test that should hang
 * visibly into one that hangs with no output at all. Advancing means such a
 * loop terminates and the test reports something. */
uint64 timer_ticks_now(void) {
    static uint64 ticks;
    return ++ticks;
}

void sched_sleep_until(struct process *p, uint64 tick) {
    (void)p; (void)tick;
    /* Nothing to do: with no scheduler, the caller returns to a spin and
     * timer_ticks_now above guarantees it makes progress. */
}
