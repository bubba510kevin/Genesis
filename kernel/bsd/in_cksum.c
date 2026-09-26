/* The IP checksum, VENDORED WHOLE from netinet/in_cksum.c.
 *
 * The one part of the IP layer that is real here, and the easiest vendoring
 * decision in the network stack: it is arithmetic over an mbuf chain with no
 * dependencies beyond the mbuf itself, and it compiled with zero errors on
 * the first attempt.
 *
 * Worth vendoring rather than writing for a specific reason: the one's-
 * complement sum has to handle an odd-length segment in the middle of a
 * chain, where the byte that pairs with the last byte of one mbuf is the
 * first byte of the next. Every hand-rolled version gets that wrong, and the
 * symptom is packets that validate on some paths and not others.
 */
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <machine/in_cksum.h>

#include "vendor/in_cksum.inc"
