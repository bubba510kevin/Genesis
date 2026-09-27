/* What the vendored TCP needs from files that are not vendored.
 *
 * Each entry says which upstream file it would have come from and why that
 * file is not here. None of them is a placeholder for behaviour TCP depends
 * on to move data: they are statistics, a hash TCP asks for but this machine
 * has no hardware to use, and the sendfile hook.
 */
#include "route_prelude.h"

#include <sys/counter.h>
#include <sys/sockbuf.h>
#include <sys/socketvar.h>
#include <netinet/in.h>
#include <netinet/in_rss.h>

/* --- netinet/tcp_lro.c's counters -----------------------------------------
 *
 * Large receive offload coalesces received segments before TCP sees them.
 * tcp_lro.c is not vendored - no driver here does LRO - but tcp_subr.c
 * allocates these statistics at init unconditionally and tcp_input bumps a
 * couple of them, so they are defined where upstream defines them: beside
 * the LRO code that would own them. They count real events or stay zero. */
counter_u64_t tcp_inp_lro_direct_queue;
counter_u64_t tcp_inp_lro_wokeup_queue;
counter_u64_t tcp_inp_lro_compressed;
counter_u64_t tcp_inp_lro_locks_taken;
counter_u64_t tcp_extra_mbuf;
counter_u64_t tcp_would_have_but;
counter_u64_t tcp_comp_total;
counter_u64_t tcp_uncomp_total;
counter_u64_t tcp_bad_csums;

/* --- netinet/in_rss.c ------------------------------------------------------
 *
 * Receive-side scaling picks a CPU per flow from a Toeplitz hash, so that a
 * NIC with several queues spreads connections across processors. TCP asks
 * for the software hash of every new connection so the flow id matches the
 * one the NIC would stamp on its packets.
 *
 * No NIC here has multiple receive queues, so there is no hardware hash for
 * the software one to agree with. The answer is "no hash": flowid 0 with
 * M_HASHTYPE_NONE, which is what upstream's own code produces on a kernel
 * built without "options RSS", and what TCP handles by not using the flowid
 * for anything. */
int
rss_proto_software_hash_v4(struct in_addr src, struct in_addr dst,
    u_short src_port, u_short dst_port, int proto, uint32_t *hashval,
    uint32_t *hashtype)
{
    (void)src; (void)dst; (void)src_port; (void)dst_port; (void)proto;
    *hashval = 0;
    *hashtype = M_HASHTYPE_NONE;
    return (0);
}

/* --- kern/kern_sendfile.c --------------------------------------------------
 *
 * pr_sendfile_wait is the protocol hook sendfile(2) calls to wait for send
 * buffer space. tcp_usrreq's protosw points at it; there is no sendfile
 * syscall here, so nothing reaches it. It answers the way an unsupported
 * operation answers rather than pretending to have waited. */
int
sendfile_wait_generic(struct socket *so, off_t need, int *space)
{
    (void)so; (void)need;
    *space = 0;
    return (EOPNOTSUPP);
}
