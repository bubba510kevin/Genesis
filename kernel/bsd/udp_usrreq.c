/* netinet/udp_usrreq.c, VENDORED WHOLE.
 *
 * UDP: udp_input (a datagram arrives, the 4-tuple is looked up in the inpcb
 * hash, the payload is appended to the socket's receive buffer), udp_send
 * (the reverse), the checksum including the pseudo-header, and udp_ctlinput -
 * how an ICMP error about a UDP datagram reaches the socket that sent it.
 *
 * It also carries UDP-Lite, which shares the file upstream and shares it
 * here: netinet/in_proto.c's protosw table names both, so omitting one would
 * mean editing that table.
 *
 * --- what makes this real rather than decorative ------------------------
 * The checksum. UDP's covers a PSEUDO-HEADER - the source and destination
 * addresses and the protocol number, which are in the IP header, not the UDP
 * one - and a datagram whose checksum is computed over the UDP header alone
 * is accepted by nothing. That plus the "a zero checksum means unchecked, and
 * a computed checksum of zero must be transmitted as 0xffff" rule is the
 * whole reason to take upstream's version.
 */

#include "route_prelude.h"

#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/ucred.h>
#include <sys/jail.h>
#include <sys/priv.h>
#include <sys/eventhandler.h>
#include <sys/sdt.h>

#include <net/if_types.h>

#include <netinet/in_pcb.h>
#include <netinet/in_systm.h>
#include <netinet/in_kdtrace.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip_icmp.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>
#include <netinet/udplite.h>

#include "vendor/udp_usrreq.inc"
