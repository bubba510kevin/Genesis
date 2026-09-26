#ifndef SOCKETPAIR_H
#define SOCKETPAIR_H

struct object;

/* A connected pair of BIDIRECTIONAL endpoints - socketpair(2) for AF_UNIX,
 * SOCK_STREAM. Built out of two crossed pipes; see kernel/fs/socketpair.c for
 * why that is the design and not a shortcut, and for what it deliberately is
 * not (no addresses, no sendmsg, so no descriptor passing).
 *
 * Returns 0 with both ends stored, or a negative errno. */
int socketpair_create(struct object **end0, struct object **end1);

#endif
