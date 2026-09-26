/* The kernel time interface <sys/time.h> declares, backed by Genesis's timer.
 *
 * <sys/time.h> is VENDORED - it was a 75-line Genesis shim providing
 * sbintime_t and four constants, and that stopped being enough the moment the
 * IP layer went in: ip_input.c timestamps received packets with bintime(),
 * nanotime(), microtime() and getboottimebin(), and the tcp and socket code
 * uses the whole set. Writing a second, smaller time API next to the real one
 * would mean every vendored file that touched time had to be edited.
 *
 * So this file is the other half of that decision. Upstream's time.h declares
 * about twenty functions and half a dozen globals; what follows implements
 * them against timer.c, which is the only clock this machine has.
 *
 * --- the resolution, stated once ----------------------------------------
 * timer.c counts PIT ticks at 100Hz. Everything here is therefore accurate to
 * 10 milliseconds and no better, whatever unit it is expressed in. That is
 * genuinely worse than FreeBSD's timecounters, which read a hardware counter
 * (TSC, HPET, ACPI) on every call and interpolate between ticks.
 *
 * It matters where it matters and nowhere else. TCP's retransmit timers are
 * measured in ticks upstream too, so they are unaffected. A packet timestamp
 * is coarse. What would be affected is anything measuring a duration shorter
 * than a tick, and nothing in the network stack does.
 *
 * The bin/nano/micro/get* families are all the same clock here. Upstream
 * splits them because the "get" variants read a cached per-tick value and the
 * others do a live hardware read - a real performance difference there, and
 * no difference at all here, because the cached value IS the only value.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>
/* declares `extern volatile int ticks` and `ticksl`, defined at the bottom of
 * this file - the tick counter and the timecounter globals belong together */
#include <sys/kernel.h>

#include "timer.h"

/* --- the globals sys/time.h declares ----------------------------------- */

/* Seconds since the epoch, and seconds since boot. Upstream updates both from
 * the timecounter's per-tick hardclock; here they are recomputed on read,
 * which is the same value with no update path to get wrong. `volatile` in the
 * declaration is upstream's, because there it really is written by another
 * context. */
volatile time_t time_second = 1;
volatile time_t time_uptime = 1;

/* Set by callout.c at init from timer.h's real programmed rate. Declared here
 * as well as there because sys/time.h declares them extern and
 * <sys/callout.h>'s callout_reset() macro multiplies by tick_sbt directly. */
extern int hz;
extern sbintime_t tick_sbt;

/* `tick` - microseconds per tick, which is 1000000 / hz. A separate global
 * from tick_sbt because a great deal of older code (and kern/subr_counter.c's
 * rate limiter) is written in microseconds rather than sbintime. Set by
 * genesis_time_init() below rather than #define'd, because hz is read from
 * the PIT's real programmed rate and is not a compile-time constant. */
int tick;

struct bintime tick_bt;
struct bintime tc_tick_bt;
sbintime_t tc_tick_sbt;
time_t tick_seconds_max = 0x7FFFFFFF;

/* The thresholds the "get" variants would use to decide whether a cached
 * reading is fresh enough. Nothing here reads them - see the file comment on
 * why get* and non-get* are the same clock - but they are declared extern by
 * the vendored header, so they have to exist. */
int tc_precexp;
int tc_timepercentage = 1;
struct bintime bt_timethreshold;
struct bintime bt_tickthreshold;
sbintime_t sbt_timethreshold;
sbintime_t sbt_tickthreshold;

volatile int rtc_generation = 1;

/* --- conversion ---------------------------------------------------------
 *
 * A struct bintime is {sec, frac} where frac is a 64-bit binary fraction of a
 * second. So nanoseconds convert as ns * 2^64 / 1e9, and the multiply has to
 * be done in a way that does not overflow: ns is under 1e9, and
 * 18446744073709551616 / 1000000000 is 18446744073, which fits.
 */
#define FRAC_PER_NS  18446744073ULL     /* 2^64 / 1e9, truncated */

static void ns_to_bintime(uint64 ns, struct bintime *bt) {
    bt->sec = (time_t)(ns / 1000000000ULL);
    bt->frac = (ns % 1000000000ULL) * FRAC_PER_NS;
}

/* Boot time as a bintime. timer.c holds the epoch the CMOS clock was read at;
 * the wall clock is that plus uptime, so boot time is realtime minus uptime,
 * which is exactly the epoch it was given. Recomputing it that way rather
 * than storing a second copy keeps the two from ever disagreeing. */
static void boottime_bintime(struct bintime *bt) {
    uint64 real = timer_realtime_ns();
    uint64 up = timer_ns();

    ns_to_bintime(real > up ? real - up : 0, bt);
}

/* --- uptime ------------------------------------------------------------- */

void binuptime(struct bintime *bt) {
    ns_to_bintime(timer_ns(), bt);
}

void nanouptime(struct timespec *tsp) {
    uint64 ns = timer_ns();

    tsp->tv_sec = (time_t)(ns / 1000000000ULL);
    tsp->tv_nsec = (long)(ns % 1000000000ULL);
}

void microuptime(struct timeval *tvp) {
    uint64 ns = timer_ns();

    tvp->tv_sec = (time_t)(ns / 1000000000ULL);
    tvp->tv_usec = (suseconds_t)((ns % 1000000000ULL) / 1000ULL);
}

void getbinuptime(struct bintime *bt)     { binuptime(bt); }
void getnanouptime(struct timespec *tsp)  { nanouptime(tsp); }
void getmicrouptime(struct timeval *tvp)  { microuptime(tvp); }

/* --- wall clock --------------------------------------------------------- */

void bintime(struct bintime *bt) {
    ns_to_bintime(timer_realtime_ns(), bt);
}

void nanotime(struct timespec *tsp) {
    uint64 ns = timer_realtime_ns();

    tsp->tv_sec = (time_t)(ns / 1000000000ULL);
    tsp->tv_nsec = (long)(ns % 1000000000ULL);
}

void microtime(struct timeval *tvp) {
    uint64 ns = timer_realtime_ns();

    tvp->tv_sec = (time_t)(ns / 1000000000ULL);
    tvp->tv_usec = (suseconds_t)((ns % 1000000000ULL) / 1000ULL);
}

void getbintime(struct bintime *bt)     { bintime(bt); }
void getnanotime(struct timespec *tsp)  { nanotime(tsp); }
void getmicrotime(struct timeval *tvp)  { microtime(tvp); }

void getboottimebin(struct bintime *boottimebin) {
    boottime_bintime(boottimebin);
}

void getboottime(struct timeval *boottime) {
    struct bintime bt;

    boottime_bintime(&bt);
    bintime2timeval(&bt, boottime);
}

/* --- the pieces the vendored network code actually calls ---------------- */

/* Rate-limit a log message: true if the caller may print, and the timestamp
 * is updated when it may. Used by ip_input.c's "bad options" path and by
 * icmp's error limiter, where a hostile packet stream is exactly the case
 * that must not fill the console.
 *
 * The real one is in kern_time.c upstream and this is the same algorithm:
 * compare against the last permitted time, and reset the clock if it is in
 * the future (which happens when the wall clock is set backwards).
 */
int ratecheck(struct timeval *lasttime, const struct timeval *mininterval) {
    struct timeval tv, delta;
    int rv = 0;

    getmicrouptime(&tv);

    delta = tv;
    timevalsub(&delta, lasttime);

    /* Permit if the interval has elapsed, or if lasttime is unset or in the
     * future - the latter is what makes this safe against a clock that moved
     * backwards, rather than blocking every message until it catches up. */
    if (lasttime->tv_sec == 0 || delta.tv_sec < 0 ||
        timevalcmp(&delta, mininterval, >=)) {
        *lasttime = tv;
        rv = 1;
    }
    return (rv);
}

/* ppsratecheck: the same idea counting EVENTS PER SECOND rather than a
 * minimum gap. maxpps < 0 means unlimited, 0 means blocked - both upstream's
 * conventions, and both relied on by callers that pass a sysctl value. */
int ppsratecheck(struct timeval *lasttime, int *curpps, int maxpps) {
    struct timeval tv, delta;
    int rv;

    if (maxpps == 0) {
        *curpps = *curpps + 1;
        return (0);
    }
    if (maxpps < 0) {
        *curpps = *curpps + 1;
        return (1);
    }

    getmicrouptime(&tv);
    delta = tv;
    timevalsub(&delta, lasttime);

    if (delta.tv_sec > 1 || lasttime->tv_sec == 0) {
        *lasttime = tv;
        *curpps = 0;
    }
    rv = (*curpps < maxpps);
    *curpps = *curpps + 1;
    return (rv);
}

void timevaladd(struct timeval *t1, const struct timeval *t2) {
    t1->tv_sec += t2->tv_sec;
    t1->tv_usec += t2->tv_usec;
    while (t1->tv_usec >= 1000000) {
        t1->tv_sec++;
        t1->tv_usec -= 1000000;
    }
}

void timevalsub(struct timeval *t1, const struct timeval *t2) {
    t1->tv_sec -= t2->tv_sec;
    t1->tv_usec -= t2->tv_usec;
    while (t1->tv_usec < 0) {
        t1->tv_sec--;
        t1->tv_usec += 1000000;
    }
}

/* Convert a timeval to a tick count, rounding UP and never returning 0 -
 * a timeout of "zero ticks" would fire immediately, which is not what any
 * caller asking to wait a positive duration meant. Upstream's tvtohz has the
 * same two properties for the same reason. */
int tvtohz(struct timeval *tv) {
    int64 ticks;

    if (tv->tv_sec < 0 || (tv->tv_sec == 0 && tv->tv_usec <= 0)) {
        return (1);
    }
    ticks = (int64)tv->tv_sec * hz +
            ((int64)tv->tv_usec * hz + 999999) / 1000000;
    if (ticks <= 0) {
        ticks = 1;
    }
    if (ticks > 0x7FFFFFFF) {
        ticks = 0x7FFFFFFF;
    }
    return ((int)ticks);
}

/* Refresh time_second/time_uptime. Called from the tick, so the two globals
 * the vendored code reads directly (rather than through a function) are not
 * stuck at their initial values. */
void genesis_time_tick(void) {
    ticks++;
    ticksl++;
    time_uptime = (time_t)(timer_ns() / 1000000000ULL);
    time_second = (time_t)(timer_realtime_ns() / 1000000000ULL);
}

/* One-time setup of the tick-length constants sys/time.h publishes. Called
 * from callout.c's init, which is where hz and tick_sbt are set. */
void genesis_time_init(void) {
    tick = 1000000 / (hz > 0 ? hz : 100);
    ns_to_bintime(1000000000ULL / (uint64)(hz > 0 ? hz : 100), &tick_bt);
    tc_tick_bt = tick_bt;
    tc_tick_sbt = tick_sbt;
    genesis_time_tick();
}

/* --- `ticks` -------------------------------------------------------------
 *
 * The hz-rate counter <sys/kernel.h> declares. Upstream it is the kernel's
 * cheapest clock: a plain int, incremented by hardclock, wrapping every 25
 * days at 1000Hz (497 days here at 100Hz), and every FreeBSD timeout that is
 * not expressed in sbintime is expressed in this.
 *
 * It existed before this file, as an int in kernel/bsd/uma_vendor.c that
 * NOTHING EVER INCREMENTED. That was harmless while its only reader was UMA's
 * warning rate limiter - a clock stuck at zero makes a rate limiter permanent,
 * not wrong. It stops being harmless the moment TCP is in the tree: every
 * retransmit, persist and keepalive deadline is `ticks + something`, and a
 * frozen clock means a timer that is always in the future.
 *
 * `volatile` and `int` (not int64) are upstream's, and both matter: the type
 * is what the wraparound-safe comparisons in tcp_timer.c are written against.
 */
volatile int ticks;
volatile long ticksl;
