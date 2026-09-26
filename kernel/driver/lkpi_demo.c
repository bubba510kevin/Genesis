#include "linux/device.h"
#include "linux/module.h"
#include "linux/pci.h"
#include "linux/types.h"

/* Proof-of-concept Linux-shaped driver, the same role pci_generic.c plays
 * for Newbus: real driver-source idioms (struct pci_driver, PCI_DEVICE(),
 * module_pci_driver()), proving lkpi.c's whole chain end to end -
 * linux_pci_register_driver -> driver_t -> bus_probe_and_attach -> this
 * probe() -> dev_err -> kprintf.
 *
 * Matches QEMU's emulated e1000 (8086:100e) specifically, rather than
 * PCI_ANY_ID: pci_generic (Newbus), this, and wdm_demo (WDM) are all
 * registered on the same "pci" devclass, and bus_probe_and_attach picks
 * exactly one winner per device by priority (bus.h) - three simultaneous
 * catch-alls would mean only the highest-priority one ever attaches, and
 * the other two demos would never print. A real vendor/device match at
 * BUS_PROBE_DEFAULT, same as any real driver would use, is what actually
 * proves this chain reaches a specific piece of hardware rather than
 * "whatever's left after the others lost". */

static const struct pci_device_id lkpi_demo_ids[] = {
    { PCI_DEVICE(0x8086, 0x100e) }, /* QEMU emulated e1000 */
    { 0, 0, 0, 0, 0, 0, 0 },
};
MODULE_DEVICE_TABLE(pci, lkpi_demo_ids);

static int lkpi_demo_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
    (void)id;
    dev_err(&pdev->dev, "lkpi_demo: matched %x:%x\n", pdev->vendor, pdev->device);
    return 0;
}

static void lkpi_demo_remove(struct pci_dev *pdev) {
    (void)pdev;
}

static struct pci_driver lkpi_demo_driver = {
    .name     = "lkpi_demo",
    .id_table = lkpi_demo_ids,
    .probe    = lkpi_demo_probe,
    .remove   = lkpi_demo_remove,
};

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Genesis");
MODULE_DESCRIPTION("Linux-shaped driver-model proof of concept");

module_pci_driver(lkpi_demo_driver);
