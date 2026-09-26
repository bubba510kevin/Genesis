/* The subsystems the vendored network stack calls into that are NOT here.
 *
 * Every symbol in this file is a WHOLE FreeBSD subsystem that Genesis does not
 * have, reduced to the smallest thing that lets the vendored code link and
 * behave correctly without it. They are collected in one file rather than
 * scattered, because the list itself is the useful artefact: it is the exact
 * boundary of what "the FreeBSD network stack, ported" means here.
 *
 * The rule each of these follows, and it is the rule that makes them safe:
 *
 *   an absence must either be a state upstream itself supports, or it must
 *   FAIL. It must never return a plausible success.
 *
 * A kqueue with no filters registered is a state upstream supports. An AIO
 * queue with no requests is a state upstream supports. A socket option that
 * cannot be set returns EOPNOTSUPP rather than pretending it was set.
 *
 * Each group below says what is missing, what the consequence is, and what
 * closing it would take.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/sockopt.h>
#include <sys/sysctl.h>
#include <sys/ucred.h>
#include <sys/proc.h>
#include <sys/jail.h>
#include <sys/resourcevar.h>
#include <sys/event.h>
#include <sys/osd.h>
#include <sys/capsicum.h>
#include <net/pfil.h>
#include <sys/uio.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/route.h>
#include <net/route/route_ctl.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include "kprintf.h"

/* ========================================================================
 * CREDENTIALS - kern/kern_prot.c
 * ------------------------------------------------------------------------
 * A struct ucred is who a process is: uid, gid list, jail, MAC label. Genesis
 * has no user identity at all - every process runs with the same authority -
 * so there is nothing for a credential to hold and nothing to compare.
 *
 * so->so_cred is therefore always NULL. Every function here is written for
 * that, and each is reached: in_pcb.c holds a reference to the creating
 * socket's credential, and uipc_sockbuf.c charges buffer space against it.
 *
 * Consequence: no per-user socket buffer limits, and cr_canseeinpcb() -
 * "may this credential see that socket" - always says yes, which is what
 * netstat(1) would use to hide other users' sockets. On a machine with one
 * user that is not a leak.
 *
 * Closing it means a real credential in struct process and a uid in the
 * syscall layer, which is item-3 work rather than network work.
 * ==================================================================== */

/* THE credential, and the jail it is in.
 *
 * There is exactly one of each, and every thread points at them. Upstream
 * calls the always-present unrestricted jail `prison0` and gives the boot
 * thread a credential inside it; this is the same arrangement with the parts
 * that vary removed.
 *
 * Real objects rather than NULL, and that distinction cost a page fault to
 * learn: socreate() does `so->so_cred = crhold(cred)` and sbreserve() then
 * reads `so->so_cred->cr_uidinfo`. A NULL credential is not a state upstream
 * has, so vendored code does not test for it - a socket ALWAYS has one.
 *
 * cr_uidinfo is where per-uid socket buffer accounting would go. It is a real
 * structure so the pointer is valid; chgsbsize() below does not consult it,
 * which is where the "no per-user limits" statement above becomes concrete. */
struct prison prison0 = {
    .pr_name = "0",
};

static struct uidinfo genesis_uidinfo0;

/* The resource limits every thread is charged against - all infinite.
 *
 * A real structure rather than a NULL td_limit, for the same reason the
 * credential above is real: <sys/resourcevar.h>'s lim_cur() is a macro that
 * reads `td->td_limit->pl_rlimit[which].rlim_cur` DIRECTLY for the common
 * limits, with no null check, because a thread upstream always has one.
 * soreserve() reads RLIMIT_SBSIZE through it on every socket created.
 *
 * Infinite is the honest value: Genesis enforces no resource limits, and a
 * finite number here would be a limit nothing else in the kernel maintains.
 * Filled in by genesis_limits_init() rather than statically because
 * RLIM_INFINITY has to be written into every slot. */
static struct plimit genesis_limit0;

void genesis_limits_init(void) {
    int i;

    for (i = 0; i < RLIM_NLIMITS; i++) {
        genesis_limit0.pl_rlimit[i].rlim_cur = RLIM_INFINITY;
        genesis_limit0.pl_rlimit[i].rlim_max = RLIM_INFINITY;
    }
    genesis_limit0.pl_refcnt = 1;
}

struct plimit *genesis_limit(void) {
    return (&genesis_limit0);
}

struct ucred genesis_ucred0 = {
    .cr_ref = 1,
    .cr_users = 1,
    .cr_uid = 0,
    .cr_ruid = 0,
    .cr_svuid = 0,
    .cr_gid = 0,
    .cr_rgid = 0,
    .cr_svgid = 0,
    .cr_uidinfo = &genesis_uidinfo0,
    .cr_ruidinfo = &genesis_uidinfo0,
    .cr_prison = &prison0,
};

struct ucred *crhold(struct ucred *cr) {
    /* Nothing is refcounted because nothing is allocated. Returning the
     * argument is upstream's contract - callers write `p->cred = crhold(c)`. */
    return (cr);
}

void crfree(struct ucred *cr) {
    (void)cr;
}

/* "May the credential `u1` see a socket owned by `u2`?" Always. */
int cr_canseeinpcb(struct ucred *cred, struct inpcb *inp) {
    (void)cred; (void)inp;
    return (0);
}

/* Export a credential to userland's xucred form. There is no userland
 * consumer (no netstat, no sysctl(2)), so this zeroes the destination rather
 * than filling in a fictional uid 0 - a caller reading it back gets an
 * obviously-empty record, not a plausible one. */
void cru2x(struct ucred *cr, struct xucred *xcr) {
    (void)cr;
    if (xcr != NULL) {
        memset(xcr, 0, sizeof(*xcr));
    }
}

/* Charge socket buffer space to a uid. Upstream enforces RLIMIT_SBSIZE this
 * way. Returning 1 means "allowed"; the buffer accounting in sbreserve still
 * happens, only the PER-USER cap is absent. */
int chgsbsize(struct uidinfo *uip, u_int *hiwat, u_int to, rlim_t max) {
    (void)uip; (void)max;
    *hiwat = to;
    return (1);
}

/* ========================================================================
 * KQUEUE - kern/kern_event.c
 * ------------------------------------------------------------------------
 * kqueue(2) is FreeBSD's readiness notification interface. A socket keeps a
 * knote list per direction (so_rdsel.si_note, so_wrsel.si_note) and calls
 * knote() when it becomes readable or writable.
 *
 * There is no kqueue(2) here - it would need a file descriptor, and there are
 * no socket descriptors (see kernel/bsd/uipc_socket.c). So the lists are
 * initialised, stay empty, and knote() has nobody to tell. That is exactly
 * the state a FreeBSD socket is in until something calls kevent() on it.
 *
 * knlist_empty() answering true is what makes this correct rather than merely
 * quiet: the socket layer tests it before doing wakeup work, so an empty list
 * means the work is skipped rather than done for no recipient.
 * ==================================================================== */

void knlist_init(struct knlist *knl, void *lock, void (*kl_lock)(void *),
                 void (*kl_unlock)(void *), void (*kl_assert_lock)(void *, int)) {
    (void)lock; (void)kl_lock; (void)kl_unlock; (void)kl_assert_lock;
    if (knl != NULL) {
        memset(knl, 0, sizeof(*knl));
    }
}

void knlist_destroy(struct knlist *knl) {
    (void)knl;
}

void knlist_add(struct knlist *knl, struct knote *kn, int islocked) {
    (void)knl; (void)kn; (void)islocked;
}

void knlist_remove(struct knlist *knl, struct knote *kn, int islocked) {
    (void)knl; (void)kn; (void)islocked;
}

int knlist_empty(struct knlist *knl) {
    (void)knl;
    return (1);     /* always - nothing can register */
}

void knote(struct knlist *list, long hint, int lockflags) {
    (void)list; (void)hint; (void)lockflags;
}

/* The trivial knote-copy filterop. A socket's filterops table names it; with
 * no knotes it is never called. */
int knote_triv_copy(struct knote *kn, struct proc *p1) {
    (void)kn; (void)p1;
    return (0);
}

/* ========================================================================
 * ASYNCHRONOUS I/O - kern/vfs_aio.c
 * ------------------------------------------------------------------------
 * aio_read(2)/aio_write(2) against a socket. The socket layer has hooks so
 * that a completed transfer can retire a pending AIO request.
 *
 * No AIO, no descriptors, no requests - so these are called and find nothing
 * queued, which is the same thing that happens on a FreeBSD socket with no
 * outstanding AIO.
 * ==================================================================== */

void soaio_rcv(void *context, int pending) {
    (void)context; (void)pending;
}

void soaio_snd(void *context, int pending) {
    (void)context; (void)pending;
}

int soaio_queue_generic(struct socket *so, struct kaiocb *job) {
    (void)so; (void)job;
    return (EOPNOTSUPP);
}

void sowakeup_aio(struct socket *so, sb_which which) {
    (void)so; (void)which;
}

/* ========================================================================
 * FILE DESCRIPTORS - kern/kern_descrip.c, kern/uipc_syscalls.c
 * ------------------------------------------------------------------------
 * This is the group that matters most, and it is the one that says where the
 * port stops: THERE IS NO socket(2).
 *
 * A socket becomes reachable from a process by being wrapped in a struct file
 * and installed in a descriptor table. kern/uipc_syscalls.c does that, and it
 * is not vendored, because it is VFS work rather than network work - it needs
 * Genesis's fileobj/vfs layer to grow a fileops vector for sockets.
 *
 * So sockets here are kernel objects only. socreate() and the rest are
 * callable, and kernel/bsd/net_selftest.c calls them.
 * ==================================================================== */

int _fdrop(struct file *fp, struct thread *td) {
    (void)fp; (void)td;
    return (0);
}

int getsock(struct thread *td, int fd, const cap_rights_t *rightsp,
            struct file **fpp) {
    (void)td; (void)fd; (void)rightsp;
    *fpp = NULL;
    return (EBADF);     /* no descriptor is ever a socket */
}

int maxfiles = 0;

/* Capsicum capability rights. There is no capability mode and no descriptor
 * to attach rights to; these exist because uipc_socket.c names them when it
 * looks a descriptor up. */
const cap_rights_t cap_send_rights;
const cap_rights_t cap_recv_rights;

/* ========================================================================
 * SIGNALS TO A PROCESS
 * ------------------------------------------------------------------------
 * SIGPIPE on a write to a closed socket, SIGURG on out-of-band data. Genesis
 * has signals (kernel/proc/signal.c) but no way to name the process that owns
 * a socket, because ownership is set through fcntl(F_SETOWN) on a descriptor.
 *
 * So the signal has no destination. Named here rather than routed to the
 * current process, which would be wrong: the sender of a packet is not the
 * process that should be signalled.
 * ==================================================================== */

void kern_psignal(struct proc *p, int sig) {
    (void)p; (void)sig;
}

void tdsignal(struct thread *td, int sig) {
    (void)td; (void)sig;
}

/* ========================================================================
 * THE ROUTING SOCKET - net/rtsock.c
 * ------------------------------------------------------------------------
 * PF_ROUTE sockets: how route(8) and routed listen for "a route changed" and
 * "an interface address appeared". net/route/route_ctl.c announces every
 * change through these four.
 *
 * Not vendored because a routing socket is a SOCKET - it needs the descriptor
 * layer above, which is the same boundary as socket(2). The routing table
 * itself is entirely real (kernel/bsd/route*.c); what is missing is only the
 * notification channel out of it.
 *
 * Consequence: nothing external learns about a route change. Nothing external
 * exists to learn.
 * ==================================================================== */

void rt_missmsg_fib(int type, struct rt_addrinfo *rtinfo, int flags, int error,
                    int fibnum) {
    (void)type; (void)rtinfo; (void)flags; (void)error; (void)fibnum;
}

void rtsock_addrmsg(int cmd, struct ifaddr *ifa, int fibnum) {
    (void)cmd; (void)ifa; (void)fibnum;
}

void rtsock_routemsg(int cmd, struct rtentry *rt, struct nhop_object *nh,
                     int fibnum) {
    (void)cmd; (void)rt; (void)nh; (void)fibnum;
}

void rtsock_routemsg_info(int cmd, struct rt_addrinfo *info, int fibnum) {
    (void)cmd; (void)info; (void)fibnum;
}

/* The two notification bridges route_ctl.c dispatches through.
 *
 * NOT NULL, and that is the whole point of this comment. rt_ifmsg() is
 *
 *     rtsock_callback_p->ifmsg_f(ifp, if_flags_mask);
 *     netlink_callback_p->ifmsg_f(ifp, if_flags_mask);
 *
 * with no null check, because upstream guarantees both are set: rtsock.c and
 * netlink install theirs at SI_SUB_PROTO_DOMAIN and a kernel without either
 * still gets a static default. Leaving them NULL here page-faulted inside
 * rt_ifmsg the first time the NIC's link came up, three frames below
 * mii_attach, which reads as an mii bug.
 *
 * So they point at a bridge whose two handlers do nothing. That is a real
 * "no subscriber" state rather than a missing pointer. */
static void bridge_route_noop(uint32_t fibnum, const struct rib_cmd_info *rc) {
    (void)fibnum; (void)rc;
}

static void bridge_ifmsg_noop(struct ifnet *ifp, int if_flags_mask) {
    (void)ifp; (void)if_flags_mask;
}

static struct rtbridge genesis_route_bridge = {
    .route_f = bridge_route_noop,
    .ifmsg_f = bridge_ifmsg_noop,
};

struct rtbridge *rtsock_callback_p  = &genesis_route_bridge;
struct rtbridge *netlink_callback_p = &genesis_route_bridge;

MALLOC_DEFINE(M_RTABLE, "routetbl", "routing tables");

SYSCTL_NODE(_net, OID_AUTO, route, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "Routing");

/* ========================================================================
 * HELPER HOOKS, OSD, AND hhook - kern/kern_khelp.c, kern_osd.c, kern_hhook.c
 * ------------------------------------------------------------------------
 * Three generic extension mechanisms: hhook lets a module insert itself at a
 * named point in another subsystem (IPsec uses two in ip_input/ip_output),
 * osd attaches arbitrary per-object data, khelp is TCP's version of the same.
 *
 * No modules register through any of them, so every hook list is empty and
 * every registration succeeds trivially. Registration RETURNING SUCCESS is
 * correct here and not a cheat: the caller is asking to create an empty hook
 * point, and an empty hook point is what it gets.
 * ==================================================================== */

int hhook_head_register(int32_t hhook_type, int32_t hhook_id, void **hhh,
                        uint32_t flags) {
    (void)hhook_type; (void)hhook_id; (void)flags;
    if (hhh != NULL) {
        *hhh = NULL;    /* an empty hook point; the caller tests for NULL */
    }
    return (0);
}

int osd_register(u_int type, osd_destructor_t destructor,
                 const osd_method_t *methods) {
    (void)type; (void)destructor; (void)methods;
    return (0);
}

int khelp_init_osd(uint32_t classes, void *osd_list) {
    (void)classes; (void)osd_list;
    return (0);
}

int khelp_destroy_osd(void *osd_list) {
    (void)osd_list;
    return (0);
}

/* ========================================================================
 * ACCEPT FILTERS - kern/uipc_accf.c
 * ------------------------------------------------------------------------
 * SO_ACCEPTFILTER: defer accept(2) until the first request has arrived, so a
 * web server is not woken for a connection with no data. A TCP feature, and
 * there is no TCP here.
 * ==================================================================== */

int accept_filt_getopt(struct socket *so, struct sockopt *sopt) {
    (void)so; (void)sopt;
    return (EOPNOTSUPP);
}

int accept_filt_setopt(struct socket *so, struct sockopt *sopt) {
    (void)so; (void)sopt;
    return (EOPNOTSUPP);
}

/* ========================================================================
 * KERNEL THREADS - kern/kern_kthread.c
 * ------------------------------------------------------------------------
 * kproc_kthread_add() creates a kernel thread. Genesis's scheduler schedules
 * processes with user address spaces and has no concept of one (this is the
 * same gap kernel/bsd/kern_synch.c's header describes from the sleep side).
 *
 * The socket layer asks for one to run its splice worker. Refusing means the
 * splice path is unavailable - SO_SPLICE returns an error - rather than a
 * worker that was never started being waited on forever.
 * ==================================================================== */

int kproc_kthread_add(void (*func)(void *), void *arg, struct proc **procptr,
                      struct thread **tdptr, int flags, int pages,
                      const char *procname, const char *fmt, ...) {
    (void)func; (void)arg; (void)procptr; (void)tdptr;
    (void)flags; (void)pages; (void)procname; (void)fmt;
    return (ENOTSUP);
}

/* ========================================================================
 * IP FEATURES NOT BUILT
 * ==================================================================== */

/* ip_fastfwd.c - the fast-path forwarder. This machine does not forward (see
 * net.inet.ip.forwarding, which is 0), so ip_input.c never reaches it; the
 * symbol exists because the call site is compiled either way. Returning the
 * mbuf unchanged means "I did not handle it", and ip_input falls through to
 * its own slow forwarding path - which then also declines, correctly. */
struct mbuf *ip_tryforward(struct mbuf *m) {
    return (m);
}

/* ip_encap.c - IP-in-IP and GRE tunnel demultiplexing. No tunnel interfaces
 * exist, so no encapsulation registration exists to match against.
 * IPPROTO_DONE tells ip_input the mbuf is consumed; returning that without
 * freeing would leak, so the mbuf is freed here. */
int encap4_input(struct mbuf **mp, int *offp, int proto) {
    (void)offp; (void)proto;
    m_freem(*mp);
    *mp = NULL;
    return (IPPROTO_DONE);
}

/* pfil's forward hook. The other two directions are in kernel/bsd/netglue.c
 * with the rest of pfil; this one was missed because only ip_fastfwd calls
 * it. PFIL_PASS is what an empty hook chain returns. */
int pfil_mbuf_fwd(pfil_head_t head, struct mbuf **mp, struct ifnet *ifp,
                  struct inpcb *inp) {
    (void)head; (void)mp; (void)ifp; (void)inp;
    return (PFIL_PASS);
}

/* ========================================================================
 * TCP
 * ------------------------------------------------------------------------
 * netinet/in_proto.c's protocol switch table names tcp_protosw, so the symbol
 * must exist for the inet domain to link. TCP ITSELF IS NOT VENDORED.
 *
 * This is the one absence in this file that is a missing FEATURE rather than
 * a missing Genesis subsystem, so it deserves the plainest statement: this
 * machine speaks IPv4, ICMP, ARP and UDP. It does not speak TCP.
 *
 * What it would take is bounded and known: netinet/tcp_{input,output,subr,
 * usrreq,timer,reass,sack,timewait}.c plus the congestion-control modules,
 * the syncache and the host cache. Every dependency they have below them -
 * in_pcb, the socket layer, the routing table, sleep, sysctl - is now here,
 * which is why this is a next step rather than a rewrite.
 *
 * pr_attach returning EPROTONOSUPPORT is what socreate() turns into the error
 * a caller sees for socket(AF_INET, SOCK_STREAM, 0). That is the correct
 * answer for a kernel without TCP, and it is what a caller can distinguish
 * from a bug.
 * ==================================================================== */

static int tcp_attach_notsup(struct socket *so, int proto, struct thread *td) {
    (void)so; (void)proto; (void)td;
    return (EPROTONOSUPPORT);
}

struct protosw tcp_protosw = {
    .pr_type =      SOCK_STREAM,
    .pr_protocol =  IPPROTO_TCP,
    .pr_flags =     PR_CONNREQUIRED | PR_WANTRCVD | PR_CAPATTACH,
    .pr_attach =    tcp_attach_notsup,
};
