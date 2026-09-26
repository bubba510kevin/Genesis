/* eventfd(2) - a counter you can wait on with poll(2).
 *
 * --- what it is for, since "a counter in a file descriptor" undersells it --
 *
 * The problem it solves is that poll(2) can only wait on descriptors, so
 * anything that is NOT a descriptor - a thread finishing, a queue becoming
 * non-empty, a signal handler wanting to say something - cannot wake a poll
 * loop. Before eventfd the standard answer was the self-pipe trick: make a
 * pipe, poll the read end, write one byte to the write end to wake it. That
 * works and it costs two descriptors, a buffer, and a page of kernel state
 * to carry one bit.
 *
 * An eventfd is that trick with the pipe taken out: ONE descriptor, one 64-bit
 * counter, no buffer. Writes add, reads take the total and reset, and poll
 * reports readable exactly when the counter is non-zero.
 *
 * --- the two modes, and why the flag is not cosmetic ----------------------
 *
 * Default: a read returns the WHOLE counter and zeroes it. Eight writes of 1
 * followed by one read yields 8. That is the right shape for "how much work
 * has arrived since I last looked" - the reader gets a total and cannot fall
 * behind, because the count is a running sum rather than a queue.
 *
 * EFD_SEMAPHORE: a read returns 1 and decrements by 1. Eight writes then need
 * eight reads. That is the right shape for "wake exactly one waiter per
 * event", and folding it into the default would make an eventfd shared by
 * several readers hand the entire count to whichever read first.
 *
 * --- the counter's ceiling is part of the interface ----------------------
 *
 * The maximum is 0xFFFFFFFFFFFFFFFE, one short of the full range, and that is
 * upstream's value rather than an off-by-one. It leaves ULLONG_MAX free to be
 * the value a write can never legally produce, so "the counter is full" is a
 * state a write can detect and block on rather than a wrap that silently
 * loses events. A write that would exceed it blocks (or answers -EAGAIN),
 * which is why poll reports POLLOUT conditionally rather than always.
 *
 * --- blocking, and where O_NONBLOCK is handled --------------------------
 *
 * Two wait queues, not one, and for the reason kern_taskqueue.c gives for its
 * two condvars: the two waits are woken by OPPOSITE events. A reader waits
 * for the counter to become non-zero and a writer waits for it to drop below
 * the ceiling, so one queue would wake every reader on every read.
 *
 * O_NONBLOCK is NOT tested here. It is a status flag on the OPEN INSTANCE
 * rather than on the object - two descriptors onto the same eventfd may
 * disagree about it - so the syscall layer asks ob_poll first and answers
 * -EAGAIN itself (see do_read in kernel/proc/syscall.c). Testing it here as
 * well would be a second copy of that policy, and the copy that is wrong is
 * the one that cannot see which descriptor asked.
 */

#include "eventfd.h"
#include "object.h"
#include "kheap.h"
#include "waitq.h"

#define EVENTFD_MAX  0xFFFFFFFFFFFFFFFEULL

typedef struct {
    uint64       count;
    int          semaphore;   /* EFD_SEMAPHORE: read takes 1, not the lot */
    wait_queue_t not_empty;   /* readers wait here  */
    wait_queue_t not_full;    /* writers wait here  */
} eventfd_t;

static int evfd_readable(void *ctx) {
    return ((const eventfd_t *)ctx)->count != 0;
}

/* "Room for the smallest possible write", which is one. A predicate that
 * asked about the actual pending add would need the amount, and a writer
 * woken for a different writer's amount would spin; one is the amount that
 * makes progress possible for somebody. */
static int evfd_writable(void *ctx) {
    return ((const eventfd_t *)ctx)->count < EVENTFD_MAX;
}

/* A read or a write is always exactly eight bytes. Not "at least eight":
 * upstream answers -EINVAL for a shorter buffer rather than transferring
 * part of the counter, because half a 64-bit count is not a smaller count,
 * it is a different number. */
static int64 evfd_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    eventfd_t *e = (eventfd_t *)obj->body;
    uint64     taken;

    (void)offset;
    if (e == NULL) {
        return -22;
    }
    if (n < sizeof(uint64)) {
        return -22;                     /* -EINVAL */
    }

    while (e->count == 0) {
        if (!waitq_wait(&e->not_empty, evfd_readable, e)) {
            return -4;                  /* -EINTR */
        }
    }

    if (e->semaphore) {
        taken = 1;
        e->count -= 1;
    } else {
        taken = e->count;
        e->count = 0;
    }
    *(uint64 *)buf = taken;
    /* A read always makes room, so a blocked writer may now proceed. */
    waitq_wake_all(&e->not_full);
    return (int64)sizeof(uint64);
}

static int64 evfd_write(object_t *obj, const void *buf, uint64 n,
                        uint64 *offset) {
    eventfd_t *e = (eventfd_t *)obj->body;
    uint64     add;

    (void)offset;
    if (e == NULL) {
        return -22;
    }
    if (n < sizeof(uint64)) {
        return -22;
    }
    add = *(const uint64 *)buf;

    /* ULLONG_MAX is refused outright rather than treated as a large add. It
     * is the one value the counter can never hold (see EVENTFD_MAX above), so
     * a write of it could never succeed however long it waited - blocking
     * forever on it would be a hang rather than back-pressure. */
    if (add == 0xFFFFFFFFFFFFFFFFULL) {
        return -22;
    }
    if (add == 0) {
        return (int64)sizeof(uint64);   /* legal, and a no-op */
    }

    while (e->count > EVENTFD_MAX - add) {
        if (!waitq_wait(&e->not_full, evfd_writable, e)) {
            return -4;
        }
    }
    e->count += add;
    waitq_wake_all(&e->not_empty);
    return (int64)sizeof(uint64);
}

/* Readable when non-zero, writable when there is room for at least one more.
 *
 * POLLOUT is CONDITIONAL, which is the half an implementation is tempted to
 * skip - a counter is almost never full, so "always writable" passes every
 * test anyone is likely to write. It is still wrong: the one caller that
 * hits the ceiling gets told it may write, writes, and blocks inside a call
 * poll just promised would not block. */
static int evfd_poll(object_t *obj, int events) {
    eventfd_t *e = (eventfd_t *)obj->body;
    int ready = 0;

    (void)events;
    if (e == NULL) {
        return 0;
    }
    if (e->count != 0) {
        ready |= OB_POLLIN;
    }
    if (e->count < EVENTFD_MAX) {
        ready |= OB_POLLOUT;
    }
    return ready;
}

static void evfd_destroy(object_t *obj) {
    if (obj->body != NULL) {
        kfree(obj->body);
        obj->body = NULL;
    }
}

static const object_type_t eventfd_type = {
    .name    = "eventfd",
    .klass   = OBJ_EVENTFD,
    .read    = evfd_read,
    .write   = evfd_write,
    .poll    = evfd_poll,
    .destroy = evfd_destroy
};

int eventfd_create(object_t **out, uint64 initval, int semaphore) {
    eventfd_t *e;
    object_t  *obj;

    if (out == NULL) {
        return -22;
    }
    if (initval > EVENTFD_MAX) {
        return -22;
    }
    e = (eventfd_t *)kcalloc(1, sizeof(*e));
    if (e == NULL) {
        return -12;                     /* -ENOMEM */
    }
    e->count     = initval;
    e->semaphore = semaphore ? 1 : 0;
    waitq_init(&e->not_empty);
    waitq_init(&e->not_full);

    obj = ob_create(&eventfd_type, e);
    if (obj == NULL) {
        kfree(e);
        return -23;                     /* -ENFILE */
    }
    *out = obj;
    return 0;
}

int eventfd_is_eventfd(const object_t *obj) {
    return obj != NULL && obj->type == &eventfd_type;
}
