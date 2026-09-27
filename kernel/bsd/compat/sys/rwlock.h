#ifndef GENESIS_BSD_COMPAT_SYS_RWLOCK_H
#define GENESIS_BSD_COMPAT_SYS_RWLOCK_H

/* FreeBSD's rwlock(9) over Genesis's rwlock_t (kernel/mtx.c). uma_core.c
 * uses exactly one, uma_rwlock, guarding the zone list. */

#include "klock.h"
#include <sys/lock.h>

struct rwlock {
    struct lock_object lock_object;
    rwlock_t           grw;
};

/* Padded to a cache line upstream, to stop false sharing between locks that
 * are hot on different CPUs. Genesis does not pad, so this is the same
 * struct under a second name - the padding is what the name describes, not a
 * behavioural difference. */
struct rwlock_padalign {
    struct lock_object lock_object;
    rwlock_t           grw;
};

void genesis_rw_init(struct rwlock *rw, const char *name);
void genesis_rw_init_pad(struct rwlock_padalign *rw, const char *name);

/* _Generic, so one macro serves both struct rwlock and struct
 * rwlock_padalign - uma_core.c initialises uma_rwlock, which is padalign,
 * through the same rw_init name. Casting instead would compile and would
 * silently accept anything. */
#define rw_init(rw, name)     _Generic((rw),                          \
        struct rwlock *:          genesis_rw_init,                     \
        struct rwlock_padalign *: genesis_rw_init_pad)((rw), (name))
#define rw_destroy(rw)        do { } while (0)
#define rw_rlock(rw)          krw_rlock(&(rw)->grw)
#define rw_runlock(rw)        krw_runlock(&(rw)->grw)
#define rw_wlock(rw)          krw_wlock(&(rw)->grw)
#define rw_wunlock(rw)        krw_wunlock(&(rw)->grw)
#define rw_assert(rw, what)   do { } while (0)
#define RA_LOCKED   0
#define RA_RLOCKED  0
#define RA_WLOCKED  0


/* rw_init_flags() and its option bits. The only option any vendored caller
 * passes is RW_NEW ("this lock's memory is freshly allocated, do not expect
 * WITNESS to know it"), which is meaningless without WITNESS - so the flags
 * are accepted and dropped, and the lock is initialised exactly as rw_init
 * would. Upstream's values, so a caller ORing two of them still compiles to
 * something recognisable. */
#define	RW_DUPOK	0x01
#define	RW_NOPROFILE	0x02
#define	RW_NOWITNESS	0x04
#define	RW_QUIET	0x08
#define	RW_RECURSE	0x10
#define	RW_NEW		0x20

#define rw_init_flags(rw, n, opts)   do { (void)(opts); rw_init((rw), (n)); } while (0)

/* RW_SYSINIT declares an rwlock and a SYSINIT that initialises it, the same
 * way MTX_SYSINIT does for a mutex. Upstream's definition; it works because
 * SYSINIT is real (kernel/bsd/sysinit.c). */
#define	RW_SYSINIT_FLAGS(name, rw, desc, flags)				\
	static void name##_rw_sysinit(void *arg __unused)		\
	{								\
		rw_init_flags((rw), (desc), (flags));			\
	}								\
	SYSINIT(name##_rw_sysinit, SI_SUB_LOCK, SI_ORDER_MIDDLE,	\
	    name##_rw_sysinit, NULL)

#define	RW_SYSINIT(name, rw, desc)	RW_SYSINIT_FLAGS(name, rw, desc, 0)

/* rw_sleep - drop this rwlock, sleep on a channel, retake it. Declared here
 * rather than in <sys/systm.h> because the lock type has to be complete.
 * kernel/bsd/kern_synch.c. */
int genesis_rw_sleep(const void *chan, struct rwlock *rw, int pri,
                     const char *wmesg, int timo);
#define rw_sleep(chan, rw, pri, wmesg, timo) \
        genesis_rw_sleep((chan), (rw), (pri), (wmesg), (timo))

/* Non-blocking acquire: true if the lock was taken, false if it was busy.
 * netinet/in_pcb.c uses it to avoid a lock-order reversal - it wants the
 * pcbinfo lock while holding an inpcb lock, and drops back and retries in the
 * other order rather than blocking.
 *
 * The underlying kmtx_trylock is a real try, so this really can fail and the
 * retry path really is exercised. A version that always succeeded would turn
 * that reversal into a deadlock rather than a retry. */
/* Each takes &rw->grw, the Genesis rwlock_t inside the FreeBSD struct rwlock -
 * the same unwrapping every other operation in this header does. */
#define rw_try_rlock(rw)   krw_tryrlock(&(rw)->grw)
#define rw_try_wlock(rw)   genesis_rw_try_wlock(rw)
#define rw_try_upgrade(rw) krw_tryupgrade(&(rw)->grw)
#define rw_downgrade(rw)   krw_downgrade(&(rw)->grw)

/* rw_unlock - release whichever way the caller holds it. Upstream can tell
 * because its rwlock records an owner; krw_unlock reads the reader count
 * instead. It was #define'd to rw_wunlock, which zeroed the count out from
 * under any other reader. */
#define rw_unlock(rw)      krw_unlock(&(rw)->grw)

/* "Do I hold it for writing" - in_pcb.h's INP_WLOCKED, which TCP asks
 * throughout. The rwlock records its writer's CPU for exactly this. */
#define rw_wowned(rw)      krw_wowned(&(rw)->grw)

int genesis_rw_try_wlock(struct rwlock *rw);

#endif
