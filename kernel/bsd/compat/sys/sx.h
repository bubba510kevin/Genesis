#ifndef GENESIS_BSD_COMPAT_SYS_SX_H
#define GENESIS_BSD_COMPAT_SYS_SX_H

/* sx(9) - FreeBSD's shared/exclusive SLEEPABLE lock, over Genesis's
 * rwlock_t.
 *
 * The difference that matters: an sx lock may be held across a sleep, an
 * rwlock may not. UMA holds uma_reclaim_lock across a reclaim that can block.
 *
 * This used to say Genesis's reclaim path could not block because the reclaim
 * task was never scheduled. Half of that changed: taskqueue_enqueue_timeout
 * is real now (kernel/bsd/kern_taskqueue.c), so uma_timeout DOES run, every
 * UMA_TIMEOUT seconds, on the taskqueue's kernel thread - which is a context
 * that CAN block.
 *
 * What has not changed is that nothing on that path actually blocks:
 * uma_timeout calls bucket_enable and zone_timeout, neither of which sleeps,
 * and uma_reclaim - the one that would - is only reached from vm_lowmem,
 * which nothing in this kernel raises (ROADMAP item 7's fourth blocker). So
 * the mapping is still safe, for a narrower reason than before: not "the
 * caller cannot sleep" but "this particular caller does not". The day
 * something raises vm_lowmem is the day sx has to stop being a spinning
 * rwlock. Recorded rather than assumed. */

#include "klock.h"
#include <sys/lock.h>

struct sx {
    struct lock_object lock_object;
    rwlock_t           grw;
};

void genesis_sx_init(struct sx *sx, const char *name);

#define sx_init(sx, name)          genesis_sx_init((sx), (name))
#define sx_init_flags(sx, n, f)    genesis_sx_init((sx), (n))
#define sx_destroy(sx)             do { } while (0)
#define sx_slock(sx)               krw_rlock(&(sx)->grw)
#define sx_sunlock(sx)             krw_runlock(&(sx)->grw)
#define sx_xlock(sx)               krw_wlock(&(sx)->grw)
#define sx_xunlock(sx)             krw_wunlock(&(sx)->grw)
#define sx_try_xlock(sx)           (krw_wlock(&(sx)->grw), 1)
#define sx_assert(sx, what)        do { } while (0)
/* sx_sleep used to be (0) - "the sleep never happens and the caller
 * immediately re-tests". That is only safe when the condition is guaranteed
 * to become true without anyone waiting for it, and the socket layer's use is
 * not: soclose() sleeps for a connection to drain. It is a real sleep now -
 * see kernel/bsd/kern_synch.c. */
#define sx_sleep(chan, sx, pri, wmesg, timo) \
        genesis_sx_sleep((chan), (sx), (pri), (wmesg), (timo))

/* The interruptible exclusive acquire. There are no signals deliverable to a
 * kernel context here, so nothing can interrupt it and this is the plain
 * form - which is upstream's own behaviour for a thread with no pending
 * signal, rather than a shortcut. */
#define sx_xlock_sig(sx)   (sx_xlock(sx), 0)
#define sx_slock_sig(sx)   (sx_slock(sx), 0)

int genesis_sx_sleep(const void *chan, struct sx *sx, int pri,
                     const char *wmesg, int timo);
#define SX_DUPOK    0
#define SA_XLOCKED  0
#define SA_SLOCKED  0


/* sx_init_flags() option bits. Same story as RW_NEW above: the only one a
 * vendored caller passes is SX_NEW, and it is a WITNESS hint. */
#define	SX_DUPOK		0x01
#define	SX_NOPROFILE		0x02
#define	SX_NOWITNESS		0x04
#define	SX_QUIET		0x08
#define	SX_RECURSE		0x20
#define	SX_NEW			0x40

/* SX_SYSINIT declares a lock and a SYSINIT that initialises it. Upstream's
 * definition, which now works because SYSINIT is real (see
 * kernel/bsd/sysinit.c) - it used to expand to a SYSINIT that expanded to
 * nothing, leaving the lock uninitialised with nothing to say so. */
#define	SX_SYSINIT_FLAGS(name, sxa, desc, flags)			\
	static void name##_sx_sysinit(void *arg __unused)		\
	{								\
		sx_init_flags((sxa), (desc), (flags));			\
	}								\
	SYSINIT(name##_sx_sysinit, SI_SUB_LOCK, SI_ORDER_MIDDLE,	\
	    name##_sx_sysinit, NULL)

#define	SX_SYSINIT(name, sxa, desc)	SX_SYSINIT_FLAGS(name, sxa, desc, 0)

#endif
