/* Host tests for the calendar arithmetic behind the wall clock.
 *
 * The hardware half of rtc.c cannot run here - it is port I/O - and does not
 * need to: the part that goes wrong is the date conversion, and it goes wrong
 * in the way arithmetic does. A leap-year rule that is off by one century
 * produces a date that is plausible for 99 years out of 100. Nothing checks a
 * date; it just shows up in a file listing, once, in 2100.
 *
 * The expected values below are not derived from the same algorithm - that
 * would only prove it agrees with itself. They are the answers to dates whose
 * day counts are fixed points every calendar implementation is checked
 * against: the epoch, the century rules at 1900 and 2000, and the day counts
 * of full years. */

#include <stdio.h>

#include "rtc.h"
#include "typesk.h"

static int rtc_failures;

static void eq(int64 got, int64 want, const char *what) {
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s  (got %lld, wanted %lld)\n", what,
               (long long)got, (long long)want);
        rtc_failures++;
    }
}

int rtc_run_tests(void) {
    rtc_failures = 0;

    printf("\nrtc: days from a civil date\n");

    eq(rtc_days_from_civil(1970, 1, 1), 0, "the epoch itself is day zero");
    eq(rtc_days_from_civil(1970, 1, 2), 1, "the day after is day one");
    eq(rtc_days_from_civil(1969, 12, 31), -1,
       "and the day before is negative, not a huge unsigned number");

    /* A common year is 365 days and a leap year 366. Checking the year
     * BOUNDARIES catches an off-by-one in the day-of-year term that checking
     * mid-year dates does not. */
    eq(rtc_days_from_civil(1971, 1, 1), 365, "1970 was 365 days");
    eq(rtc_days_from_civil(1973, 1, 1), 365 * 3 + 1,
       "1972 was a leap year, so three years is 1096 days");

    /* The century rule, which is the whole reason this is not (y % 4). 1900
     * was NOT a leap year - divisible by 100 and not by 400 - and 2000 WAS.
     * An implementation that gets these backwards is right about every date
     * in living memory. */
    eq(rtc_days_from_civil(1900, 3, 1) - rtc_days_from_civil(1900, 2, 28), 1,
       "1900 had no 29th of February - divisible by 100, not by 400");
    eq(rtc_days_from_civil(2000, 3, 1) - rtc_days_from_civil(2000, 2, 28), 2,
       "2000 did - divisible by 400");
    eq(rtc_days_from_civil(2100, 3, 1) - rtc_days_from_civil(2100, 2, 28), 1,
       "and 2100 will not, which is the case a y%4 rule gets wrong");

    /* 400 years is exactly 146097 days, always. That is the invariant the
     * era arithmetic is built on, so it is worth asserting directly. */
    eq(rtc_days_from_civil(2400, 1, 1) - rtc_days_from_civil(2000, 1, 1),
       146097, "a 400-year era is exactly 146097 days");

    /* Two known-good waypoints, in seconds, as any Unix would report them. */
    eq(rtc_days_from_civil(2000, 1, 1) * 86400, 946684800,
       "2000-01-01 is 946684800 seconds after the epoch");
    eq(rtc_days_from_civil(2026, 8, 5) * 86400, 1785888000,
       "2026-08-05 is 1785888000");

    /* Month ends, where the March-shifted (153*m+2)/5 term does its work. A
     * mistake there lands on one month and no other. */
    eq(rtc_days_from_civil(2026, 2, 1) - rtc_days_from_civil(2026, 1, 1), 31,
       "January has 31 days");
    eq(rtc_days_from_civil(2026, 3, 1) - rtc_days_from_civil(2026, 2, 1), 28,
       "February has 28 in a common year");
    eq(rtc_days_from_civil(2026, 8, 1) - rtc_days_from_civil(2026, 7, 1), 31,
       "July has 31 - the mid-year month a bad table gets wrong");
    eq(rtc_days_from_civil(2027, 1, 1) - rtc_days_from_civil(2026, 12, 1), 31,
       "December has 31, and the year rolls over");

    return rtc_failures;
}
