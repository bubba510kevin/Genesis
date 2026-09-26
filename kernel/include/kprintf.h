#ifndef KPRINTF_H
#define KPRINTF_H

#include "typesk.h"
#include <stdarg.h>

/* Minimal freestanding formatter over print_char/print_string (screen.h).
 *
 * Genesis's convention is to duplicate trivial helpers per file rather than
 * share them, but format-string parsing is real logic, not a trivial helper,
 * and it's needed in the identical shape by three callers at once: the
 * Linux-shaped shim's dev_err/dev_warn/dev_info (kernel/lkpi.c), WDM's
 * DbgPrint-equivalent (kernel/wdm.c), and any kernel code that just wants
 * one line without hand-assembling print_string/print_hex calls. One shared
 * primitive here is the right call.
 *
 * Conversions: %d %u %x %lx %s %%. That is the whole set - no width/
 * precision/padding, no %p, no float. Every caller in this tree needing more
 * than that is a sign kprintf grew past what it's for, not a reason to add
 * a flag parser. */

void kprintf(const char *fmt, ...);

/* Same, explicit color (screen.h's VGA text attribute byte) rather than
 * kprintf's fixed default - dev_err/dev_warn/DbgPrint-equivalent want to
 * pick their own. */
void kprintf_c(uint8 color, const char *fmt, ...);

/* The va_list-taking core both of the above forward to - exposed so a
 * caller that already has its own `...` (DbgPrint, wdm.c) can forward
 * straight through without a second variadic hop. */
void kvprintf(uint8 color, const char *fmt, va_list ap);

#endif
