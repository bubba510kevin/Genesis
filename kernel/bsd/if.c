/* The interface layer, VENDORED WHOLE.
 *
 * net/if.c, 5139 lines, unmodified. This is if_alloc/if_attach/if_detach (an
 * interface joins and leaves the system), the interface INDEX TABLE, the
 * address list and its five lookups, the ioctl surface behind ifconfig(8),
 * the multicast address list, if_clone, and the hundred small accessors
 * (if_getsoftc, if_setflags, if_gethwaddr, ...) that modern drivers use
 * instead of touching struct ifnet directly.
 *
 * --- what this replaces --------------------------------------------------
 * kernel/bsd/ifnet.c: a hand-written lifecycle (a fixed pool of eight
 * interfaces, an index table that was an array, an if_attach that built the
 * link-layer address by hand) with the accessors vendored beside it as a
 * fragment. Its own header comment described that split as "what remains
 * here is the lifecycle that net/if.c does through subsystems Genesis has no
 * equivalent of ... written by hand, which is the third tier and only where
 * the first two genuinely could not reach."
 *
 * That was true then and stopped being true. What net/if.c needed and did
 * not have was epoch, CK, and a real sysctl - all three of which the protocol
 * layer had to build anyway. Once they existed, the hand-written half was
 * costing correctness for nothing: it was the file where if_getlladdr()
 * returned a parallel array that nothing ever wrote, which sent ARP requests
 * with an all-zero source MAC and looked exactly like working hardware.
 *
 * --- what is Genesis's, and it is below this comment only ----------------
 * Everything before the #include at the bottom: the globals net/if.c expects
 * another file to define, and the handful of subsystems it calls into that
 * this kernel answers differently (the routing socket, if_clone's rendezvous
 * with devfs, the ifnet departure event). Each is named where it appears.
 */

#define _KERNEL 1

#include <sys/param.h>
/* socket.h BEFORE mbuf.h and systm.h, and the order is load-bearing: struct
 * ifreq embeds a struct sockaddr by value, so net/if.h needs the complete
 * type. Something in the systm/mbuf chain reaches net/if.h first, and the
 * error when it does is "field ifru_addr has incomplete type" in net/if.h -
 * which points at the wrong file entirely. */
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/sockio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/sx.h>
#include <sys/ck.h>
#include <sys/epoch.h>
#include <sys/refcount.h>
#include <sys/sysctl.h>
#include <sys/sbuf.h>
#include <sys/syslog.h>
#include <sys/taskqueue.h>
/* <sys/eventhandler.h> BEFORE <net/if_var.h>, and the order is load-bearing:
 * if_var.h wraps its EVENTHANDLER_DECLAREs in `#ifdef _SYS_EVENTHANDLER_H_`,
 * so a translation unit that has not seen eventhandler.h yet gets the
 * typedefs and not the per-event entry structs - and then EVENTHANDLER_INVOKE
 * fails on an incomplete type with no hint that an include is missing. */
#include <sys/eventhandler.h>
#include <sys/devctl.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/jail.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_arp.h>
#include <net/if_clone.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_media.h>
#include <net/ethernet.h>
#include <net/radix.h>
#include <net/route.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/ip_var.h>

#include "kheap.h"
#include "kprintf.h"
#include "timer.h"

/* net/if_dead.c - the method table an interface is switched to the moment
 * if_detach starts. Every entry returns ENXIO instead of touching hardware
 * that may already be gone, which closes the window between "the driver has
 * begun detaching" and "nothing holds a reference any more". Vendored whole;
 * it is 150 lines and every one of them is a refusal. */
#include "vendor/if_dead.inc"

#include "vendor/if.inc"
