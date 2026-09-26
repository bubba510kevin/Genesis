#ifndef _NET_DEBUGNET_H_
#define _NET_DEBUGNET_H_

/* <net/debugnet.h> - FreeBSD's kernel debugging network transport (netdump,
 * the debugger over the wire), compiled out.
 *
 * A driver opts in with DEBUGNET_DEFINE(name), which upstream generates a
 * small method table from so the debugger can drive the NIC with interrupts
 * off and no scheduler. There is no kernel debugger here to drive it and no
 * netdump target, so the macro expands to nothing.
 *
 * The declarations a driver's own debugnet methods are written against still
 * have to parse - a driver defines those functions whether or not anything
 * calls them - which is why the types below exist.
 */

#include <net/if.h>

struct debugnet_pcb;
typedef void debugnet_init_t(if_t, int *, int *, int *);
typedef int  debugnet_event_t(if_t, int);
typedef int  debugnet_transmit_t(if_t, struct mbuf *);
typedef void debugnet_poll_t(if_t, int);

#define DEBUGNET_DEFINE(driver)
#define DEBUGNET_SET(ifp, driver)  do { (void)(ifp); } while (0)
#define DEBUGNET_NOTIFY_MBUF(ifp)  do { (void)(ifp); } while (0)
/* net/if.c fires this when an interface's MTU changes, so a debugnet session
 * can resize its buffers. Same story as the mbuf notification above: there is
 * no debugnet, so there is nothing to tell. */
#define DEBUGNET_NOTIFY_MTU(ifp)   do { (void)(ifp); } while (0)

/* The event codes a driver's debugnet event method switches on. */
#define DEBUGNET_START   0
#define DEBUGNET_END     1

#endif
