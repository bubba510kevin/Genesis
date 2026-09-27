/* socket(2) and friends - the bridge between a BSD socket and a descriptor.
 *
 * --- why this was blocked, and what actually unblocked it ----------------
 *
 * ROADMAP item 6 recorded this as "there is no socket(2)... because bridging a
 * socket to a file descriptor is VFS work rather than network work: it needs
 * Genesis's fileobj/vfs layer to grow a fileops vector."
 *
 * That reading was of upstream's shape rather than this kernel's. FreeBSD
 * needs a fileops vector because its descriptors point at struct file, which
 * dispatches through a per-type table it has to be taught about sockets.
 * Genesis's descriptors point at an object_t behind an object_type_t vtable -
 * which IS a fileops vector, and has been since the object manager existed.
 * So nothing had to grow: a socket becomes a descriptor by being an object
 * type, exactly the way pipes, eventfds and socketpairs already are.
 *
 * --- where the line between this file and syscall.c is drawn -------------
 *
 * kernel/proc/syscall.c cannot see `struct socket` - it is a FreeBSD compat
 * type and syscall.c is not a BSD translation unit (see kern_devsysctl.c for
 * the same split and why). So everything here takes plain scalars and void
 * pointers, and the socket layer is entirely on this side of the boundary.
 * The syscall layer's job reduces to descriptor lookup and user pointers.
 *
 * --- the ABI boundary: this file speaks LINUX outward ---------------------
 *
 * The stack is FreeBSD's and the programs are Linux's (and Win32's, whose
 * Winsock structures are laid out the same way on the wire of the syscall).
 * Three things differ between the two and all three are translated HERE, at
 * the one boundary, so neither side has to know about the other:
 *
 *   sockaddr   Linux: u16 family at offset 0. BSD: u8 length, u8 family.
 *              Every address in is rebuilt; every address out is rewritten.
 *   errno      BSD numbers every network error differently (ECONNREFUSED is
 *              61 there and 111 in Linux). to_lx() maps them.
 *   options    SOL_SOCKET is 1 in Linux and 0xffff in BSD, and most option
 *              names differ too. sockopt_map() maps the ones that exist here.
 *   flags      MSG_DONTWAIT, MSG_WAITALL, MSG_NOSIGNAL... also differ.
 *
 * Before TCP this file passed BSD errnos and BSD sockaddrs straight through,
 * which worked only for a test that had been written to the BSD layout.
 *
 * --- what works ------------------------------------------------------------
 *
 * AF_INET SOCK_DGRAM (UDP) and SOCK_STREAM (TCP), both vendored whole, over
 * the NIC and over lo0. Blocking and non-blocking connect, listen/accept,
 * shutdown, socket options, getsockname/getpeername.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/protosw.h>
#include <sys/domain.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sockopt.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "object.h"
#include "socketfd.h"
#include "kprintf.h"

/* --- errno ---------------------------------------------------------------
 *
 * 1..34 are the same numbers in both (the V7 set) apart from EDEADLK, which
 * BSD put at 11 and Linux gave 11 to EAGAIN. Everything from 35 up differs.
 * The table is every BSD errno the socket layer can produce, and the rest
 * that a vendored path could conceivably hand back. */
static int to_lx(int e) {
    switch (e) {
    case 0:               return 0;
    case ERESTART:        return 4;       /* a signal interrupted the sleep */
    case EJUSTRETURN:     return 4;
    case EDEADLK:         return 35;
    case EAGAIN:          return 11;
    case EINPROGRESS:     return 115;
    case EALREADY:        return 114;
    case ENOTSOCK:        return 88;
    case EDESTADDRREQ:    return 89;
    case EMSGSIZE:        return 90;
    case EPROTOTYPE:      return 91;
    case ENOPROTOOPT:     return 92;
    case EPROTONOSUPPORT: return 93;
    case ESOCKTNOSUPPORT: return 94;
    case EOPNOTSUPP:      return 95;
    case EPFNOSUPPORT:    return 96;
    case EAFNOSUPPORT:    return 97;
    case EADDRINUSE:      return 98;
    case EADDRNOTAVAIL:   return 99;
    case ENETDOWN:        return 100;
    case ENETUNREACH:     return 101;
    case ENETRESET:       return 102;
    case ECONNABORTED:    return 103;
    case ECONNRESET:      return 104;
    case ENOBUFS:         return 105;
    case EISCONN:         return 106;
    case ENOTCONN:        return 107;
    case ESHUTDOWN:       return 108;
    case ETOOMANYREFS:    return 109;
    case ETIMEDOUT:       return 110;
    case ECONNREFUSED:    return 111;
    case EHOSTDOWN:       return 112;
    case EHOSTUNREACH:    return 113;
    case ELOOP:           return 40;
    case ENAMETOOLONG:    return 36;
    case ENOTEMPTY:       return 39;
    case ENOLCK:          return 37;
    case ENOSYS:          return 38;
    case EOVERFLOW:       return 75;
    case ECANCELED:       return 125;
    case EBADMSG:         return 74;
    case EPROTO:          return 71;
    default:
        return e >= 1 && e <= 34 ? e : 22;   /* unknown: EINVAL, not garbage */
    }
}

/* The syscall layer's convention: a negative Linux errno. */
static int err(int bsd) {
    return -to_lx(bsd);
}

int socketfd_errno_to_linux(int bsd_errno) {
    return to_lx(bsd_errno);
}

/* --- sockaddr --------------------------------------------------------------
 *
 * In: sa_len is REBUILT from the caller's addrlen rather than read (Linux has
 * no such byte - offset 0 is the low half of a 16-bit family), and the family
 * is read as the 16-bit value it is. AF_INET6 is the one family whose number
 * differs (10 vs 28). */
#define LX_AF_INET6 10

static int copy_sockaddr(struct sockaddr_storage *dst, const void *src,
                         unsigned int len) {
    const unsigned char *b = src;
    unsigned int family;

    if (src == NULL || len < 2 || len > sizeof(struct sockaddr_storage)) {
        return -22;
    }
    family = (unsigned int)b[0] | ((unsigned int)b[1] << 8);
    if (family == LX_AF_INET6) {
        family = AF_INET6;
    }
    if (family == AF_INET && len < sizeof(struct sockaddr_in)) {
        return -22;
    }
    memset(dst, 0, sizeof(*dst));
    memcpy(dst, src, len);
    dst->ss_len = (unsigned char)len;
    dst->ss_family = (sa_family_t)family;
    return 0;
}

/* Out: a BSD sockaddr written to the caller in Linux layout. `alen` is in/out
 * - the caller's buffer size going in, the address's REAL length coming out,
 * which may be larger (meaning truncation), as getsockname/accept/recvfrom
 * all specify. */
static void put_sockaddr(void *out, unsigned int *alen,
                         const struct sockaddr *sa) {
    unsigned char tmp[sizeof(struct sockaddr_storage)];
    unsigned int real, n, family;

    real = sa->sa_len != 0 ? sa->sa_len : (unsigned int)sizeof(struct sockaddr_in);
    if (real > sizeof(tmp)) {
        real = sizeof(tmp);
    }
    memcpy(tmp, sa, real);
    family = sa->sa_family == AF_INET6 ? LX_AF_INET6 : sa->sa_family;
    tmp[0] = (unsigned char)family;
    tmp[1] = (unsigned char)(family >> 8);
    n = real < *alen ? real : *alen;
    memcpy(out, tmp, n);
    *alen = real;
}

/* --- MSG_ flags --------------------------------------------------------------
 *
 * Refused if unknown, rather than ignored: MSG_OOB, MSG_DONTROUTE and the rest
 * each change what the call does, and a caller silently given the default
 * gets behaviour it did not ask for at a moment it cannot see. MSG_MORE is
 * the exception - it is a coalescing HINT, and ignoring a hint is honouring
 * it. */
#define LX_MSG_OOB        0x1
#define LX_MSG_PEEK       0x2
#define LX_MSG_DONTROUTE  0x4
#define LX_MSG_CTRUNC     0x8
#define LX_MSG_TRUNC      0x20
#define LX_MSG_DONTWAIT   0x40
#define LX_MSG_EOR        0x80
#define LX_MSG_WAITALL    0x100
#define LX_MSG_NOSIGNAL   0x4000
#define LX_MSG_MORE       0x8000

static int msg_flags_in(int lx, int *bsd) {
    int out = 0;

    if (lx & ~(LX_MSG_OOB | LX_MSG_PEEK | LX_MSG_DONTROUTE | LX_MSG_DONTWAIT |
               LX_MSG_EOR | LX_MSG_WAITALL | LX_MSG_NOSIGNAL | LX_MSG_MORE |
               LX_MSG_TRUNC)) {
        return -95;                     /* -EOPNOTSUPP */
    }
    if (lx & LX_MSG_OOB)       out |= MSG_OOB;
    if (lx & LX_MSG_PEEK)      out |= MSG_PEEK;
    if (lx & LX_MSG_DONTROUTE) out |= MSG_DONTROUTE;
    if (lx & LX_MSG_DONTWAIT)  out |= MSG_DONTWAIT;
    if (lx & LX_MSG_EOR)       out |= MSG_EOR;
    if (lx & LX_MSG_WAITALL)   out |= MSG_WAITALL;
    if (lx & LX_MSG_NOSIGNAL)  out |= MSG_NOSIGNAL;
    if (lx & LX_MSG_TRUNC)     out |= MSG_TRUNC;
    *bsd = out;
    return 0;
}

static int msg_flags_out(int bsd) {
    int out = 0;

    if (bsd & MSG_OOB)    out |= LX_MSG_OOB;
    if (bsd & MSG_EOR)    out |= LX_MSG_EOR;
    if (bsd & MSG_TRUNC)  out |= LX_MSG_TRUNC;
    if (bsd & MSG_CTRUNC) out |= LX_MSG_CTRUNC;
    return out;
}

/* --- the object type ------------------------------------------------------
 *
 * body is the struct socket. No wrapper allocation: the socket already has a
 * lifetime managed by the socket layer and one more heap object between the
 * descriptor and it would be a second thing to free on the error paths. */

/* A uio over one buffer, which is what most calls here need.
 *
 * UIO_SYSSPACE and not UIO_USERSPACE, and this is a real decision rather than
 * a convenience. A syscall in this kernel runs on the CALLER'S page tables -
 * there is no separate kernel address space to copy across - so a user
 * pointer is directly dereferenceable here and UIO_SYSSPACE is the accurate
 * description of that. The syscall layer has already validated the range
 * (user_range_ok) before anything reaches this file. */
static void uio_one(struct uio *uio, struct iovec *iov, void *buf,
                    uint64_t len, enum uio_rw rw) {
    iov->iov_base = buf;
    iov->iov_len  = (size_t)len;
    memset(uio, 0, sizeof(*uio));
    uio->uio_iov    = iov;
    uio->uio_iovcnt = 1;
    uio->uio_offset = 0;
    uio->uio_resid  = (ssize_t)len;
    uio->uio_segflg = UIO_SYSSPACE;
    uio->uio_rw     = rw;
    uio->uio_td     = curthread;
}

/* EPIPE from a send means the peer is gone, and a Linux program expects
 * SIGPIPE with it unless it said MSG_NOSIGNAL or set SO_NOSIGPIPE - exactly
 * what upstream's soo_write/sendit do. Raised by the syscall layer, which
 * owns signals; this says whether to. */
static int want_sigpipe(struct socket *so, int error, int bsdflags) {
    return error == EPIPE && (bsdflags & MSG_NOSIGNAL) == 0 &&
           (so->so_options & SO_NOSIGPIPE) == 0;
}

static void (*sigpipe_hook)(void);

void socketfd_set_sigpipe_hook(void (*fn)(void)) {
    sigpipe_hook = fn;
}

static int64 do_send(struct socket *so, struct uio *uio, struct sockaddr *to,
                     int bsdflags) {
    uint64 len = (uint64)uio->uio_resid;
    int error;

    error = sosend(so, to, uio, NULL, NULL, bsdflags, curthread);
    /* A partial send interrupted by a signal or a timeout still sent
     * something, and that count is the answer - upstream's sendit makes the
     * same exception. */
    if (error != 0 && (uint64)uio->uio_resid != len &&
        (error == ERESTART || error == EINTR || error == EWOULDBLOCK)) {
        error = 0;
    }
    if (error != 0) {
        if (want_sigpipe(so, error, bsdflags) && sigpipe_hook != NULL) {
            sigpipe_hook();
        }
        return err(error);
    }
    return (int64)(len - (uint64)uio->uio_resid);
}

static int64 do_recv(struct socket *so, struct uio *uio, struct sockaddr **from,
                     int *bsdflags) {
    uint64 len = (uint64)uio->uio_resid;
    int error;

    error = soreceive(so, from, uio, NULL, NULL, bsdflags);
    if (error != 0 && (uint64)uio->uio_resid != len &&
        (error == ERESTART || error == EINTR || error == EWOULDBLOCK)) {
        error = 0;
    }
    if (error != 0) {
        return err(error);
    }
    /* What was actually transferred, which is what the caller asked for minus
     * what is left. soreceive updates uio_resid rather than returning a
     * count. */
    return (int64)(len - (uint64)uio->uio_resid);
}

static int64 sock_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    struct socket *so = (struct socket *)obj->body;
    struct uio     uio;
    struct iovec   iov;

    (void)offset;                       /* a socket has no position */
    if (so == NULL) {
        return -5;                      /* -EIO */
    }
    if (n == 0) {
        return 0;
    }
    if (SOLISTENING(so)) {
        return -107;                    /* -ENOTCONN: read(2) on a listener */
    }
    uio_one(&uio, &iov, buf, n, UIO_READ);
    return do_recv(so, &uio, NULL, NULL);
}

static int64 sock_write(object_t *obj, const void *buf, uint64 n,
                        uint64 *offset) {
    struct socket *so = (struct socket *)obj->body;
    struct uio     uio;
    struct iovec   iov;

    (void)offset;
    if (so == NULL) {
        return -5;
    }
    if (SOLISTENING(so)) {
        return -107;
    }
    uio_one(&uio, &iov, (void *)(uintptr_t)buf, n, UIO_WRITE);
    return do_send(so, &uio, NULL, 0);
}

/* Readiness, by upstream's own predicates (soreadable / sowriteable in
 * socketvar.h, spelled out because a listening socket must not reach them).
 *
 * A LISTENING socket's buffers do not exist - solisten reuses that part of
 * struct socket for the accept queues - so it is asked a different question:
 * readable means a completed connection is waiting for accept(2).
 *
 * Read straight off the socket rather than through sopoll_generic, because
 * sopoll's contract is to REGISTER the caller on a select wait list as well
 * as to report - and ob_poll's contract is that it must not change anything
 * (see object.h). */
static int sock_poll(object_t *obj, int events) {
    struct socket *so = (struct socket *)obj->body;
    int ready = 0;

    (void)events;
    if (so == NULL) {
        return OB_POLLERR;
    }
    if (SOLISTENING(so)) {
        if (!TAILQ_EMPTY(&so->sol_comp) || so->so_error != 0) {
            ready |= OB_POLLIN;
        }
        return ready;
    }
    if (sbavail(&so->so_rcv) >= so->so_rcv.sb_lowat ||
        (so->so_rcv.sb_state & SBS_CANTRCVMORE) || so->so_error != 0) {
        ready |= OB_POLLIN;
    }
    if (so->so_snd.sb_state & SBS_CANTSENDMORE) {
        ready |= OB_POLLOUT;            /* a write will fail at once: EPIPE */
        if (so->so_rcv.sb_state & SBS_CANTRCVMORE) {
            ready |= OB_POLLHUP;
        }
    } else if (sbspace(&so->so_snd) >= so->so_snd.sb_lowat &&
               ((so->so_state & SS_ISCONNECTED) ||
                (so->so_proto->pr_flags & PR_CONNREQUIRED) == 0)) {
        ready |= OB_POLLOUT;
    }
    if (so->so_error != 0) {
        ready |= OB_POLLERR | OB_POLLOUT;
    }
    return ready;
}

static void sock_destroy(object_t *obj) {
    struct socket *so = (struct socket *)obj->body;

    if (so != NULL) {
        (void)soclose(so);
        obj->body = NULL;
    }
}

static const object_type_t socket_type = {
    .name    = "socket",
    .klass   = OBJ_SOCKET,
    .read    = sock_read,
    .write   = sock_write,
    .poll    = sock_poll,
    .destroy = sock_destroy
};

int socketfd_is_socket(const object_t *obj) {
    return obj != NULL && obj->type == &socket_type;
}

static struct socket *sock_of(object_t *obj) {
    if (!socketfd_is_socket(obj)) {
        return NULL;
    }
    return (struct socket *)obj->body;
}

/* --- the operations the syscall layer calls ------------------------------ */

int socketfd_create(int domain, int type, int protocol, object_t **out) {
    struct socket *so = NULL;
    object_t      *obj;
    int            error;

    if (out == NULL) {
        return -22;
    }
    if (domain == LX_AF_INET6) {
        domain = AF_INET6;
    }
    /* The credential is passed EXPLICITLY. socreate stores what it is given
     * in so->so_cred and sbreserve then reads so->so_cred->cr_uidinfo without
     * checking - a socket upstream always has one. There is exactly one in
     * this kernel (kernel/bsd/net_absences.c) and this is it. */
    error = socreate(domain, &so, type, protocol,
                     curthread->td_ucred, curthread);
    if (error != 0 || so == NULL) {
        return err(error != 0 ? error : ENOMEM);
    }

    obj = ob_create(&socket_type, so);
    if (obj == NULL) {
        (void)soclose(so);
        return -23;                     /* -ENFILE */
    }
    *out = obj;
    return 0;
}

/* O_NONBLOCK lives on the open file (fcntl sets it there); the socket layer
 * reads SS_NBIO. The syscall layer keeps the two in step through this, at
 * socket(SOCK_NONBLOCK), accept4(SOCK_NONBLOCK) and fcntl(F_SETFL). */
void socketfd_set_nonblock(object_t *obj, int on) {
    struct socket *so = sock_of(obj);

    if (so == NULL) {
        return;
    }
    SOCK_LOCK(so);
    if (on) {
        so->so_state |= SS_NBIO;
    } else {
        so->so_state &= ~SS_NBIO;
    }
    SOCK_UNLOCK(so);
}

int socketfd_bind(object_t *obj, const void *addr, unsigned int len) {
    struct socket *so = sock_of(obj);
    struct sockaddr_storage ss;
    int error;

    if (so == NULL) {
        return -88;                     /* -ENOTSOCK */
    }
    error = copy_sockaddr(&ss, addr, len);
    if (error != 0) {
        return error;
    }
    return err(sobind(so, (struct sockaddr *)&ss, curthread));
}

/* connect(2), with the wait upstream's kern_connectat does.
 *
 * soconnect only STARTS a TCP connection: it sends the SYN and returns with
 * SS_ISCONNECTING set. A blocking connect then sleeps on so_timeo until
 * soisconnected() or a failure (which sets so_error) wakes it, and the answer
 * is whatever so_error says. A non-blocking one returns EINPROGRESS at once,
 * and the caller learns the outcome from poll(POLLOUT) and SO_ERROR. UDP
 * never sets SS_ISCONNECTING, so for it this is the plain call it was. */
int socketfd_connect(object_t *obj, const void *addr, unsigned int len) {
    struct socket *so = sock_of(obj);
    struct sockaddr_storage ss;
    int error;

    if (so == NULL) {
        return -88;
    }
    error = copy_sockaddr(&ss, addr, len);
    if (error != 0) {
        return error;
    }
    if (so->so_state & SS_ISCONNECTING) {
        return err(EALREADY);
    }
    error = soconnect(so, (struct sockaddr *)&ss, curthread);
    if (error != 0) {
        return err(error);
    }
    if ((so->so_state & SS_NBIO) && (so->so_state & SS_ISCONNECTING)) {
        return err(EINPROGRESS);
    }
    SOCK_LOCK(so);
    while ((so->so_state & SS_ISCONNECTING) && so->so_error == 0) {
        error = msleep(&so->so_timeo, SOCK_MTX(so), PSOCK | PCATCH,
                       "connec", 0);
        if (error != 0) {
            break;
        }
    }
    if (error == 0) {
        error = so->so_error;
        so->so_error = 0;
    }
    SOCK_UNLOCK(so);
    return err(error);
}

int socketfd_listen(object_t *obj, int backlog) {
    struct socket *so = sock_of(obj);

    if (so == NULL) {
        return -88;
    }
    return err(solisten(so, backlog, curthread));
}

/* accept(2) / accept4(2): upstream's kern_accept4, minus the descriptor
 * bookkeeping the syscall layer does.
 *
 * solisten_dequeue sleeps on sol_comp until a connection completes (or
 * returns EWOULDBLOCK for a non-blocking listener), takes the first one off
 * the queue and releases the listen lock. soaccept then asks the protocol for
 * the peer's address. The new socket is a new object and a new descriptor;
 * the listener is untouched. */
int socketfd_accept(object_t *obj, int nonblock, object_t **out,
                    void *addr, unsigned int *alen) {
    struct socket *head = sock_of(obj), *so;
    struct sockaddr_storage ss;
    object_t *nobj;
    int error;

    if (head == NULL) {
        return -88;
    }
    if (!SOLISTENING(head)) {
        return -22;                     /* -EINVAL: not listening */
    }
    SOLISTEN_LOCK(head);
    error = solisten_dequeue(head, &so, nonblock ? SOCK_NONBLOCK : 0);
    if (error != 0) {
        return err(error);
    }
    memset(&ss, 0, sizeof(ss));
    ss.ss_len = sizeof(ss);
    error = soaccept(so, (struct sockaddr *)&ss);
    if (error != 0) {
        (void)soclose(so);
        return err(error);
    }
    nobj = ob_create(&socket_type, so);
    if (nobj == NULL) {
        (void)soclose(so);
        return -23;
    }
    if (addr != NULL && alen != NULL) {
        put_sockaddr(addr, alen, (struct sockaddr *)&ss);
    }
    *out = nobj;
    return 0;
}

int socketfd_shutdown(object_t *obj, int how) {
    struct socket *so = sock_of(obj);
    int error;

    if (so == NULL) {
        return -88;
    }
    if (how < SHUT_RD || how > SHUT_RDWR) {
        return -22;
    }
    error = soshutdown(so, (enum shutdown_how)how);
    /* upstream's kern_shutdown: a datagram socket that was never connected
     * answers 0, because programs (syslogd among them) depend on it. */
    if (error == ENOTCONN && so->so_type == SOCK_DGRAM) {
        error = 0;
    }
    return err(error);
}

int64 socketfd_sendto(object_t *obj, const void *buf, uint64 len, int flags,
                      const void *addr, unsigned int alen) {
    struct socket *so = sock_of(obj);
    struct sockaddr_storage ss;
    struct uio   uio;
    struct iovec iov;
    int          error, bsdflags;

    if (so == NULL) {
        return -88;
    }
    error = msg_flags_in(flags, &bsdflags);
    if (error != 0) {
        return error;
    }
    if (addr != NULL) {
        error = copy_sockaddr(&ss, addr, alen);
        if (error != 0) {
            return error;
        }
    }
    uio_one(&uio, &iov, (void *)(uintptr_t)buf, len, UIO_WRITE);
    return do_send(so, &uio, addr != NULL ? (struct sockaddr *)&ss : NULL,
                   bsdflags);
}

int64 socketfd_recvfrom(object_t *obj, void *buf, uint64 len, int flags,
                        void *addr, unsigned int *alen) {
    struct socket   *so = sock_of(obj);
    struct sockaddr *from = NULL;
    struct uio       uio;
    struct iovec     iov;
    int              error, bsdflags;
    int64            got;

    if (so == NULL) {
        return -88;
    }
    error = msg_flags_in(flags, &bsdflags);
    if (error != 0) {
        return error;
    }
    uio_one(&uio, &iov, buf, len, UIO_READ);
    got = do_recv(so, &uio, addr != NULL ? &from : NULL, &bsdflags);

    /* soreceive ALLOCATES the source address and hands ownership over. Not
     * freeing it is a leak on every datagram received, so it is freed on
     * both paths, including the one where the caller did not want it. A
     * connected TCP socket reports no source (Linux leaves addrlen alone
     * there too, but a 0 is what upstream answers and is harmless). */
    if (from != NULL) {
        if (got >= 0 && addr != NULL && alen != NULL) {
            put_sockaddr(addr, alen, from);
        }
        free(from, M_SONAME);
    } else if (got >= 0 && alen != NULL) {
        *alen = 0;
    }
    return got;
}

/* sendmsg / recvmsg over an iovec array. The iovecs have been range-checked
 * by the syscall layer; ancillary data (msg_control) is refused on send and
 * returned empty on receive - there is no SCM_RIGHTS (AF_UNIX is a
 * socketpair of pipes here) and no IP_RECV* option that produces any. */
int64 socketfd_sendmsg(object_t *obj, const void *iov, int iovcnt, int flags,
                       const void *addr, unsigned int alen) {
    struct socket *so = sock_of(obj);
    struct sockaddr_storage ss;
    struct uio uio;
    ssize_t total = 0;
    int i, error, bsdflags;

    if (so == NULL) {
        return -88;
    }
    error = msg_flags_in(flags, &bsdflags);
    if (error != 0) {
        return error;
    }
    if (addr != NULL) {
        error = copy_sockaddr(&ss, addr, alen);
        if (error != 0) {
            return error;
        }
    }
    for (i = 0; i < iovcnt; i++) {
        total += (ssize_t)((const struct iovec *)iov)[i].iov_len;
    }
    memset(&uio, 0, sizeof(uio));
    uio.uio_iov    = (struct iovec *)(uintptr_t)iov;
    uio.uio_iovcnt = iovcnt;
    uio.uio_resid  = total;
    uio.uio_segflg = UIO_SYSSPACE;
    uio.uio_rw     = UIO_WRITE;
    uio.uio_td     = curthread;
    return do_send(so, &uio, addr != NULL ? (struct sockaddr *)&ss : NULL,
                   bsdflags);
}

int64 socketfd_recvmsg(object_t *obj, void *iov, int iovcnt, int flags,
                       void *addr, unsigned int *alen, int *flags_out) {
    struct socket *so = sock_of(obj);
    struct sockaddr *from = NULL;
    struct uio uio;
    ssize_t total = 0;
    int i, error, bsdflags;
    int64 got;

    if (so == NULL) {
        return -88;
    }
    error = msg_flags_in(flags, &bsdflags);
    if (error != 0) {
        return error;
    }
    for (i = 0; i < iovcnt; i++) {
        total += (ssize_t)((struct iovec *)iov)[i].iov_len;
    }
    memset(&uio, 0, sizeof(uio));
    uio.uio_iov    = (struct iovec *)iov;
    uio.uio_iovcnt = iovcnt;
    uio.uio_resid  = total;
    uio.uio_segflg = UIO_SYSSPACE;
    uio.uio_rw     = UIO_READ;
    uio.uio_td     = curthread;
    got = do_recv(so, &uio, addr != NULL ? &from : NULL, &bsdflags);
    if (from != NULL) {
        if (got >= 0 && alen != NULL) {
            put_sockaddr(addr, alen, from);
        }
        free(from, M_SONAME);
    } else if (got >= 0 && alen != NULL) {
        *alen = 0;
    }
    if (got >= 0 && flags_out != NULL) {
        *flags_out = msg_flags_out(bsdflags);
    }
    return got;
}

/* getsockname(2) - what address this socket actually has. The ONLY way to
 * find out what a bind to port 0 chose. sosockaddr asks the protocol
 * (in_getsockaddr for both UDP and TCP) rather than reading the PCB. */
int64 socketfd_getsockname(object_t *obj, void *addr, unsigned int *alen) {
    struct socket   *so = sock_of(obj);
    struct sockaddr_storage ss;
    int              error;

    if (so == NULL) {
        return -88;                     /* -ENOTSOCK */
    }
    if (addr == NULL || alen == NULL) {
        return -22;
    }
    memset(&ss, 0, sizeof(ss));
    ss.ss_len = sizeof(ss);
    error = sosockaddr(so, (struct sockaddr *)&ss);
    if (error != 0) {
        return err(error);
    }
    put_sockaddr(addr, alen, (struct sockaddr *)&ss);
    return 0;
}

/* getpeername(2). ENOTCONN on an unconnected socket - upstream's
 * kern_getpeername checks before asking the protocol, and so does this. */
int64 socketfd_getpeername(object_t *obj, void *addr, unsigned int *alen) {
    struct socket   *so = sock_of(obj);
    struct sockaddr_storage ss;
    int              error;

    if (so == NULL) {
        return -88;
    }
    if (addr == NULL || alen == NULL) {
        return -22;
    }
    if (SOLISTENING(so) ||
        (so->so_state & SS_ISCONNECTED) == 0) {
        return err(ENOTCONN);
    }
    memset(&ss, 0, sizeof(ss));
    ss.ss_len = sizeof(ss);
    error = sopeeraddr(so, (struct sockaddr *)&ss);
    if (error != 0) {
        return err(error);
    }
    put_sockaddr(addr, alen, (struct sockaddr *)&ss);
    return 0;
}

/* --- socket options ----------------------------------------------------------
 *
 * Linux (level, name) -> BSD (level, name). Only options whose VALUE has the
 * same layout on both sides are mapped - an int, a struct linger, a struct
 * timeval (both 16 bytes on amd64), a struct ip_mreq(n). An option with no
 * entry is ENOPROTOOPT, which is what either kernel answers for an option it
 * does not have. */
#define LX_SOL_SOCKET 1
#define LX_SOL_IP     0
#define LX_SOL_TCP    6

/* Answered here rather than by the stack: Linux-only, read-only. */
#define LX_SO_PROTOCOL 38
#define LX_SO_DOMAIN   39
#define LX_SO_ERROR    4

static const struct { int lx_level, lx_name, level, name; } sockopt_table[] = {
    { LX_SOL_SOCKET,  1, SOL_SOCKET, SO_DEBUG },
    { LX_SOL_SOCKET,  2, SOL_SOCKET, SO_REUSEADDR },
    { LX_SOL_SOCKET,  3, SOL_SOCKET, SO_TYPE },
    { LX_SOL_SOCKET,  4, SOL_SOCKET, SO_ERROR },
    { LX_SOL_SOCKET,  5, SOL_SOCKET, SO_DONTROUTE },
    { LX_SOL_SOCKET,  6, SOL_SOCKET, SO_BROADCAST },
    { LX_SOL_SOCKET,  7, SOL_SOCKET, SO_SNDBUF },
    { LX_SOL_SOCKET,  8, SOL_SOCKET, SO_RCVBUF },
    { LX_SOL_SOCKET,  9, SOL_SOCKET, SO_KEEPALIVE },
    { LX_SOL_SOCKET, 10, SOL_SOCKET, SO_OOBINLINE },
    { LX_SOL_SOCKET, 13, SOL_SOCKET, SO_LINGER },
    { LX_SOL_SOCKET, 15, SOL_SOCKET, SO_REUSEPORT },
    { LX_SOL_SOCKET, 18, SOL_SOCKET, SO_RCVLOWAT },
    { LX_SOL_SOCKET, 19, SOL_SOCKET, SO_SNDLOWAT },
    { LX_SOL_SOCKET, 20, SOL_SOCKET, SO_RCVTIMEO },
    { LX_SOL_SOCKET, 21, SOL_SOCKET, SO_SNDTIMEO },
    { LX_SOL_SOCKET, 30, SOL_SOCKET, SO_ACCEPTCONN },
    { LX_SOL_TCP,     1, IPPROTO_TCP, TCP_NODELAY },
    { LX_SOL_TCP,     2, IPPROTO_TCP, TCP_MAXSEG },
    { LX_SOL_TCP,     4, IPPROTO_TCP, TCP_KEEPIDLE },
    { LX_SOL_TCP,     5, IPPROTO_TCP, TCP_KEEPINTVL },
    { LX_SOL_TCP,     6, IPPROTO_TCP, TCP_KEEPCNT },
    { LX_SOL_TCP,    13, IPPROTO_TCP, TCP_CONGESTION },
    { LX_SOL_IP,      1, IPPROTO_IP, IP_TOS },
    { LX_SOL_IP,      2, IPPROTO_IP, IP_TTL },
    { LX_SOL_IP,      3, IPPROTO_IP, IP_HDRINCL },
    { LX_SOL_IP,     32, IPPROTO_IP, IP_MULTICAST_IF },
    { LX_SOL_IP,     33, IPPROTO_IP, IP_MULTICAST_TTL },
    { LX_SOL_IP,     34, IPPROTO_IP, IP_MULTICAST_LOOP },
    { LX_SOL_IP,     35, IPPROTO_IP, IP_ADD_MEMBERSHIP },
    { LX_SOL_IP,     36, IPPROTO_IP, IP_DROP_MEMBERSHIP },
};

static int sockopt_map(int lx_level, int lx_name, int *level, int *name) {
    unsigned i;

    for (i = 0; i < sizeof(sockopt_table) / sizeof(sockopt_table[0]); i++) {
        if (sockopt_table[i].lx_level == lx_level &&
            sockopt_table[i].lx_name == lx_name) {
            *level = sockopt_table[i].level;
            *name  = sockopt_table[i].name;
            return 0;
        }
    }
    return -92;                         /* -ENOPROTOOPT */
}

int socketfd_setsockopt(object_t *obj, int lx_level, int lx_name,
                        const void *val, unsigned int len) {
    struct socket *so = sock_of(obj);
    struct sockopt sopt;
    int error, level, name;

    if (so == NULL) {
        return -88;
    }
    error = sockopt_map(lx_level, lx_name, &level, &name);
    if (error != 0) {
        return error;
    }
    memset(&sopt, 0, sizeof(sopt));
    sopt.sopt_dir     = SOPT_SET;
    sopt.sopt_level   = level;
    sopt.sopt_name    = name;
    sopt.sopt_val     = (void *)(uintptr_t)val;
    sopt.sopt_valsize = len;
    sopt.sopt_td      = NULL;           /* sooptcopyin: kernel-space bcopy */
    return err(sosetopt(so, &sopt));
}

int socketfd_getsockopt(object_t *obj, int lx_level, int lx_name,
                        void *val, unsigned int *len) {
    struct socket *so = sock_of(obj);
    struct sockopt sopt;
    int error, level, name, v;

    if (so == NULL) {
        return -88;
    }
    if (lx_level == LX_SOL_SOCKET &&
        (lx_name == LX_SO_DOMAIN || lx_name == LX_SO_PROTOCOL)) {
        if (*len < sizeof(int)) {
            return -22;
        }
        if (lx_name == LX_SO_DOMAIN) {
            v = so->so_proto->pr_domain->dom_family;
            v = v == AF_INET6 ? LX_AF_INET6 : v;
        } else {
            v = so->so_proto->pr_protocol;
        }
        memcpy(val, &v, sizeof(v));
        *len = sizeof(int);
        return 0;
    }
    error = sockopt_map(lx_level, lx_name, &level, &name);
    if (error != 0) {
        return error;
    }
    memset(&sopt, 0, sizeof(sopt));
    sopt.sopt_dir     = SOPT_GET;
    sopt.sopt_level   = level;
    sopt.sopt_name    = name;
    sopt.sopt_val     = val;
    sopt.sopt_valsize = *len;
    sopt.sopt_td      = NULL;
    error = sogetopt(so, &sopt);
    if (error != 0) {
        return err(error);
    }
    /* SO_ERROR's VALUE is an errno, so it is translated too - otherwise a
     * non-blocking connect that was refused reports 61 to a program that
     * compares against ECONNREFUSED (111). */
    if (lx_level == LX_SOL_SOCKET && lx_name == LX_SO_ERROR &&
        sopt.sopt_valsize >= sizeof(int)) {
        memcpy(&v, val, sizeof(v));
        v = to_lx(v);
        memcpy(val, &v, sizeof(v));
    }
    *len = (unsigned int)sopt.sopt_valsize;
    return 0;
}
