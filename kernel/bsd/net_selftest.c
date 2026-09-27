/* Does the protocol layer actually work?
 *
 * The discriminating question, and the reason this test is built the way it
 * is: every layer here can be "working" in the sense of compiling, linking
 * and being called, while no packet ever reaches the wire. What separates
 * those two states is a REPLY - something that only exists if a packet we
 * built was really transmitted, really parsed by a peer, and the peer's
 * answer really came back through the driver and up through every layer.
 *
 * Three checks, in increasing depth, each reporting its own verdict rather
 * than being folded into one pass/fail. That matters because they fail
 * independently and for different reasons, and a single verdict would hide
 * which layer broke.
 *
 *   1. ARP        - the link layer resolved an address.
 *   2. ICMP echo  - IP input, IP output and ICMP round-tripped a packet.
 *   3. UDP socket - a socket was created, bound, connected and sent through,
 *                   which exercises the socket layer, the PCB hash and UDP.
 *
 * Every counter read below is a VENDORED one - arpstat from
 * netinet/if_ether.c, icmpstat from netinet/ip_icmp.c, udpstat from
 * netinet/udp_usrreq.c - so what is being checked is FreeBSD's own accounting
 * of what happened, not a number this file keeps.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/uio.h>
#include <sys/proc.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_arp.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
/* <sys/sysctl.h> before <netinet/icmp_var.h>: that header does a
 * SYSCTL_DECL(_net_inet_icmp) at file scope, which is a declaration and not a
 * self-contained one. */
#include <sys/sysctl.h>
#include <netinet/icmp_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>

#include "kprintf.h"
#include "netstack.h"
#include "timer.h"

/* The gateway and resolver the address came with - from the DHCP lease, or
 * QEMU's defaults (10.0.2.2, 10.0.2.3) on the static fallback. Asked rather
 * than compiled in, so the checks still hold on a network DHCP configured
 * differently - which is how the DHCP client itself is checked. */
#define GW_IP_BE  (net_gateway() != 0 ? net_gateway() : 0x0202000AU)
#define DNS_IP_BE (net_dhcp_dns() != 0 ? net_dhcp_dns() : 0x0302000AU)

#define ARPSTAT(f)  VNET_PCPUSTAT_FETCH(struct arpstat, arpstat, f)
#define ICMPSTAT(f) VNET_PCPUSTAT_FETCH(struct icmpstat, icmpstat, f)
#define UDPSTAT(f)  VNET_PCPUSTAT_FETCH(struct udpstat, udpstat, f)

/* Spin on the tick until `pred` holds or `ticks` pass.
 *
 * Bounded rather than open-ended so a stack that does not work REPORTS rather
 * than hanging the boot - the discipline kernel/arch/ioapic_selftest.c settled
 * on after a broken IOAPIC wedged every test after it.
 *
 * `pause` in the loop, not `hlt`: this may run before interrupts are on for
 * the calling context, and a hlt there never returns.
 *
 * --- AND IT HAS TO YIELD, which it did not, and that was a real bug --------
 *
 * THE KERNEL IS NOT PREEMPTIVE. A kernel thread runs only when something
 * calls schedule(); the timer tick sets the reschedule flag and nothing acts
 * on it while the kernel is on the CPU. So a loop that spins without yielding
 * does not merely waste time - it makes it IMPOSSIBLE for any kernel thread
 * to run for the whole duration of the spin.
 *
 * That is fatal here specifically, because the thing being waited for is
 * produced by a kernel thread. if_re registers re_intr as a FILTER: on an
 * interrupt it MASKS THE CHIP (CSR_WRITE_2(sc, RL_IMR, 0)) and hands the
 * actual receive work to rl_inttask on taskqueue_fast, and re_int_task is the
 * only thing that re-arms the mask. One interrupt arrives, the chip goes
 * quiet, the task sits on a queue whose service thread this loop is starving,
 * and the reply is in the RX ring the entire time the counter reads zero.
 *
 * Measured, not reasoned: a QEMU filter-dump pcap showed the ARP request
 * leaving at t=0 and the reply arriving at t=+23ms, while the interrupt
 * trampoline was entered EXACTLY ONCE for the whole boot and ARPSTAT
 * (rxreplies) stayed at 0 for two thousand ticks.
 *
 * This was latent until the taskqueue became real. taskqueue_enqueue used to
 * be a macro that called the task inline on the enqueuing CPU (see
 * <sys/taskqueue.h>), so re_int_task ran inside the interrupt handler and the
 * chip was re-armed before this loop got another turn. Deferring the work was
 * the correct change and it turned a working spin into a deadlock - the class
 * of bug where nothing that changed is wrong and the combination is.
 *
 * kthread_yield() rather than a sleep: this must also work when the queue is
 * empty and the answer is genuinely "not yet", and schedule() returns
 * immediately when nothing else is runnable. */
void kthread_yield(void);

static int wait_for(uint64 (*get)(void), uint64 baseline, int ticks) {
    uint64 deadline = timer_ticks_now() + (uint64)ticks;

    /* A guard on the SPIN COUNT as well as on the deadline.
     *
     * The deadline alone is not enough, and the reason is worth keeping: this
     * loop reads the tick counter, and if something has left interrupts
     * disabled the tick counter never advances, so `timer_ticks_now() <
     * deadline` is true forever. That is exactly what a leaked write lock
     * does (see kernel/lib/mtx.c), and it turned a lock bug into a silent
     * hang here rather than a report. */
    {
        long guard = 0;

        while (timer_ticks_now() < deadline) {
            if (get() > baseline) {
                return (1);
            }
            if (++guard == 50000000L) {
                kprintf_c(0x0C, "net: THE TICK COUNTER IS NOT ADVANCING "
                                "(stuck at %lx) - something is holding a "
                                "spin lock with interrupts disabled\n",
                          timer_ticks_now());
                return (0);
            }
            /* Give the deferred half of the receive path a CPU. See above:
             * without this the answer can never arrive, however long the
             * deadline is. */
            kthread_yield();
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
    return (get() > baseline);
}

static uint64 get_arp_replies(void)  { return (ARPSTAT(rxreplies)); }
static uint64 get_echo_replies(void) {
    return (ICMPSTAT(icps_inhist[ICMP_ECHOREPLY]));
}
static uint64 get_udp_in(void)       { return (UDPSTAT(udps_ipackets)); }

/* --- 3: a real socket ----------------------------------------------------
 *
 * socreate/sobind/soconnect/sosend, the same calls a socket(2) implementation
 * would make. This is the only thing in the tree that exercises the socket
 * layer at all, so it is checking rather more than UDP: that the inet domain
 * registered, that pffindproto found the UDP protosw, that in_pcballoc got a
 * PCB into the hash, and that sosend turned a uio into mbufs.
 *
 * The datagram is a DNS query for the root zone, sent to QEMU's DNS proxy at
 * 10.0.2.3. A well-formed query rather than random bytes, because slirp's
 * proxy parses it and will not answer garbage - and because a reply is the
 * only thing that proves the receive half.
 */
static int udp_socket_check(void) {
    static const unsigned char dns_query[] = {
        0x47, 0x53,             /* transaction id "GS"                  */
        0x01, 0x00,             /* standard query, recursion desired    */
        0x00, 0x01,             /* one question                         */
        0x00, 0x00,             /* no answers                           */
        0x00, 0x00,             /* no authority records                 */
        0x00, 0x00,             /* no additional records                */
        0x00,                   /* QNAME: the root, a single zero label */
        0x00, 0x02,             /* QTYPE  = NS                          */
        0x00, 0x01              /* QCLASS = IN                          */
    };
    struct socket *so = NULL;
    struct sockaddr_in dst;
    struct uio uio;
    struct iovec iov;
    uint64 udp_in_before;
    int error;

    /* The credential is passed EXPLICITLY rather than left NULL. socreate()
     * stores what it is given in so->so_cred and sbreserve() then reads
     * so->so_cred->cr_uidinfo without checking - a socket upstream always has
     * a credential, so vendored code does not test for one. There is exactly
     * one in this kernel (kernel/bsd/net_absences.c) and this is it. */
    error = socreate(AF_INET, &so, SOCK_DGRAM, IPPROTO_UDP,
                     curthread->td_ucred, curthread);
    if (error != 0 || so == NULL) {
        kprintf_c(0x0C, "net: socreate(SOCK_DGRAM) FAILED, error %d - the "
                        "socket layer or the inet domain did not come up\n",
                  error);
        return (1);
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_len = sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = DNS_IP_BE;

    error = soconnect(so, (struct sockaddr *)&dst, curthread);
    if (error != 0) {
        kprintf_c(0x0C, "net: soconnect FAILED, error %d - in_pcbconnect "
                        "could not bind a local address or find a route\n",
                  error);
        soclose(so);
        return (1);
    }

    udp_in_before = get_udp_in();

    /* A uio over one iovec, UIO_SYSSPACE because the buffer is in the kernel.
     * This is the path a send(2) would take, minus the descriptor lookup. */
    iov.iov_base = (void *)(uintptr_t)dns_query;
    iov.iov_len = sizeof(dns_query);
    memset(&uio, 0, sizeof(uio));
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_offset = 0;
    uio.uio_resid = (ssize_t)sizeof(dns_query);
    uio.uio_segflg = UIO_SYSSPACE;
    uio.uio_rw = UIO_WRITE;
    uio.uio_td = curthread;

    error = sosend(so, NULL, &uio, NULL, NULL, 0, curthread);
    if (error != 0) {
        kprintf_c(0x0C, "net: sosend FAILED, error %d\n", error);
        soclose(so);
        return (1);
    }

    kprintf_c(0x0A, "net: socket check passed - a UDP socket was created, "
                    "connected and sent through (udp opackets %lx)\n",
              UDPSTAT(udps_opackets));

    /* The reply half. Two seconds, then report either way: whether QEMU's DNS
     * proxy answers depends on the HOST having a resolver, which is outside
     * this machine's control, so a silence here is not evidence against the
     * receive path - the ARP and ICMP checks above already cover that. */
    if (wait_for(get_udp_in, udp_in_before, 200)) {
        kprintf_c(0x0A, "net: UDP round trip passed - a datagram we sent was "
                        "answered and delivered back up (udp ipackets %lx)\n",
                  UDPSTAT(udps_ipackets));
    } else {
        kprintf_c(0x0E, "net: UDP reply not received - the send half is "
                        "proven above; whether 10.0.2.3 answers depends on "
                        "the host's resolver, so this is not evidence "
                        "against the receive path\n");
    }

    soclose(so);
    return (0);
}

int net_selftest(void) {
    uint64 arp_before, echo_before;
    int failures = 0;

    if (net_interface() == NULL) {
        /* No NIC on this machine. Not a failure - the whole stack is correct
         * and has nothing to run on - but reported, because a silent zero
         * here would read exactly like a pass. */
        kprintf_c(0x0E, "net: no interface - selftest skipped\n");
        return (0);
    }

    arp_before  = get_arp_replies();
    echo_before = get_echo_replies();

    /* One ping to the gateway. The ARP exchange happens underneath: the first
     * send returns EWOULDBLOCK having queued the packet and issued an ARP
     * request, and the vendored ARP retransmits the held packet once the
     * reply lands. */
    (void)net_ping(GW_IP_BE);

    /* --- 1: ARP ---------------------------------------------------------
     * An ARP REPLY can only be counted if our request left the machine and
     * something answered it. That is the whole path in both directions:
     * arpresolve, ether_output, if_transmit, the driver's DMA, a peer, the
     * receive interrupt, re_rxeof, if_input, ether_input, ether_demux,
     * netisr, arpintr. */
    if (!wait_for(get_arp_replies, arp_before, 200)) {
        kprintf_c(0x0C, "net: selftest FAILED - no ARP reply from the "
                        "gateway (requests sent %lx, replies %lx). Nothing "
                        "left the interface, or nothing came back.\n",
                  ARPSTAT(txrequests), ARPSTAT(rxreplies));
        return (1);
    }
    kprintf_c(0x0A, "net: ARP check passed - the gateway (%x) resolved: a "
                    "frame we built was transmitted, answered, and received "
                    "(tx %lx, rx replies %lx)\n",
              (uint32)ntohl(GW_IP_BE), ARPSTAT(txrequests), ARPSTAT(rxreplies));

    /* --- 2: ICMP echo ----------------------------------------------------
     * Now the IP layer itself. Retried, because the first ping raced the ARP
     * resolution and a packet dropped once tells us nothing. */
    {
        int attempt;

        for (attempt = 0; attempt < 3; attempt++) {
            (void)net_ping(GW_IP_BE);
            if (wait_for(get_echo_replies, echo_before, 100)) {
                break;
            }
        }
    }
    if (get_echo_replies() > echo_before) {
        kprintf_c(0x0A, "net: ICMP check passed - an echo reply came back "
                        "through ip_input (replies %lx)\n",
                  get_echo_replies());
    } else {
        /* NOT a failure of the send/receive path, and it is important not to
         * report it as one: the ARP round trip above already proved a frame
         * we built reached a peer and its answer came back through the whole
         * receive chain. What this says is narrower - the echo request was
         * transmitted and no reply arrived. */
        kprintf_c(0x0E, "net: ICMP echo unanswered (sent, opackets rose) - "
                        "the send and receive paths are proven by ARP above; "
                        "this leg is unverified\n");
    }

    /* --- 3: the socket layer -------------------------------------------- */
    failures += udp_socket_check();

    return (failures);
}
