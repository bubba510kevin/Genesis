/* netinet/igmp.c, VENDORED WHOLE.
 *
 * IGMP: how this host tells the local router which multicast groups it wants,
 * and how it answers the router's queries. Versions 1, 2 and 3, with the
 * per-interface version negotiation and the report timers.
 *
 * Here because netinet/in.c calls igmp_domifattach()/igmp_domifdetach() from
 * in_ifattach()/in_ifdetach(), and netinet/in_mcast.c drives its state
 * machine. Nothing in this tree joins a group beyond the all-hosts one, so
 * what actually runs is the attach, the detach, and answering a query if one
 * arrives.
 */

#include "route_prelude.h"

#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/ktr.h>
#include <sys/taskqueue.h>

#include <net/if_dl.h>
#include <net/if_types.h>

#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip_options.h>
#include <netinet/igmp.h>
#include <netinet/igmp_var.h>

#include "vendor/igmp.inc"
