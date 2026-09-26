/* The IPv4 address layer and the inet domain, VENDORED.
 *
 *   netinet/in.c       - what it means for an address to belong to this host.
 *                        The in_ifaddr list, in_localip(), in_canforward(),
 *                        in_ifaddr_broadcast(), the SIOCAIFADDR/SIOCDIFADDR
 *                        ioctl handlers that add and remove an address, and
 *                        in_ifattach/in_ifdetach.
 *   netinet/in_proto.c - the inet DOMAIN: the protocol switch table that
 *                        says which handler owns which IP protocol number,
 *                        and the net.inet.* sysctl nodes every other file in
 *                        the stack hangs its own knobs off.
 *
 * --- why these two, and why together ------------------------------------
 * ip_input.c's central question is "is this packet for me", and the answer is
 * a walk of the in_ifaddr hash that in.c owns. There is no smaller version of
 * that: the netstack.c this replaces compared against a single hardcoded
 * address, which cannot express an alias, a broadcast address, or a second
 * interface.
 *
 * in_proto.c comes with it because ip_input.c's protocol dispatch reads
 * inetdomain's protosw table, and because the sysctl nodes net.inet.ip and
 * net.inet.icmp are DEFINED there and merely declared everywhere else - so
 * without it every SYSCTL_INT in ip_input.c and ip_icmp.c is a dangling
 * reference.
 *
 * --- what is stubbed underneath, and why it is honest -------------------
 * in_proto.c's protosw table names tcp, udp, sctp, divert and raw. Only the
 * ones that exist here are filled in; see the protosw definitions below the
 * include for exactly which, and what a packet for a missing one does (it is
 * answered with an ICMP protocol-unreachable by ip_input.c's default case,
 * which is the correct behaviour for a host that does not speak it).
 */

#define _KERNEL 1

/* Upstream's netinet/in.c opens with this, and it has to be here rather than
 * inside the .inc because the .inc is included AFTER the headers it affects.
 * <netinet/in.h> hides IN_CLASSA/B/C from the kernel unless it is set, and
 * in.c is the one kernel file that still needs them - in_socktrim() and
 * in_ifinit() fall back to the classful mask when no netmask is supplied. */
#define IN_HISTORICAL_NETS		/* include class masks */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/rmlock.h>
#include <sys/sysctl.h>
#include <sys/priv.h>
#include <sys/proc.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_types.h>
#include <net/if_dl.h>
#include <net/if_llatbl.h>
#include <net/route.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/in_pcb.h>
#include <netinet/ip_var.h>
#include <netinet/igmp_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>

#include "kprintf.h"

#include "vendor/in.inc"
#include "vendor/in_proto.inc"
