#ifndef DEV_PCI_PCIVAR_H
#define DEV_PCI_PCIVAR_H

#include "sys/bus.h"

/* <dev/pci/pcivar.h> - what real FreeBSD PCI driver source reads a device's
 * identity and config space through.
 *
 * Implemented in kernel/driver/newbus_compat.c over the pci_ivars_t the bus
 * already filled in during enumeration, so none of these re-scan anything.
 *
 * This file used to carry exactly two accessors - vendor and device - which
 * was enough for the demo driver that was the only consumer. A real driver
 * reads more than that before it decides what it is looking at: the revision
 * to pick a chip errata workaround, the subvendor to tell one board from
 * another with the same silicon, the class to sanity-check the match.
 */

unsigned short pci_get_vendor(device_t dev);
unsigned short pci_get_device(device_t dev);
unsigned short pci_get_subvendor(device_t dev);
unsigned short pci_get_subdevice(device_t dev);
unsigned char  pci_get_revid(device_t dev);
unsigned char  pci_get_class(device_t dev);
unsigned char  pci_get_subclass(device_t dev);
unsigned char  pci_get_progif(device_t dev);

/* The full 32-bit vendor:device, as driver source compares against a table.
 * Note the ORDER: device in the high half, vendor in the low. That is
 * upstream's convention and it is the opposite of how the pair is usually
 * written, which is exactly why a driver copying an ID table from FreeBSD
 * source has to get it from here rather than composing it by hand. */
unsigned int   pci_get_devid(device_t dev);

/* Config space. `width` is 1, 2 or 4 BYTES - not bits, and not a shift.
 * A driver passing 32 here reads four bytes at a wildly wrong width. */
unsigned int pci_read_config(device_t dev, int reg, int width);
void         pci_write_config(device_t dev, int reg, unsigned int val,
                              int width);

/* Bus mastering. Without this the device cannot initiate DMA at all - it
 * can be read and written by the CPU and it can raise interrupts, but a
 * descriptor ring it is meant to fetch is never fetched, and nothing
 * anywhere reports an error. Every DMA-capable driver calls this. */
void pci_enable_busmaster(device_t dev);
void pci_disable_busmaster(device_t dev);
void pci_enable_io(device_t dev, int space);

/* Does this function have a PCI power-management capability? A driver asks
 * before trying to move the device between D0 and D3, and one that assumes
 * the capability exists writes to a config-space offset that means something
 * else entirely. Answered by walking the capability list, which pci.c
 * already does for MSI. */
int pci_has_pm(device_t dev);
int pci_get_powerstate(device_t dev);

/* Arm PME - the wake-from-low-power signal - on a device that has a power
 * management capability. A driver calls this when Wake-on-LAN is enabled.
 * Genesis never suspends, so nothing will ever wake; the bit is still
 * programmed, because leaving it clear while telling the driver WOL is on
 * would be a lie the driver has no way to detect. */
void pci_enable_pme(device_t dev);

/* --- MSI / MSI-X ---------------------------------------------------------
 *
 * How many message-signalled interrupts this function supports, and the
 * allocate/release pair a driver uses to take them.
 *
 * Genesis has real MSI (kernel/dev/pci.c's pci_msi_alloc, and the LAPIC
 * decodes the message - see kernel/include/lapic.h). It does NOT have MSI-X,
 * whose vector table lives in a BAR rather than in config space; pci_msix_count
 * therefore answers 0, which is the same answer a device without the
 * capability gives and which every driver already handles by falling back to
 * MSI or to a legacy line.
 *
 * pci_alloc_msi takes a count IN/OUT: the driver asks for N and is told how
 * many it got. Asking for more than one gets one, because Genesis allocates a
 * single vector per function. */
int pci_msi_count(device_t dev);
int pci_msix_count(device_t dev);
int pci_alloc_msi(device_t dev, int *count);
int pci_alloc_msix(device_t dev, int *count);
int pci_release_msi(device_t dev);

/* Find a capability by ID, returning its config-space offset in *capreg.
 * The older pci_find_capability spelling exists in Genesis's own pci.h;
 * this is upstream's, which reports found/not-found separately from the
 * offset so that offset 0 is unambiguous. */
int pci_find_cap(device_t dev, int capability, int *capreg);
int pci_find_extcap(device_t dev, int capability, int *capreg);

/* The PCIe maximum-read-request size, in bytes. A driver lowers it when its
 * DMA engine cannot cope with long completions. Returns the value actually
 * set, which on a device with no PCIe capability is the 512-byte default -
 * reported honestly rather than echoing back what was asked for. */
int pci_set_max_read_req(device_t dev, int size);

#define PCIY_PMG      0x01
#define PCIY_MSI      0x05
#define PCIY_PCIX     0x07
#define PCIY_EXPRESS  0x10
#define PCIY_MSIX     0x11
int pci_set_powerstate(device_t dev, int state);

#define PCI_POWERSTATE_D0  0
#define PCI_POWERSTATE_D1  1
#define PCI_POWERSTATE_D2  2
#define PCI_POWERSTATE_D3  3
#define PCI_POWERSTATE_UNKNOWN (-1)

/* The config-space offsets and command bits driver source names.
 *
 * These belong to <dev/pci/pcireg.h>, which real driver source includes and
 * which is now vendored verbatim at kernel/bsd/compat/dev/pci/pcireg.h. They
 * stay here as well because a driver on the modules/ include path sees only
 * kernel/include and would otherwise have nowhere to get them - so both
 * spellings exist and the guard makes whichever is included first win rather
 * than warn. */
#ifndef PCIR_COMMAND
#define PCIR_COMMAND      0x04
#define PCIR_REVID        0x08
#define PCIR_PROGIF       0x09
#define PCIR_SUBCLASS     0x0A
#define PCIR_CLASS        0x0B
#define PCIR_INTLINE      0x3C
#define PCIR_INTPIN       0x3D

#define PCIM_CMD_PORTEN   0x0001
#define PCIM_CMD_MEMEN    0x0002
#define PCIM_CMD_BUSMASTEREN 0x0004
#endif /* PCIR_COMMAND */

#endif
