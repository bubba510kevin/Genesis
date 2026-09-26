#ifndef LINUX_JIFFIES_H
#define LINUX_JIFFIES_H

#include "linux/types.h"

/* <linux/jiffies.h>.
 *
 * A jiffy is one timer tick, which here is 10ms - Genesis programs the PIT
 * at 100Hz, so HZ is 100. Linux's own HZ is a config option and driver source
 * is written to read HZ rather than assume a value, so nothing has to change.
 *
 * jiffies is a function call dressed as a variable, via the macro below,
 * because Genesis keeps the tick count in timer.c and a driver reading a
 * copied variable would read a stale one. Driver source writes `jiffies`
 * bare and this is what makes that work.
 */

#define HZ 100

unsigned long linux_jiffies(void);
#define jiffies (linux_jiffies())

/* The comparisons are written as signed subtraction, not as `a > b`, and
 * that is the entire point of them: the tick counter wraps, and a plain
 * comparison across the wrap gives the wrong answer for half the range.
 * Subtracting and testing the sign is correct across a single wrap. */
#define time_after(a, b)       ((long)((b) - (a)) < 0)
#define time_before(a, b)      time_after(b, a)
#define time_after_eq(a, b)    ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b)   time_after_eq(b, a)

#define msecs_to_jiffies(m)  (((unsigned long)(m) * HZ) / 1000)
#define usecs_to_jiffies(u)  (((unsigned long)(u) * HZ) / 1000000)
#define jiffies_to_msecs(j)  (((unsigned long)(j) * 1000) / HZ)
#define jiffies_to_usecs(j)  (((unsigned long)(j) * 1000000) / HZ)

#endif
