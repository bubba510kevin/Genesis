#include "bus.h"
#include "pci.h"
#include "pci_generic.h"
#include "screen.h"
#include "typesk.h"

/* See pci_generic.h - this driver's only job is to prove the devclass/
 * probe/attach machinery works, by matching everything and printing what
 * it found. Not a real driver for anything. */

static int generic_probe(bus_dev_t *dev) {
    (void)dev;
    /* HOOVER, not GENERIC, now that bus.h carries upstream's full scale.
     * This driver matches literally everything and exists only so an
     * unclaimed function still gets reported - which is precisely what
     * upstream reserves BUS_PROBE_HOOVER for. No behaviour change (it was
     * already the lowest-priority candidate); the value now says why. */
    return BUS_PROBE_HOOVER;
}

static int generic_attach(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);

    print_string("  pci_generic: ", 0x0B);
    print_hex(f->bus, 0x0B);
    print_string(":", 0x0B);
    print_hex(f->slot, 0x0B);
    print_string(".", 0x0B);
    print_hex(f->func, 0x0B);
    print_string("  ", 0x0B);
    print_hex(f->vendor_id, 0x0B);
    print_string(":", 0x0B);
    print_hex(f->device_id, 0x0B);
    print_string("  matched\n", 0x0B);
    return 0;
}

/* Nothing to undo, and that is exactly why this exists rather than staying
 * NULL.
 *
 * bus.c's try_replace refuses to take a device away from a driver with no
 * detach method, and it is right to: a NULL detach means nobody knows what
 * the driver registered, so tearing it out would leave interrupt handlers
 * installed and resources allocated against a softc about to be freed.
 *
 * This driver genuinely registers nothing - it printed a line at attach and
 * has held no state since. Saying so with an empty detach is what lets a
 * real driver, loaded from a module after boot, displace this one on a
 * device it merely reported. Leaving it NULL would have pinned every PCI
 * function in the machine to the reporting driver forever. */
static void generic_detach(bus_dev_t *dev) {
    (void)dev;
}

static driver_t pci_generic_driver = {
    .name       = "pci_generic",
    .probe      = generic_probe,
    .attach     = generic_attach,
    .detach     = generic_detach,
    .softc_size = 0,
};

void pci_generic_register(void) {
    devclass_add_driver(devclass_find("pci"), &pci_generic_driver);
}
