/* epoll(7).
 *
 * An epoll instance is an object holding an interest list: one item per
 * (open file, fd) pair registered with epoll_ctl, with the events asked for
 * and the 64 bits of user data handed back. epoll_wait is poll(2) over that
 * list - the same ob_poll question, asked while parked on the same shared
 * readiness queue (waitq.h) - with three differences poll does not have:
 *
 *  - the list lives in the kernel and outlives the call, so a program with
 *    ten thousand descriptors does not copy them in every time;
 *  - an item follows the OPEN FILE, not the descriptor number: it stays
 *    while any descriptor for that open file does (a dup, an inherited
 *    copy) and goes when the last one closes (epoll_file_released);
 *  - EPOLLONESHOT disables an item once reported, until EPOLL_CTL_MOD; and
 *    EPOLLET reports a ready item again only after something has changed
 *    since its last report (waitq_generation - see waitq.h for why that
 *    never misses an edge). Linux's edge is narrower - THIS file's state
 *    changed - so a program may see an extra report here, never a missing
 *    one; edge-triggered programs read until EAGAIN and are correct either
 *    way.
 *
 * Reported items move to the back of the list, so a call with a small
 * maxevents takes turns over a large ready set instead of starving the
 * tail - Linux's ready list round-robins the same way.
 *
 * An epoll instance may watch another (it is readable while it has a ready
 * item), but never itself, directly or through a chain: that is -ELOOP, as
 * is a chain deeper than EP_MAX_NEST, Linux's limit. */

#include "epoll.h"
#include "kheap.h"
#include "object.h"
#include "waitq.h"

#define EP_MAX_NEST  4

typedef struct ep_item {
    struct ep_item *next;
    open_file_t    *of;           /* not referenced: see epoll_file_released */
    int             fd;
    uint32          events;
    uint64          data;
    int             disabled;     /* EPOLLONESHOT, reported */
    uint64          reported_gen; /* EPOLLET: waitq_generation at report */
    int             reported;
} ep_item_t;

typedef struct epoll {
    struct epoll *next_inst;      /* every instance, for the release hook */
    ep_item_t    *items;
    int           busy;           /* in a nested poll: cycle guard */
} epoll_t;

static epoll_t *instances;

static const object_type_t epoll_type;

static int item_ready(ep_item_t *it) {
    int ev;

    if (it->disabled || it->of == NULL || it->of->obj == NULL) {
        return 0;
    }
    ev = ob_poll(it->of->obj, (int)(it->events & 0xFFFFu));
    ev &= (int)((it->events & 0xFFFFu) | EPOLLERR | EPOLLHUP);
    if (ev != 0 && (it->events & EPOLLET) && it->reported &&
        it->reported_gen == waitq_generation()) {
        return 0;                 /* nothing has happened since */
    }
    return ev;
}

static int ep_poll(object_t *obj, int events) {
    epoll_t *ep = (epoll_t *)obj->body;
    ep_item_t *it;
    int ready = 0;

    (void)events;
    if (ep == NULL || ep->busy) {
        return 0;
    }
    ep->busy = 1;
    for (it = ep->items; it != NULL; it = it->next) {
        if (item_ready(it)) {
            ready = OB_POLLIN;
            break;
        }
    }
    ep->busy = 0;
    return ready;
}

static void ep_destroy(object_t *obj) {
    epoll_t *ep = (epoll_t *)obj->body;
    epoll_t **at;

    if (ep == NULL) {
        return;
    }
    for (at = &instances; *at != NULL; at = &(*at)->next_inst) {
        if (*at == ep) {
            *at = ep->next_inst;
            break;
        }
    }
    while (ep->items != NULL) {
        ep_item_t *it = ep->items;

        ep->items = it->next;
        kfree(it);
    }
    kfree(ep);
    obj->body = NULL;
}

static const object_type_t epoll_type = {
    .name    = "epoll",
    .klass   = OBJ_EPOLL,
    .poll    = ep_poll,
    .destroy = ep_destroy
};

int epoll_create_object(object_t **out) {
    epoll_t *ep = (epoll_t *)kcalloc(1, sizeof(*ep));
    object_t *obj;

    if (ep == NULL) {
        return -12;
    }
    obj = ob_create(&epoll_type, ep);
    if (obj == NULL) {
        kfree(ep);
        return -23;
    }
    ep->next_inst = instances;
    instances = ep;
    *out = obj;
    return 0;
}

int epoll_is_epoll(const object_t *obj) {
    return obj != NULL && obj->type == &epoll_type;
}

/* Does `from` reach `target` through watched epoll instances, or is the
 * chain below it deeper than `depth` allows? Either is -ELOOP. */
static int reaches(epoll_t *from, epoll_t *target, int depth) {
    ep_item_t *it;

    if (from == target) {
        return 1;
    }
    if (depth <= 0) {
        return 1;
    }
    for (it = from->items; it != NULL; it = it->next) {
        object_t *o = (it->of != NULL) ? it->of->obj : NULL;

        if (o != NULL && o->type == &epoll_type &&
            reaches((epoll_t *)o->body, target, depth - 1)) {
            return 1;
        }
    }
    return 0;
}

int epoll_ctl_object(object_t *obj, int op, int fd, open_file_t *of,
                     uint32 events, uint64 data) {
    epoll_t *ep = (epoll_t *)obj->body;
    ep_item_t **at, *it;

    for (at = &ep->items; *at != NULL; at = &(*at)->next) {
        if ((*at)->of == of && (*at)->fd == fd) {
            break;
        }
    }
    it = *at;

    switch (op) {
    case EPOLL_CTL_ADD:
        if (it != NULL) {
            return -17;                     /* -EEXIST */
        }
        if (of->obj == obj) {
            return -22;                     /* itself: -EINVAL */
        }
        /* Linux refuses what it cannot poll - a regular file is always
         * ready, and watching one is a bug the caller should hear about. */
        if (of->obj == NULL || of->obj->type == NULL ||
            of->obj->type->poll == NULL) {
            return -1;                      /* -EPERM */
        }
        if (of->obj->type == &epoll_type &&
            reaches((epoll_t *)of->obj->body, ep, EP_MAX_NEST)) {
            return -40;                     /* -ELOOP */
        }
        /* EPOLLEXCLUSIVE with ONESHOT is refused, as on Linux. */
        if ((events & EPOLLEXCLUSIVE) && (events & EPOLLONESHOT)) {
            return -22;
        }
        it = (ep_item_t *)kcalloc(1, sizeof(*it));
        if (it == NULL) {
            return -12;
        }
        it->of = of;
        it->fd = fd;
        it->events = events;
        it->data = data;
        it->next = NULL;
        *at = it;                           /* at the end */
        /* A new interest may already be satisfied: a waiter on this
         * instance (or one watching it) must look again. */
        waitq_wake_all(waitq_readiness());
        return 0;

    case EPOLL_CTL_DEL:
        if (it == NULL) {
            return -2;                      /* -ENOENT */
        }
        *at = it->next;
        kfree(it);
        return 0;

    case EPOLL_CTL_MOD:
        if (it == NULL) {
            return -2;
        }
        /* EPOLLEXCLUSIVE can be given only at ADD. */
        if (events & EPOLLEXCLUSIVE) {
            return -22;
        }
        it->events = events | (it->events & EPOLLEXCLUSIVE);
        it->data = data;
        it->disabled = 0;                   /* re-arms a one-shot */
        it->reported = 0;                   /* and an edge */
        waitq_wake_all(waitq_readiness());
        return 0;
    }
    return -22;
}

int epoll_collect(object_t *obj, struct epoll_event *out, int max) {
    epoll_t *ep = (epoll_t *)obj->body;
    ep_item_t *done = NULL, **done_tail = &done;
    ep_item_t **at;
    int n = 0;

    if (ep->busy) {
        return 0;
    }
    ep->busy = 1;
    at = &ep->items;
    while (*at != NULL && n < max) {
        ep_item_t *it = *at;
        int ev = item_ready(it);

        if (ev == 0) {
            at = &it->next;
            continue;
        }
        out[n].events = (uint32)ev;
        out[n].data = it->data;
        n++;
        if (it->events & EPOLLONESHOT) {
            it->disabled = 1;
        }
        it->reported = 1;
        it->reported_gen = waitq_generation();
        /* Unlinked here, appended below: the round robin. */
        *at = it->next;
        it->next = NULL;
        *done_tail = it;
        done_tail = &it->next;
    }
    /* `at` is somewhere in the list; the reported ones go after its end. */
    while (*at != NULL) {
        at = &(*at)->next;
    }
    *at = done;
    ep->busy = 0;
    return n;
}

void epoll_file_released(open_file_t *of) {
    epoll_t *ep;

    for (ep = instances; ep != NULL; ep = ep->next_inst) {
        ep_item_t **at = &ep->items;

        while (*at != NULL) {
            if ((*at)->of == of) {
                ep_item_t *it = *at;

                *at = it->next;
                kfree(it);
            } else {
                at = &(*at)->next;
            }
        }
    }
}
