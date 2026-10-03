/* signalfd and pidfd - see sigfd.h. */

#include "sigfd.h"
#include "kheap.h"
#include "object.h"
#include "process.h"
#include "signal.h"
#include "waitq.h"

/* --- signalfd ------------------------------------------------------------ */

typedef struct {
    uint64 mask;
} signalfd_t;

/* SIGKILL and SIGSTOP cannot be read this way, as they cannot be blocked. */
static uint64 sfd_clean(uint64 mask) {
    return mask & ~(sigmask_of(SIGKILL) | sigmask_of(SIGSTOP));
}

static uint64 sfd_ready_bits(const signalfd_t *s) {
    process_t *me = proc_current();

    return (me != NULL) ? (me->sig_pending & s->mask) : 0;
}

static int sfd_readable(void *ctx) {
    return sfd_ready_bits((const signalfd_t *)ctx) != 0;
}

/* One struct signalfd_siginfo per pending signal, as many as fit. The
 * sender is not recorded by this kernel's pending set (a bitmask), so
 * ssi_pid and ssi_uid are 0 and ssi_code SI_USER. */
static int64 sfd_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    signalfd_t *s = (signalfd_t *)obj->body;
    process_t *me = proc_current();
    uint8 *out = (uint8 *)buf;
    uint64 done = 0;

    (void)offset;
    if (s == NULL || me == NULL || n < 128) {
        return -22;
    }
    while (sfd_ready_bits(s) == 0) {
        if (!waitq_wait(waitq_readiness(), sfd_readable, s)) {
            return -4;
        }
    }
    while (done + 128 <= n) {
        uint64 bits = sfd_ready_bits(s);
        int signo, i;

        if (bits == 0) {
            break;
        }
        signo = __builtin_ctzll(bits) + 1;
        me->sig_pending &= ~sigmask_of(signo);
        for (i = 0; i < 128; i++) {
            out[done + i] = 0;
        }
        *(uint32 *)(out + done) = (uint32)signo;    /* ssi_signo */
        done += 128;
    }
    return (int64)done;
}

static int sfd_poll(object_t *obj, int events) {
    signalfd_t *s = (signalfd_t *)obj->body;

    (void)events;
    return (s != NULL && sfd_ready_bits(s) != 0) ? OB_POLLIN : 0;
}

static void sfd_destroy(object_t *obj) {
    if (obj->body != NULL) {
        kfree(obj->body);
        obj->body = NULL;
    }
}

static const object_type_t signalfd_type = {
    .name    = "signalfd",
    .klass   = OBJ_SIGNALFD,
    .read    = sfd_read,
    .poll    = sfd_poll,
    .destroy = sfd_destroy
};

int signalfd_create(object_t **out, uint64 mask) {
    signalfd_t *s = (signalfd_t *)kcalloc(1, sizeof(*s));
    object_t *obj;

    if (s == NULL) {
        return -12;
    }
    s->mask = sfd_clean(mask);
    obj = ob_create(&signalfd_type, s);
    if (obj == NULL) {
        kfree(s);
        return -23;
    }
    *out = obj;
    return 0;
}

int signalfd_set_mask(object_t *obj, uint64 mask) {
    if (obj == NULL || obj->type != &signalfd_type || obj->body == NULL) {
        return -22;
    }
    ((signalfd_t *)obj->body)->mask = sfd_clean(mask);
    waitq_wake_all(waitq_readiness());
    return 0;
}

/* --- pidfd ---------------------------------------------------------------- */

typedef struct {
    int       pid;
    object_t *proc_obj;      /* the leader's process object, referenced */
} pidfd_t;

static int pfd_poll(object_t *obj, int events) {
    pidfd_t *d = (pidfd_t *)obj->body;

    (void)events;
    if (d == NULL) {
        return 0;
    }
    /* No object: the process had already ended when the pidfd was made. */
    if (d->proc_obj == NULL) {
        return OB_POLLIN;
    }
    return (ob_poll(d->proc_obj, OB_POLLIN) & OB_POLLIN) ? OB_POLLIN : 0;
}

static void pfd_destroy(object_t *obj) {
    pidfd_t *d = (pidfd_t *)obj->body;

    if (d != NULL) {
        if (d->proc_obj != NULL) {
            ob_deref(d->proc_obj);
        }
        kfree(d);
        obj->body = NULL;
    }
}

static const object_type_t pidfd_type = {
    .name    = "pidfd",
    .klass   = OBJ_PIDFD,
    .poll    = pfd_poll,
    .destroy = pfd_destroy
};

int pidfd_create(object_t **out, process_t *leader) {
    pidfd_t *d = (pidfd_t *)kcalloc(1, sizeof(*d));
    object_t *obj;

    if (d == NULL) {
        return -12;
    }
    d->pid = leader->pid;
    d->proc_obj = proc_process_object(leader);
    if (d->proc_obj != NULL) {
        ob_ref(d->proc_obj);
    }
    obj = ob_create(&pidfd_type, d);
    if (obj == NULL) {
        if (d->proc_obj != NULL) {
            ob_deref(d->proc_obj);
        }
        kfree(d);
        return -23;
    }
    *out = obj;
    return 0;
}

int pidfd_pid(const object_t *obj) {
    if (obj == NULL || obj->type != &pidfd_type || obj->body == NULL) {
        return -1;
    }
    return ((const pidfd_t *)obj->body)->pid;
}
