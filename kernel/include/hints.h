#ifndef HINTS_H
#define HINTS_H

#include "typesk.h"

/* A compiled-in device.hints, plus the kernel environment it doubles as.
 *
 * ROADMAP's NOWILDCARD note said this had to exist before Part 16 could:
 * "hint.foo.0.at=\"pci0\" has to come from somewhere: a compiled-in
 * device.hints, or a kernel environment handed over by the loader. Genesis
 * has neither, and the bootloader passes no environment at all."
 *
 * This is the first of those two. Compiled in rather than read from a file,
 * deliberately: the hints have to be readable BEFORE the bus enumerates
 * anything, and at that point in flk.c there is no filesystem. FreeBSD has
 * exactly the same constraint and solves it exactly this way - a static
 * hints array linked into the kernel, overridden later by the loader if one
 * supplied an environment.
 *
 * The format is upstream's, verbatim, so a real device.hints line can be
 * pasted in:
 *
 *     hint.<driver>.<unit>.<key>="<value>"
 *
 * Genesis reads two keys today: `at` (which bus this device is on) and
 * `disabled`. Everything else parses and is retrievable, so a driver can
 * ask for its own without this file changing.
 */

/* Look up hint.<driver>.<unit>.<key>. Returns the value, or NULL if there is
 * no such hint. The pointer is into the static table and outlives every
 * caller. */
const char *hint_get(const char *driver, int unit, const char *key);

/* Look up a bare environment variable - the whole "name" before the '=',
 * rather than the hint.<driver>.<unit>.<key> decomposition above. This is
 * what kernel/bsd/kern_env.c's kern_getenv() reads, which is in turn what
 * every vendored TUNABLE_*_FETCH resolves to. Same table, same lifetime. */
const char *hint_env(const char *name);

/* Non-zero if hint.<driver>.<unit>.disabled is set to something other than
 * "0". Upstream's convention, and the one thing a hint is most often used
 * for. */
int hint_disabled(const char *driver, int unit);

/* Every hint that names a driver, one line each. */
void hints_report(uint8 color);

/* How many hints are compiled in. */
int hint_count(void);

#endif
