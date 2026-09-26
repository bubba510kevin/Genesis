#ifndef GENESIS_BSD_COMPAT_SYS_SLEEPQUEUE_H
#define GENESIS_BSD_COMPAT_SYS_SLEEPQUEUE_H

/* The sleepqueue primitives, and where sleep/wakeup actually live now.
 *
 * This file used to define msleep/wakeup/wakeup_one/pause itself, onto a
 * genesis_msleep() in kernel/bsd/uma_vendor.c that RETURNED IMMEDIATELY - the
 * comment there explains why (a `struct thread` tag collision with Genesis's
 * own process.h stopped it being wired to kernel/proc/waitq.c) and records
 * what was lost: UMA's M_WAITOK path did not block.
 *
 * That was tolerable while UMA was the only caller, because nothing in the
 * tree caps a zone. It is not tolerable with the socket layer in: soclose()
 * sleeps for a connection to drain, sbwait() sleeps for data, and a sleep
 * that returns immediately turns each of those into a busy loop that never
 * makes progress.
 *
 * So the real implementation is kernel/bsd/kern_synch.c and the declarations
 * are in <sys/systm.h> with the rest of sleep(9). This file keeps only the
 * SLEEPQUEUE primitives - the lower-level interface UMA takes directly.
 */

#include <sys/types.h>

/* UMA takes a sleepq lock around its bucket-exhaustion wait, then calls
 * sleepq_wait. With a real sleep available these could be wired to it; they
 * are not, because UMA's use is the one case where the wait genuinely cannot
 * happen (nothing caps a zone) and building a second path to the same place
 * would be two mechanisms for one thing.
 *
 * A lock around a wait that never waits protects nothing, which is why these
 * are honestly empty rather than pretending. */
#define sleepq_lock(wchan)                  do { } while (0)
#define sleepq_release(wchan)               do { } while (0)
#define sleepq_add(w, l, m, f, q)           do { } while (0)
#define sleepq_wait(wchan, pri)             do { } while (0)
#define sleepq_broadcast(w, f, p, q)        (0)
#define sleepq_signal(w, f, p, q)           (0)
#define SLEEPQ_SLEEP    0
#define SLEEPQ_INTERRUPTIBLE 0

#endif
