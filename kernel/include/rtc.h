#ifndef RTC_H
#define RTC_H

#include "typesk.h"

/* The CMOS real-time clock, read once at boot.
 *
 * --- Why this exists now ------------------------------------------------
 * musl asks the kernel what time it is and believes the answer. Without a
 * wall clock the only honest value for CLOCK_REALTIME is uptime, which puts
 * every timestamp in 1970 - and the failure that produces is not "the date
 * looks wrong". It is `make` seeing sources newer than the objects it just
 * built, `ls -l` disagreeing with the FAT directory entries the same kernel
 * wrote, and a tar archive that cannot be extracted without warnings. A clock
 * that is wrong by fifty years is worse than one that is absent, because
 * nothing checks it.
 *
 * The RTC is read ONCE, at boot, and the running clock is derived from the
 * timer tick after that. Re-reading it per call would be a pair of port I/O
 * accesses inside every gettimeofday, and would inherit the RTC's one-second
 * resolution besides.
 *
 * --- What it cannot do --------------------------------------------------
 * There is no timezone here and there is not meant to be. The CMOS clock may
 * hold local time or UTC and nothing in the hardware says which; this assumes
 * UTC, which is what a machine configured by a Unix installs to, and is the
 * assumption to revisit if a dual-boot machine reads an hour off. Timezone is
 * a libc concern (TZ and /etc/localtime), not a kernel one. */

/* Read the CMOS clock and convert to seconds since the Unix epoch. Returns 0
 * if the hardware reports something that cannot be a date - which is what a
 * machine with a dead battery does - and the caller then has uptime and
 * nothing else, which is at least an honest zero. */
uint64 rtc_read_epoch(void);

/* Days since 1970-01-01 for a proleptic Gregorian date. Split out from the
 * hardware because it is the half that can be wrong in ways that are hard to
 * see: leap years, the century rule, and the fact that a year starting in
 * March makes the arithmetic branchless. It is pure, so it is tested on the
 * host against dates whose answers are known.
 *
 * y is the full year (2026, not 26), m is 1..12, d is 1..31. */
int64 rtc_days_from_civil(int64 y, uint32 m, uint32 d);

#endif
