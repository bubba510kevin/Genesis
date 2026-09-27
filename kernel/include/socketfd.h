#ifndef SOCKETFD_H
#define SOCKETFD_H

#include "typesk.h"

struct object;

/* socket(2) and friends, as far as the syscall layer can see them.
 *
 * Everything here takes plain scalars and void pointers on purpose:
 * kernel/proc/syscall.c cannot see `struct socket`, which is a FreeBSD compat
 * type, so the whole socket layer stays on the other side of this header.
 * See kernel/bsd/kern_socketfd.c for why that split exists.
 *
 * The values crossing this header are LINUX's: sockaddrs in Linux layout,
 * MSG_ flags and socket-option numbers as Linux defines them. The
 * translation to the vendored FreeBSD stack happens behind it.
 *
 * All of these return 0 or a byte count on success, or a NEGATIVE LINUX
 * ERRNO - already negated, so the syscall layer passes the value straight
 * out. `alen` arguments are in/out: the caller's buffer size going in, the
 * address's real length coming out - which may be LARGER, meaning the
 * address was truncated. */
int   socketfd_create(int domain, int type, int protocol,
                      struct object **out);
void  socketfd_set_nonblock(struct object *obj, int on);
int   socketfd_bind(struct object *obj, const void *addr, unsigned int len);
int   socketfd_connect(struct object *obj, const void *addr, unsigned int len);
int   socketfd_listen(struct object *obj, int backlog);
int   socketfd_accept(struct object *obj, int nonblock, struct object **out,
                      void *addr, unsigned int *alen);
int   socketfd_shutdown(struct object *obj, int how);
int64 socketfd_sendto(struct object *obj, const void *buf, uint64 len,
                      int flags, const void *addr, unsigned int alen);
int64 socketfd_recvfrom(struct object *obj, void *buf, uint64 len,
                        int flags, void *addr, unsigned int *alen);
/* `iov` is an array of struct iovec (base, len), already range-checked. */
int64 socketfd_sendmsg(struct object *obj, const void *iov, int iovcnt,
                       int flags, const void *addr, unsigned int alen);
int64 socketfd_recvmsg(struct object *obj, void *iov, int iovcnt, int flags,
                       void *addr, unsigned int *alen, int *flags_out);
int64 socketfd_getsockname(struct object *obj, void *addr,
                           unsigned int *alen);
int64 socketfd_getpeername(struct object *obj, void *addr,
                           unsigned int *alen);
int   socketfd_setsockopt(struct object *obj, int level, int name,
                          const void *val, unsigned int len);
int   socketfd_getsockopt(struct object *obj, int level, int name,
                          void *val, unsigned int *len);

/* A FreeBSD errno as the Linux number - for callers outside this file that
 * receive one from the stack. */
int   socketfd_errno_to_linux(int bsd_errno);

/* Called when a send fails with EPIPE and the caller did not suppress
 * SIGPIPE; the syscall layer installs the function that raises it. */
void  socketfd_set_sigpipe_hook(void (*fn)(void));

/* Non-zero if this object is a socket. fstat needs it for the same reason
 * pipe_is_pipe does: a libc works out what it holds from st_mode. */
int   socketfd_is_socket(const struct object *obj);

#endif
