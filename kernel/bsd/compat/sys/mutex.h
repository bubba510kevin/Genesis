#ifndef GENESIS_BSD_COMPAT_SYS_MUTEX_H
#define GENESIS_BSD_COMPAT_SYS_MUTEX_H

/* FreeBSD's mtx(9), over Genesis's real spin locks (kernel/mtx.c).
 *
 * This file is why Genesis's own lock header is called klock.h and not
 * sys/mutex.h: kernel/bsd is compiled with -Ikernel/bsd/compat FIRST, so
 * whichever file answers <sys/mutex.h> here wins for every vendored FreeBSD
 * translation unit. Two different files under one name, chosen by directory,
 * is exactly the include-path trap kernel/bsd/README.md documents.
 *
 * Genesis's mtx is a spin lock that disables interrupts. FreeBSD's MTX_DEF
 * is a SLEEPABLE mutex and MTX_SPIN is the spinning one, so this maps both
 * onto spinning - which is correct-but-stricter: a sleepable mutex used as a
 * spin lock is safe, the reverse is not. The cost is that a long hold blocks
 * rather than yields, and UMA's holds are short by construction. */

#include "klock.h"
#include <sys/lock.h>

struct mtx {
    struct lock_object lock_object;
    mtx_t              gmtx;
};

/* Upstream pads some locks to a cache line to stop false sharing between
 * per-domain lists. Genesis does not, and the padding is what the name is
 * about rather than any behavioural difference, so it is the same struct. */
struct mtx_padalign {
    struct lock_object lock_object;
    mtx_t              gmtx;
};

#define MTX_DEF          0x00000000
#define MTX_SPIN         0x00000001
#define MTX_RECURSE      0x00000004
#define MTX_NOWITNESS    0x00000008
#define MTX_DUPOK        0x00000020
#define MTX_NEW          0x00000040

void genesis_mtx_init(struct mtx *m, const char *name, const char *type,
                      int opts);

#define mtx_init(m, name, type, opts)  genesis_mtx_init((struct mtx *)(m), (name), (type), (opts))
#define mtx_destroy(m)                 do { } while (0)
#define mtx_lock(m)                    kmtx_lock(&((struct mtx *)(m))->gmtx)
#define mtx_unlock(m)                  kmtx_unlock(&((struct mtx *)(m))->gmtx)
#define mtx_lock_spin(m)               kmtx_lock(&((struct mtx *)(m))->gmtx)
#define mtx_unlock_spin(m)             kmtx_unlock(&((struct mtx *)(m))->gmtx)
#define mtx_trylock(m)                 kmtx_trylock(&((struct mtx *)(m))->gmtx)
#define mtx_owned(m)                   kmtx_owned(&((struct mtx *)(m))->gmtx)
#define mtx_assert(m, what)            do { } while (0)
#define mtx_initialized(m)             (1)

#define MA_OWNED     0
#define MA_NOTOWNED  0


/* The conventional name a network driver gives its own mutex. Upstream this
 * is just a string constant, used for lock-order checking in WITNESS; there
 * is no WITNESS here, so it names the lock for kmtx_init and nothing more. */
#ifndef MTX_NETWORK_LOCK
#define MTX_NETWORK_LOCK "network driver"
#endif

/* MTX_SYSINIT declares a mutex and a SYSINIT that initialises it before
 * anything can take it. Upstream's definition; it works here because SYSINIT
 * is real (kernel/bsd/sysinit.c) rather than the no-op it used to be. */
#define	MTX_SYSINIT(name, mtx, desc, opts)				\
	static void name##_mtx_sysinit(void *arg __unused)		\
	{								\
		mtx_init((mtx), (desc), NULL, (opts));			\
	}								\
	SYSINIT(name##_mtx_sysinit, SI_SUB_LOCK, SI_ORDER_MIDDLE,	\
	    name##_mtx_sysinit, NULL)

#endif
