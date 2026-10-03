#include "kheap.h"
#include "ntsync.h"
#include "object.h"
#include "process.h"
#include "sched.h"
#include "typesk.h"
#include "waitq.h"

/* Keyed events and I/O completion ports - see ntsync.h for what each is.
 * Everything here runs under the big kernel lock, like the rest of the
 * object manager: a match and the wake it causes are one step. */

/* --- keyed events ------------------------------------------------------------
 *
 * One pending rendezvous per thread at most - a thread blocked in one call
 * cannot be in another - so the pending set is a table indexed by process
 * slot, not a list hung off each object. A match is made by whichever call
 * arrives SECOND: it finds the oldest unmatched entry of the other kind on
 * the same (object, key), marks it matched, and wakes its thread. The first
 * call only ever registers and sleeps. That split is what makes the two
 * directions symmetric without either side having to retry. */

typedef struct {
    object_t *obj;          /* NULL: the slot has nothing pending          */
    uint64    key;
    uint64    seq;          /* arrival order, for oldest-first matching    */
    uint8     kind;         /* KEYED_WAIT or KEYED_RELEASE                 */
    uint8     matched;
} ke_pending_t;

static ke_pending_t ke_pending[MAX_PROCESSES];
static uint64       ke_seq;
static wait_queue_t ke_q;
static int          ke_q_ready;

static const object_type_t keyed_event_type = {
    .name  = "KeyedEvent",
    .klass = OBJ_KEYED_EVENT,
};

object_t *keyed_event_create(void) {
    return ob_create(&keyed_event_type, NULL);
}

object_t *keyed_event_global(void) {
    static object_t *global;

    if (global == NULL) {
        global = keyed_event_create();      /* held for good */
    }
    return global;
}

void keyed_event_forget(process_t *p) {
    ke_pending_t *e;

    if (p == NULL) {
        return;
    }
    e = &ke_pending[proc_index(p)];
    if (e->obj != NULL) {
        object_t *obj = e->obj;

        e->obj = NULL;
        e->matched = 0;
        ob_deref(obj);
    }
}

static int ke_matched(void *ctx) {
    return ((ke_pending_t *)ctx)->matched;
}

int keyed_event_rendezvous(object_t *obj, uint64 key, int kind,
                           uint64 deadline) {
    process_t *me = proc_current();
    ke_pending_t *mine, *best = NULL;
    int i, rc;

    if (obj == NULL || obj->type == NULL ||
        obj->type->klass != OBJ_KEYED_EVENT || me == NULL) {
        return -22;
    }
    if (!ke_q_ready) {
        waitq_init(&ke_q);
        ke_q_ready = 1;
    }

    /* Second to arrive: complete the oldest partner and go. */
    for (i = 0; i < proc_slots_used(); i++) {
        ke_pending_t *e = &ke_pending[i];

        if (e->obj == obj && e->key == key && e->kind != (uint8)kind &&
            !e->matched && (best == NULL || e->seq < best->seq)) {
            best = e;
        }
    }
    if (best != NULL) {
        best->matched = 1;
        sched_wake(proc_at((int)(best - ke_pending)));
        return 0;
    }

    /* First: register, then sleep until a partner marks the entry. */
    mine = &ke_pending[proc_index(me)];
    mine->obj     = obj;
    mine->key     = key;
    mine->seq     = ++ke_seq;
    mine->kind    = (uint8)kind;
    mine->matched = 0;
    ob_ref(obj);

    rc = deadline == 0 ? waitq_wait(&ke_q, ke_matched, mine)
                       : waitq_wait_until(&ke_q, ke_matched, mine, deadline);

    /* Matched wins over a deadline or a signal that arrived with it: the
     * partner has already gone on believing it was paired, so this side
     * must report the pairing too or one call is lost. */
    rc = mine->matched ? 0 : (rc == WAITQ_TIMEOUT ? -110 : -4);
    mine->obj = NULL;
    mine->matched = 0;
    ob_deref(obj);
    return rc;
}

/* --- I/O completion ports ------------------------------------------------- */

typedef struct io_node {
    struct io_node *next;
    io_packet_t     pk;
} io_node_t;

typedef struct {
    io_node_t   *head, *tail;
    int          depth;
    uint32       concurrency;
    wait_queue_t waiters;
} io_port_t;

static void io_port_destroy(object_t *obj) {
    io_port_t *port = obj->body;

    while (port->head != NULL) {
        io_node_t *n = port->head;

        port->head = n->next;
        kfree(n);
    }
    kfree(port);
}

/* Ready for poll(2) purposes when a packet is queued - not that anything
 * polls a port today, but the answer is known and "always ready" (what a
 * type without a poll method reports) would be wrong. */
static int io_port_poll(object_t *obj, int events) {
    io_port_t *port = obj->body;

    (void)events;
    return port->depth > 0 ? OB_POLLIN : 0;
}

static const object_type_t io_completion_type = {
    .name    = "IoCompletion",
    .klass   = OBJ_IO_COMPLETION,
    .poll    = io_port_poll,
    .destroy = io_port_destroy,
};

static io_port_t *port_of(object_t *obj) {
    if (obj == NULL || obj->type != &io_completion_type) {
        return NULL;
    }
    return obj->body;
}

object_t *io_completion_create(uint32 concurrency) {
    io_port_t *port = kmalloc(sizeof(*port));
    object_t *obj;

    if (port == NULL) {
        return NULL;
    }
    port->head = port->tail = NULL;
    port->depth = 0;
    port->concurrency = concurrency;
    waitq_init(&port->waiters);
    obj = ob_create(&io_completion_type, port);
    if (obj == NULL) {
        kfree(port);
    }
    return obj;
}

int io_completion_post(object_t *obj, const io_packet_t *pk) {
    io_port_t *port = port_of(obj);
    io_node_t *n;

    if (port == NULL) {
        return -22;
    }
    n = kmalloc(sizeof(*n));
    if (n == NULL) {
        return -12;
    }
    n->next = NULL;
    n->pk = *pk;
    if (port->tail != NULL) {
        port->tail->next = n;
    } else {
        port->head = n;
    }
    port->tail = n;
    port->depth++;
    waitq_wake_all(&port->waiters);
    return 0;
}

typedef struct {
    io_port_t *port;
    process_t *alertable;       /* NULL for a plain wait */
} io_wait_t;

static int apc_queued(const process_t *t) {
    return t != NULL && t->nt_apc_head != NULL;
}

static int port_ready(void *ctx) {
    io_wait_t *w = ctx;

    return w->port->depth > 0 || apc_queued(w->alertable);
}

int io_completion_remove(object_t *obj, io_packet_t *out, int max,
                         uint64 deadline, int alertable) {
    io_port_t *port = port_of(obj);
    wait_queue_t *q;
    io_wait_t w;
    int taken = 0, rc;

    if (port == NULL || max < 1) {
        return -22;
    }
    w.port = port;
    w.alertable = alertable ? proc_current() : NULL;
    /* An APC queued before the wait wins over a packet that is ready, as
     * in dispatch_wait_multiple. */
    if (apc_queued(w.alertable)) {
        return IO_REMOVE_APC;
    }
    /* Every waiter is woken by a post and the ones that find the queue
     * empty again sleep again; the re-test is waitq_wait's. An alertable
     * waiter parks on the readiness queue instead, because that is what
     * queuing an APC wakes - and every post wakes it too, since
     * waitq_wake_all always wakes the readiness queue as well. */
    q = alertable ? waitq_readiness() : &port->waiters;
    rc = deadline == 0 ? waitq_wait(q, port_ready, &w)
                       : waitq_wait_until(q, port_ready, &w, deadline);
    if (rc != WAITQ_READY) {
        return rc == WAITQ_TIMEOUT ? -110 : -4;
    }
    if (port->depth == 0 && apc_queued(w.alertable)) {
        return IO_REMOVE_APC;
    }
    while (taken < max && port->head != NULL) {
        io_node_t *n = port->head;

        port->head = n->next;
        if (port->head == NULL) {
            port->tail = NULL;
        }
        port->depth--;
        out[taken++] = n->pk;
        kfree(n);
    }
    return taken;
}

int io_completion_depth(object_t *obj) {
    io_port_t *port = port_of(obj);

    return port == NULL ? -22 : port->depth;
}
