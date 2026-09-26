#ifndef LINUX_DELAY_H
#define LINUX_DELAY_H

/* <linux/delay.h>.
 *
 * udelay and mdelay BUSY-WAIT, in Linux too - they are for the short waits a
 * device datasheet specifies, and they burn the CPU by design. msleep and
 * ssleep BLOCK in Linux, yielding to the scheduler.
 *
 * Here msleep also blocks, through Genesis's scheduler, but only when there
 * is something to yield to. Called before the scheduler is running - which a
 * driver attached during boot enumeration genuinely is - it falls back to
 * busy-waiting on the timer tick. Getting that backwards would be a hang at
 * boot, so the fallback is in the implementation rather than being a caller's
 * problem to know about. See kernel/driver/lkpi.c.
 *
 * The resolution of anything sleep-shaped is one timer tick, 10ms at the
 * 100Hz this kernel runs the PIT at. msleep(1) therefore sleeps up to 10ms,
 * not 1ms. That is a real limitation - ROADMAP's Owed section already carries
 * "Clock resolution is one tick" - and a driver timing a hardware reset with
 * msleep(1) in a loop will be slower here than on Linux, not faster. udelay
 * is unaffected: it spins on the TSC and is accurate.
 */

void udelay(unsigned long usecs);
void mdelay(unsigned long msecs);
void msleep(unsigned int msecs);
void ssleep(unsigned int secs);
void usleep_range(unsigned long min_us, unsigned long max_us);

#define cpu_relax() __asm__ __volatile__("pause" ::: "memory")

#endif
