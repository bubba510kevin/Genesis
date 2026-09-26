#include "hints.h"
#include "kprintf.h"
#include "typesk.h"

/* See hints.h. */

/* THE hints. Upstream's device.hints format, one string per line.
 *
 * Parsed at lookup time rather than into a table at boot: there are a
 * handful of them, lookups happen during enumeration and never again, and a
 * parse step would need somewhere to put the result before the allocator is
 * necessarily up. Upstream's own static hints array is a flat string list
 * for the same reason.
 *
 * The entries below are REAL and are what Part 16's selftest reads. A hints
 * file with nothing in it would make every hint lookup return NULL, and a
 * test against that could not tell a working lookup from a missing one. */
static const char *const hints[] = {
    /* The NOWILDCARD demo device, named explicitly so it can be probed by a
     * driver that refuses to be matched by wildcard. See kernel/bus_selftest.c
     * and ROADMAP's NOWILDCARD entry - this line is the whole reason the
     * hint mechanism had to exist before that could be finished. */
    "hint.nowild.0.at=\"pci0\"",

    /* A disabled device, so the disabled path has something to answer for.
     * Nothing claims this name; it exists to be found switched off. */
    "hint.nowild.1.at=\"pci0\"",
    "hint.nowild.1.disabled=\"1\"",

    NULL
};

static int digits_of(int v) {
    int n = 1;

    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

/* Does `s` start with hint.<driver>.<unit>.<key>= ? Returns the offset of
 * the value if so, or 0.
 *
 * Written as one matcher rather than a splitter, because a splitter needs
 * somewhere to put the pieces and this has to work before the heap is
 * necessarily up. */
static uint32 match_prefix(const char *s, const char *driver, int unit,
                           const char *key) {
    static const char lead[] = "hint.";
    uint32 i = 0, k;
    int d, place;

    for (k = 0; lead[k] != '\0'; k++, i++) {
        if (s[i] != lead[k]) {
            return 0;
        }
    }
    for (k = 0; driver[k] != '\0'; k++, i++) {
        if (s[i] != driver[k]) {
            return 0;
        }
    }
    if (s[i++] != '.') {
        return 0;
    }

    /* The unit, as decimal digits. Compared digit by digit rather than by
     * parsing the string's number, so "10" never matches unit 1. */
    d = digits_of(unit);
    for (place = d - 1; place >= 0; place--) {
        int div = 1, p;
        int digit;

        for (p = 0; p < place; p++) {
            div *= 10;
        }
        digit = (unit / div) % 10;
        if (s[i++] != (char)('0' + digit)) {
            return 0;
        }
    }
    if (s[i++] != '.') {
        return 0;
    }
    for (k = 0; key[k] != '\0'; k++, i++) {
        if (s[i] != key[k]) {
            return 0;
        }
    }
    if (s[i++] != '=') {
        return 0;
    }
    return i;
}

/* Values are quoted in the file format. The quotes are stripped into a small
 * static buffer rather than returned with them, because every caller wants
 * the value and none wants the syntax. One buffer is enough: a hint value is
 * consumed immediately by the caller that asked for it. */
static char value_buf[64];

const char *hint_get(const char *driver, int unit, const char *key) {
    int i;

    if (driver == NULL || key == NULL || unit < 0) {
        return NULL;
    }
    for (i = 0; hints[i] != NULL; i++) {
        uint32 at = match_prefix(hints[i], driver, unit, key);
        uint32 k = 0;
        const char *v;

        if (at == 0) {
            continue;
        }
        v = hints[i] + at;
        if (*v == '"') {
            v++;
        }
        while (v[k] != '\0' && v[k] != '"' && k < sizeof(value_buf) - 1) {
            value_buf[k] = v[k];
            k++;
        }
        value_buf[k] = '\0';
        return value_buf;
    }
    return NULL;
}

int hint_disabled(const char *driver, int unit) {
    const char *v = hint_get(driver, unit, "disabled");

    /* Absent means enabled. Present and "0" means enabled - upstream's
     * convention, and worth honouring rather than treating any value as
     * true, because `disabled="0"` is how somebody writes down that they
     * deliberately left it on. */
    return v != NULL && v[0] != '0';
}

/* --- the kernel environment --------------------------------------------
 *
 * The same table, read by name instead of by (driver, unit, key). FreeBSD's
 * static hints array IS its boot-time environment - `hint.foo.0.at` and
 * `net.inet.ip.forwarding` sit side by side in it and kern_getenv() searches
 * the lot - so this is not a second mechanism, it is the other half of the
 * one that was already here.
 *
 * It exists because vendored network source reads tunables through
 * TUNABLE_INT_FETCH at init: <sys/kernel.h>'s macro calls getenv_int(), and
 * without a backing store every tunable silently keeps its compiled-in
 * default. That is the right default in every case here - but "silently"
 * is the part worth removing, because the next vendored file to arrive may
 * have a tunable whose default is not what this machine wants.
 */
const char *hint_env(const char *name) {
    int i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; hints[i] != NULL; i++) {
        const char *s = hints[i];
        uint32 k = 0;
        const char *v;
        uint32 j = 0;

        while (name[k] != '\0' && s[k] == name[k]) {
            k++;
        }
        if (name[k] != '\0' || s[k] != '=') {
            continue;
        }
        v = s + k + 1;
        if (*v == '"') {
            v++;
        }
        while (v[j] != '\0' && v[j] != '"' && j < sizeof(value_buf) - 1) {
            value_buf[j] = v[j];
            j++;
        }
        value_buf[j] = '\0';
        return value_buf;
    }
    return NULL;
}

int hint_count(void) {
    int i = 0;

    while (hints[i] != NULL) {
        i++;
    }
    return i;
}

void hints_report(uint8 color) {
    int i;

    kprintf_c(color, "hints: %d compiled in\n", hint_count());
    for (i = 0; hints[i] != NULL; i++) {
        kprintf_c(color, "  %s\n", hints[i]);
    }
}
