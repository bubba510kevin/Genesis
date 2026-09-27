/* A DHCP client (RFC 2131), in the kernel.
 *
 * rc.d would run dhclient(8) here; there is no userland to run it, so the
 * boot path does what dhclient does: DISCOVER, wait for an OFFER, REQUEST
 * it, wait for the ACK, apply the lease - then a kernel thread renews it at
 * T1 for as long as the machine runs. No server answering is not an error:
 * the compiled-in QEMU address is applied instead and the boot goes on.
 *
 * --- how it sends with no address -------------------------------------------
 *
 * Before the ACK the interface has no IPv4 address, so the socket layer
 * cannot send - there is no source address and no route. dhclient solves
 * this with BPF; this client builds the IP/UDP datagram itself and hands it
 * to the interface's if_output as an AF_INET packet flagged M_BCAST, which
 * ether_output sends to ff:ff:ff:ff:ff:ff with no ARP and no route. The
 * source is 0.0.0.0, which is what RFC 2131 says a client with no address
 * uses.
 *
 * --- how it receives -----------------------------------------------------------
 *
 * Through an ordinary UDP socket bound to 0.0.0.0:68. The server answers to
 * the limited broadcast address (the request asks it to, with the BROADCAST
 * flag), and ip_input accepts 255.255.255.255 on an interface with no
 * address - so the vendored stack delivers the reply exactly as it would to
 * dhclient's socket. */

#include "route_prelude.h"

#include <sys/socketvar.h>
#include <sys/sockopt.h>
#include <sys/uio.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <machine/in_cksum.h>

#include "kprintf.h"
#include "netstack.h"

int genesis_kthread_spawn(void (*fn)(void *), void *arg, const char *name);
uint64 timer_ticks_now(void);

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define BOOTREQUEST 1
#define BOOTREPLY   2

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

#define OPT_PAD        0
#define OPT_SUBNET     1
#define OPT_ROUTER     3
#define OPT_DNS        6
#define OPT_HOSTNAME   12
#define OPT_REQ_IP     50
#define OPT_LEASE      51
#define OPT_MSGTYPE    53
#define OPT_SERVER_ID  54
#define OPT_PARAMS     55
#define OPT_T1         58
#define OPT_T2         59
#define OPT_CLIENT_ID  61
#define OPT_END        255

#define DHCP_MAGIC 0x63825363U

struct bootp {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    char     sname[64];
    char     file[128];
    uint32_t magic;
    uint8_t  options[308];
} __packed;
CTASSERT(sizeof(struct bootp) == 548);

/* What the last lease said. Network byte order for addresses. */
struct lease {
    uint32_t addr, mask, router, dns, server;
    uint32_t lease_secs, t1_secs;
};

static struct socket *dhcp_so;
static struct lease   cur_lease;
static uint32_t       xid_seed;
static uint64         renewals;
static int            have_lease;

uint32 net_dhcp_server(void)        { return have_lease ? cur_lease.server : 0; }
uint32 net_dhcp_dns(void)           { return have_lease ? cur_lease.dns : 0; }
uint32 net_dhcp_lease_seconds(void) { return have_lease ? cur_lease.lease_secs : 0; }
uint64 net_dhcp_renewals(void)      { return renewals; }

/* --- the socket replies arrive on ---------------------------------------------- */

static int dhcp_open(void) {
    struct sockaddr_in sin;
    int error, on = 1;
    struct sockopt sopt;

    if (dhcp_so != NULL) {
        return (0);
    }
    error = socreate(AF_INET, &dhcp_so, SOCK_DGRAM, IPPROTO_UDP,
                     curthread->td_ucred, curthread);
    if (error != 0) {
        dhcp_so = NULL;
        return (error);
    }
    memset(&sopt, 0, sizeof(sopt));
    sopt.sopt_dir = SOPT_SET;
    sopt.sopt_level = SOL_SOCKET;
    sopt.sopt_name = SO_REUSEADDR;
    sopt.sopt_val = &on;
    sopt.sopt_valsize = sizeof(on);
    (void)sosetopt(dhcp_so, &sopt);
    sopt.sopt_name = SO_BROADCAST;
    (void)sosetopt(dhcp_so, &sopt);

    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(DHCP_CLIENT_PORT);
    sin.sin_addr.s_addr = INADDR_ANY;
    error = sobind(dhcp_so, (struct sockaddr *)&sin, curthread);
    if (error != 0) {
        (void)soclose(dhcp_so);
        dhcp_so = NULL;
    }
    return (error);
}

/* --- building and sending a request ------------------------------------------ */

static uint8_t *opt_put(uint8_t *p, uint8_t code, uint8_t len, const void *data) {
    *p++ = code;
    *p++ = len;
    memcpy(p, data, len);
    return (p + len);
}

/* A DISCOVER or REQUEST. `req_ip` and `server` are 0 for a DISCOVER; a
 * REQUEST in the SELECTING state names both, one renewing names neither
 * but fills ciaddr (RFC 2131 4.3.2). */
static int dhcp_send(struct ifnet *ifp, uint8_t type, uint32_t xid,
                     uint32_t req_ip, uint32_t server, uint32_t ciaddr) {
    struct mbuf *m;
    struct ip *ip;
    struct udphdr *uh;
    struct bootp *bp;
    struct sockaddr_in dst;
    uint8_t *o;
    uint8_t cid[7];
    static const uint8_t params[] = { OPT_SUBNET, OPT_ROUTER, OPT_DNS,
                                      OPT_LEASE, OPT_T1, OPT_T2 };
    int len = (int)(sizeof(*ip) + sizeof(*uh) + sizeof(*bp));

    m = m_getcl(M_WAITOK, MT_DATA, M_PKTHDR);
    if (m == NULL) {
        return (ENOBUFS);
    }
    m->m_len = m->m_pkthdr.len = len;
    memset(mtod(m, void *), 0, len);
    ip = mtod(m, struct ip *);
    uh = (struct udphdr *)(ip + 1);
    bp = (struct bootp *)(uh + 1);

    bp->op    = BOOTREQUEST;
    bp->htype = 1;                             /* Ethernet */
    bp->hlen  = ETHER_ADDR_LEN;
    bp->xid   = xid;
    /* BROADCAST: answer to 255.255.255.255, which this interface can
     * receive before it has an address. Not set once renewing: the server
     * then answers unicast, to the address we have. */
    bp->flags = ciaddr == 0 ? htons(0x8000) : 0;
    bp->ciaddr = ciaddr;
    memcpy(bp->chaddr, IF_LLADDR(ifp), ETHER_ADDR_LEN);
    bp->magic = htonl(DHCP_MAGIC);

    o = bp->options;
    o = opt_put(o, OPT_MSGTYPE, 1, &type);
    cid[0] = 1;
    memcpy(cid + 1, IF_LLADDR(ifp), ETHER_ADDR_LEN);
    o = opt_put(o, OPT_CLIENT_ID, sizeof(cid), cid);
    if (req_ip != 0) {
        o = opt_put(o, OPT_REQ_IP, 4, &req_ip);
    }
    if (server != 0) {
        o = opt_put(o, OPT_SERVER_ID, 4, &server);
    }
    o = opt_put(o, OPT_HOSTNAME, 7, "genesis");
    o = opt_put(o, OPT_PARAMS, sizeof(params), params);
    *o++ = OPT_END;

    uh->uh_sport = htons(DHCP_CLIENT_PORT);
    uh->uh_dport = htons(DHCP_SERVER_PORT);
    uh->uh_ulen  = htons((uint16_t)(sizeof(*uh) + sizeof(*bp)));
    uh->uh_sum   = 0;                          /* optional over IPv4 */

    ip->ip_v   = IPVERSION;
    ip->ip_hl  = sizeof(*ip) >> 2;
    ip->ip_len = htons((uint16_t)len);
    ip->ip_id  = htons((uint16_t)(xid ^ (xid >> 16)));
    ip->ip_ttl = 64;
    ip->ip_p   = IPPROTO_UDP;
    ip->ip_src.s_addr = ciaddr;                /* 0.0.0.0 until leased */
    ip->ip_dst.s_addr = INADDR_BROADCAST;
    ip->ip_sum = 0;
    ip->ip_sum = in_cksum_hdr(ip);

    memset(&dst, 0, sizeof(dst));
    dst.sin_len = sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = INADDR_BROADCAST;
    m->m_flags |= M_BCAST;
    return (ifp->if_output(ifp, m, (struct sockaddr *)&dst, NULL));
}

/* --- receiving and reading a reply ----------------------------------------------- */

/* Parse a reply for this xid. Returns its DHCP message type (0 if it is not
 * ours or not DHCP) and fills `l`. */
static int dhcp_parse(const struct bootp *bp, int len, uint32_t xid,
                      const uint8_t *mac, struct lease *l) {
    const uint8_t *o, *end;
    int type = 0;

    if (len < (int)offsetof(struct bootp, options) || bp->op != BOOTREPLY ||
        bp->xid != xid || memcmp(bp->chaddr, mac, ETHER_ADDR_LEN) != 0 ||
        ntohl(bp->magic) != DHCP_MAGIC) {
        return (0);
    }
    memset(l, 0, sizeof(*l));
    l->addr = bp->yiaddr;
    o = bp->options;
    end = (const uint8_t *)bp + len;
    while (o < end && *o != OPT_END) {
        uint8_t code = *o++, olen;

        if (code == OPT_PAD) {
            continue;
        }
        if (o >= end) {
            break;
        }
        olen = *o++;
        if (o + olen > end) {
            break;
        }
        switch (code) {
        case OPT_MSGTYPE:   if (olen >= 1) type = o[0]; break;
        case OPT_SUBNET:    if (olen >= 4) memcpy(&l->mask, o, 4); break;
        case OPT_ROUTER:    if (olen >= 4) memcpy(&l->router, o, 4); break;
        case OPT_DNS:       if (olen >= 4) memcpy(&l->dns, o, 4); break;
        case OPT_SERVER_ID: if (olen >= 4) memcpy(&l->server, o, 4); break;
        case OPT_LEASE:
            if (olen >= 4) { uint32_t v; memcpy(&v, o, 4); l->lease_secs = ntohl(v); }
            break;
        case OPT_T1:
            if (olen >= 4) { uint32_t v; memcpy(&v, o, 4); l->t1_secs = ntohl(v); }
            break;
        default:
            break;
        }
        o += olen;
    }
    return (type);
}

/* Wait up to `ticks` for a reply of `want` type (or a NAK) to `xid`. */
static int dhcp_wait(struct ifnet *ifp, uint32_t xid, int want, uint64 ticks,
                     struct lease *l) {
    uint64 deadline = timer_ticks_now() + ticks;
    uint8_t buf[sizeof(struct bootp)];

    while (timer_ticks_now() < deadline) {
        struct uio uio;
        struct iovec iov;
        int flags = MSG_DONTWAIT, error, got, type;

        if (sbavail(&dhcp_so->so_rcv) == 0) {
            pause("dhcp", 1);
            continue;
        }
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        memset(&uio, 0, sizeof(uio));
        uio.uio_iov = &iov;
        uio.uio_iovcnt = 1;
        uio.uio_resid = sizeof(buf);
        uio.uio_segflg = UIO_SYSSPACE;
        uio.uio_rw = UIO_READ;
        uio.uio_td = curthread;
        error = soreceive(dhcp_so, NULL, &uio, NULL, NULL, &flags);
        if (error != 0) {
            continue;
        }
        got = (int)(sizeof(buf) - uio.uio_resid);
        type = dhcp_parse((const struct bootp *)buf, got, xid, IF_LLADDR(ifp), l);
        if (type == want || type == DHCPNAK) {
            return (type);
        }
    }
    return (0);
}

static uint32_t next_xid(struct ifnet *ifp) {
    const uint8_t *mac = IF_LLADDR(ifp);

    xid_seed = xid_seed * 1103515245U + 12345U +
               ((uint32_t)mac[5] << 8) + mac[4] + (uint32_t)timer_ticks_now();
    return (xid_seed);
}

static void lease_apply(const struct lease *l) {
    uint32_t mask = l->mask != 0 ? l->mask : htonl(0xFFFFFF00U);

    (void)net_configure(l->addr, mask, l->router);
    cur_lease = *l;
    cur_lease.mask = mask;
    if (cur_lease.lease_secs == 0) {
        cur_lease.lease_secs = 3600;
    }
    if (cur_lease.t1_secs == 0 || cur_lease.t1_secs >= cur_lease.lease_secs) {
        cur_lease.t1_secs = cur_lease.lease_secs / 2;
    }
    have_lease = 1;
}

/* One full exchange: DISCOVER/OFFER/REQUEST/ACK. 0 with a lease applied. */
static int dhcp_acquire(struct ifnet *ifp) {
    struct lease offer, ack;
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        uint32_t xid = next_xid(ifp);

        if (dhcp_send(ifp, DHCPDISCOVER, xid, 0, 0, 0) != 0) {
            continue;
        }
        if (dhcp_wait(ifp, xid, DHCPOFFER, 100 + 50 * attempt, &offer) != DHCPOFFER ||
            offer.addr == 0) {
            continue;
        }
        if (dhcp_send(ifp, DHCPREQUEST, xid, offer.addr, offer.server, 0) != 0) {
            continue;
        }
        if (dhcp_wait(ifp, xid, DHCPACK, 150, &ack) != DHCPACK || ack.addr == 0) {
            continue;                    /* a NAK, or silence: start over */
        }
        if (ack.server == 0) {
            ack.server = offer.server;
        }
        lease_apply(&ack);
        return (0);
    }
    return (ETIMEDOUT);
}

/* --- renewal --------------------------------------------------------------------
 *
 * At T1 the lease is renewed with a REQUEST carrying our address in ciaddr
 * (RENEWING). No answer by T2's worth of retries means start over from
 * DISCOVER; a NAK means the same. Seconds become ticks at 100Hz. */
static void dhcp_renew_thread(void *arg) {
    struct ifnet *ifp = arg;

    for (;;) {
        uint64 wait = (uint64)cur_lease.t1_secs * 100;
        uint64 until = timer_ticks_now() + wait;
        struct lease ack;
        uint32_t xid;
        int tries, type = 0;

        while (timer_ticks_now() < until) {
            pause("dhcpt1", 100);
        }
        for (tries = 0; tries < 3 && type != DHCPACK; tries++) {
            xid = next_xid(ifp);
            if (dhcp_send(ifp, DHCPREQUEST, xid, 0, 0, cur_lease.addr) != 0) {
                continue;
            }
            type = dhcp_wait(ifp, xid, DHCPACK, 200, &ack);
        }
        if (type == DHCPACK && ack.addr != 0) {
            if (ack.server == 0) {
                ack.server = cur_lease.server;
            }
            lease_apply(&ack);
            renewals++;
            continue;
        }
        kprintf_c(0x0E, "dhcp: renewal got no ACK - starting over\n");
        if (dhcp_acquire(ifp) != 0) {
            /* Keep the address we have and try again after a minute. */
            cur_lease.t1_secs = 60;
        }
    }
}

static void ip_str(uint32_t a, char *out) {
    const uint8_t *b = (const uint8_t *)&a;

    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void net_dhcp_report(uint8 color) {
    char a[16], m[16], g[16], d[16], s[16];

    if (!have_lease) {
        kprintf_c(color, "dhcp: no lease (static address in use)\n");
        return;
    }
    ip_str(cur_lease.addr, a);
    ip_str(cur_lease.mask, m);
    ip_str(cur_lease.router, g);
    ip_str(cur_lease.dns, d);
    ip_str(cur_lease.server, s);
    kprintf_c(color, "dhcp: leased %s mask %s gateway %s dns %s from %s, "
                     "%u s (renew at %u s)\n", a, m, g, d, s,
              cur_lease.lease_secs, cur_lease.t1_secs);
}

int net_dhcp_start(void) {
    struct ifnet *ifp = net_interface();

    if (ifp == NULL) {
        return (1);
    }
    if (dhcp_open() != 0 || dhcp_acquire(ifp) != 0) {
        kprintf_c(0x0E, "dhcp: no server answered - using the static "
                        "10.0.2.15/24\n");
        (void)net_configure_static();
        return (1);
    }
    net_dhcp_report(0x0A);
    (void)genesis_kthread_spawn(dhcp_renew_thread, ifp, "dhclient");
    return (0);
}

/* One renewal now, rather than at T1 twelve hours in: a REQUEST in the
 * RENEWING state (ciaddr set, no server id), which has to come back an ACK
 * for the address already held. The only way the renewal code runs during
 * a test. 0 on success; also 0, with a note, on the static fallback. */
int net_dhcp_selftest(void) {
    struct ifnet *ifp = net_interface();
    struct lease ack;
    uint32_t xid;
    int type;

    if (ifp == NULL || !have_lease) {
        kprintf_c(0x0E, "dhcp selftest: no lease - renewal not exercised\n");
        return (0);
    }
    if (net_my_addr() != cur_lease.addr) {
        kprintf_c(0x0C, "dhcp selftest: the interface does not have the "
                        "leased address\n");
        return (1);
    }
    xid = next_xid(ifp);
    if (dhcp_send(ifp, DHCPREQUEST, xid, 0, 0, cur_lease.addr) != 0) {
        kprintf_c(0x0C, "dhcp selftest: the renewal REQUEST could not be sent\n");
        return (1);
    }
    type = dhcp_wait(ifp, xid, DHCPACK, 200, &ack);
    if (type != DHCPACK || ack.addr != cur_lease.addr) {
        kprintf_c(0x0C, "dhcp selftest: renewal answered %d, address %x\n",
                  type, ack.addr);
        return (1);
    }
    renewals++;
    kprintf("dhcp: selftest passed - the lease renewed (RENEWING -> ACK for "
            "the same address)\n");
    return (0);
}
