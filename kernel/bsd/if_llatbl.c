/* net/if_llatbl.c, VENDORED WHOLE.
 *
 * The LINK-LAYER ADDRESS TABLE: the per-interface cache mapping a protocol
 * address to a link-layer address, with the state machine (INCOMPLETE ->
 * REACHABLE -> STALE -> DELAY -> PROBE) that decides when an entry is trusted,
 * when it is re-verified, and when it is thrown away.
 *
 * This is what ARP is built ON, and it is the reason kernel/bsd/arp.c existed
 * as a hand-written cache: netinet/if_ether.c cannot be vendored without it.
 * It is also what makes the answer to "is this address resolved" the same
 * answer everywhere, rather than one cache for ARP and another for whatever
 * asks next.
 */

#include "route_prelude.h"

#include <net/if_llatbl.h>
#include <net/if_arp.h>
#include <netinet/if_ether.h>

#include "vendor/if_llatbl.inc"
