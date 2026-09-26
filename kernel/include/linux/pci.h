#ifndef LINUX_PCI_H
#define LINUX_PCI_H

#include "linux/types.h"
#include "linux/device.h"
#include "linux/io.h"
#include "modinit.h"

/* Angle-bracket, not quoted: this file is itself named pci.h, and GCC's
 * quote-form search checks the including file's own directory first - a
 * quoted #include "pci.h" here would resolve to itself (a no-op, since its
 * include guard is already open) instead of kernel/include/pci.h, and
 * pci_ivars_t below would silently fail to resolve. The angle-bracket form
 * skips straight to the -I search path and finds the real one. */
#include <pci.h>

/* Minimal subset of FreeBSD LinuxKPI's linux/pci.h (vendsrc/sys/compat/
 * linuxkpi/common/include/linux/pci.h), confirmed against how real vendored
 * driver source actually uses it (e.g. vendsrc/sys/contrib/dev/rtw88/
 * rtw8723de.c's struct pci_driver + PCI_DEVICE() + module_pci_driver()).
 * Just enough for a simple PCI driver's probe/remove; anything a real
 * driver needs beyond this gets added when that driver lands, not
 * speculatively. */

struct pci_device_id {
    u32     vendor;
    u32     device;
    u32     subvendor;
    u32     subdevice;
    u32     class;
    u32     class_mask;
    uintptr driver_data;
};

#define PCI_ANY_ID ((u32)-1)

#define PCI_DEVICE(_vendor, _device) \
    .vendor = (_vendor), .device = (_device), \
    .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID

/* Genesis's reduced struct pci_dev: dev embeds the generic linux/device.h
 * shim, ivars is the bus-owned pci_ivars_t (pci.h) the matched bus_dev_t
 * carries - read-only, not freed here, same contract bus_get_ivars
 * documents. vendor/device/class/revision are copied out of *ivars for the
 * common case (driver code reads pdev->vendor directly, the way real
 * LinuxKPI's struct pci_dev also exposes them) without every caller having
 * to chase the pointer. */
struct pci_dev {
    struct device dev;
    pci_ivars_t  *ivars;
    u16 vendor, device;
    u32 class;
    u8  revision;
    void *driver_data;
};

static inline void  pci_set_drvdata(struct pci_dev *pdev, void *data) {
    pdev->driver_data = data;
}
static inline void *pci_get_drvdata(struct pci_dev *pdev) {
    return pdev->driver_data;
}

/* pci_enable_device and pci_set_master are REAL now - they were bookkeeping
 * no-ops while there was no way to write config space, and there is one.
 *
 * They matter. pci_set_master sets PCI_COMMAND_MASTER, without which the
 * device cannot initiate DMA at all: a driver that programs a descriptor
 * ring, kicks the device and waits gets silence, and nothing anywhere
 * reports an error. pci_enable_device sets the I/O and memory decode bits,
 * without which the BARs do not respond.
 *
 * pci_request_regions stays bookkeeping. It reserves a range against other
 * drivers, and Genesis's bus.c already does real conflict detection through
 * bus_alloc_resource - so there is a mechanism, it is just not this one. */
int  pci_enable_device(struct pci_dev *pdev);
void pci_set_master(struct pci_dev *pdev);
void pci_clear_master(struct pci_dev *pdev);

static inline int pci_request_regions(struct pci_dev *pdev, const char *name) {
    (void)pdev; (void)name;
    return 0;
}
static inline void pci_release_regions(struct pci_dev *pdev) {
    (void)pdev;
}
static inline void pci_disable_device(struct pci_dev *pdev) {
    (void)pdev;
}

/* --- BARs ----------------------------------------------------------------
 *
 * pci_iomap USED to return NULL unconditionally, because mapping an
 * arbitrary physical BAR into kernel virtual space needed an entry point
 * that did not exist. It exists now - ioremap, in linux/io.h, over the same
 * kvm_alloc_range + vmm_map_page path kernel/arch/lapic.c and
 * kernel/arch/ioapic.c already map their register pages with - so this is
 * real. A driver can now actually touch its device.
 *
 * The resource_start/len/flags accessors are what driver source reads before
 * mapping, and they come straight off pci_bar_size (pci.h). */
#define IORESOURCE_IO   0x00000100
#define IORESOURCE_MEM  0x00000200

unsigned long pci_resource_start(struct pci_dev *pdev, int bar);
unsigned long pci_resource_len(struct pci_dev *pdev, int bar);
unsigned long pci_resource_flags(struct pci_dev *pdev, int bar);

void *pci_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);
void  pci_iounmap(struct pci_dev *pdev, void *addr);
static inline void *pci_ioremap_bar(struct pci_dev *pdev, int bar) {
    return pci_iomap(pdev, bar, 0);
}

/* --- config space --------------------------------------------------------
 *
 * Linux's accessors return an error code and deliver the value through a
 * pointer, which is not how Genesis's pci_cfg_read* work - those return the
 * value. Wrapped rather than re-spelled, because driver source writes
 * `pci_read_config_word(pdev, PCI_COMMAND, &cmd)` and must not have to
 * change. They always return 0: a config-space read on a function that was
 * already enumerated cannot fail. */
int pci_read_config_byte(struct pci_dev *pdev, int where, u8 *val);
int pci_read_config_word(struct pci_dev *pdev, int where, u16 *val);
int pci_read_config_dword(struct pci_dev *pdev, int where, u32 *val);
int pci_write_config_byte(struct pci_dev *pdev, int where, u8 val);
int pci_write_config_word(struct pci_dev *pdev, int where, u16 val);
int pci_write_config_dword(struct pci_dev *pdev, int where, u32 val);

/* The config-space register offsets driver source names. Same values as the
 * PCI spec and as Genesis's own pci.c uses internally. */
#define PCI_VENDOR_ID        0x00
#define PCI_DEVICE_ID        0x02
#define PCI_COMMAND          0x04
#define PCI_STATUS           0x06
#define PCI_REVISION_ID      0x08
#define PCI_CLASS_DEVICE     0x0A
#define PCI_HEADER_TYPE      0x0E
#define PCI_BASE_ADDRESS_0   0x10
#define PCI_INTERRUPT_LINE   0x3C
#define PCI_INTERRUPT_PIN    0x3D

#define PCI_COMMAND_IO       0x1
#define PCI_COMMAND_MEMORY   0x2
#define PCI_COMMAND_MASTER   0x4

/* The IRQ line this device's interrupts arrive on, for request_irq. Read off
 * the ivars rather than out of config space each time - pci.c recorded it
 * during enumeration. */
unsigned int pci_irq_line(struct pci_dev *pdev);

struct pci_driver {
    const char *name;
    const struct pci_device_id *id_table;
    int  (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
    void (*remove)(struct pci_dev *dev);
};

/* Implemented in kernel/lkpi.c: builds one driver_t (bus.h) per registered
 * Linux driver and adds it to the "pci" devclass, reusing bus_probe_and_
 * attach unchanged. */
int linux_pci_register_driver(struct pci_driver *pdrv);

/* Real driver source calls this with a variable name, not a pointer or a
 * function body - module_pci_driver(rtw88_pci_driver); - and needs zero
 * edits to work here.
 *
 * It expands to a generator function AND a .genesis_modinit entry. The
 * function keeps its old name because flk.c calls it explicitly for drivers
 * compiled INTO the kernel, where there is no loader to do it (bus.h's "no
 * magic" rule still holds - nothing runs by itself). The section entry is
 * what kldload calls when the same source arrives as a .ko instead.
 *
 * Before this, only the named function existed, and kldload searched for
 * three hardcoded names that did not include it - so a real LinuxKPI driver
 * dropped in /lib/modules relocated correctly and then failed with
 * KLD_ERR_NOENTRY, its entry point sitting untouched in its own symbol
 * table. See modinit.h. */
#define module_pci_driver(_drv) \
    int _drv##_lkpi_module_init(void) { return linux_pci_register_driver(&(_drv)); } \
    GENESIS_MODULE_INIT(_drv##_lkpi_module_init)

#endif
