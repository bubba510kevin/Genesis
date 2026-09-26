/* The IPv4 protocol layer, VENDORED.
 *
 * Five files out of vendsrc/sys/netinet, unmodified, compiled as one
 * translation unit because build.py's sources() skips any directory named
 * vendor/ and this is the file that #includes them:
 *
 *   ip_input.c   - a datagram arrives: version, header length and checksum
 *                  checked, options processed, reassembled if fragmented,
 *                  matched against our addresses, and handed to the protocol
 *                  handler for its ip_p.
 *   ip_output.c  - a datagram leaves: source address selection, the route
 *                  lookup, fragmentation when the path MTU is smaller than
 *                  the packet, and the handoff to the interface.
 *   ip_reass.c   - the fragment reassembly queue, with the bucket hashing
 *                  and the drop policy that stop a fragment flood from
 *                  consuming the machine.
 *   ip_icmp.c    - ICMP: echo, and the error messages (unreachable, time
 *                  exceeded, redirect, quench) the rest of the stack sends
 *                  through icmp_error().
 *   ip_id.c      - IP identifier generation, which is a security property
 *                  rather than a counter: a predictable ID leaks the host's
 *                  packet rate and enables idle scanning.
 *   ip_options.c - IP header options: source routing, record route and
 *                  timestamp on the way in, and the option-copying rules
 *                  fragmentation has to follow on the way out.
 *
 * --- what this replaces --------------------------------------------------
 * kernel/bsd/netstack.c had a hand-written 45-line IPv4 receive path: check
 * the header, answer ICMP echo, drop everything else. Its own comment said
 * ip_input.c "does not stand alone - it wants the routing table, in_pcb, the
 * fragment queue and netisr's queueing mode", and that reaching all of that
 * to answer a ping was the wrong trade.
 *
 * That was true when the choice was between a ping and nothing. It stopped
 * being true once the goal was a protocol layer, because every one of those
 * dependencies is something a transport protocol needs anyway. So they are
 * built rather than avoided: see kernel/bsd/route.c for the routing table
 * this sits on, and kernel/bsd/in_addr.c for the interface-address list.
 *
 * --- what is Genesis's, and where --------------------------------------
 * Nothing in this file is a reimplementation of anything above. What follows
 * the #includes is the environment: the globals upstream's other .c files
 * would have defined, and the handful of hooks (firewall, multicast routing,
 * IPsec) that are function pointers upstream and are NULL here - which is
 * exactly the state a GENERIC kernel with those options off runs in.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/rmlock.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/time.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_types.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <net/route/nhop.h>
#include <net/netisr.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_kdtrace.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip_options.h>
#include <netinet/in_pcb.h>

#include <machine/in_cksum.h>

#include "kprintf.h"

#include "vendor/ip_id.inc"
#include "vendor/ip_reass.inc"
#include "vendor/ip_input.inc"
#include "vendor/ip_output.inc"
#include "vendor/ip_options.inc"
#include "vendor/ip_icmp.inc"
