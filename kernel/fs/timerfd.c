/* timerfd(2) - a timer you can read and poll.
 *
 * The expiry is counted by the timer interrupt, not by whoever reads: a
 * ktimer (ktimer.h) fires at the due tick, adds the expirations since the
 * last fire to `count`, re-arms for the next period, and wakes the readers
 * and with them the readiness queue - which is what lets a poll or an
 * epoll_wait with no deadline of its own come back when the timer goes off.
 * Reads take the count and zero it. Resolution is one tick, like every
 * other time in this kernel; a 1ms interval simply reports ten expirations
 * per 10ms tick, which is the count Linux would give too. */

#include "timerfd.h"
#include "ktimer.h"
#include "object.h"
#include "kheap.h"
#include "timer.h"
#include "waitq.h"

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1
#define CLOCK_BOOTTIME   7

typedef struct {
    ktimer_t      kt;               /* first: the fire callback casts back */
    int           clockid;
    uint64        due_ns;           /* monotonic; 0 = disarmed */
    uint64        interval_ns;
    uint64        count;            /* expirations not yet read */
    wait_queue_t  readers;
} timerfd_t;

static void tfd_fire(ktimer_t *kt) {
    timerfd_t *t = (timerfd_t *)kt;
    uint64 now = timer_ns();

    if (t->due_ns == 0 || now < t->due_ns) {
        /* The tick rounded up past a due time it has not reached in ns
         * terms cannot happen (ns_to_tick rounds up), but an early fire must
         * not count - re-arm and wait for it. */
        if (t->due_ns != 0) {
            ktimer_arm(&t->kt, ktimer_ns_to_tick(t->due_ns));
        }
        return;
    }
    if (t->interval_ns == 0) {
        t->count++;
        t->due_ns = 0;
    } else {
        uint64 n = 1 + (now - t->due_ns) / t->interval_ns;

        t->count += n;
        t->due_ns += n * t->interval_ns;      /* strictly after now */
        ktimer_arm(&t->kt, ktimer_ns_to_tick(t->due_ns));
    }
    waitq_wake_all(&t->readers);
}

static int tfd_readable(void *ctx) {
    return ((const timerfd_t *)ctx)->count != 0;
}

static int64 tfd_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    timerfd_t *t = (timerfd_t *)obj->body;

    (void)offset;
    if (t == NULL || n < sizeof(uint64)) {
        return -22;                         /* -EINVAL, as on Linux */
    }
    while (t->count == 0) {
        if (!waitq_wait(&t->readers, tfd_readable, t)) {
            return -4;                      /* -EINTR */
        }
    }
    *(uint64 *)buf = t->count;
    t->count = 0;
    return (int64)sizeof(uint64);
}

static int tfd_poll(object_t *obj, int events) {
    timerfd_t *t = (timerfd_t *)obj->body;

    (void)events;
    return (t != NULL && t->count != 0) ? OB_POLLIN : 0;
}

static void tfd_destroy(object_t *obj) {
    timerfd_t *t = (timerfd_t *)obj->body;

    if (t != NULL) {
        ktimer_cancel(&t->kt);
        kfree(t);
        obj->body = NULL;
    }
}

static const object_type_t timerfd_type = {
    .name    = "timerfd",
    .klass   = OBJ_TIMERFD,
    .read    = tfd_read,
    .poll    = tfd_poll,
    .destroy = tfd_destroy
};

int timerfd_create(object_t **out, int clockid) {
    timerfd_t *t;
    object_t *obj;

    /* The alarm clocks (8, 9) need CAP_WAKE_ALARM and a wake-capable RTC;
     * refused like an unknown clock rather than accepted as a plain one. */
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC &&
        clockid != CLOCK_BOOTTIME) {
        return -22;
    }
    t = (timerfd_t *)kcalloc(1, sizeof(*t));
    if (t == NULL) {
        return -12;
    }
    t->kt.fire = tfd_fire;
    t->clockid = clockid;
    waitq_init(&t->readers);
    obj = ob_create(&timerfd_type, t);
    if (obj == NULL) {
        kfree(t);
        return -23;
    }
    *out = obj;
    return 0;
}

int timerfd_is_timerfd(const object_t *obj) {
    return obj != NULL && obj->type == &timerfd_type;
}

void timerfd_gettime(object_t *obj, uint64 *value_ns, uint64 *interval_ns) {
    timerfd_t *t = (timerfd_t *)obj->body;
    uint64 now = timer_ns();

    /* Time LEFT, always relative, whatever flags it was set with - and an
     * expired one-shot reads as disarmed (0). A due time within the current
     * tick reads as 1ns rather than 0, which would say "disarmed". */
    *interval_ns = t->interval_ns;
    if (t->due_ns == 0) {
        *value_ns = 0;
    } else {
        *value_ns = (t->due_ns > now) ? t->due_ns - now : 1;
    }
}

int timerfd_settime(object_t *obj, uint32 flags, uint64 value_ns,
                    uint64 interval_ns, uint64 *old_value_ns,
                    uint64 *old_interval_ns) {
    timerfd_t *t = (timerfd_t *)obj->body;
    uint64 now = timer_ns();

    timerfd_gettime(obj, old_value_ns, old_interval_ns);
    ktimer_cancel(&t->kt);
    /* Setting the timer, armed or not, discards expirations not yet read -
     * Linux's timerfd_settime does the same (ctx->ticks = 0). */
    t->count = 0;
    t->interval_ns = interval_ns;
    if (value_ns == 0) {
        t->due_ns = 0;
        t->interval_ns = 0;
        return 0;
    }
    if (flags & TFD_TIMER_ABSTIME) {
        uint64 due = value_ns;

        if (t->clockid == CLOCK_REALTIME) {
            uint64 offset = timer_realtime_ns() - now;   /* epoch at boot */

            due = (due > offset) ? due - offset : 0;
        }
        /* An absolute time already past expires at once (the next tick),
         * as on Linux - and 0 here would mean "disarmed", so it is 1. */
        t->due_ns = (due == 0) ? 1 : due;
    } else {
        t->due_ns = now + value_ns;
    }
    ktimer_arm(&t->kt, ktimer_ns_to_tick(t->due_ns));
    return 0;
}
