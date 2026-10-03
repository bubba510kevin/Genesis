#include "object.h"
#include "pipe.h"
#include "process.h"
#include "signal.h"
#include "typesk.h"
#include "waitq.h"

/* See pipe.h for the shape. What follows is the buffer and the two blocking
 * paths, which are the parts with contracts worth stating.
 *
 * PIPE_BUF is the size POSIX attaches an atomicity guarantee to: a write of
 * at most this many bytes either lands whole or blocks until it can, and must
 * never be interleaved with another writer's. That is what makes two
 * processes appending short lines to one pipe produce whole lines rather than
 * shredded ones, and it is why the writer below waits for room for the ENTIRE
 * request rather than dribbling bytes in as space appears. Above PIPE_BUF the
 * guarantee does not apply and a partial write is legal. */
#define PIPE_BUF_SIZE 4096
#define MAX_PIPES        8

typedef struct pipe {
    uint8  buf[PIPE_BUF_SIZE];

    /* head is where the next byte is written, tail where the next is read,
     * and count is how many are held. The keyboard's ring gets away without a
     * count because a producer in an interrupt and a consumer in a syscall
     * each own one index; here both ends run in process context, so a count
     * is both safe and worth having - it lets the buffer hold all
     * PIPE_BUF_SIZE bytes instead of one less, and "full" stops being
     * indistinguishable from "empty". */
    uint32 head;
    uint32 tail;
    uint32 count;

    /* How many ends of each kind are open. Zero or one today, because a
     * handle table shares one object rather than making a second - see
     * pipe.h. They are counts rather than flags so that the day something
     * DOES hand out a second read end, the EOF condition is still "no
     * readers" and not "the read end object was destroyed". */
    int    readers;
    int    writers;

    int    in_use;

    wait_queue_t not_empty;   /* readers wait here  */
    wait_queue_t not_full;    /* writers wait here  */
} pipe_t;

static pipe_t pipe_pool[MAX_PIPES];

static pipe_t *pipe_alloc(void) {
    int i;

    for (i = 0; i < MAX_PIPES; i++) {
        if (!pipe_pool[i].in_use) {
            pipe_t *p = &pipe_pool[i];

            p->head    = 0;
            p->tail    = 0;
            p->count   = 0;
            p->readers = 1;
            p->writers = 1;
            p->in_use  = 1;
            waitq_init(&p->not_empty);
            waitq_init(&p->not_full);
            return p;
        }
    }
    return NULL;
}

/* Returned to the pool only when BOTH ends are gone. A process blocked in
 * read or write is holding the handle it blocked on, so its end cannot have
 * reached zero - which is what makes it safe to reuse the wait queues here
 * without checking whether anybody is still parked on them. */
static void pipe_release(pipe_t *p) {
    if (p->readers == 0 && p->writers == 0) {
        p->in_use = 0;
    }
}

/* --- reading ------------------------------------------------------------- */

/* Woken and worth re-checking when either there is something to read or there
 * is nobody left who could ever write. Both have to be in the predicate: a
 * reader parked on an empty pipe whose last writer has just gone must wake up
 * and return end of file rather than sleeping forever. */
static int pipe_readable(void *ctx) {
    const pipe_t *p = (const pipe_t *)ctx;

    return p->count > 0 || p->writers == 0;
}

static int64 pipe_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    pipe_t *p = (pipe_t *)obj->body;
    uint8  *dst = (uint8 *)buf;
    uint64  got = 0;

    (void)offset;                        /* a pipe has no position */
    if (p == NULL) {
        return -5;                       /* -EIO */
    }
    if (n == 0) {
        return 0;
    }

    while (p->count == 0) {
        /* End of file, and only here. Checked before blocking as well as
         * after waking, because the writer may already have gone before this
         * read was ever made. */
        if (p->writers == 0) {
            return 0;
        }
        if (!waitq_wait(&p->not_empty, pipe_readable, p)) {
            return -4;                   /* -EINTR */
        }
    }

    /* Whatever is there, not whatever was asked for. A read that waited for n
     * bytes would deadlock the moment a writer sent fewer and then waited for
     * a reply - which is exactly what a shell pipeline does. */
    while (got < n && p->count > 0) {
        dst[got++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
    }

    waitq_wake_all(&p->not_full);
    return (int64)got;
}

/* --- writing ------------------------------------------------------------- */

struct write_wait {
    pipe_t *pipe;
    uint64  need;                        /* bytes of room the writer wants */
};

static int pipe_writable(void *ctx) {
    const struct write_wait *w = (const struct write_wait *)ctx;

    return (PIPE_BUF_SIZE - w->pipe->count) >= w->need || w->pipe->readers == 0;
}

static int64 pipe_write(object_t *obj, const void *buf, uint64 n,
                        uint64 *offset) {
    pipe_t      *p   = (pipe_t *)obj->body;
    const uint8 *src = (const uint8 *)buf;
    uint64       done = 0;
    struct write_wait w;

    (void)offset;
    if (p == NULL) {
        return -5;
    }
    if (n == 0) {
        return 0;
    }

    /* The atomicity rule, in one line. At or below PIPE_BUF the whole request
     * has to fit before any of it is copied; above it, any room at all is
     * progress and a short write is what the caller gets. */
    w.pipe = p;
    w.need = (n <= PIPE_BUF_SIZE) ? n : 1;

    while (done < n) {
        uint64 room;

        if (p->readers == 0) {
            /* Nobody will ever read this. SIGPIPE is raised only on the write
             * that returns -EPIPE: if some bytes went in before the reader
             * left, the short count is the honest answer and killing the
             * process over it would lose data the reader did receive. */
            if (done > 0) {
                return (int64)done;
            }
            /* A Windows process has no SIGPIPE: NT reports the closed pipe
             * as a status (STATUS_PIPE_CLOSING, ERROR_NO_DATA) and the
             * program carries on. Sending it would kill a PE process
             * whose only mistake was writing to a reader that left. */
            {
                process_t *me = proc_current();

                if (me == NULL || me->personality != PERSONALITY_WINDOWS) {
                    signal_send(me, SIGPIPE);
                }
            }
            return -32;                  /* -EPIPE */
        }

        room = PIPE_BUF_SIZE - p->count;
        if (room < w.need) {
            if (!waitq_wait(&p->not_full, pipe_writable, &w)) {
                /* Interrupted. Same rule as -EPIPE: bytes already delivered
                 * are reported, and only a write that delivered nothing fails
                 * outright. */
                return done > 0 ? (int64)done : -4;   /* -EINTR */
            }
            continue;                    /* re-test readers as well as room */
        }

        while (done < n && p->count < PIPE_BUF_SIZE) {
            p->buf[p->head] = src[done++];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
            p->count++;
        }
        waitq_wake_all(&p->not_empty);
    }
    return (int64)done;
}

/* --- the two ends -------------------------------------------------------- */

static void pipe_read_destroy(object_t *obj) {
    pipe_t *p = (pipe_t *)obj->body;

    if (p == NULL) {
        return;
    }
    p->readers--;
    if (p->readers == 0) {
        /* Wake the writers so they can discover there is nobody left and
         * take the SIGPIPE path. Without this a writer blocked on a full
         * pipe waits for room that will never be made. */
        waitq_wake_all(&p->not_full);
    }
    pipe_release(p);
}

static void pipe_write_destroy(object_t *obj) {
    pipe_t *p = (pipe_t *)obj->body;

    if (p == NULL) {
        return;
    }
    p->writers--;
    if (p->writers == 0) {
        /* The end-of-file wakeup. A reader parked on an empty pipe has no
         * other way to learn the writer is gone. */
        waitq_wake_all(&p->not_empty);
    }
    pipe_release(p);
}

/* --- readiness -----------------------------------------------------------
 *
 * The question poll(2) asks, answered without touching the buffer. Both ends
 * report the far end's departure, and which bit that is differs:
 *
 *   read end,  no writers left -> POLLHUP. End of file is a READABLE event,
 *              not a blocked one. A poll that reports "nothing to read" on a
 *              pipe whose writer has gone hangs on exactly the case poll was
 *              added to handle - waiting for a child that has already exited.
 *              POLLIN is set too, because a read really would return (zero),
 *              and a program that only asked about POLLIN and got only
 *              POLLHUP is entitled to assume reading is still pointless.
 *
 *   write end, no readers left -> POLLERR. Not POLLHUP: a write to this pipe
 *              does not end quietly, it raises SIGPIPE and returns -EPIPE,
 *              and POLLERR is the bit that says "the operation will fail"
 *              rather than "the stream is over".
 *
 * The write end's readiness threshold is one byte of room, not PIPE_BUF. The
 * atomicity rule pipe_write enforces is about a write that has ALREADY been
 * issued; poll answers "would a write make progress", and a 4-byte write into
 * a pipe with 100 bytes free makes progress. Reporting unwritable until
 * PIPE_BUF bytes are free would make a poll loop over a busy pipe stall for
 * no reason, since the writer is not obliged to write PIPE_BUF at a time. */
static int pipe_read_poll(object_t *obj, int events) {
    const pipe_t *p = (const pipe_t *)obj->body;
    int ready = 0;

    (void)events;
    if (p == NULL) {
        return OB_POLLERR;
    }
    if (p->count > 0) {
        ready |= OB_POLLIN;
    }
    if (p->writers == 0) {
        ready |= OB_POLLHUP | OB_POLLIN;
    }
    return ready;
}

static int pipe_write_poll(object_t *obj, int events) {
    const pipe_t *p = (const pipe_t *)obj->body;
    int ready = 0;

    (void)events;
    if (p == NULL) {
        return OB_POLLERR;
    }
    if (p->readers == 0) {
        return OB_POLLERR | OB_POLLOUT;
    }
    if (p->count < PIPE_BUF_SIZE) {
        ready |= OB_POLLOUT;
    }
    return ready;
}

/* Designated initialisers, as everywhere: a positional one silently rebinds
 * the moment a slot is added to object_type_t, which is how destroy once got
 * installed as a directory reader. */
static const object_type_t pipe_read_type = {
    .name    = "pipe",
    .klass   = OBJ_PIPE,
    .read    = pipe_read,
    .poll    = pipe_read_poll,
    .destroy = pipe_read_destroy
    /* No write: the read end is read-only, and a program that gets it
     * backwards should be told so rather than have it silently work. */
};

static const object_type_t pipe_write_type = {
    .name    = "pipe",
    .klass   = OBJ_PIPE,
    .write   = pipe_write,
    .poll    = pipe_write_poll,
    .destroy = pipe_write_destroy
};

int pipe_create(object_t **read_end, object_t **write_end) {
    pipe_t   *p;
    object_t *rd;
    object_t *wr;

    if (read_end == NULL || write_end == NULL) {
        return -22;                      /* -EINVAL */
    }
    p = pipe_alloc();
    if (p == NULL) {
        return -23;                      /* -ENFILE */
    }

    rd = ob_create(&pipe_read_type, p);
    if (rd == NULL) {
        p->readers = 0;
        p->writers = 0;
        pipe_release(p);
        return -23;
    }
    wr = ob_create(&pipe_write_type, p);
    if (wr == NULL) {
        /* Unwind by hand rather than through ob_deref: the read end's
         * destructor would decrement readers and leave writers at one, and
         * the pipe would never be released. */
        p->readers = 0;
        p->writers = 0;
        rd->body = NULL;
        ob_deref(rd);
        pipe_release(p);
        return -23;
    }

    *read_end  = rd;
    *write_end = wr;
    return 0;
}

uint32 pipe_available(const object_t *read_end) {
    const pipe_t *p;

    if (read_end == NULL || read_end->type != &pipe_read_type) {
        return 0;
    }
    p = (const pipe_t *)read_end->body;
    return (p != NULL) ? p->count : 0;
}

uint32 pipe_peek(const object_t *read_end, void *buf, uint32 n) {
    const pipe_t *p;
    uint8 *out = (uint8 *)buf;
    uint32 i;

    if (read_end == NULL || read_end->type != &pipe_read_type) {
        return 0;
    }
    p = (const pipe_t *)read_end->body;
    if (p == NULL) {
        return 0;
    }
    for (i = 0; i < n && i < p->count; i++) {
        out[i] = p->buf[(p->tail + i) % PIPE_BUF_SIZE];
    }
    return i;
}

int pipe_is_pipe(const object_t *obj) {
    return obj != NULL &&
           (obj->type == &pipe_read_type || obj->type == &pipe_write_type);
}
