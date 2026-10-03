#ifndef EPOLL_H
#define EPOLL_H

#include "typesk.h"

struct object;
struct open_file;

/* epoll(7) - an interest list kept in the kernel, waited on as one
 * descriptor. See kernel/fs/epoll.c. */

#define EPOLL_CTL_ADD   1
#define EPOLL_CTL_DEL   2
#define EPOLL_CTL_MOD   3

#define EPOLLIN         0x001u
#define EPOLLPRI        0x002u
#define EPOLLOUT        0x004u
#define EPOLLERR        0x008u
#define EPOLLHUP        0x010u
#define EPOLLRDNORM     0x040u
#define EPOLLRDBAND     0x080u
#define EPOLLWRNORM     0x100u
#define EPOLLWRBAND     0x200u
#define EPOLLMSG        0x400u
#define EPOLLRDHUP      0x2000u
#define EPOLLEXCLUSIVE  (1u << 28)
#define EPOLLWAKEUP     (1u << 29)
#define EPOLLONESHOT    (1u << 30)
#define EPOLLET         (1u << 31)

/* The user's structure. PACKED on x86-64 - 12 bytes, the one architecture
 * where Linux declares it so (__EPOLL_PACKED) - and getting that wrong
 * shifts every `data` after the first by four bytes. */
struct epoll_event {
    uint32 events;
    uint64 data;
} __attribute__((packed));

int epoll_create_object(struct object **out);
int epoll_is_epoll(const struct object *obj);

/* EPOLL_CTL_*. `of` is what `fd` names in the caller's table now; the
 * entry is keyed by both, as Linux keys it by (file, fd). Returns 0 or a
 * negative errno (-EEXIST, -ENOENT, -EINVAL, -ELOOP, -EPERM, -ENOMEM). */
int epoll_ctl_object(struct object *ep, int op, int fd, struct open_file *of,
                     uint32 events, uint64 data);

/* Fill up to `max` events, consuming one-shot and edge-triggered reports;
 * the number filled, 0 when nothing is ready. Never blocks - the caller
 * waits on the readiness queue between calls. */
int epoll_collect(struct object *ep, struct epoll_event *out, int max);

/* An open instance's last reference is going: drop it from every interest
 * list, which is when Linux drops it too (closing ONE of several
 * descriptors for the same open file does not). Called from of_deref. */
void epoll_file_released(struct open_file *of);

#endif
