#include "sys/bus.h"
#include "atkbdc.h"
#include "bkl.h"
#include "kprintf.h"
#include "mouse.h"
#include "psm.h"
#include "timer.h"

/* psm - the PS/2 mouse. ROADMAP item 14(j).
 *
 * Written the way a FreeBSD driver is: DEVMETHOD table, DRIVER_MODULE onto
 * the atkbdc bus, a softc, bus_alloc_resource for the IRQ and bus_setup_intr
 * with a filter. atkbdc.c adds psm0 as the controller's aux-port child; this
 * probes it, configures the mouse and turns its packets into mouse_report()
 * calls (kernel/dev/mouse.c), which is where /dev/mouse0 lives.
 *
 * --- The protocol -------------------------------------------------------
 * A standard PS/2 mouse streams 3-byte packets:
 *
 *   byte 0   bit 0 left, 1 right, 2 middle, 3 ALWAYS 1, 4 X sign, 5 Y sign,
 *            6 X overflow, 7 Y overflow
 *   byte 1   X movement, low 8 bits of a 9-bit two's-complement value
 *   byte 2   Y movement, likewise - and UP is positive
 *
 * An IntelliMouse (device ID 3, after the 200/100/80 sample-rate knock)
 * adds a fourth byte, the wheel, signed, positive toward the user. ID 4
 * (IntelliMouse Explorer) keeps the wheel in the low nibble of it.
 *
 * Three things the decoder has to get right, each of them checked by the
 * boot selftest:
 *   - SYNCHRONISATION. Bit 3 of byte 0 is the only framing there is. A byte
 *     lost or inserted anywhere leaves every later packet shifted, reading
 *     motion as buttons; a candidate first byte without bit 3 is discarded
 *     until one arrives with it. A packet left unfinished for PSM_GAP_TICKS
 *     is abandoned too - the next byte then starts a new one.
 *   - THE NINTH BIT. Movement is -256..255, with the sign in byte 0.
 *   - OVERFLOW. A saturated count is worse than none (the pointer leaps), so
 *     an overflowed packet's motion is dropped and its buttons kept.
 * Y is negated on the way out: mouse.h's convention is screen coordinates,
 * down positive.
 */

#define ENXIO 6

#define PSM_CMD_SET_RATE      0xF3
#define PSM_CMD_GET_ID        0xF2
#define PSM_CMD_ENABLE        0xF4
#define PSM_CMD_DISABLE       0xF5
#define PSM_CMD_SET_DEFAULTS  0xF6

#define PSM_ACK               0xFA

/* Ticks (10ms each) after which a half-received packet is abandoned. At a
 * 100Hz report rate the bytes of one packet arrive within a millisecond of
 * each other, so anything this late is the start of the next packet. */
#define PSM_GAP_TICKS  5

struct psm_softc {
    device_t          dev;
    struct resource  *irq;
    int               irq_rid;
    void             *ih;

    uint8             id;           /* 0 standard, 3 wheel, 4 5-button   */
    int               packet_size;  /* 3 or 4                             */
    uint8             packet[4];
    int               pos;
    uint64            last_tick;

    /* The interrupt-driven command path (psm_command): while cmd_pending,
     * the handler stores the next byte as the reply instead of decoding. */
    volatile int      cmd_pending;
    volatile int      cmd_have;
    volatile uint8    cmd_reply;

    uint64            packets;
    uint64            resyncs;
    uint64            overflows;
};

static struct psm_softc *psm_sc;

/* --- decoding -------------------------------------------------------------- */

static void psm_decode(struct psm_softc *sc) {
    uint8 b0 = sc->packet[0];
    int32 dx = 0, dy = 0, wheel = 0;

    if (b0 & 0xC0) {
        sc->overflows++;             /* motion saturated: keep the buttons */
    } else {
        dx = (int32)sc->packet[1] - (int32)((b0 << 4) & 0x100);
        dy = (int32)sc->packet[2] - (int32)((b0 << 3) & 0x100);
    }
    if (sc->packet_size == 4) {
        int8 z = (int8)sc->packet[3];

        if (sc->id == 4) {
            z = (int8)((int8)(sc->packet[3] << 4) >> 4);
        }
        wheel = -(int32)z;           /* PS/2 counts toward the user */
    }
    sc->packets++;
    mouse_report(dx, -dy, wheel, b0 & 0x07);
}

static void psm_input(struct psm_softc *sc, uint8 byte) {
    uint64 now = timer_ticks_now();

    if (sc->pos > 0 && now - sc->last_tick > PSM_GAP_TICKS) {
        sc->pos = 0;
        sc->resyncs++;
    }
    sc->last_tick = now;

    if (sc->pos == 0 && !(byte & 0x08)) {
        sc->resyncs++;               /* not a first byte: wait for one */
        return;
    }
    sc->packet[sc->pos++] = byte;
    if (sc->pos < sc->packet_size) {
        return;
    }
    sc->pos = 0;
    psm_decode(sc);
}

/* The filter. IRQ 12 is the aux port's own line, but the handler still reads
 * only what the controller marks as aux data: a keyboard byte left in the
 * output buffer belongs to IRQ 1. */
static int psm_intr(void *arg) {
    struct psm_softc *sc = (struct psm_softc *)arg;
    int handled = 0;
    int n;
    uint8 byte;

    for (n = 0; n < 16 && atkbdc_aux_poll(&byte); n++) {
        handled = 1;
        if (sc->cmd_pending) {
            sc->cmd_reply = byte;
            sc->cmd_have  = 1;
            sc->cmd_pending = 0;
            continue;
        }
        psm_input(sc, byte);
    }
    return handled ? FILTER_HANDLED : FILTER_STRAY;
}

/* A mouse command once the handler is live: write it, and let the handler
 * catch the ACK. Waits with the big kernel lock released (the interrupt may
 * be delivered to another CPU, which needs the lock to run the handler) and
 * for at most 20 ticks. */
static int psm_command(struct psm_softc *sc, uint8 cmd) {
    uint64 deadline;

    sc->cmd_have = 0;
    sc->cmd_pending = 1;
    if (atkbdc_aux_write(cmd) != 0) {
        sc->cmd_pending = 0;
        return -1;
    }
    deadline = timer_ticks_now() + 20;
    while (!sc->cmd_have && timer_ticks_now() < deadline) {
        bkl_wait_for_interrupt();
    }
    sc->cmd_pending = 0;
    return (sc->cmd_have && sc->cmd_reply == PSM_ACK) ? 0 : -1;
}

/* --- probe and attach ------------------------------------------------------ */

static int psm_get_id(uint8 *id) {
    if (atkbdc_aux_command(PSM_CMD_GET_ID) != 0) {
        return -1;
    }
    return atkbdc_aux_read(id, 100);
}

static int psm_set_rate(uint8 rate) {
    if (atkbdc_aux_command(PSM_CMD_SET_RATE) != 0) {
        return -1;
    }
    return atkbdc_aux_command(rate);
}

static int psm_probe(device_t dev) {
    const atkbdc_ivars_t *iv = (const atkbdc_ivars_t *)device_get_ivars(dev);
    uint8 id;

    if (iv == NULL || iv->port != ATKBDC_PORT_AUX) {
        return ENXIO;
    }
    /* Something on the port answers like a mouse: it ACKs a command and
     * reports a pointing-device ID. Streaming off first, so the ID is not
     * interleaved with a packet. */
    if (atkbdc_aux_command(PSM_CMD_DISABLE) != 0 || psm_get_id(&id) != 0) {
        return ENXIO;
    }
    if (id != 0x00 && id != 0x03 && id != 0x04) {
        return ENXIO;
    }
    return BUS_PROBE_DEFAULT;
}

static int psm_attach(device_t dev) {
    struct psm_softc *sc = device_get_softc(dev);
    const atkbdc_ivars_t *iv = (const atkbdc_ivars_t *)device_get_ivars(dev);
    uint8 id = 0;

    sc->dev = dev;
    if (atkbdc_aux_command(PSM_CMD_SET_DEFAULTS) != 0) {
        device_printf(dev, "set-defaults was not acknowledged\n");
        return ENXIO;
    }

    /* The IntelliMouse knock: rates 200, 100, 80 in a row turn on the wheel,
     * which the next GET_ID then admits to (3). A mouse without one ignores
     * the sequence and still says 0. Then back to 100 reports a second. */
    if (psm_set_rate(200) == 0 && psm_set_rate(100) == 0 &&
        psm_set_rate(80) == 0 && psm_get_id(&id) == 0) {
        sc->id = id;
    }
    sc->packet_size = (sc->id == 3 || sc->id == 4) ? 4 : 3;
    (void)psm_set_rate(100);

    sc->irq_rid = 0;
    sc->irq = bus_alloc_resource(dev, SYS_RES_IRQ, &sc->irq_rid,
                                 (unsigned long long)iv->irq,
                                 (unsigned long long)iv->irq, 1, RF_ACTIVE);
    if (sc->irq == NULL) {
        device_printf(dev, "could not allocate IRQ %d\n", iv->irq);
        return ENXIO;
    }
    if (bus_setup_intr(dev, sc->irq, INTR_TYPE_MISC | INTR_MPSAFE,
                       psm_intr, NULL, sc, &sc->ih) != 0) {
        device_printf(dev, "could not set up the interrupt\n");
        bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
        sc->irq = NULL;
        return ENXIO;
    }

    /* Streaming on (still polled - the interrupt is off in the controller),
     * THEN the interrupt. The other order hands the ACK to the handler as a
     * packet byte. */
    if (atkbdc_aux_command(PSM_CMD_ENABLE) != 0 || atkbdc_aux_irq(1) != 0) {
        device_printf(dev, "could not enable reporting\n");
        bus_teardown_intr(dev, sc->irq, sc->ih);
        bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
        sc->irq = NULL;
        return ENXIO;
    }
    psm_sc = sc;
    device_printf(dev, "<PS/2 Mouse> id %d, %d-byte packets, irq %d\n",
                  (int)sc->id, sc->packet_size, iv->irq);
    return 0;
}

static void psm_detach(device_t dev) {
    struct psm_softc *sc = device_get_softc(dev);

    if (sc == NULL || sc->irq == NULL) {
        return;
    }
    (void)atkbdc_aux_irq(0);
    bus_teardown_intr(dev, sc->irq, sc->ih);
    bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
    sc->irq = NULL;
    if (psm_sc == sc) {
        psm_sc = NULL;
    }
}

static device_method_t psm_methods[] = {
    DEVMETHOD(device_probe,  psm_probe),
    DEVMETHOD(device_attach, psm_attach),
    DEVMETHOD(device_detach, psm_detach),
    DEVMETHOD_END,
};

static driver_t psm_driver = {
    .name    = "psm",
    .methods = psm_methods,
    .size    = sizeof(struct psm_softc),
    .pass    = BUS_PASS_DEFAULT,
};

DRIVER_MODULE(psm, atkbdc, psm_driver, 0, 0);

/* --- selftest --------------------------------------------------------------- */

static int st_failures;

static void st_fail(const char *what, int step) {
    st_failures++;
    kprintf_c(0x0C, "psm: selftest: %s (step %d)\n", what, step);
}

/* Push a packet through the controller's loopback and wait - with the lock
 * released - until `events` events are queued or 50 ticks pass. */
static int inject(const uint8 *bytes, int n, int events) {
    uint64 deadline;
    int i;

    for (i = 0; i < n; i++) {
        uint64 by = timer_ticks_now() + 20;

        if (atkbdc_aux_loopback(bytes[i]) != 0) {
            return -1;
        }
        /* One byte at a time: the output buffer holds one, and a second
         * loopback before the handler has drained the first would
         * overwrite it. "Drained" is visible as the decoder moving: the
         * packet position changed, a packet completed, or a byte was
         * discarded for want of bit 3. */
        {
            uint64 before = psm_sc->packets + psm_sc->resyncs;
            int    pos    = psm_sc->pos;

            while (psm_sc->pos == pos &&
                   psm_sc->packets + psm_sc->resyncs == before &&
                   timer_ticks_now() < by) {
                bkl_wait_for_interrupt();
            }
        }
    }
    deadline = timer_ticks_now() + 50;
    while (mouse_pending() < events && timer_ticks_now() < deadline) {
        bkl_wait_for_interrupt();
    }
    return mouse_pending();
}

static void expect(int step, uint16 type, uint16 code, int32 value) {
    mouse_event_t ev;

    if (!mouse_take(&ev)) {
        st_fail("an expected event never arrived", step);
        return;
    }
    if (ev.type != type || ev.code != code || ev.value != value) {
        st_failures++;
        kprintf_c(0x0C, "psm: selftest: step %d wanted %x/%x/%d, got "
                        "%x/%x/%d\n", step, (uint32)type, (uint32)code,
                  value, (uint32)ev.type, (uint32)ev.code, ev.value);
    }
}

static void expect_empty(int step) {
    if (mouse_pending() != 0) {
        st_fail("events left over that nothing should have produced", step);
        mouse_flush();
    }
}

int psm_selftest(void) {
    struct psm_softc *sc = psm_sc;
    uint8 pkt[5];
    int sz;

    if (sc == NULL) {
        kprintf_c(0x0E, "psm: selftest skipped - no PS/2 mouse attached\n");
        return 0;
    }
    st_failures = 0;
    sz = sc->packet_size;

    /* Quiet the real mouse (through the interrupt-driven path, which is
     * itself under test here), and start from nothing. */
    if (psm_command(sc, PSM_CMD_DISABLE) != 0) {
        st_fail("disable-streaming was not acknowledged via IRQ 12", 0);
    }
    mouse_flush();
    sc->pos = 0;

    /* 1: left down, 5 right, 3 UP in PS/2 terms -> REL_Y -3. */
    pkt[0] = 0x09; pkt[1] = 0x05; pkt[2] = 0x03; pkt[3] = 0x00;
    inject(pkt, sz, 4);
    expect(1, EV_REL, REL_X, 5);
    expect(1, EV_REL, REL_Y, -3);
    expect(1, EV_KEY, BTN_LEFT, 1);
    expect(1, EV_SYN, SYN_REPORT, 0);
    expect_empty(1);

    /* 2: negative both ways through the ninth bit (-2 across, 4 DOWN), and
     * left released. */
    pkt[0] = 0x38; pkt[1] = 0xFE; pkt[2] = 0xFC; pkt[3] = 0x00;
    inject(pkt, sz, 4);
    expect(2, EV_REL, REL_X, -2);
    expect(2, EV_REL, REL_Y, 4);
    expect(2, EV_KEY, BTN_LEFT, 0);
    expect(2, EV_SYN, SYN_REPORT, 0);
    expect_empty(2);

    /* 3: a stray byte without bit 3, then a real packet (right button). The
     * stray must not become the start of a packet. */
    pkt[0] = 0x00;
    inject(pkt, 1, 0);
    pkt[0] = 0x0A; pkt[1] = 0x00; pkt[2] = 0x00; pkt[3] = 0x00;
    inject(pkt, sz, 2);
    expect(3, EV_KEY, BTN_RIGHT, 1);
    expect(3, EV_SYN, SYN_REPORT, 0);
    expect_empty(3);

    /* 3b: motion with the right button STILL held - a drag. The button is
     * not reported again: only changes become events. */
    pkt[0] = 0x0A; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x00;
    inject(pkt, sz, 2);
    expect(3, EV_REL, REL_X, 1);
    expect(3, EV_SYN, SYN_REPORT, 0);
    expect_empty(3);

    /* 4: X overflow - motion dropped, the right button released. */
    pkt[0] = 0x48; pkt[1] = 0x7F; pkt[2] = 0x10; pkt[3] = 0x00;
    inject(pkt, sz, 2);
    expect(4, EV_KEY, BTN_RIGHT, 0);
    expect(4, EV_SYN, SYN_REPORT, 0);
    expect_empty(4);

    /* 5: the wheel, one notch AWAY from the user (PS/2 -1 -> REL_WHEEL +1),
     * on a mouse that has one. */
    if (sz == 4) {
        pkt[0] = 0x08; pkt[1] = 0x00; pkt[2] = 0x00; pkt[3] = 0xFF;
        inject(pkt, sz, 2);
        expect(5, EV_REL, REL_WHEEL, 1);
        expect(5, EV_SYN, SYN_REPORT, 0);
        expect_empty(5);
    }

    /* 6: a report that changes nothing produces nothing. */
    pkt[0] = 0x08; pkt[1] = 0x00; pkt[2] = 0x00; pkt[3] = 0x00;
    inject(pkt, sz, 0);
    expect_empty(6);

    /* 7: a packet abandoned after its first byte (a byte lost on the wire
     * looks exactly like this), then silence longer than PSM_GAP_TICKS, then
     * a whole packet: the middle button. Without the timeout the stale
     * first byte would frame the new packet one byte late. */
    pkt[0] = 0x09;
    inject(pkt, 1, 0);
    {
        uint64 until = timer_ticks_now() + PSM_GAP_TICKS + 3;

        while (timer_ticks_now() < until) {
            bkl_wait_for_interrupt();
        }
    }
    pkt[0] = 0x0C; pkt[1] = 0x00; pkt[2] = 0x00; pkt[3] = 0x00;
    inject(pkt, sz, 2);
    expect(7, EV_KEY, BTN_MIDDLE, 1);
    expect(7, EV_SYN, SYN_REPORT, 0);
    expect_empty(7);
    pkt[0] = 0x08;
    inject(pkt, sz, 2);
    expect(7, EV_KEY, BTN_MIDDLE, 0);
    expect(7, EV_SYN, SYN_REPORT, 0);
    expect_empty(7);

    /* Back to streaming, with a clean queue for the first real reader. */
    if (psm_command(sc, PSM_CMD_ENABLE) != 0) {
        st_fail("enable-streaming was not acknowledged via IRQ 12", 8);
    }
    mouse_flush();
    sc->pos = 0;

    if (st_failures == 0) {
        kprintf_c(0x0A, "psm: selftest passed (%d-byte packets)\n", sz);
    } else {
        kprintf_c(0x0C, "psm: selftest FAILED (%d)\n", st_failures);
    }
    return st_failures;
}

void psm_report(uint8 color) {
    struct psm_softc *sc = psm_sc;

    if (sc == NULL) {
        kprintf_c(color, "psm: no PS/2 mouse\n");
        return;
    }
    kprintf_c(color, "psm0: id %d, %d packets, %d resyncs, %d overflows, "
                     "%d reports dropped\n",
              (int)sc->id, (int)sc->packets, (int)sc->resyncs,
              (int)sc->overflows, (int)mouse_dropped());
}
