/* Network stack bring-up: what /etc/rc.conf and ifconfig would do.
 *
 * --- what this file is NOT any more --------------------------------------
 * It used to be a 45-line hand-written IPv4 receive path with its own ARP
 * cache underneath it, because netinet/ip_input.c "wants the routing table,
 * in_pcb for delivery to sockets, the fragment reassembly queue, and netisr's
 * queueing mode", and reaching all of that to answer an ICMP echo was the
 * wrong trade.
 *
 * All of that is here now - see kernel/bsd/ip.c, kernel/bsd/route*.c,
 * kernel/bsd/in.c and kernel/bsd/if_ether.c - so this file no longer
 * implements any protocol. What is left is CONFIGURATION: run the
 * initialisers, give the interface an address, add a default route, and bring
 * it up. On a FreeBSD machine every one of those is a line in rc.conf and a
 * fork of ifconfig(8) or route(8); there is no userland here to run them, so
 * they are calls.
 *
 * Each one goes through the SAME entry point the userland tool would use -
 * in_control(SIOCAIFADDR) for the address, rib_add_default_route() for the
 * route, if_init for the up - rather than reaching into a structure. That is
 * the point: if the ioctl path is wrong, this finds out at boot rather than
 * the first time something tries to use it.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/sockio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/ck.h>
#include <sys/epoch.h>
#include <sys/proc.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/netisr.h>
#include <net/route.h>
#include <net/route/route_ctl.h>
#include <net/route/nhop.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/in_systm.h>
#include <netinet/in_fib.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip_var.h>
#include <netinet/icmp_var.h>
#include <machine/in_cksum.h>

#include "kprintf.h"
#include "netstack.h"

VNET_DECLARE(struct ifnethead, ifnet);
#define V_ifnet_list  VNET(ifnet)

/* QEMU user-mode networking's defaults. The guest is .15, the gateway and DNS
 * are .2 and .3, and the host is reachable at .2.
 *
 * Compiled in because there is no DHCP client. That is the one piece of
 * configuration this machine cannot discover, and it is the reason these are
 * three constants rather than something learned - see netstack.h. */
#define MY_IP    0x0F02000AU        /* 10.0.2.15, network byte order */
#define MY_MASK  0x00FFFFFFU        /* /24                           */
#define GW_IP    0x0202000AU        /* 10.0.2.2                      */

/* The interface everything above is configured on. There is one NIC; this is
 * the pointer to it, kept so the report and the ping can find it without
 * walking the list again. */
static struct ifnet *net_ifp;
static uint64 stat_pings_sent;

struct ifnet *net_interface(void) {
    return (net_ifp);
}

uint32_t net_my_addr(void) {
    return (MY_IP);
}

uint64 net_stat_rx_frames(void) {
    return (net_ifp == NULL ? 0
            : if_getcounter(net_ifp, IFCOUNTER_IPACKETS));
}

uint64 net_stat_icmp_echoes(void) {
    /* icmpstat is a counter ARRAY (see net/vnet.h's VNET_PCPUSTAT), indexed
     * by field offset. icps_reflect counts ICMP messages this host generated
     * in reply to one it received - which, for a machine that originates
     * nothing else, is the echo replies it sent. */
    return (VNET_PCPUSTAT_FETCH(struct icmpstat, icmpstat, icps_reflect));
}

/* --- address configuration ---------------------------------------------
 *
 * SIOCAIFADDR, exactly as `ifconfig re0 inet 10.0.2.15/24` issues it. The
 * request carries three sockaddrs - the address, the netmask and the
 * broadcast address - and in_control() does the rest: allocates the
 * in_ifaddr, links it into the interface's address list and the global
 * IN_IFADDR hash, adds the interface route for the /24, joins the all-hosts
 * multicast group, and hands the address to the link-layer table so ARP can
 * answer for it.
 *
 * A null thread is passed for the credential. in_control() reads td->td_ucred
 * for its privilege check and accepts NULL as "kernel", which is what this
 * is.
 */
static int configure_address(struct ifnet *ifp, uint32_t addr, uint32_t mask) {
    struct in_aliasreq ifra;
    struct sockaddr_in *sin;

    memset(&ifra, 0, sizeof(ifra));
    strncpy(ifra.ifra_name, ifp->if_xname, sizeof(ifra.ifra_name) - 1);

    sin = &ifra.ifra_addr;
    sin->sin_len = sizeof(*sin);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = addr;

    sin = (struct sockaddr_in *)&ifra.ifra_mask;
    sin->sin_len = sizeof(*sin);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = mask;

    /* A broadcast address only on an interface that has broadcast; lo0 is
     * point-to-self and gets none, as `ifconfig lo0 127.0.0.1/8` gives it. */
    if ((ifp->if_flags & IFF_BROADCAST) != 0) {
        sin = &ifra.ifra_broadaddr;
        sin->sin_len = sizeof(*sin);
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = (addr & mask) | ~mask;
    }

    return (in_control(NULL, SIOCAIFADDR, (caddr_t)&ifra, ifp, NULL));
}

/* --- the default route --------------------------------------------------
 *
 * `route add default 10.0.2.2`. rib_add_default_route() is upstream's own
 * helper for exactly this, and it does the part that is easy to get wrong:
 * resolving the gateway to an interface, creating (or finding an existing,
 * shared) nexthop object for it, and installing a route with a zero-length
 * prefix so it loses to every more specific match.
 */
static int configure_default_route(struct ifnet *ifp) {
    struct sockaddr_in gw;
    struct rib_cmd_info rc;

    memset(&gw, 0, sizeof(gw));
    gw.sin_len = sizeof(gw);
    gw.sin_family = AF_INET;
    gw.sin_addr.s_addr = GW_IP;

    memset(&rc, 0, sizeof(rc));
    return (rib_add_default_route(RT_DEFAULT_FIB, AF_INET, ifp,
                                  (struct sockaddr *)&gw, &rc));
}

/* --- an ICMP echo request, sent from here -------------------------------
 *
 * Built and sent through the VENDORED ip_output(), so the transmit path this
 * exercises is the one every other protocol would use: source address
 * selection, the route lookup, the nexthop, arpresolve, ether_output.
 *
 * The reply is not correlated - there is no socket to deliver it to. What
 * proves it arrived is icmpstat's icps_inhist[ICMP_ECHOREPLY], which
 * ip_icmp.c increments and net_stack_report() prints.
 */
int net_ping(uint32 dst_be) {
    struct mbuf *m;
    struct icmp *ic;
    struct ip *ih;
    const int icmplen = 8 + 8;      /* header plus a little payload */
    int i;

    if (net_ifp == NULL) {
        return (ENXIO);
    }
    m = m_gethdr(M_NOWAIT, MT_DATA);
    if (m == NULL) {
        return (ENOBUFS);
    }
    m->m_len = m->m_pkthdr.len = (int)sizeof(struct ip) + icmplen;
    memset(mtod(m, void *), 0, (size_t)m->m_len);

    ic = (struct icmp *)(mtod(m, char *) + sizeof(struct ip));
    ic->icmp_type = ICMP_ECHO;
    ic->icmp_code = 0;
    ic->icmp_id   = htons(0x4753);      /* "GS" */
    stat_pings_sent++;
    ic->icmp_seq  = htons((uint16_t)stat_pings_sent);
    for (i = 8; i < icmplen; i++) {
        ((unsigned char *)ic)[i] = (unsigned char)i;
    }
    /* The ICMP checksum covers the ICMP message only - not the IP header,
     * which has its own, and not a pseudo header, which is what TCP and UDP
     * add. Three different rules in one packet is why this is worth saying
     * rather than assuming. */
    ic->icmp_cksum = in_cksum_skip(m, (int)sizeof(struct ip) + icmplen,
                                   (int)sizeof(struct ip));

    /* ip_output fills in the version, header length, identifier and checksum
     * from these fields plus the route it finds.
     *
     * ip_len goes in NETWORK byte order, and getting that backwards is not a
     * quiet mistake: ip_output reads it as `ntohs(ip->ip_len)` on its very
     * first line, so a host-order 36 reads back as 9216, which is larger than
     * the 1500-byte MTU, so a 36-byte ping was handed to ip_fragment() - which
     * then walked off the end of a one-mbuf chain in m_copym. The fault was
     * three frames deep in fragmentation code for a packet that could not
     * possibly need fragmenting. */
    ih = mtod(m, struct ip *);
    ih->ip_v   = 4;
    ih->ip_hl  = sizeof(struct ip) >> 2;
    ih->ip_len = htons((u_short)(sizeof(struct ip) + icmplen));
    ih->ip_ttl = 64;
    ih->ip_p   = IPPROTO_ICMP;
    ih->ip_src.s_addr = MY_IP;
    ih->ip_dst.s_addr = dst_be;

    return (ip_output(m, NULL, NULL, 0, NULL, NULL));
}

/* lo0: `ifconfig lo0 inet 127.0.0.1/8 up`, as rc.d/netif does before any
 * other interface. net/if_loop.c created lo0 at SYSINIT time; this gives it
 * its address, which also installs the 127/8 interface route every
 * connection to 127.0.0.1 is looked up through. */
static void configure_loopback(void) {
    int error;

    if (V_loif == NULL) {
        kprintf_c(0x0E, "net: no lo0 - 127.0.0.1 is not reachable\n");
        return;
    }
    V_loif->if_flags |= IFF_UP;
    V_loif->if_drv_flags |= IFF_DRV_RUNNING;
    error = configure_address(V_loif, htonl(INADDR_LOOPBACK),
                              htonl(0xff000000U));
    if (error != 0) {
        kprintf_c(0x0C, "net: SIOCAIFADDR on lo0 failed, error %d\n", error);
        return;
    }
    kprintf_c(0x0A, "net: lo0 is 127.0.0.1/8\n");
}

void net_stack_init(void) {
    struct ifnet *ifp;
    int error;

    configure_loopback();

    /* The interface everything else is configured on: the first real NIC -
     * lo0 is on the same list and is skipped. */
    CK_STAILQ_FOREACH(ifp, &V_ifnet_list, if_link) {
        if (ifp->if_xname[0] != '\0' &&
            (ifp->if_flags & IFF_LOOPBACK) == 0) {
            net_ifp = ifp;
            break;
        }
    }
    if (net_ifp == NULL) {
        kprintf_c(0x0E, "net: no interface attached - nothing to configure\n");
        return;
    }

    /* BRING THE INTERFACE UP, before the address.
     *
     * This is what "ifconfig re0 up" does, and without it nothing works: a
     * driver's attach initialises the chip but leaves the receiver off and
     * the descriptor rings unarmed. if_init is the driver's own routine that
     * programs the rings, enables the receiver and unmasks the device's
     * interrupt.
     *
     * Upstream it is reached through the SIOCSIFFLAGS ioctl when userland
     * sets IFF_UP. There is no userland to issue it, so it is called
     * directly - the same way every other bring-up step in flk.c is.
     *
     * Found by its absence once already: the interface attached, reported the
     * right MAC, and received exactly zero frames. */
    net_ifp->if_flags |= IFF_UP;
    if (net_ifp->if_init != NULL) {
        net_ifp->if_init(net_ifp->if_softc);
    }

    error = configure_address(net_ifp, MY_IP, MY_MASK);
    if (error != 0) {
        kprintf_c(0x0C, "net: SIOCAIFADDR on %s failed, error %d\n",
                  net_ifp->if_xname, error);
        return;
    }

    error = configure_default_route(net_ifp);
    if (error != 0) {
        /* Not fatal: the /24 is still reachable through the interface route
         * in_control just added. Only off-link traffic is affected, and
         * saying which is more useful than "route add failed". */
        kprintf_c(0x0E, "net: default route via 10.0.2.2 failed, error %d - "
                        "the local /24 still works, off-link does not\n",
                  error);
    }

    kprintf_c(0x0A, "net: %s is 10.0.2.15/24, gateway 10.0.2.2, "
                    "drv flags %x\n",
              net_ifp->if_xname, net_ifp->if_drv_flags);
}

void net_stack_report(uint8 color) {
    struct ifnet *ifp = net_ifp;
    struct nhop_object *nh;
    struct in_addr dst;

    if (ifp == NULL) {
        kprintf_c(color, "net: no interface\n");
        return;
    }
    /* The interface's OWN counters, which the driver and the vendored
     * Ethernet layer maintain - so they say whether a packet reached the
     * driver at all, independently of anything the IP layer counts. */
    kprintf_c(color, "net: %s  opackets %lx  oerrors %lx  ipackets %lx  "
                     "ierrors %lx  drv_flags %x  link %d\n",
              ifp->if_xname,
              if_getcounter(ifp, IFCOUNTER_OPACKETS),
              if_getcounter(ifp, IFCOUNTER_OERRORS),
              if_getcounter(ifp, IFCOUNTER_IPACKETS),
              if_getcounter(ifp, IFCOUNTER_IERRORS),
              ifp->if_drv_flags, ifp->if_link_state);

    /* ipstat and icmpstat, from the VENDORED counters rather than anything
     * this file keeps. That is the point of them being vendored: the numbers
     * come from the same increments a FreeBSD machine reports. */
    kprintf_c(color, "net: ip  total %lx  delivered %lx  badsum %lx  "
                     "badlen %lx  toosmall %lx  noproto %lx\n",
              VNET_PCPUSTAT_FETCH(struct ipstat, ipstat, ips_total), VNET_PCPUSTAT_FETCH(struct ipstat, ipstat, ips_delivered),
              VNET_PCPUSTAT_FETCH(struct ipstat, ipstat, ips_badsum), VNET_PCPUSTAT_FETCH(struct ipstat, ipstat, ips_badlen),
              VNET_PCPUSTAT_FETCH(struct ipstat, ipstat, ips_toosmall), VNET_PCPUSTAT_FETCH(struct ipstat, ipstat, ips_noproto));
    kprintf_c(color, "net: icmp  replies generated %lx  "
                     "echo replies received %lx  errors sent %lx\n",
              VNET_PCPUSTAT_FETCH(struct icmpstat, icmpstat, icps_reflect),
              VNET_PCPUSTAT_FETCH(struct icmpstat, icmpstat, icps_inhist[ICMP_ECHOREPLY]),
              VNET_PCPUSTAT_FETCH(struct icmpstat, icmpstat, icps_error));

    /* And the route the stack would actually use for an off-link packet.
     * Reading it back through fib4_lookup is the same call ip_output makes,
     * so this reports what the send path will do rather than what was asked
     * for. */
    dst.s_addr = 0x08080808U;      /* 8.8.8.8, deliberately off-link */
    nh = fib4_lookup(RT_DEFAULT_FIB, dst, 0, NHR_NONE, 0);
    if (nh == NULL) {
        kprintf_c(color, "net: no route to an off-link address\n");
    } else {
        kprintf_c(color, "net: off-link route is via %s, mtu %d, flags %x\n",
                  nh->nh_ifp != NULL ? nh->nh_ifp->if_xname : "?",
                  (int)nh->nh_mtu, nh->nh_flags);
    }
}
