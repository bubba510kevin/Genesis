#include "device.h"
#include "kprintf.h"
#include "mouse.h"
#include "object.h"
#include "timer.h"
#include "waitq.h"

/* Pointer events, device-independent - see mouse.h. ROADMAP item 14(j). */

/* --- the queue ------------------------------------------------------------
 * Single producer (a driver's interrupt handler, via mouse_report), single
 * consumer (read(2), or mouse_take), with the same arrangement as the
 * keyboard's ring in keyboard.c: head written only by the producer, tail only
 * by the consumer, both volatile because an interrupt moves one of them.
 *
 * Capacity is in EVENTS. The largest report is seven of them (three axes,
 * three buttons, the SYN), so 256 holds three dozen reports - about a third
 * of a second of a PS/2 mouse at its default 100 reports a second, which is
 * plenty for a reader that is scheduled at all. */
#define MOUSE_RING    256
#define REPORT_MAX    7

static mouse_event_t  ring[MOUSE_RING];
static volatile uint32 ring_head;
static volatile uint32 ring_tail;
static uint32          button_state;
static uint64          dropped;
static wait_queue_t    readers;

static uint32 ring_used(void) {
    return (ring_head - ring_tail + MOUSE_RING) % MOUSE_RING;
}

static uint32 ring_free(void) {
    return MOUSE_RING - 1 - ring_used();
}

void mouse_init(void) {
    ring_head = 0;
    ring_tail = 0;
    button_state = 0;
    dropped = 0;
    waitq_init(&readers);
}

int mouse_pending(void) {
    return (int)ring_used();
}

uint64 mouse_dropped(void) {
    return dropped;
}

void mouse_flush(void) {
    ring_tail = ring_head;
    button_state = 0;
}

int mouse_take(mouse_event_t *ev) {
    if (ring_tail == ring_head) {
        return 0;
    }
    *ev = ring[ring_tail];
    ring_tail = (ring_tail + 1) % MOUSE_RING;
    return 1;
}

void mouse_report(int32 dx, int32 dy, int32 wheel, uint32 buttons) {
    static const struct { uint32 bit; uint16 code; } btn[3] = {
        { MOUSE_BUTTON_LEFT,   BTN_LEFT   },
        { MOUSE_BUTTON_RIGHT,  BTN_RIGHT  },
        { MOUSE_BUTTON_MIDDLE, BTN_MIDDLE },
    };
    mouse_event_t ev[REPORT_MAX];
    uint64 ticks = timer_ticks_now();
    uint32 hz = timer_hz();
    uint32 changed = (buttons ^ button_state) & 0x7;
    int n = 0, i;

    if (dx != 0) {
        ev[n].type = EV_REL; ev[n].code = REL_X;     ev[n].value = dx; n++;
    }
    if (dy != 0) {
        ev[n].type = EV_REL; ev[n].code = REL_Y;     ev[n].value = dy; n++;
    }
    if (wheel != 0) {
        ev[n].type = EV_REL; ev[n].code = REL_WHEEL; ev[n].value = wheel; n++;
    }
    for (i = 0; i < 3; i++) {
        if (changed & btn[i].bit) {
            ev[n].type  = EV_KEY;
            ev[n].code  = btn[i].code;
            ev[n].value = (buttons & btn[i].bit) ? 1 : 0;
            n++;
        }
    }
    if (n == 0) {
        return;                 /* nothing moved, nothing changed */
    }
    ev[n].type = EV_SYN; ev[n].code = SYN_REPORT; ev[n].value = 0; n++;

    if ((uint32)n > ring_free()) {
        /* Dropped whole, and the button state is NOT advanced: the next
         * report that fits then carries the change again, so a press is
         * delayed rather than lost. */
        dropped++;
        return;
    }
    button_state = buttons & 0x7;

    for (i = 0; i < n; i++) {
        if (hz != 0) {
            ev[i].tv_sec  = ticks / hz;
            ev[i].tv_usec = (ticks % hz) * (1000000ULL / hz);
        } else {
            ev[i].tv_sec  = 0;
            ev[i].tv_usec = 0;
        }
        ring[ring_head] = ev[i];
        ring_head = (ring_head + 1) % MOUSE_RING;
    }
    /* Readers, and - through waitq_wake_all's shared readiness queue - every
     * poll(2) that might be waiting on /dev/mouse0. */
    waitq_wake_all(&readers);
}

/* --- /dev/mouse0 ----------------------------------------------------------- */

static int have_events(void *ctx) {
    (void)ctx;
    return ring_tail != ring_head;
}

static int64 mousedev_read(device_t *dev, uint64 offset, void *buf, uint64 n) {
    mouse_event_t *out = (mouse_event_t *)buf;
    uint64 max = n / sizeof(mouse_event_t);
    uint64 got = 0;

    (void)dev;
    (void)offset;
    if (max == 0) {
        return -22;             /* -EINVAL: evdev's answer to a short buffer */
    }
    if (!waitq_wait(&readers, have_events, NULL)) {
        return -4;              /* -EINTR */
    }
    while (got < max && mouse_take(&out[got])) {
        got++;
    }
    return (int64)(got * sizeof(mouse_event_t));
}

static int mousedev_poll(device_t *dev, int events) {
    (void)dev;
    (void)events;
    return ring_tail != ring_head ? OB_POLLIN : 0;
}

static const device_ops_t mousedev_ops = {
    .name = "mouse",
    .read = mousedev_read,
    .poll = mousedev_poll,
};

void mouse_register_device(void) {
    device_t *dev = dev_alloc();
    int rc;

    if (dev == NULL) {
        kprintf_c(0x0C, "mouse: no device slot for /dev/mouse0\n");
        return;
    }
    dev->ops  = &mousedev_ops;
    dev->kind = DEVICE_KIND_CHAR;
    rc = dev_attach(dev, "\\Device\\mouse0", NULL);
    if (rc != 0) {
        kprintf_c(0x0C, "mouse: attaching \\Device\\mouse0 failed (%d)\n", rc);
        dev_free(dev);
    }
}
