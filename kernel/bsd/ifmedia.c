/* The media-selection layer, VENDORED.
 *
 * net/if_media.c is one of the few pieces of FreeBSD's network stack that
 * genuinely stands alone: it is a list of supported media words, a currently
 * selected one, and the ioctl that reads and writes them. It touches no
 * protocol state, no routing, no mbufs. So unlike net/if.h - where the real
 * header dragged in epoch and altq and had to be shimmed - this one is
 * vendored whole, which is the posture this tree prefers wherever it is
 * available.
 *
 * The .inc convention: upstream's functions are compiled into ONE translation
 * unit, and build.py's sources() skips any directory named vendor/ so the
 * fragment is never compiled on its own. Same arrangement as
 * kernel/bsd/mbuf.c and kernel/zfs/zfs_vendor.c.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/queue.h>
#include <net/if.h>
#include <net/if_media.h>

#include "kprintf.h"

/* Upstream logs media changes under a compile-time debug switch. Off, which
 * is what a GENERIC kernel does. */
#undef IFMEDIA_DEBUG

/* --- what the vendored file needs that Genesis does not already have ----- */

/* The malloc type ifmedia_add tags its entries with. Genesis's malloc(9)
 * ignores the type entirely (see kernel/bsd/compat/sys/malloc.h), so this
 * only has to exist. */
static struct malloc_type genesis_m_ifaddr[1];
#define M_IFADDR genesis_m_ifaddr

/* Two errnos upstream uses that Genesis's reduced sys/errno.h does not
 * carry. EDOOFUS is FreeBSD's own "programming error" code - it really is
 * spelled that way, and if_media.c returns it when a driver asks for a media
 * word it never added. */
#ifndef E2BIG
#define E2BIG   7
#endif
#ifndef EDOOFUS
#define EDOOFUS 88
#endif

/* if_printf lives in kernel/bsd/ifnet.c, where the interface name it
 * prefixes actually is. It was briefly duplicated here as a static, which
 * the compiler caught as soon as the real one was declared in net/if.h. */

/* copyout() used to be a static memcpy here, with a comment saying there was
 * no genuine userland path to this code. There is a REAL one now - declared
 * by <sys/systm.h>, implemented in kernel/bsd/kern_env.c over syscall.c's
 * user_ptr_ok() - and this file has to use it rather than shadow it: a static
 * of the same name is a hard error once the real declaration is in scope, and
 * silently shadowing it would have been worse. */

/* Bandwidth helpers from upstream's net/if.h, which is hand-written here and
 * omitted them - they are only used by if_media.h's descriptor tables. */
#define IF_Kbps(x) ((uint64)(x) * 1000)
#define IF_Mbps(x) (IF_Kbps((x) * 1000))
#define IF_Gbps(x) (IF_Mbps((x) * 1000))

#include "vendor/if_media.inc"
