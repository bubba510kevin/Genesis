/* socket(2) - the bridge between a BSD socket and a file descriptor.
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
 * That was not obvious until three other types had been written the same way.
 * Recorded because the entry above will otherwise read as a thing that got
 * easier by itself.
 *
 * --- where the line between this file and syscall.c is drawn -------------
 *
 * kernel/proc/syscall.c cannot see `struct socket` - it is a FreeBSD compat
 * type and syscall.c is not a BSD translation unit (see kern_devsysctl.c for
 * the same split and why). So everything here takes plain scalars and void
 * pointers, and the socket layer is entirely on this side of the boundary.
 * The syscall layer's job reduces to descriptor lookup and errno.
 *
 * --- what works, and what is refused ------------------------------------
 *
 * AF_INET/SOCK_DGRAM works end to end, because that is what is vendored: UDP,
 * ICMP, ARP and the routing table are all real (item 6 verifies them at boot).
 *
 * SOCK_STREAM is REFUSED BY THE PROTOCOL SWITCH, not by this file, and the
 * distinction matters. netinet/in_proto.c names a tcp_protosw whose pr_attach
 * returns EPROTONOSUPPORT, so socreate() fails with the correct errno for the
 * correct reason. Adding a check here would produce the same answer today and
 * the WRONG answer on the day TCP is vendored - the check would still be here
 * refusing it.
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
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/poll.h>
#include <netinet/in.h>

#include "object.h"
#include "socketfd.h"
#include "kprintf.h"

/* --- the object type ------------------------------------------------------
 *
 * body is the struct socket. No wrapper allocation: the socket already has a
 * lifetime managed by the socket layer and one more heap object between the
 * descriptor and it would be a second thing to free on the error paths. */

/* A uio over one buffer, which is what every call here needs.
 *
 * UIO_SYSSPACE and not UIO_USERSPACE, and this is a real decision rather than
 * a convenience. A syscall in this kernel runs on the CALLER'S page tables -
 * there is no separate kernel address space to copy across - so a user
 * pointer is directly dereferenceable here and UIO_SYSSPACE is the accurate
 * description of that. UIO_USERSPACE would route through copyin/copyout,
 * which do the same thing more slowly and would start to matter only if the
 * kernel ever stopped mapping the caller. The syscall layer has already
 * validated the range (user_range_ok) before anything reaches this file. */
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

static int64 sock_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    struct socket *so = (struct socket *)obj->body;
    struct uio     uio;
    struct iovec   iov;
    int            error;

    (void)offset;                       /* a socket has no position */
    if (so == NULL) {
        return -5;                      /* -EIO */
    }
    if (n == 0) {
        return 0;
    }
    uio_one(&uio, &iov, buf, n, UIO_READ);
    error = soreceive(so, NULL, &uio, NULL, NULL, NULL);
    if (error != 0) {
        return -(int64)error;
    }
    /* What was actually transferred, which is what the caller asked for minus
     * what is left. soreceive updates uio_resid rather than returning a
     * count - reading the count off the return value instead would report
     * every successful receive as zero bytes. */
    return (int64)(n - (uint64)uio.uio_resid);
}

static int64 sock_write(object_t *obj, const void *buf, uint64 n,
                        uint64 *offset) {
    struct socket *so = (struct socket *)obj->body;
    struct uio     uio;
    struct iovec   iov;
    int            error;

    (void)offset;
    if (so == NULL) {
        return -5;
    }
    uio_one(&uio, &iov, (void *)(uintptr_t)buf, n, UIO_WRITE);
    error = sosend(so, NULL, &uio, NULL, NULL, 0, curthread);
    if (error != 0) {
        return -(int64)error;
    }
    return (int64)(n - (uint64)uio.uio_resid);
}

/* Readable when the receive buffer has something in it or the peer is gone;
 * writable when the send buffer has room.
 *
 * Read straight off the socket buffers rather than through sopoll_generic,
 * because sopoll's contract is to REGISTER the caller on a select wait list
 * as well as to report - and ob_poll's contract is that it must not change
 * anything (see object.h). The two disagree, and the one that must win here
 * is ob_poll's, because poll(2) calls this speculatively on every descriptor
 * in the set. */
static int sock_poll(object_t *obj, int events) {
    struct socket *so = (struct socket *)obj->body;
    int ready = 0;

    (void)events;
    if (so == NULL) {
        return OB_POLLERR;
    }
    if (sbavail(&so->so_rcv) > 0 || (so->so_rcv.sb_state & SBS_CANTRCVMORE)) {
        ready |= OB_POLLIN;
    }
    if (so->so_snd.sb_state & SBS_CANTSENDMORE) {
        ready |= OB_POLLHUP;
    } else if (sbspace(&so->so_snd) > 0) {
        ready |= OB_POLLOUT;
    }
    if (so->so_error != 0) {
        ready |= OB_POLLERR;
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
    /* The credential is passed EXPLICITLY. socreate stores what it is given
     * in so->so_cred and sbreserve then reads so->so_cred->cr_uidinfo without
     * checking - a socket upstream always has one. There is exactly one in
     * this kernel (kernel/bsd/net_absences.c) and this is it. */
    error = socreate(domain, &so, type, protocol,
                     curthread->td_ucred, curthread);
    if (error != 0 || so == NULL) {
        return -(error != 0 ? error : 12);
    }

    obj = ob_create(&socket_type, so);
    if (obj == NULL) {
        (void)soclose(so);
        return -23;                     /* -ENFILE */
    }
    *out = obj;
    return 0;
}

/* A sockaddr arrives from ring 3 as bytes. sa_len is REWRITTEN from the
 * caller's addrlen rather than trusted, because the vendored code reads
 * sa_len and a program built against Linux's sockaddr does not set it -
 * Linux has no such field. Trusting it would make every Linux-shaped
 * sockaddr look like a zero-length address. */
static int copy_sockaddr(struct sockaddr_storage *dst, const void *src,
                         unsigned int len) {
    if (src == NULL || len < sizeof(struct sockaddr) ||
        len > sizeof(struct sockaddr_storage)) {
        return -22;
    }
    memcpy(dst, src, len);
    dst->ss_len = (unsigned char)len;
    return 0;
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
    error = sobind(so, (struct sockaddr *)&ss, curthread);
    return error != 0 ? -error : 0;
}

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
    error = soconnect(so, (struct sockaddr *)&ss, curthread);
    return error != 0 ? -error : 0;
}

int64 socketfd_sendto(object_t *obj, const void *buf, uint64 len,
                      const void *addr, unsigned int alen) {
    struct socket *so = sock_of(obj);
    struct sockaddr_storage ss;
    struct uio   uio;
    struct iovec iov;
    int          error;

    if (so == NULL) {
        return -88;
    }
    if (addr != NULL) {
        error = copy_sockaddr(&ss, addr, alen);
        if (error != 0) {
            return error;
        }
    }
    uio_one(&uio, &iov, (void *)(uintptr_t)buf, len, UIO_WRITE);
    error = sosend(so, addr != NULL ? (struct sockaddr *)&ss : NULL,
                   &uio, NULL, NULL, 0, curthread);
    if (error != 0) {
        return -(int64)error;
    }
    return (int64)(len - (uint64)uio.uio_resid);
}

int64 socketfd_recvfrom(object_t *obj, void *buf, uint64 len,
                        void *addr, unsigned int *alen) {
    struct socket   *so = sock_of(obj);
    struct sockaddr *from = NULL;
    struct uio       uio;
    struct iovec     iov;
    int              error;
    int64            got;

    if (so == NULL) {
        return -88;
    }
    uio_one(&uio, &iov, buf, len, UIO_READ);
    error = soreceive(so, addr != NULL ? &from : NULL, &uio, NULL, NULL, NULL);
    if (error != 0) {
        if (from != NULL) {
            free(from, M_SONAME);
        }
        return -(int64)error;
    }
    got = (int64)(len - (uint64)uio.uio_resid);

    /* soreceive ALLOCATES the source address and hands ownership over. Not
     * freeing it is a leak on every datagram received, which is the kind that
     * only shows up under load - so it is freed here on both paths, including
     * the one where the caller did not want it. */
    if (from != NULL) {
        if (addr != NULL && alen != NULL) {
            unsigned int n = from->sa_len;

            if (n > *alen) {
                n = *alen;              /* truncated, as recvfrom specifies */
            }
            memcpy(addr, from, n);
            *alen = from->sa_len;
        }
        free(from, M_SONAME);
    } else if (alen != NULL) {
        *alen = 0;
    }
    return got;
}

/* getsockname(2) - what address this socket actually has.
 *
 * The reason it earns its place rather than being a completeness item: it is
 * the ONLY way to find out what bind(2) did. A bind to port 0 asks the kernel
 * to choose, and without this the caller cannot learn what it chose - so a
 * program that binds a wildcard port and then has to tell a peer where to
 * reply simply cannot be written. It is also what makes the bind test in
 * systest a test rather than a call that returned zero: bind can only be
 * shown to have BOUND something by reading the address back.
 *
 * pr_sockaddr is the protocol's own hook (in_getsockaddr for UDP), so this
 * asks the protocol rather than reading the PCB directly - which is what
 * keeps it correct for the next protocol vendored rather than for UDP only. */
int64 socketfd_getsockname(object_t *obj, void *addr, unsigned int *alen) {
    struct socket   *so = sock_of(obj);
    struct sockaddr_storage ss;
    unsigned int     n;
    int              error;

    if (so == NULL) {
        return -88;                     /* -ENOTSOCK */
    }
    if (addr == NULL || alen == NULL) {
        return -22;
    }
    if (so->so_proto == NULL || so->so_proto->pr_sockaddr == NULL) {
        return -95;                     /* -EOPNOTSUPP */
    }
    memset(&ss, 0, sizeof(ss));
    error = (*so->so_proto->pr_sockaddr)(so, (struct sockaddr *)&ss);
    if (error != 0) {
        return -(int64)error;
    }

    n = ss.ss_len != 0 ? ss.ss_len : (unsigned int)sizeof(struct sockaddr_in);
    if (n > *alen) {
        n = *alen;                      /* truncated, as getsockname says */
    }
    memcpy(addr, &ss, n);
    /* The REAL length, not the truncated one - that is how a caller learns
     * its buffer was too small. */
    *alen = ss.ss_len != 0 ? ss.ss_len : (unsigned int)sizeof(struct sockaddr_in);
    return 0;
}
