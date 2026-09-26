#ifndef NET_H
#define NET_H

#include "typesk.h"

/* Genesis's own view of the vendored FreeBSD networking subsystems under
 * kernel/bsd/. Deliberately tiny: everything else in there is reached by
 * including <sys/mbuf.h> the way FreeBSD source does, and that header is
 * only visible to files under kernel/bsd/ (see build.py's EXTRA_INCLUDES).
 *
 * The rest of the kernel talks to mbufs through this file, or it moves into
 * kernel/bsd/ and becomes FreeBSD-shaped. That boundary is on purpose - it
 * is what keeps two vendored trees with two different <sys/param.h> from
 * colliding, and it is why there is no mbuf type mentioned below. */

/* Bring up UMA and the mbuf zones. Must run after kheap_init(), because
 * every zone slab comes out of the kernel heap. Safe to call once. */
void net_mbuf_init(void);

/* Exercise the vendored allocation, cluster and chain paths. Returns the
 * number of failures, 0 on success. Prints its own diagnosis. */
int net_mbuf_selftest(void);

/* Bring up the callout(9) timer wheel. After timer_init(), because it reads
 * the rate the PIT was actually programmed at. Before interrupts are
 * enabled is fine and is where flk.c calls it; net_callout_tick() does
 * nothing until this has run. */
void net_callout_init(void);

/* Advance the wheel and run whatever is due. Called from timer.c's tick
 * handler, in interrupt context - so callout handlers run with interrupts
 * disabled and must be short. */
void net_callout_tick(void);

/* Exercise scheduling, deadline ordering, stop, and a handler that
 * reschedules itself. Spins on real ticks, so it must run with interrupts
 * enabled. Returns the number of failures. */
int net_callout_selftest(void);

/* --- deferred work: condvars and the taskqueue ---------------------------
 *
 * ROADMAP item 7's second blocker. Both became buildable when kernel threads
 * did - a condvar whose wait cannot deschedule is a spin loop with a nicer
 * name, and a taskqueue with no thread runs its task inline, which is what
 * this tree had.
 *
 * genesis_taskqueue_init creates the one queue and starts its servicing
 * kernel thread. Call it after kthread_init(). Everything works before it
 * runs - taskqueue_enqueue falls back to running the task at its call site -
 * so the ordering decides when work starts being deferred, not whether it
 * happens at all.
 *
 * Both selftests need interrupts ENABLED: each waits for a real deadline and
 * for a kernel thread to be scheduled, and neither can happen with the timer
 * masked. */
void genesis_taskqueue_init(void);
void genesis_taskqueue_report(uint8 color);
int  genesis_taskqueue_selftest(void);
int  genesis_condvar_selftest(void);


/* Bring up UMA. Must run before any zone is created, so before
 * net_mbuf_init. Implemented in kernel/bsd/uma_vendor.c, which is where the
 * boot-order reasoning is written down. */
void genesis_uma_init(void);

/* --- the vendored network stack's initialisation -------------------------
 *
 * Three entry points, called in this order from flk.c. See the call site
 * there for why the order matters.
 */

/* Point every per-CPU thread structure at the one process and the one
 * credential. kernel/bsd/netglue.c. */
void genesis_threads_init(void);

/* Link every static SYSCTL_ OID into the MIB tree. kernel/bsd/kern_sysctl.c. */
void net_sysctl_init(void);
int  net_sysctl_count(void);
void net_sysctl_report(uint8 color);

/* Read one OID by dotted name. Returns the byte count the handler produced,
 * or a NEGATIVE errno - Genesis's convention, because this is Genesis's entry
 * point into the tree rather than a vendored one. */
int  net_sysctl_read(const char *name, void *buf, unsigned long len);
int  net_sysctl_read_int(const char *name, int *out);

/* Run every SYSINIT the vendored tree registered, in subsystem order.
 * Idempotent. kernel/bsd/sysinit.c. */
void genesis_sysinit_run(void);
int  genesis_sysinit_count(void);
void genesis_sysinit_report(uint8 color);

/* Which events have subscribers, and how many. kernel/bsd/kern_eventhandler.c. */
void genesis_eventhandler_report(uint8 color);

#endif
