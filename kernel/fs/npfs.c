/* The named-pipe file system - see npfs.h.
 *
 * Byte-mode pipes. The connection is a socketpair (two crossed pipes): the
 * server's instance object holds one end while connected, the client's
 * handle IS the other. Closing either end is what the peer sees - EOF on
 * read (STATUS_PIPE_BROKEN), EPIPE on write (STATUS_PIPE_CLOSING) - with no
 * extra bookkeeping here, which is why the duplex was reused rather than
 * written again.
 *
 * Names compare ignoring ASCII case, as NPFS's do. */

#include "npfs.h"
#include "device.h"
#include "kheap.h"
#include "ns.h"
#include "object.h"
#include "process.h"
#include "socketpair.h"
#include "timer.h"
#include "waitq.h"

#define NP_NAME_MAX   64
#define NP_MAX_PIPES  64

typedef struct np_instance np_instance_t;

typedef struct np_pipe {
    int            in_use;
    char           name[NP_NAME_MAX];
    uint32         max_instances;      /* 0: unlimited */
    uint32         instances;
    np_instance_t *list;
} np_pipe_t;

struct np_instance {
    np_pipe_t     *pipe;
    int            state;
    object_t      *end;                /* the server's end while connected */
    wait_queue_t   q;                  /* a listen waiting for a client    */
    np_instance_t *next;
};

static np_pipe_t pipes[NP_MAX_PIPES];
static wait_queue_t free_q;            /* WaitNamedPipe waiters            */
static device_t *np_dev;
static object_t *np_root;              /* the device's object, referenced */

static int lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int name_eq(const char *a, const char *b) {
    while (*a != '\0' && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static np_pipe_t *find_pipe(const char *name) {
    int i;

    for (i = 0; i < NP_MAX_PIPES; i++) {
        if (pipes[i].in_use && name_eq(pipes[i].name, name)) {
            return &pipes[i];
        }
    }
    return NULL;
}

/* A client has gone if the connection's read side reports a hangup. */
static int peer_gone(const np_instance_t *in) {
    return in->end != NULL && (ob_poll(in->end, OB_POLLIN) & OB_POLLHUP);
}

/* --- the server's instance object -------------------------------------- */

/* Read or write the connection, holding a reference on its end for the
 * whole call: a read can block, and a DisconnectNamedPipe from another
 * thread meanwhile drops the instance's own reference. */
static int64 through(np_instance_t *in, void *buf, uint64 n, uint64 *off,
                     int writing) {
    object_t *e = in->end;
    int64 rc;

    ob_ref(e);
    rc = writing ? e->type->write(e, buf, n, off)
                 : e->type->read(e, buf, n, off);
    ob_deref(e);
    return rc;
}

static const object_type_t np_server_type;

static int64 srv_read(object_t *obj, void *buf, uint64 n, uint64 *off) {
    np_instance_t *in = (np_instance_t *)obj->body;

    if (in == NULL) {
        return -5;
    }
    if (in->state == NP_STATE_LISTENING) {
        return -107;                         /* STATUS_PIPE_LISTENING   */
    }
    if (in->state != NP_STATE_CONNECTED || in->end == NULL) {
        return -108;                         /* STATUS_PIPE_DISCONNECTED */
    }
    return through(in, buf, n, off, 0);
}

static int64 srv_write(object_t *obj, const void *buf, uint64 n,
                       uint64 *off) {
    np_instance_t *in = (np_instance_t *)obj->body;

    if (in == NULL) {
        return -5;
    }
    if (in->state == NP_STATE_LISTENING) {
        return -107;
    }
    if (in->state != NP_STATE_CONNECTED || in->end == NULL) {
        return -108;
    }
    return through(in, (void *)buf, n, off, 1);
}

static int srv_poll(object_t *obj, int events) {
    np_instance_t *in = (np_instance_t *)obj->body;

    if (in == NULL || in->state != NP_STATE_CONNECTED || in->end == NULL) {
        return 0;
    }
    return ob_poll(in->end, events);
}

static void srv_destroy(object_t *obj) {
    np_instance_t *in = (np_instance_t *)obj->body;
    np_pipe_t *p;
    np_instance_t **at;

    if (in == NULL) {
        return;
    }
    p = in->pipe;
    if (in->end != NULL) {
        ob_deref(in->end);                   /* the client sees EOF/EPIPE */
        in->end = NULL;
    }
    for (at = &p->list; *at != NULL; at = &(*at)->next) {
        if (*at == in) {
            *at = in->next;
            break;
        }
    }
    if (--p->instances == 0) {
        p->in_use = 0;
    }
    waitq_wake_all(&in->q);
    kfree(in);
    obj->body = NULL;
}

static const object_type_t np_server_type = {
    .name    = "NamedPipe",
    .klass   = OBJ_PIPE,
    .read    = srv_read,
    .write   = srv_write,
    .poll    = srv_poll,
    .destroy = srv_destroy
};

int npfs_create(const char *name, uint32 max_instances, int first,
                object_t **out) {
    np_pipe_t *p;
    np_instance_t *in;
    object_t *obj;
    int i, n;

    for (n = 0; name[n] != '\0'; n++) {
        if (name[n] == '\\') {
            return -22;                      /* no subdirectories */
        }
    }
    if (n == 0 || n >= NP_NAME_MAX) {
        return -22;
    }
    if (max_instances >= 255) {
        max_instances = 0;
    }
    p = find_pipe(name);
    if (p != NULL) {
        if (first) {
            return -17;
        }
        if (p->max_instances != 0 && p->instances >= p->max_instances) {
            return -16;
        }
    } else {
        for (i = 0; i < NP_MAX_PIPES && pipes[i].in_use; i++) {
        }
        if (i == NP_MAX_PIPES) {
            return -23;
        }
        p = &pipes[i];
        for (n = 0; name[n] != '\0'; n++) {
            p->name[n] = name[n];
        }
        p->name[n] = '\0';
        p->max_instances = max_instances;
        p->instances = 0;
        p->list = NULL;
    }
    in = (np_instance_t *)kcalloc(1, sizeof(*in));
    if (in == NULL) {
        return -12;
    }
    obj = ob_create(&np_server_type, in);
    if (obj == NULL) {
        kfree(in);
        return -23;
    }
    p->in_use = 1;
    p->instances++;
    in->pipe = p;
    in->state = NP_STATE_LISTENING;
    waitq_init(&in->q);
    in->next = p->list;
    p->list = in;
    waitq_wake_all(&free_q);                 /* a WaitNamedPipe may pass */
    *out = obj;
    return 0;
}

static int connected(void *ctx) {
    np_instance_t *in = (np_instance_t *)ctx;

    return in->pipe == NULL || in->state != NP_STATE_LISTENING;
}

int npfs_listen(object_t *srv) {
    np_instance_t *in;

    if (srv == NULL || srv->type != &np_server_type) {
        return -22;
    }
    in = (np_instance_t *)srv->body;
    if (in->state == NP_STATE_CONNECTED) {
        return peer_gone(in) ? -32 : -106;
    }
    if (in->state == NP_STATE_DISCONNECTED) {
        in->state = NP_STATE_LISTENING;
        waitq_wake_all(&free_q);
    }
    while (in->state == NP_STATE_LISTENING) {
        if (!waitq_wait(&in->q, connected, in)) {
            return -4;
        }
    }
    return 0;
}

int npfs_listen_poll(object_t *srv) {
    np_instance_t *in;

    if (srv == NULL || srv->type != &np_server_type || srv->body == NULL) {
        return -22;
    }
    in = (np_instance_t *)srv->body;
    if (in->state == NP_STATE_DISCONNECTED) {
        in->state = NP_STATE_LISTENING;
        waitq_wake_all(&free_q);
    }
    if (in->state == NP_STATE_LISTENING) {
        return -11;
    }
    return 0;
}

int npfs_disconnect(object_t *srv) {
    np_instance_t *in;

    if (srv == NULL || srv->type != &np_server_type) {
        return -22;
    }
    in = (np_instance_t *)srv->body;
    if (in->end != NULL) {
        ob_deref(in->end);
        in->end = NULL;
    }
    in->state = NP_STATE_DISCONNECTED;
    return 0;
}

struct wait_ctx {
    const char *name;
    int         gone;
};

static int instance_free(void *ctx) {
    struct wait_ctx *w = (struct wait_ctx *)ctx;
    np_pipe_t *p = find_pipe(w->name);
    np_instance_t *in;

    if (p == NULL) {
        w->gone = 1;
        return 1;
    }
    for (in = p->list; in != NULL; in = in->next) {
        if (in->state == NP_STATE_LISTENING) {
            return 1;
        }
    }
    return 0;
}

int npfs_wait(const char *name, uint64 deadline) {
    struct wait_ctx w;
    int rc;

    w.name = name;
    w.gone = 0;
    if (find_pipe(name) == NULL) {
        return -2;
    }
    rc = waitq_wait_until(&free_q, instance_free, &w, deadline);
    if (rc == WAITQ_TIMEOUT) {
        return -110;
    }
    if (rc == WAITQ_SIGNAL) {
        return -4;
    }
    return w.gone ? -2 : 0;
}

int npfs_peek(object_t *obj, uint32 *available, uint32 *state) {
    if (obj == NULL) {
        return -22;
    }
    if (obj->type == &np_server_type) {
        np_instance_t *in = (np_instance_t *)obj->body;

        *state = (in->state == NP_STATE_CONNECTED && peer_gone(in))
                     ? NP_STATE_CLOSING : (uint32)in->state;
        *available = (in->end != NULL) ? socketpair_available(in->end) : 0;
        return 0;
    }
    /* A client end is a plain duplex. */
    if (obj->type != NULL && obj->type->klass == OBJ_PIPE) {
        *available = socketpair_available(obj);
        *state = (ob_poll(obj, OB_POLLIN) & OB_POLLHUP) ? NP_STATE_CLOSING
                                                        : NP_STATE_CONNECTED;
        return 0;
    }
    return -22;
}

uint32 npfs_peek_data(object_t *obj, void *buf, uint32 n) {
    if (obj == NULL) {
        return 0;
    }
    if (obj->type == &np_server_type) {
        np_instance_t *in = (np_instance_t *)obj->body;

        return (in->end != NULL) ? socketpair_peek(in->end, buf, n) : 0;
    }
    return socketpair_peek(obj, buf, n);
}

/* --- the device: a client open by name ---------------------------------- */

static int np_parse(device_t *dev, const char *remainder, uint32 access,
                    object_t **out) {
    np_pipe_t *p;
    np_instance_t *in;
    object_t *srv_end = NULL, *cli_end = NULL;
    int rc;

    (void)dev;
    (void)access;
    if (remainder[0] == '\\') {
        remainder++;
    }
    if (remainder[0] == '\0') {
        /* \Device\NamedPipe\ - the root itself, which is what
         * WaitNamedPipe opens to send FSCTL_PIPE_WAIT. */
        if (np_root == NULL) {
            return -22;
        }
        ob_ref(np_root);
        *out = np_root;
        return 0;
    }
    p = find_pipe(remainder);
    if (p == NULL) {
        return -2;
    }
    for (in = p->list; in != NULL; in = in->next) {
        if (in->state == NP_STATE_LISTENING) {
            break;
        }
    }
    if (in == NULL) {
        return -16;                          /* STATUS_PIPE_NOT_AVAILABLE */
    }
    rc = socketpair_create(&srv_end, &cli_end);
    if (rc != 0) {
        return rc;
    }
    in->end = srv_end;
    in->state = NP_STATE_CONNECTED;
    waitq_wake_all(&in->q);
    *out = cli_end;
    return 0;
}

static const device_ops_t npfs_ops = {
    .name  = "npfs",
    .parse = np_parse,
};

int npfs_is_root(const object_t *obj) {
    return np_dev != NULL && dev_from_object(obj) == np_dev;
}

void npfs_init(void) {
    device_t *dev = dev_alloc();

    waitq_init(&free_q);
    if (dev == NULL) {
        return;
    }
    dev->ops = &npfs_ops;
    dev->kind = DEVICE_KIND_CHAR;
    if (dev_attach(dev, "\\Device\\NamedPipe", "pipe") != 0) {
        dev_free(dev);
        return;
    }
    np_dev = dev;
    {
        char rest[8];

        if (ns_lookup("\\Device\\NamedPipe", &np_root, rest,
                      sizeof(rest)) != 0) {
            np_root = NULL;
        }
    }
}
