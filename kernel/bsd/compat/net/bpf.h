#ifndef _NET_BPF_H_
#define _NET_BPF_H_

/* <net/bpf.h> - the packet filter tap points, as no-ops.
 *
 * BPF_MTAP is what a driver calls to hand a copy of each packet to any
 * attached tcpdump. There is no packet filter in Genesis and nothing to
 * attach, so these compile away entirely.
 *
 * No-ops rather than absent: a driver calls BPF_MTAP on its receive and
 * transmit paths and must not have to be edited. And no-ops rather than a
 * queue nothing drains, which would be a slow leak dressed as support.
 *
 * The bpfattach in ether_ifattach's place is likewise absent. When a real
 * BPF lands these become real, and every call site is already correct.
 */

#define BPF_MTAP(_ifp, _m)        do { (void)(_ifp); (void)(_m); } while (0)
#define BPF_MTAP2(_ifp, _d, _l, _m) \
    do { (void)(_ifp); (void)(_d); (void)(_l); (void)(_m); } while (0)
/* net/ethernet.h defines this too - a driver includes both and either may
 * come first, so it is guarded rather than duplicated. */
#ifndef ETHER_BPF_MTAP
#define ETHER_BPF_MTAP(_ifp, _m)  BPF_MTAP(_ifp, _m)
#endif
#define bpfattach(ifp, dlt, hdrlen)   do { } while (0)
#define bpfattach2(ifp, dlt, hdrlen, dp) do { } while (0)
#define bpfdetach(ifp)                do { } while (0)
/* Always false: there is no BPF and nothing can be listening. Every caller
 * checks this before building a copy of the packet, so the tap costs
 * nothing on the receive path rather than costing an mbuf copy that is then
 * discarded. */
#define bpf_peers_present(bpf)        (0)

#define DLT_EN10MB 1

/* The tap points as FUNCTIONS, not macros.
 *
 * Vendored source calls bpf_mtap(ifp->if_bpf, m) directly rather than
 * through the BPF_MTAP macro, so a macro alone leaves them undefined. They
 * are real symbols that do nothing - defined in kernel/bsd/ifnet.c - rather
 * than macros, because taking their address is legal and upstream does it.
 *
 * bpf_peers_present is what guards the call: it answers false, so a caller
 * that checks first never even builds the copy. */
struct bpf_if;
void bpf_mtap(struct bpf_if *bp, struct mbuf *m);
void bpf_mtap2(struct bpf_if *bp, void *data, unsigned int dlen,
               struct mbuf *m);
void bpf_tap(struct bpf_if *bp, unsigned char *pkt, unsigned int pktlen);

#endif
