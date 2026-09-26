/* netinet/raw_ip.c, VENDORED WHOLE.
 *
 * Raw IP sockets - SOCK_RAW - and, less obviously, the DEFAULT INPUT HANDLER
 * for every IP protocol number nothing else claims. ip_input.c's protocol
 * dispatch falls through to rip_input(), which either delivers the packet to
 * a raw socket that asked for that protocol or answers with an ICMP
 * protocol-unreachable.
 *
 * That second job is why this file is not optional. Without it a datagram for
 * an unimplemented protocol is dropped silently, where a host is supposed to
 * say it does not speak it - and `ip_defttl`, the default TTL every outbound
 * packet gets, is defined here too.
 *
 * It is also what a ping(8) would use: ICMP echo from userland goes through a
 * raw socket, not a special case.
 */

#include "route_prelude.h"

#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/ucred.h>
#include <sys/jail.h>
#include <sys/priv.h>
#include <sys/eventhandler.h>
#include <sys/signalvar.h>

#include <net/if_types.h>
#include <net/route/route_ctl.h>

#include <netinet/in_pcb.h>
#include <netinet/in_systm.h>
#include <netinet/in_kdtrace.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip_mroute.h>

#include "vendor/raw_ip.inc"
