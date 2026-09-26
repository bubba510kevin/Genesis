/* The Ethernet layer, VENDORED WHOLE.
 *
 * net/if_ethersubr.c, 1459 lines, unmodified. This is ether_input (a frame
 * arrives from a driver and is classified and dispatched), ether_output (a
 * packet is given a header and handed to the driver), ether_demux (which
 * protocol handler gets it) and ether_ifattach (a driver's interface becomes
 * an Ethernet one).
 *
 * It is the file that turns "a driver that can DMA frames" into "an
 * interface". Vendoring it rather than writing it is the difference between
 * a plausible Ethernet layer and the one that has been debugged against real
 * hardware for thirty years - the VLAN tag handling, the length checks, the
 * multicast classification, the bridge and lagg hooks are all things a
 * hand-written version gets subtly wrong.
 *
 * What it required was the real struct ifnet (see kernel/bsd/ifnet.c), the
 * real mbuf (already vendored), and about thirty compat headers - most
 * vendored, a few adapted, all in kernel/bsd/compat.
 *
 * The .inc convention: build.py's sources() skips any directory named
 * vendor/, so the fragment is compiled here and nowhere else.
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/ck.h>
#include <sys/epoch.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>

#include "kprintf.h"

#include "vendor/if_ethersubr.inc"

/* Run what upstream's SYSINIT would have run.
 *
 * net/if_ethersubr.c ends with SYSINIT(ether, SI_SUB_INIT_IF, ...) calling
 * ether_init, which registers the NETISR_ETHER handler. Genesis has no
 * SYSINIT mechanism, so it is called explicitly from net_stack_init - the
 * same arrangement kernel/bsd/uma_vendor.c's genesis_uma_init uses, and the
 * same "no magic" rule kernel/include/bus.h states.
 *
 * Its absence was invisible in the most misleading way possible: a frame
 * arrived, the driver counted it, ether_input ran and called
 * netisr_dispatch(NETISR_ETHER, m) - and netisr had no handler for that
 * protocol, so the packet was freed. Every counter said the hardware worked
 * and nothing above the driver ever saw a byte.
 *
 * ether_init is static in the vendored file, which is why this wrapper is
 * here rather than in netstack.c: this is the translation unit that has it.
 */
void genesis_ether_init(void) {
    /* TWO SYSINITs, not one, and missing the second looks exactly like
     * missing the first.
     *
     * ether_init registers the NETISR_ETHER handler. vnet_ether_init - a
     * VNET_SYSINIT, which is a separate mechanism upstream - creates the
     * link-layer pfil head that ether_demux tests on every received frame.
     * With it unset, PFIL_HOOKED_IN dereferences NULL and the first packet
     * the interface receives faults in ether_demux. */
    ether_init(NULL);
    vnet_ether_init(NULL);
}
