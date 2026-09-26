#include "dev/pci/pcivar.h"
#include "kprintf.h"
#include "sys/bus.h"

/* Proof-of-concept FreeBSD Newbus source-compat driver - the same role
 * pci_generic.c/lkpi_demo.c/wdm_demo.c play for their models, written
 * against the real DEVMETHOD/DRIVER_MODULE/device_t/pci_get_vendor idiom
 * (see kernel/include/sys/bus.h, kernel/include/dev/pci/pcivar.h). Not a
 * real driver - see the plan's Non-goals: a real virtio/e1000 port needs
 * callout/mtx/bus_alloc_resource/iflib, none of which exist yet.
 *
 * Matches the PIIX3 IDE controller (8086:7010) specifically, one of the
 * four functions pci_generic currently wins by default - proving
 * priority ranking scales to a fourth competing driver on the same "pci"
 * devclass, the same way lkpi_demo/wdm_demo proved it against
 * pci_generic last pass with e1000/std-VGA. */

static int newbus_demo_probe(device_t dev) {
    if (pci_get_vendor(dev) != 0x8086 || pci_get_device(dev) != 0x7010) {
        return 1; /* not mine */
    }
    return BUS_PROBE_DEFAULT;
}

/* Softc, so the resource can be held for the life of the attachment rather
 * than allocated and dropped inside attach - which is what a real driver
 * does and what makes the conflict detection mean anything. */
struct newbus_demo_softc {
    struct resource *bar;
    int              rid;
};

static int newbus_demo_attach(device_t dev) {
    struct newbus_demo_softc *sc = device_get_softc(dev);

    kprintf_c(0x0A, "newbus_compat_demo: matched %x:%x\n",
              pci_get_vendor(dev), pci_get_device(dev));

    /* The real bus_alloc_resource_any idiom, written exactly as FreeBSD
     * driver source writes it - PCIR_BAR(n) as the rid, RF_ACTIVE, and
     * rman_get_start to read it back. This is the gap item 11 names being
     * closed at the point it was complained about: before, a driver reached
     * pci_bar_size straight off the raw ivars and nothing recorded that the
     * region was taken.
     *
     * BAR 4 on the PIIX3 IDE controller is its bus-master I/O range. This
     * driver does not program it - it is not a real IDE driver, see the
     * header comment - it reserves it, which is the part being demonstrated.
     *
     * A failure is logged and NOT fatal: the emulated device may not
     * implement the BAR, and refusing to attach over that would make this
     * demo fail on a machine where the rest of it works fine. */
    sc->rid = PCIR_BAR(4);
    sc->bar = bus_alloc_resource_any(dev, SYS_RES_IOPORT, &sc->rid,
                                     RF_ACTIVE);
    if (sc->bar == NULL) {
        kprintf_c(0x0E, "newbus_compat_demo: BAR4 unavailable\n");
    } else {
        kprintf_c(0x0A, "newbus_compat_demo: BAR4 io %lx size %lx\n",
                  rman_get_start(sc->bar), rman_get_size(sc->bar));
    }
    return 0;
}

static void newbus_demo_detach(device_t dev) {
    struct newbus_demo_softc *sc = device_get_softc(dev);

    /* Nothing calls detach yet (no hot-remove - see bus.h), but releasing
     * here is what makes the allocation above a matched pair rather than a
     * one-way reservation, and it is what a reader copying this file as a
     * template needs to see. */
    if (sc != NULL && sc->bar != NULL) {
        bus_release_resource(dev, SYS_RES_IOPORT, sc->rid, sc->bar);
        sc->bar = NULL;
    }
}

static device_method_t newbus_demo_methods[] = {
    DEVMETHOD(device_probe,  newbus_demo_probe),
    DEVMETHOD(device_attach, newbus_demo_attach),
    DEVMETHOD(device_detach, newbus_demo_detach),
    DEVMETHOD_END,
};

static driver_t newbus_demo_driver = {
    .name    = "newbus_compat_demo",
    .methods = newbus_demo_methods,
    .size    = sizeof(struct newbus_demo_softc),
    .pass    = BUS_PASS_DEFAULT,
};

DRIVER_MODULE(newbus_compat_demo, pci, newbus_demo_driver, 0, 0);
