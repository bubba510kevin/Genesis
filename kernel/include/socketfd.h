#ifndef SOCKETFD_H
#define SOCKETFD_H

#include "typesk.h"

struct object;

/* socket(2) and friends, as far as the syscall layer can see them.
 *
 * Everything here takes plain scalars and void pointers on purpose:
 * kernel/proc/syscall.c cannot see `struct socket`, which is a FreeBSD compat
 * type, so the whole socket layer stays on the other side of this header.
 * See kernel/bsd/kern_socketfd.c for why that split exists and for why a
 * socket needed no new VFS machinery to become a descriptor.
 *
 * All of these return 0 or a byte count on success, or a NEGATIVE ERRNO -
 * already negated, so the syscall layer passes the value straight out. */
int   socketfd_create(int domain, int type, int protocol,
                      struct object **out);
int   socketfd_bind(struct object *obj, const void *addr, unsigned int len);
int   socketfd_connect(struct object *obj, const void *addr, unsigned int len);
int64 socketfd_sendto(struct object *obj, const void *buf, uint64 len,
                      const void *addr, unsigned int alen);
/* `alen` is in/out: the caller's buffer size going in, the address's real
 * length coming out - which may be LARGER, meaning the address was
 * truncated. That is recvfrom(2)'s contract. */
int64 socketfd_recvfrom(struct object *obj, void *buf, uint64 len,
                        void *addr, unsigned int *alen);

/* The socket's own address. `alen` is in/out on the same terms as
 * recvfrom's. The only way to learn what a bind to port 0 chose. */
int64 socketfd_getsockname(struct object *obj, void *addr,
                           unsigned int *alen);

/* Non-zero if this object is a socket. fstat needs it for the same reason
 * pipe_is_pipe does: a libc works out what it holds from st_mode. */
int   socketfd_is_socket(const struct object *obj);

#endif
