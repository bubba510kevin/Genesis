/* ARP, VENDORED WHOLE.
 *
 * netinet/if_ether.c, unmodified: arpresolve() (the caller wants to send to
 * an IPv4 address and needs a MAC), arprequest(), arpintr()/in_arpinput() (a
 * request or reply arrived), the gratuitous-ARP and proxy-ARP paths, and the
 * garbage-collection of stale entries.
 *
 * --- what this replaces --------------------------------------------------
 * kernel/bsd/arp.c: a hand-written, fixed-size ARP cache. Its own comment
 * said it was written by hand because "netinet/if_ether.c rests on the
 * link-layer table AND the routing table, and vendoring those pulls in
 * rtsock and the socket layer to answer 'what MAC is 10.0.2.2' on a
 * one-subnet machine".
 *
 * That was a correct reading of the cost at the time and it is no longer the
 * cost: net/if_llatbl.c and the whole net/route tree are vendored now,
 * because the IP layer needed them anyway. So this file arrives for free, and
 * "for free" is worth being precise about - what it buys over the
 * hand-written version is everything the hand-written version left out:
 *
 *   - entry EXPIRY and the REACHABLE/STALE/DELAY/PROBE state machine, so a
 *     host that changes its MAC is noticed rather than cached forever.
 *   - the queue of packets waiting on an unresolved address, so the first
 *     packet to a new destination is delivered rather than dropped.
 *   - arp_maxtries / arpt_down, which is what stops an unreachable address
 *     from generating an ARP request per outbound packet forever.
 *   - the duplicate-address detection that logs "arp: %s is using my IP
 *     address" - a real misconfiguration this machine could not previously
 *     see.
 *   - proxy ARP, published entries, and the gratuitous ARP an interface
 *     sends when its address changes.
 */

#include "route_prelude.h"

#include <sys/socketvar.h>
#include <sys/protosw.h>
#include <sys/taskqueue.h>
#include <sys/ktr.h>
#include <sys/eventhandler.h>

#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_llatbl.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <net/netisr.h>
#include <net/bpf.h>

#include <netinet/if_ether.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_carp.h>

#include "vendor/if_ether.inc"
