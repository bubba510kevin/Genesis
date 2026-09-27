#ifndef TIMER_H
#define TIMER_H

#include "typesk.h"

/* The 8253/8254 PIT, channel 0.
 *
 * It free-runs at 18.2Hz out of the BIOS - a 55ms tick, which is far too
 * coarse to schedule against: a quantum that long makes an interactive
 * process wait most of a frame for the CPU, and it puts a 55ms floor under
 * anything measuring elapsed time.
 *
 * 100Hz is the conventional first choice. Higher is smoother and costs more
 * interrupts; 1000Hz on emulated hardware is mostly overhead. The divisor is
 * 16-bit, so the reachable range is about 18.2Hz to 1.19MHz. */

#define TIMER_HZ 100

/* Program channel 0 for `hz` and start it. Call after pic_remap and before
 * interrupts are enabled. */
void timer_init(uint32 hz);

/* Ticks since boot. Wraps after ~5.8 billion years at 100Hz. */
uint64 timer_ticks_now(void);

/* The timer interrupt's two halves: timer_advance is ticks++ alone, run
 * before the big kernel lock so the clock never waits for another CPU;
 * timer_tick is everything else a tick drives (callouts, serial poll). */
void timer_advance(void);
void timer_tick(void);

/* Milliseconds since boot, derived rather than counted separately so the two
 * can never disagree. */
uint64 timer_ms(void);

/* The rate actually programmed, which is not always the rate asked for - the
 * divisor is an integer, so 100Hz becomes 1193182/11932 = 99.998Hz. Exposed
 * because anything converting ticks to time needs the real number, and a
 * second copy of TIMER_HZ elsewhere would be a second copy that is wrong. */
uint32 timer_hz(void);

/* Nanoseconds since boot. The RESOLUTION is one tick - 10ms at 100Hz - and
 * calling it nanoseconds does not change that; the unit is what the callers
 * (clock_gettime, nanosleep) are defined in. A finer clock means the TSC,
 * which means calibrating it against something, and that is its own change. */
uint64 timer_ns(void);

/* The TSC, as a high-resolution counter. timer_calibrate_tsc measures its
 * rate against `pit_ticks` PIT ticks (interrupts must be on); timer_tsc_hz is
 * that rate, or 0 before calibration; timer_tsc is the raw count. What
 * QueryPerformanceCounter and KeQueryPerformanceCounter are built on. */
void   timer_calibrate_tsc(uint32 pit_ticks);
uint64 timer_tsc_hz(void);
uint64 timer_tsc(void);

/* --- wall clock ----------------------------------------------------------
 * The tick counts elapsed time and knows nothing about dates. Boot reads the
 * CMOS clock once and hands the result here; from then on the wall clock is
 * that value plus uptime, so there is exactly one running clock in the kernel
 * and the two can never drift apart.
 *
 * Before this is called - and on a machine whose RTC could not be read - the
 * wall clock is uptime alone, which is to say 1970. That is a lie, but it is
 * a monotonic and self-consistent one, and it is the same lie every Unix told
 * until its clock was set. */
void   timer_set_boot_epoch(uint64 seconds);
uint64 timer_realtime_ns(void);

#endif
