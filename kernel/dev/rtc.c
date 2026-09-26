#include "io.h"
#include "rtc.h"
#include "typesk.h"

#define CMOS_ADDRESS  0x70
#define CMOS_DATA     0x71

#define CMOS_SECONDS  0x00
#define CMOS_MINUTES  0x02
#define CMOS_HOURS    0x04
#define CMOS_DAY      0x07
#define CMOS_MONTH    0x08
#define CMOS_YEAR     0x09
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B

#define STATUS_A_UPDATING  0x80   /* an update is in progress right now     */
#define STATUS_B_24HOUR    0x02   /* hours are 0..23 rather than 1..12 + PM */
#define STATUS_B_BINARY    0x04   /* values are binary rather than BCD      */

static uint8 cmos_read(uint8 reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

/* The clock updates itself once a second, and the registers are inconsistent
 * WHILE it does - the seconds can have rolled over to 0 before the minutes
 * have caught up, which reads as a time a minute in the past, once an hour,
 * forever. Waiting for the update-in-progress flag to clear is necessary and
 * not sufficient: the update can begin between two of the reads below.
 *
 * The standard answer is to read the whole set twice and accept it only when
 * the two agree, which is what this does. The bound on the loop is there
 * because a machine with no RTC at all reads 0xFF from every port, and 0xFF
 * never equals itself in a way that terminates - a kernel that hangs at boot
 * on hardware that merely lacks a clock is a bad trade for a date. */
static int read_stable(uint8 *out) {
    static const uint8 regs[6] = {
        CMOS_SECONDS, CMOS_MINUTES, CMOS_HOURS,
        CMOS_DAY, CMOS_MONTH, CMOS_YEAR
    };
    uint8 first[6];
    int attempt, i;

    for (attempt = 0; attempt < 64; attempt++) {
        int spin = 0;

        while ((cmos_read(CMOS_STATUS_A) & STATUS_A_UPDATING) != 0) {
            if (++spin > 1000000) {
                return 0;
            }
        }
        for (i = 0; i < 6; i++) {
            first[i] = cmos_read(regs[i]);
        }
        while ((cmos_read(CMOS_STATUS_A) & STATUS_A_UPDATING) != 0) {
            if (++spin > 1000000) {
                return 0;
            }
        }
        for (i = 0; i < 6; i++) {
            out[i] = cmos_read(regs[i]);
        }
        for (i = 0; i < 6; i++) {
            if (out[i] != first[i]) {
                break;
            }
        }
        if (i == 6) {
            return 1;
        }
    }
    return 0;
}

static uint32 from_bcd(uint8 v) {
    return (uint32)((v & 0x0F) + ((v >> 4) * 10));
}

/* Days since 1970-01-01, by shifting the year to start in March.
 *
 * With March as month 1, the leap day lands at the END of the year rather
 * than in the middle of it, so no month length depends on whether the year is
 * a leap year and the whole thing is arithmetic with no table and no branch.
 * (153*m + 2)/5 is the exact count of days in the months before m under that
 * shift - the March-to-January month lengths repeat 31,30,31,30,31 in a
 * pattern whose running total that expression reproduces.
 *
 * The era arithmetic handles the century rule for free: 400 years is exactly
 * 146097 days, always, so dividing into 400-year eras removes every special
 * case except the ones inside one era, which the 1461/36524 terms cover. */
int64 rtc_days_from_civil(int64 y, uint32 m, uint32 d) {
    int64 era, yoe, doy, doe;

    y -= (m <= 2) ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                    /* 0..399      */
    doy = (int64)((153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* 0..146096   */
    return era * 146097 + doe - 719468;
}

uint64 rtc_read_epoch(void) {
    uint8 raw[6];
    uint8 status_b;
    uint32 sec, min, hour, day, month, year;
    int pm;
    int64 days;

    if (!read_stable(raw)) {
        return 0;
    }
    status_b = cmos_read(CMOS_STATUS_B);

    /* In 12-hour mode the PM bit rides in the high bit of the hours register,
     * and it has to be taken off BEFORE the BCD conversion - 0x80 | 0x09 is
     * not a valid BCD digit pair, and converting first turns 9 PM into 89. */
    pm = (status_b & STATUS_B_24HOUR) == 0 && (raw[2] & 0x80) != 0;
    raw[2] &= 0x7F;

    if (status_b & STATUS_B_BINARY) {
        sec = raw[0]; min = raw[1]; hour = raw[2];
        day = raw[3]; month = raw[4]; year = raw[5];
    } else {
        sec = from_bcd(raw[0]); min = from_bcd(raw[1]); hour = from_bcd(raw[2]);
        day = from_bcd(raw[3]); month = from_bcd(raw[4]); year = from_bcd(raw[5]);
    }

    if (pm) {
        /* 12 PM is noon and 12 AM is midnight, so the hour wraps to 0 before
         * the 12 is added back. Getting this wrong is a twelve-hour error
         * twice a day and correct the rest of the time. */
        hour = (hour % 12) + 12;
    } else if ((status_b & STATUS_B_24HOUR) == 0) {
        hour = hour % 12;
    }

    /* The century. There is a CMOS register that sometimes holds it, at an
     * offset the ACPI FADT is supposed to name, and reading it without that
     * table is guessing at which byte of NVRAM the firmware chose. The
     * windowing rule below is what every BIOS-era system used and is right
     * for any date from 1970 to 2069, which outlasts the reason this kernel
     * needs a clock. */
    year += (year < 70) ? 2000 : 1900;

    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 60) {
        return 0;                        /* dead battery, or no RTC at all */
    }

    days = rtc_days_from_civil((int64)year, month, day);
    return (uint64)(days * 86400 + (int64)hour * 3600 +
                    (int64)min * 60 + (int64)sec);
}
