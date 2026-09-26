/* netinet/in_mcast.c, VENDORED WHOLE.
 *
 * IPv4 multicast group membership: the in_multi records an interface holds,
 * the socket options (IP_ADD_MEMBERSHIP and friends) that create and destroy
 * them, and the source-filter machinery behind IGMPv3.
 *
 * It is here because netinet/in.c cannot be vendored without it -
 * in_ifattach()/in_ifdetach() join and leave the all-hosts group (224.0.0.1)
 * for every interface, and ip_output.c consults the membership list to decide
 * whether a multicast packet should be looped back.
 *
 * Nothing in this tree JOINS a group of its own accord, so what runs is the
 * all-hosts membership and nothing else. That is not a limitation of the
 * port; it is what a machine with no multicast application does.
 */

#include "route_prelude.h"

#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/tree.h>
#include <sys/ktr.h>
#include <sys/taskqueue.h>

#include <net/if_dl.h>
#include <net/if_types.h>

#include <netinet/in_pcb.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/igmp_var.h>

#include "vendor/in_mcast.inc"
