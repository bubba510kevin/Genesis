/* The glue the vendored Ethernet layer needs, and the honest inventory of
 * what is not built.
 *
 * Every symbol here is one that net/if_ethersubr.c calls. Each is either a
 * genuine adaptation onto something Genesis has, or a named absence. They
 * are together in one file so that "what is missing from the network stack"
 * is a thing you can read rather than infer.
 */

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/time.h>   /* struct itimerval, which <sys/resourcevar.h> embeds */
#include <sys/resourcevar.h>
#include <sys/priv.h>
#include <sys/taskqueue.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/ck.h>
#include <sys/epoch.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
/* AFTER net/if.h: if_clone.h declares if_clone_list(struct if_clonereq *), and
 * with net/if.h not yet seen that struct tag gets PROTOTYPE scope - so the
 * definition below would be a different, incompatible type. */
#include <net/if_clone.h>
#include <net/if_dl.h>
#include <net/ethernet.h>
#include <net/netisr.h>
#include <net/pfil.h>
#include <sys/jail.h>

#include "klock.h"
#include "kprintf.h"

/* --- netisr: how a received packet goes UP -------------------------------
 *
 * This is the most important function in this file. ether_input classifies a
 * frame and calls netisr_dispatch(NETISR_IP, m) to hand it to IP.
 *
 * Upstream, netisr can queue the packet to a per-CPU software-interrupt
 * thread or run the handler inline. Inline is called DIRECT DISPATCH and it
 * is FreeBSD's DEFAULT (net.isr.dispatch=direct) - so calling the handler
 * straight from here is not a simplification, it is the configuration a
 * stock FreeBSD kernel runs.
 *
 * What is missing is the queue, which matters for two things Genesis does
 * not do: spreading receive work across CPUs, and bounding how long a driver
 * interrupt can run. Both are real, and neither changes whether a packet
 * arrives.
 */
#define NETISR_MAX 8

static struct {
    int          proto;
    netisr_handler_t *handler;
    const char  *name;
} netisr_slots[NETISR_MAX];
static int netisr_count;

void netisr_register(const struct netisr_handler *nhp) {
    if (netisr_count >= NETISR_MAX) {
        kprintf_c(0x0C, "netisr: no room for protocol %d\n", nhp->nh_proto);
        return;
    }
    netisr_slots[netisr_count].proto   = nhp->nh_proto;
    netisr_slots[netisr_count].handler = nhp->nh_handler;
    netisr_slots[netisr_count].name    = nhp->nh_name;
    netisr_count++;
    kprintf_c(0x0A, "netisr: registered %s (proto %d)\n",
              nhp->nh_name, nhp->nh_proto);
}

int netisr_dispatch(u_int proto, struct mbuf *m) {
    int i;


    for (i = 0; i < netisr_count; i++) {
        if (netisr_slots[i].proto == (int)proto) {
            netisr_slots[i].handler(m);
            return 0;
        }
    }
    /* No handler for this protocol. The packet is FREED, not leaked - an
     * unhandled protocol is normal (a machine with no IPv6 receives IPv6
     * frames) and holding them would exhaust the mbuf zone. */
    m_freem(m);
    return 0;
}

int netisr_queue(u_int proto, struct mbuf *m) {
    return netisr_dispatch(proto, m);
}

/* Withdraw a registration. netinet/igmp.c does this on unload; nothing here
 * unloads, so it exists to link and is correct if it is ever reached - the
 * slot is removed by compacting the array, which is safe because dispatch
 * walks it under no lock and a shrinking array never exposes a stale
 * handler. */
void netisr_unregister(const struct netisr_handler *nhp) {
    int i, j;

    for (i = 0; i < netisr_count; i++) {
        if (netisr_slots[i].proto == nhp->nh_proto) {
            for (j = i; j + 1 < netisr_count; j++) {
                netisr_slots[j] = netisr_slots[j + 1];
            }
            netisr_count--;
            return;
        }
    }
}

/* The per-protocol queue knobs net/netisr.c publishes as sysctls, and which
 * netinet/ip_input.c's net.inet.ip.intr_queue_maxlen handler reads and
 * writes. There is no queue here (direct dispatch, see above), so the limit
 * is reported as zero and setting it is refused rather than accepted and
 * ignored - a caller that raises a queue limit and is told "ok" would be
 * entitled to believe the queue got deeper. */
void netisr_getqlimit(const struct netisr_handler *nhp, u_int *qlimitp) {
    (void)nhp;
    *qlimitp = 0;
}

int netisr_setqlimit(const struct netisr_handler *nhp, u_int qlimit) {
    (void)nhp; (void)qlimit;
    return (EOPNOTSUPP);
}

void netisr_getqdrops(const struct netisr_handler *nhp, u_int64_t *qdropsp) {
    (void)nhp;
    *qdropsp = 0;
}

void netisr_clearqdrops(const struct netisr_handler *nhp) {
    (void)nhp;
}

/* --- pfil: the packet filter hook points ---------------------------------
 *
 * pfil is where a firewall (pf, ipfw) inserts itself between layers.
 * Genesis has no firewall, so every hook point passes the packet through
 * unmodified. PFIL_PASS is the "not filtered" answer, which is what an
 * empty hook chain returns upstream too. */
int pfil_mbuf_in(pfil_head_t head, struct mbuf **mp, struct ifnet *ifp,
                 struct inpcb *inp) {
    (void)head; (void)mp; (void)ifp; (void)inp;
    return PFIL_PASS;
}

int pfil_mbuf_out(pfil_head_t head, struct mbuf **mp, struct ifnet *ifp,
                  struct inpcb *inp) {
    (void)head; (void)mp; (void)ifp; (void)inp;
    return PFIL_PASS;
}

/* A REAL head object, empty.
 *
 * Returning NULL was wrong in a way only the macro reveals: PFIL_HOOKED_IN(p)
 * expands to ((struct _pfil_head *)(p))->head_nhooksin > 0, so a NULL head is
 * dereferenced at offset 0 rather than tested. ether_demux does exactly that
 * on every received frame, and the fault was the first packet the interface
 * ever received.
 *
 * A small static head with both hook counts at zero answers the macro
 * correctly - "no filter is attached" - which is the true answer here. One
 * shared object for every head registered: they are all equally empty, and
 * nothing distinguishes them because nothing hooks any of them. */
static struct _pfil_head genesis_empty_pfil_head;

pfil_head_t pfil_head_register(struct pfil_head_args *pa) {
    (void)pa;
    return (pfil_head_t)&genesis_empty_pfil_head;
}

/* --- named absences ------------------------------------------------------
 *
 * Each of these is a whole FreeBSD subsystem that is not here. They are
 * pointers upstream precisely so a kernel built without them leaves them
 * NULL, and the calling code already tests for that - so these are the
 * "option not compiled in" state and not stubs.
 */

/* carp_forus_p moved to the vendored net/if.c with the rest of the CARP
 * pointers - see below. */
/* carp_output_p and the four vlan_*_p pointers moved: net/if.c is VENDORED
 * WHOLE now and upstream defines them there, which is where they belong.
 * They were here only while this file stood in for that one. */

/* if_simloop - loop a packet back to the sending interface, for a broadcast
 * or multicast the machine also has to receive. Genesis has no loopback
 * interface, so this frees the copy rather than delivering it. Real
 * consequence: the machine does not see its own broadcasts. */
int if_simloop(struct ifnet *ifp, struct mbuf *m, int af, int hlen) {
    (void)ifp; (void)af; (void)hlen;
    m_freem(m);
    return 0;
}

/* devctl - the userland device-event notification socket. Nothing listens. */
void devctl_notify(const char *system, const char *subsystem,
                   const char *type, const char *data) {
    (void)system; (void)subsystem; (void)type; (void)data;
}

/* Privilege checks. Genesis has no credentials; everything is permitted,
 * which is prison0's answer - see sys/jail.h for the same argument. */
int priv_check(struct thread *td, int priv) {
    (void)td; (void)priv;
    return 0;
}

void getcredhostuuid(struct ucred *cred, char *buf, size_t size) {
    const char *d = DEFAULT_HOSTUUID;
    size_t i;

    (void)cred;
    for (i = 0; i + 1 < size && d[i] != '\0'; i++) {
        buf[i] = d[i];
    }
    if (size > 0) {
        buf[i] = '\0';
    }
}

void getjailname(struct ucred *cred, char *name, size_t len) {
    (void)cred;
    if (len > 0) {
        name[0] = '\0';
    }
}

/* The UUID-derived Ethernet address registry. Upstream records which
 * generated addresses are in use so two interfaces cannot collide. With one
 * NIC there is nothing to collide with. */
void uuid_ether_add(const uint8_t *addr) { (void)addr; }
void uuid_ether_del(const uint8_t *addr) { (void)addr; }

/* if_name() likewise comes from the vendored net/if.c now. */

/* arc4rand() and arc4random() moved to kernel/bsd/libkern.c when the IP
 * layer arrived: ip_id.c needs the same generator, and
 * libkern/arc4random_uniform.c is vendored beside it there. Same
 * TSC-seeded, NON-CRYPTOGRAPHIC generator, one copy - see that file's
 * comment for what it is and is not safe for. */


/* --- the BPF taps, as real symbols --------------------------------------
 * See net/bpf.h for why these are functions rather than macros. */
void bpf_mtap(struct bpf_if *bp, struct mbuf *m) { (void)bp; (void)m; }
void bpf_mtap2(struct bpf_if *bp, void *data, u_int dlen, struct mbuf *m) {
    (void)bp; (void)data; (void)dlen; (void)m;
}
void bpf_tap(struct bpf_if *bp, u_char *pkt, u_int pktlen) {
    (void)bp; (void)pkt; (void)pktlen;
}

/* link_init_sdl() likewise - it was extracted from net/if.c, and net/if.c
 * is vendored whole now. */
/* llentry_mark_used() comes from the VENDORED net/if_llatbl.c now. It was a
 * no-op here while ARP kept its own cache; with the real link-layer table in
 * place it is what keeps a live entry from being expired out from under
 * traffic that is using it. */


/* asprintf(9) - format into a freshly malloc'd buffer. Used by
 * if_ethersubr.c's ether_gen_addr to build the string it hashes into a MAC
 * address. Two passes: measure with a throwaway buffer, then allocate and
 * format for real - snprintf returns the length it WANTED, which is what
 * makes that possible. */
int asprintf(char **ret, struct malloc_type *type, const char *fmt, ...) {
    va_list ap;
    char probe[2];
    int len;

    va_start(ap, fmt);
    len = vsnprintf(probe, sizeof(probe), fmt, ap);
    va_end(ap);
    if (len < 0) {
        *ret = NULL;
        return -1;
    }
    *ret = (char *)malloc((size_t)len + 1, type, M_NOWAIT);
    if (*ret == NULL) {
        return -1;
    }
    va_start(ap, fmt);
    (void)vsnprintf(*ret, (size_t)len + 1, fmt, ap);
    va_end(ap);
    return len;
}

/* --- the per-CPU thread and the single process --------------------------
 *
 * See <sys/proc.h> in kernel/bsd/compat for why these exist rather than
 * curthread being the per-CPU block reinterpreted: the old arrangement made
 * curthread->td_ucred read the parked user stack pointer.
 *
 * One entry per CPU so that curthread stays a distinct token per CPU (the
 * mutex owner field relies on that), and one shared process because there is
 * one routing table and no credentials to tell processes apart by.
 */
struct proc   genesis_proc0;
struct thread genesis_threads[SMP_MAX_CPUS];

/* Point every thread at the one process and the one credential. Called from
 * flk.c before anything that can reach curthread->td_ucred - which is the
 * socket layer, and netinet/in.c's privilege checks. */
void genesis_threads_init(void) {
    extern struct ucred genesis_ucred0;
    extern void genesis_limits_init(void);
    extern struct plimit *genesis_limit(void);
    int i;

    genesis_limits_init();
    for (i = 0; i < SMP_MAX_CPUS; i++) {
        genesis_threads[i].td_proc = &genesis_proc0;
        genesis_threads[i].td_ucred = &genesis_ucred0;
        genesis_threads[i].td_limit = genesis_limit();
        genesis_threads[i].td_tid = i;
    }
}

/* ------------------------------------------------------------------------
 * The rest of what net/if.c calls into, now that it is vendored whole.
 * ---------------------------------------------------------------------- */

/* --- interface cloning ---------------------------------------------------
 *
 * net/if_clone.c is NOT vendored, and the reason is recorded in
 * kernel/bsd/radix.c: it drags in netlink. These three are what net/if.c's
 * ioctl path calls, and "no cloners are registered" is a true statement about
 * this machine rather than a placeholder.
 *
 * The consequence, named rather than hidden: there is no lo0. Nothing can
 * create a pseudo-interface, so 127.0.0.1 is not reachable and a packet this
 * machine sends to its own address is not looped back. Every address this
 * kernel has belongs to a real NIC.
 */
int if_clone_create(char *name, size_t len, caddr_t params) {
    (void)name; (void)len; (void)params;
    return (EINVAL);
}

int if_clone_destroy(const char *name) {
    (void)name;
    return (EINVAL);
}

int if_clone_list(struct if_clonereq *ifcr) {
    ifcr->ifcr_total = 0;
    ifcr->ifcr_count = 0;
    return (0);
}

void if_clone_addgroup(struct ifnet *ifp, struct if_clone *ifc) {
    (void)ifp; (void)ifc;
}

/* --- nvlist -------------------------------------------------------------
 *
 * libnv is FreeBSD's name-value list, and net/if.c uses it for exactly one
 * thing: SIOCGIFCAPNV / SIOCSIFCAPNV, the ioctl pair that negotiates driver
 * capabilities as a serialised nvlist instead of a bitmask.
 *
 * vendsrc has libnv (sys/contrib/libnv, five files) and it is not vendored
 * here, because that ioctl has no caller: there is no userland ifconfig, and
 * no in-kernel path constructs one. Vendoring four thousand lines of
 * serialisation for a path nothing takes would be worse than saying so.
 *
 * Every function below therefore FAILS rather than returning a plausible
 * empty answer: nvlist_create returns NULL, and net/if.c's caller turns that
 * into ENOMEM for the ioctl. A caller cannot mistake this for success.
 */
MALLOC_DEFINE(M_NVLIST, "nvlist", "NVList");

/* nvlist_create/nvlist_destroy used to be absent from here, because the
 * vendored ZFS reader defined both (for the unrelated nvlist format ZFS
 * keeps its pool configuration in) and one definition is all a link gets.
 * That arrangement was quietly WRONG as well as fragile: ZFS's
 * nvlist_create SUCCEEDED, so SIOCGIFCAPNV got a real list handed to the
 * failing accessors below instead of the clean ENOMEM this block promises.
 * ZFS left the tree (2026-09-26), the link broke on exactly these two names,
 * and they fail here now like everything else in this block. */
struct nvlist *nvlist_create(int flags) {
    (void)flags;
    return (NULL);
}
void  nvlist_destroy(struct nvlist *nvl) {
    (void)nvl;                       /* nothing was ever created */
}
int   nvlist_error(const struct nvlist *nvl) { (void)nvl; return (ENOTSUP); }
bool  nvlist_exists_bool(const struct nvlist *nvl, const char *name) {
    (void)nvl; (void)name;
    return (false);
}
bool  nvlist_get_bool(const struct nvlist *nvl, const char *name) {
    (void)nvl; (void)name;
    return (false);
}
void  nvlist_add_bool(struct nvlist *nvl, const char *name, bool value) {
    (void)nvl; (void)name; (void)value;
}
void *nvlist_pack(const struct nvlist *nvl, size_t *sizep) {
    (void)nvl;
    if (sizep != NULL) {
        *sizep = 0;
    }
    return (NULL);
}
struct nvlist *nvlist_unpack(const void *buf, size_t size, int flags) {
    (void)buf; (void)size; (void)flags;
    return (NULL);
}

/* --- the task queue ------------------------------------------------------
 *
 * net/if.c enqueues if_link_task on taskqueue_swi so a link-state change is
 * reported outside the driver's interrupt handler - which is now what
 * actually happens, rather than the handler running inside the driver's
 * interrupt after all.
 *
 * taskqueue_swi was defined here, and taskqueue_thread and taskqueue_fast in
 * two other files, because each was the file that first needed a non-NULL
 * pointer and none of them was ever read through. All three are in
 * kernel/bsd/kern_taskqueue.c now, next to the queue they name. */

/* --- privilege ----------------------------------------------------------
 *
 * priv_check_cred(cred, priv) asks whether a credential may perform a
 * privileged operation - setting an interface's address, changing its MTU,
 * enabling promiscuous mode. There are no credentials here (see
 * <sys/proc.h>: td_ucred is always NULL) and no unprivileged caller, because
 * every caller is kernel code.
 *
 * So this GRANTS, and that is the honest answer for a kernel with no
 * privilege separation rather than a hole punched in one. It becomes a hole
 * the day a user process can reach one of these paths, which is why it is
 * spelled out here instead of being a macro that returns 0. */
int priv_check_cred(struct ucred *cred, int priv) {
    (void)cred; (void)priv;
    return (0);
}

/* priv_check() itself is already defined earlier in this file. */

/* --- string ------------------------------------------------------------- */

/* strnlen - strlen with a bound, so a string that is not NUL-terminated
 * inside `maxlen` bytes returns maxlen rather than running off the end. That
 * is the whole point of it and it is why net/if.c uses it on a name that came
 * from userland. */
size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;

    while (n < maxlen && s[n] != '\0') {
        n++;
    }
    return (n);
}

/* --- the loopback interface ---------------------------------------------
 *
 * `loif` is upstream's pointer to lo0, and it is NULL here.
 *
 * That is a consequence of if_clone not being vendored (see above and
 * kernel/bsd/radix.c): net/if_loop.c creates lo0 through the cloner, so with
 * no cloner there is no lo0. netinet/in.c tests loif before using it -
 * in_ifinit() adds a loopback route for each address only when one exists -
 * so a NULL here is a supported configuration and not a crash waiting.
 *
 * What it costs, plainly: 127.0.0.1 is not an address of this machine, and a
 * packet sent to one of this machine's OWN addresses goes out of the NIC
 * rather than being looped back internally.
 */
struct ifnet *loif;

/* rt_newmaddrmsg tells a routing-socket listener that a multicast address was
 * added to or removed from an interface. net/rtsock.c is not vendored (it is
 * the routing SOCKET, which needs the socket layer), so there is no listener
 * and nothing to tell. rt_ifmsg and rt_addrmsg are the same story and are
 * beside this one. */
void rt_newmaddrmsg(int cmd, struct ifmultiaddr *ifma) {
    (void)cmd; (void)ifma;
}

/* --- SIGIO ---------------------------------------------------------------
 *
 * A socket can be set to send SIGIO to a process (or process group) when it
 * becomes readable - F_SETOWN plus O_ASYNC. kern/uipc_socket.c carries the
 * owner in so_sigio and calls funsetown() to drop it on close.
 *
 * There is no path to SET one: that goes through fcntl(2) on a file
 * descriptor, and there are no socket descriptors (see kernel/bsd/
 * uipc_socket.c's header for where that line is drawn). So so_sigio is always
 * NULL and funsetown has nothing to release - but it has to exist, because
 * soclose() calls it unconditionally.
 */
/* struct sigio is defined in <sys/signalvar.h>, where upstream declares the
 * functions that take one. */
void funsetown(struct sigio **sigiop) {
    if (sigiop != NULL) {
        *sigiop = NULL;
    }
}

void pgsigio(struct sigio **sigiop, int sig, int checkctty) {
    (void)sigiop; (void)sig; (void)checkctty;
}

/* --- a non-blocking rwlock acquire --------------------------------------
 *
 * netinet/in_pcb.c uses rw_try_wlock() to avoid a lock-order reversal: it
 * wants the pcbinfo lock while holding an inpcb lock, and if it cannot get it
 * without blocking it drops back and retries in the other order.
 *
 * The attempt itself is krw_trywlock() in kernel/lib/mtx.c, next to the rest
 * of the rwlock implementation - the lock owns its own operations, and the
 * interrupt-flag handling a failed attempt needs is only correct inside that
 * file. */
int genesis_rw_try_wlock(struct rwlock *rw) {
    return (krw_trywlock(&rw->grw));
}

/* --- select/poll wakeups out of the socket layer -------------------------
 *
 * <sys/selinfo.h>'s selwakeuppri expands to this. See that header for why it
 * stopped being a no-op: poll(2) on a socket parks on Genesis's shared
 * readiness queue, and the socket layer's only wakeup path is this one.
 *
 * Declared rather than included, the usual way round in this directory:
 * kernel/include/waitq.h reaches process.h and its `struct thread`, which
 * collides with <sys/proc.h>'s. */
struct wait_queue;
struct wait_queue *waitq_readiness(void);
void waitq_wake_all(struct wait_queue *q);

void genesis_sel_wakeup(void) {
    waitq_wake_all(waitq_readiness());
}
