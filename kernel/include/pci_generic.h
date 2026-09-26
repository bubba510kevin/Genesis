#ifndef PCI_GENERIC_H
#define PCI_GENERIC_H

/* Registers pci_generic_driver on the "pci" devclass at BUS_PROBE_GENERIC -
 * the lowest priority, so any driver with a real vendor/device-ID match
 * registered after this one always wins (see bus.h's BUS_PROBE_* comment).
 * Proves the devclass_add_driver + bus_probe_and_attach path end to end by
 * matching every PCI function unconditionally and reporting what attach()
 * was handed - no hardware is touched beyond the config-space reads pci.c
 * already did during pci_init.
 *
 * Also the literal template for a real PCI driver (a NIC, eventually,
 * ROADMAP item 6): same probe()/attach() shape, with a real vendor/
 * device-ID table in place of "match everything" - see
 * vendsrc/sys/dev/e1000/if_em.c's em_vendor_info_array or
 * vendsrc/sys/dev/virtio/pci/virtio_pci_legacy.c's vtpci_legacy_probe for
 * what that table and probe function look like in FreeBSD's own tree. */
void pci_generic_register(void);

#endif
