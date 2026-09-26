/* socketpair(2) for AF_UNIX - a connected, BIDIRECTIONAL pair of descriptors.
 *
 * --- built out of two pipes, and that is the design rather than a shortcut --
 *
 * The hard part of socketpair is not the connection, it is that each end must
 * be readable AND writable. A pipe end is one or the other by construction
 * (see pipe.c's two type tables, and the comment there about why the read end
 * deliberately has no write slot), so no single pipe object can be an end of
 * this.
 *
 * Two pipes, crossed, is exactly what a bidirectional channel is:
 *
 *     end0  --write-->  pipe B  --read-->  end1
 *     end0  <--read---  pipe A  <-write--  end1
 *
 * So an end is a DUPLEX object: a pair of pointers, one pipe end to read from
 * and one to write to, forwarding each operation to the half that can do it.
 *
 * The alternative was a fresh object type with its own two ring buffers. That
 * would be a second implementation of everything pipe.c already got right -
 * the wait queues, the EOF rule, the partial-read semantics, SIGPIPE - and
 * the second implementation is the one that gets the edge cases wrong,
 * because the edge cases are why the first one looks the way it does.
 *
 * --- what falls out for free, and is worth naming ------------------------
 *
 * SHUTDOWN SEMANTICS ARE INHERITED AND CORRECT. Closing end0 drops its read
 * half (pipe A's read end) and its write half (pipe B's write end). end1 then
 * sees pipe B with no writers, so its read returns 0 - EOF, which is what a
 * closed peer must look like - and its write to pipe A finds no readers, so
 * it gets EPIPE. Neither of those is code in this file. Getting them right in
 * a hand-rolled version means remembering that a socketpair has two
 * independent directions that die separately.
 *
 * --- what this is NOT ----------------------------------------------------
 *
 * It is not a socket. There is no address, no protocol, no sendmsg, and no
 * ancillary data - so no file-descriptor passing, which is the main reason
 * programs reach for an AF_UNIX socketpair over a pipe pair in the first
 * place. SOCK_STREAM only; SOCK_DGRAM would need message boundaries, and a
 * pipe has none. Those are refused rather than approximated, because a
 * datagram channel that silently coalesces is worse than no datagram channel.
 */

#include "socketpair.h"
#include "object.h"
#include "pipe.h"
#include "kheap.h"

typedef struct {
    object_t *rd;         /* the pipe end this end reads from  */
    object_t *wr;         /* the pipe end this end writes to   */
} duplex_t;

static int64 sp_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    duplex_t *d = (duplex_t *)obj->body;

    if (d == NULL || d->rd == NULL || d->rd->type == NULL ||
        d->rd->type->read == NULL) {
        return -5;                       /* -EIO */
    }
    return d->rd->type->read(d->rd, buf, n, offset);
}

static int64 sp_write(object_t *obj, const void *buf, uint64 n,
                      uint64 *offset) {
    duplex_t *d = (duplex_t *)obj->body;

    if (d == NULL || d->wr == NULL || d->wr->type == NULL ||
        d->wr->type->write == NULL) {
        return -5;
    }
    return d->wr->type->write(d->wr, buf, n, offset);
}

/* Readability comes from one half and writability from the other, and the
 * two answers have to be MASKED APART rather than OR'd whole.
 *
 * The pipe read end reports POLLOUT for nothing and the pipe write end
 * reports POLLIN for nothing; a plain OR would therefore be harmless today
 * and wrong the moment either type reports a bit the other also uses. The
 * error bits are the case that already matters: POLLHUP from the read half
 * means "the peer stopped writing" and POLLERR from the write half means
 * "the peer stopped reading", and both must survive, which is why they are
 * taken from both sides. */
static int sp_poll(object_t *obj, int events) {
    duplex_t *d = (duplex_t *)obj->body;
    int ready = 0;

    if (d == NULL) {
        return OB_POLLERR;
    }
    if (d->rd != NULL && d->rd->type != NULL && d->rd->type->poll != NULL) {
        int r = d->rd->type->poll(d->rd, events);
        ready |= r & (OB_POLLIN | OB_POLL_ALWAYS);
    }
    if (d->wr != NULL && d->wr->type != NULL && d->wr->type->poll != NULL) {
        int w = d->wr->type->poll(d->wr, events);
        ready |= w & (OB_POLLOUT | OB_POLL_ALWAYS);
    }
    return ready;
}

static void sp_destroy(object_t *obj) {
    duplex_t *d = (duplex_t *)obj->body;

    if (d == NULL) {
        return;
    }
    /* Dropping these is what gives the peer its EOF and its EPIPE - see the
     * file comment. Order does not matter; both are independent pipes. */
    ob_deref(d->rd);
    ob_deref(d->wr);
    kfree(d);
    obj->body = NULL;
}

static const object_type_t socketpair_type = {
    .name    = "socketpair",
    .klass   = OBJ_PIPE,     /* it IS a pipe pair; fstat should say FIFO */
    .read    = sp_read,
    .write   = sp_write,
    .poll    = sp_poll,
    .destroy = sp_destroy
};

static object_t *duplex_create(object_t *rd, object_t *wr) {
    duplex_t *d = (duplex_t *)kcalloc(1, sizeof(*d));
    object_t *obj;

    if (d == NULL) {
        return NULL;
    }
    d->rd = rd;
    d->wr = wr;
    obj = ob_create(&socketpair_type, d);
    if (obj == NULL) {
        kfree(d);
        return NULL;
    }
    return obj;
}

int socketpair_create(object_t **end0, object_t **end1) {
    object_t *a_rd = NULL, *a_wr = NULL;
    object_t *b_rd = NULL, *b_wr = NULL;
    object_t *e0, *e1;

    if (end0 == NULL || end1 == NULL) {
        return -22;
    }
    if (pipe_create(&a_rd, &a_wr) != 0) {
        return -23;                      /* -ENFILE */
    }
    if (pipe_create(&b_rd, &b_wr) != 0) {
        ob_deref(a_rd);
        ob_deref(a_wr);
        return -23;
    }

    /* end0 reads A and writes B; end1 reads B and writes A. Crossed, which is
     * the whole connection. */
    e0 = duplex_create(a_rd, b_wr);
    if (e0 == NULL) {
        ob_deref(a_rd); ob_deref(a_wr); ob_deref(b_rd); ob_deref(b_wr);
        return -23;
    }
    e1 = duplex_create(b_rd, a_wr);
    if (e1 == NULL) {
        /* e0 owns a_rd and b_wr now and will drop them; the other two are
         * still this function's to release. */
        ob_deref(e0);
        ob_deref(a_wr);
        ob_deref(b_rd);
        return -23;
    }

    *end0 = e0;
    *end1 = e1;
    return 0;
}
