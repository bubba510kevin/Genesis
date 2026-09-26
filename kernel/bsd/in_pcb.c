/* netinet/in_pcb.c, VENDORED WHOLE.
 *
 * The INTERNET PROTOCOL CONTROL BLOCK: the structure that ties a socket to a
 * (local address, local port, foreign address, foreign port) tuple, and the
 * hash tables that turn an arriving packet's tuple back into the socket it
 * belongs to.
 *
 * This is the piece that makes a transport protocol possible at all. UDP and
 * TCP are both, structurally, "find the inpcb for this 4-tuple and hand it
 * the payload"; everything else they do is on top of that. It carries:
 *
 *   - in_pcbbind / in_pcbconnect: port allocation, including the ephemeral
 *     range and the wildcard/specific-address precedence rules that decide
 *     whether two sockets may share a port.
 *   - in_pcblookup: the two-pass lookup - exact 4-tuple first, then wildcard
 *     - with the hash and the reuseport groups.
 *   - the reference counting and the epoch-deferred free that let a lookup
 *     run without excluding a close.
 *
 * None of that is worth writing by hand. The bind rules alone (SO_REUSEADDR
 * versus SO_REUSEPORT versus SO_REUSEPORT_LB, and which combinations are
 * permitted against which existing binding) are a table of special cases that
 * exists because thirty years of applications depend on the exact answers.
 */

#include "route_prelude.h"

#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/sockio.h>
#include <sys/ucred.h>
#include <sys/jail.h>
#include <sys/priv.h>
#include <sys/eventhandler.h>
#include <sys/taskqueue.h>
#include <sys/smr.h>
/* <sys/domainset.h> for vm_ndomains: in_pcb.c sizes its per-domain PCB hash
 * from it. One domain here. */
#include <sys/domainset.h>
#include <sys/refcount.h>

#include <net/if_types.h>
#include <net/if_llatbl.h>

#include <netinet/in_pcb.h>
#include <netinet/in_pcb_var.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/tcp_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>

#include "vendor/in_pcb.inc"
