#ifndef NPFS_H
#define NPFS_H

#include "typesk.h"

struct object;

/* The named-pipe file system (ROADMAP 16(q)): \Device\NamedPipe, with
 * \??\pipe as its DOS name, so \\.\pipe\name reaches it. See
 * kernel/fs/npfs.c.
 *
 * A PIPE is a name with up to max_instances server INSTANCES. Each
 * instance is a handle the server holds; a client open of the name
 * connects to an instance that is free and gets the other end of a
 * byte-stream duplex (the socketpair's crossed pipes). NT's states:
 * a new instance is LISTENING (a client may connect before the server
 * asks, and the server's listen then answers PIPE_CONNECTED), CONNECTED,
 * DISCONNECTED after the server disconnects (no client may connect until it
 * listens again). */

#define NP_STATE_DISCONNECTED  1
#define NP_STATE_LISTENING     2
#define NP_STATE_CONNECTED     3
#define NP_STATE_CLOSING       4

void npfs_init(void);

/* A new instance of pipe `name`. `first` refuses an existing name (-17);
 * a pipe at its instance limit is -16. max_instances 0 or >= 255 means
 * unlimited (PIPE_UNLIMITED_INSTANCES). Later instances must not ask for
 * more than the first set. */
int npfs_create(const char *name, uint32 max_instances, int first,
                struct object **out);

/* Is this the \Device\NamedPipe device object (an open of the root)? */
int npfs_is_root(const struct object *obj);

/* Server operations on an instance. listen: 0 once a client is connected
 * (waiting for one), -106 already connected, -32 the client has gone (the
 * server must disconnect first), -4 interrupted. disconnect: 0. */
int npfs_listen(struct object *srv);
int npfs_disconnect(struct object *srv);

/* The listen without the wait, for an asynchronous ConnectNamedPipe: 0
 * connected, -11 not yet (the instance is now listening), -106, -32 or -22
 * as npfs_listen. */
int npfs_listen_poll(struct object *srv);

/* Wait until an instance of `name` is free to connect to: 0, -2 no such
 * pipe, -110 timed out (deadline in ticks, 0 forever), -4 interrupted. */
int npfs_wait(const char *name, uint64 deadline);

/* Peek at either end: bytes readable now and the NT pipe state. -22 for
 * an object that is neither. */
int npfs_peek(struct object *obj, uint32 *available, uint32 *state);

/* Copy up to n readable bytes without consuming them; how many were. */
uint32 npfs_peek_data(struct object *obj, void *buf, uint32 n);

#endif
