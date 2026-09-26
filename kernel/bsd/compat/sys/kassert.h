#ifndef _SYS_KASSERT_H_
#define _SYS_KASSERT_H_

#include <sys/systm.h>

/* <sys/kassert.h> - KASSERT and the panic family.
 *
 * Adapted, not vendored: the real one is written against FreeBSD's
 * __dead2/__va_list attribute macros and its full printf-attribute
 * machinery, none of which Genesis's cdefs.h carries.
 *
 * KASSERT is COMPILED OUT, matching a non-INVARIANTS kernel - which is what
 * a GENERIC FreeBSD kernel ships as. The argument still has to parse, so it
 * is consumed by sizeof rather than deleted: an assertion that references a
 * variable used nowhere else must not turn that variable into an unused-
 * variable warning, and must still be a compile error if it stops compiling.
 *
 * panic() is REAL and stops the machine, with a backtrace. That is the
 * important half: vendored network code calls panic on states it considers
 * impossible, and continuing past one of those corrupts something further
 * away. Part 2 of the item 11 pass built ksyms and the frame walker so that
 * stopping here says where it came from.
 */

#ifndef KASSERT
#define KASSERT(exp, msg)   do { (void)sizeof((exp)); } while (0)
#endif
#ifndef MPASS
#define MPASS(exp)          do { (void)sizeof((exp)); } while (0)
#endif
#define KKASSERT(exp)       do { (void)sizeof((exp)); } while (0)

void panic(const char *fmt, ...);

#endif
