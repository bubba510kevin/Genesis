#include "bus.h"
#include "irq.h"
#include "linux/device.h"
#include "linux/interrupt.h"
#include "linux/pci.h"
#include "linux/types.h"
#include "pci.h"
#include "typesk.h"

/* The Linux-shaped adapter: turns a registered struct pci_driver (linux/
 * pci.h) into one driver_t (bus.h) and lets bus_probe_and_attach - built
 * for Newbus, unchanged - do the actual matching. See the plan's Part 2.
 *
 * The awkward part is that driver_t's probe/attach are bare `int (*)
 * (bus_dev_t*)` function pointers with no user-data slot, so there is no
 * way for a single trampoline function to know WHICH registered
 * pci_driver it is standing in for - C has no closures. The fix is the
 * standard one for this exact limitation: a fixed number of distinct
 * trampoline functions (lkpi_probe_0..N, lkpi_attach_0..N), each baking in
 * its own slot index at compile time, with a small table on the side
 * saying which struct pci_driver* each slot currently means. Growing past
 * LKPI_MAX_DRIVERS means adding another pair, not redesigning this. */

#define LKPI_MAX_DRIVERS 4

static struct pci_driver *lkpi_slot[LKPI_MAX_DRIVERS];
static driver_t           lkpi_driver_t[LKPI_MAX_DRIVERS];
static int                lkpi_slot_count;

/* NULL id_table never matches - a driver that forgot to set one should not
 * silently claim every PCI function on the bus. Linux's own id_table
 * convention terminates the array with an all-zero entry; PCI_ANY_ID
 * (0xFFFFFFFF) is never 0, so a wildcard row never gets mistaken for the
 * terminator. */
static const struct pci_device_id *lkpi_find_id(const struct pci_driver *pdrv,
                                                  const pci_ivars_t *f) {
    const struct pci_device_id *id;

    if (pdrv->id_table == NULL) {
        return NULL;
    }
    for (id = pdrv->id_table; !(id->vendor == 0 && id->device == 0); id++) {
        if ((id->vendor == PCI_ANY_ID || id->vendor == f->vendor_id) &&
            (id->device == PCI_ANY_ID || id->device == f->device_id)) {
            return id;
        }
    }
    return NULL;
}

static int lkpi_probe_common(int slot, bus_dev_t *dev) {
    struct pci_driver *pdrv = lkpi_slot[slot];
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);

    if (lkpi_find_id(pdrv, f) != NULL) {
        return BUS_PROBE_DEFAULT;
    }
    return 1; /* not mine */
}

static int lkpi_attach_common(int slot, bus_dev_t *dev) {
    struct pci_driver *pdrv = lkpi_slot[slot];
    pci_ivars_t *f = (pci_ivars_t *)bus_get_ivars(dev);
    struct pci_dev *pdev = (struct pci_dev *)bus_get_softc(dev);
    const struct pci_device_id *id = lkpi_find_id(pdrv, f);

    pdev->dev.name   = pdrv->name;
    pdev->dev.parent = NULL;
    pdev->ivars      = f;
    pdev->vendor     = f->vendor_id;
    pdev->device     = f->device_id;
    pdev->class      = ((u32)f->class_code << 16) | ((u32)f->subclass << 8) |
                        f->prog_if;
    pdev->revision   = f->revision_id;
    pdev->driver_data = NULL;

    return pdrv->probe(pdev, id);
}

/* --- BARs and config space -----------------------------------------------
 *
 * Everything below is what a Linux driver calls once its probe is running:
 * find out where the device's registers are, map them, and read or write
 * config space. All of it goes through pci_ivars_t, which the bus already
 * filled in during enumeration - so none of it re-scans anything.
 */

unsigned long pci_resource_start(struct pci_dev *pdev, int bar) {
    uint64 base = 0, size = 0;
    int is_mem = 0;

    if (pdev == NULL || pdev->ivars == NULL) {
        return 0;
    }
    if (pci_bar_size(pdev->ivars, bar, &base, &size, &is_mem) != 0) {
        return 0;
    }
    return (unsigned long)base;
}

unsigned long pci_resource_len(struct pci_dev *pdev, int bar) {
    uint64 base = 0, size = 0;
    int is_mem = 0;

    if (pdev == NULL || pdev->ivars == NULL) {
        return 0;
    }
    if (pci_bar_size(pdev->ivars, bar, &base, &size, &is_mem) != 0) {
        return 0;
    }
    return (unsigned long)size;
}

unsigned long pci_resource_flags(struct pci_dev *pdev, int bar) {
    uint64 base = 0, size = 0;
    int is_mem = 0;

    if (pdev == NULL || pdev->ivars == NULL) {
        return 0;
    }
    if (pci_bar_size(pdev->ivars, bar, &base, &size, &is_mem) != 0) {
        return 0;
    }
    return is_mem ? IORESOURCE_MEM : IORESOURCE_IO;
}

/* Map a memory BAR. Returns NULL for an I/O-port BAR rather than mapping
 * something: a port BAR is not memory and has no physical address to map -
 * handing back a pointer to whatever lives at that physical address would
 * be a mapping that works and reads the wrong device. A driver whose BAR is
 * ports uses inb/outb on pci_resource_start instead. */
void *pci_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen) {
    unsigned long start = pci_resource_start(pdev, bar);
    unsigned long len   = pci_resource_len(pdev, bar);

    if (start == 0 || len == 0) {
        return NULL;
    }
    if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM)) {
        return NULL;
    }
    if (maxlen != 0 && maxlen < len) {
        len = maxlen;
    }
    return ioremap(start, len);
}

void pci_iounmap(struct pci_dev *pdev, void *addr) {
    (void)pdev;
    iounmap(addr);
}

unsigned int pci_irq_line(struct pci_dev *pdev) {
    if (pdev == NULL || pdev->ivars == NULL) {
        return 0;
    }
    return pdev->ivars->int_line;
}

/* Linux returns an error code and delivers the value through a pointer;
 * Genesis's pci_cfg_read* return the value. Wrapped rather than re-spelled,
 * because driver source must not have to change. Always 0: a config read on
 * an already-enumerated function cannot fail. */
int pci_read_config_byte(struct pci_dev *pdev, int where, u8 *val) {
    pci_ivars_t *f = pdev->ivars;
    *val = pci_cfg_read8(f->bus, f->slot, f->func, (uint8)where);
    return 0;
}
int pci_read_config_word(struct pci_dev *pdev, int where, u16 *val) {
    pci_ivars_t *f = pdev->ivars;
    *val = pci_cfg_read16(f->bus, f->slot, f->func, (uint8)where);
    return 0;
}
int pci_read_config_dword(struct pci_dev *pdev, int where, u32 *val) {
    pci_ivars_t *f = pdev->ivars;
    *val = pci_cfg_read32(f->bus, f->slot, f->func, (uint8)where);
    return 0;
}

/* The WRITE side is read-modify-write, because Genesis only has a 32-bit
 * config write - config space is dword-addressed in hardware and a byte or
 * word write has to preserve the rest of the dword. Writing the whole dword
 * with the other three bytes zeroed is the obvious mistake and it clears
 * whatever else shared that register, which for PCI_COMMAND means disabling
 * the device you were trying to configure. */
int pci_write_config_dword(struct pci_dev *pdev, int where, u32 val) {
    pci_ivars_t *f = pdev->ivars;
    pci_cfg_write32(f->bus, f->slot, f->func, (uint8)where, val);
    return 0;
}
int pci_write_config_word(struct pci_dev *pdev, int where, u16 val) {
    pci_ivars_t *f = pdev->ivars;
    uint8  aligned = (uint8)(where & ~3);
    uint32 shift   = (uint32)((where & 3) * 8);
    uint32 cur     = pci_cfg_read32(f->bus, f->slot, f->func, aligned);

    cur &= ~(0xFFFFu << shift);
    cur |= ((uint32)val) << shift;
    pci_cfg_write32(f->bus, f->slot, f->func, aligned, cur);
    return 0;
}
int pci_write_config_byte(struct pci_dev *pdev, int where, u8 val) {
    pci_ivars_t *f = pdev->ivars;
    uint8  aligned = (uint8)(where & ~3);
    uint32 shift   = (uint32)((where & 3) * 8);
    uint32 cur     = pci_cfg_read32(f->bus, f->slot, f->func, aligned);

    cur &= ~(0xFFu << shift);
    cur |= ((uint32)val) << shift;
    pci_cfg_write32(f->bus, f->slot, f->func, aligned, cur);
    return 0;
}

/* Set the I/O and memory decode bits, so the BARs actually respond. Genesis
 * enumerates with these already set by firmware in practice, so this is
 * belt and braces - but a driver is entitled to assume it did the enabling,
 * and a device whose decode bits were cleared by a previous driver's
 * teardown would otherwise be silently dead. */
int pci_enable_device(struct pci_dev *pdev) {
    u16 cmd = 0;

    if (pdev == NULL || pdev->ivars == NULL) {
        return -1;
    }
    pci_read_config_word(pdev, PCI_COMMAND, &cmd);
    pci_write_config_word(pdev, PCI_COMMAND,
                          (u16)(cmd | PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
    return 0;
}

/* Bus mastering. Without this bit the device cannot initiate DMA - it can be
 * read and written by the CPU and it can raise interrupts, but a descriptor
 * ring it is supposed to fetch from memory is never fetched. The failure is
 * total silence with no error anywhere, which is why every DMA-capable
 * driver calls this and why it must not stay a no-op. */
void pci_set_master(struct pci_dev *pdev) {
    u16 cmd = 0;

    if (pdev == NULL || pdev->ivars == NULL) {
        return;
    }
    pci_read_config_word(pdev, PCI_COMMAND, &cmd);
    pci_write_config_word(pdev, PCI_COMMAND, (u16)(cmd | PCI_COMMAND_MASTER));
}

void pci_clear_master(struct pci_dev *pdev) {
    u16 cmd = 0;

    if (pdev == NULL || pdev->ivars == NULL) {
        return;
    }
    pci_read_config_word(pdev, PCI_COMMAND, &cmd);
    pci_write_config_word(pdev, PCI_COMMAND,
                          (u16)(cmd & ~(u16)PCI_COMMAND_MASTER));
}

/* One pair per slot - see the file comment for why this can't be one
 * function. LKPI_MAX_DRIVERS above is the only place the count needs to
 * change; these bodies never do. */
static int lkpi_probe_0(bus_dev_t *dev)  { return lkpi_probe_common(0, dev); }
static int lkpi_attach_0(bus_dev_t *dev) { return lkpi_attach_common(0, dev); }
static int lkpi_probe_1(bus_dev_t *dev)  { return lkpi_probe_common(1, dev); }
static int lkpi_attach_1(bus_dev_t *dev) { return lkpi_attach_common(1, dev); }
static int lkpi_probe_2(bus_dev_t *dev)  { return lkpi_probe_common(2, dev); }
static int lkpi_attach_2(bus_dev_t *dev) { return lkpi_attach_common(2, dev); }
static int lkpi_probe_3(bus_dev_t *dev)  { return lkpi_probe_common(3, dev); }
static int lkpi_attach_3(bus_dev_t *dev) { return lkpi_attach_common(3, dev); }

static int (*const lkpi_probe_fns[LKPI_MAX_DRIVERS])(bus_dev_t *) = {
    lkpi_probe_0, lkpi_probe_1, lkpi_probe_2, lkpi_probe_3,
};
static int (*const lkpi_attach_fns[LKPI_MAX_DRIVERS])(bus_dev_t *) = {
    lkpi_attach_0, lkpi_attach_1, lkpi_attach_2, lkpi_attach_3,
};

/* Add `drv` to the "pci" devclass and, if the boot-time attach pass has
 * already run, probe it against the devices that pass left unclaimed.
 *
 * Both compat layers do exactly this, so it is written once. The reason it
 * is not just devclass_add_driver is that a driver arriving in a loadable
 * module registers long after bus_attach_children - see bus_driver_added in
 * kernel/driver/bus.c, which is upstream's BUS_DRIVER_ADDED and which is
 * what makes a module's driver actually reach hardware rather than silently
 * matching nothing. */
static int register_and_probe(driver_t *drv) {
    devclass_t pci = devclass_find("pci");
    int rc = devclass_add_driver(pci, drv);

    if (rc == 0) {
        bus_driver_added(pci);
    }
    return rc;
}

int linux_pci_register_driver(struct pci_driver *pdrv) {
    int slot = lkpi_slot_count;
    driver_t *drv;

    if (slot >= LKPI_MAX_DRIVERS) {
        return -1;
    }

    lkpi_slot[slot] = pdrv;
    drv = &lkpi_driver_t[slot];
    drv->name  = pdrv->name;
    drv->probe = lkpi_probe_fns[slot];
    drv->attach = lkpi_attach_fns[slot];
    /* remove() wiring is deferred with the rest of hot-remove - PCI
     * devices do not disappear at runtime in this pass, the same
     * bus.h/device.h posture everywhere else in this tree. */
    drv->detach = NULL;
    drv->softc_size = sizeof(struct pci_dev);

    lkpi_slot_count++;
    return register_and_probe(drv);
}

/* --- request_irq/free_irq (linux/interrupt.h) --------------------------
 *
 * irq_register (irq.h) takes one opaque ctx pointer per line and calls
 * handler(ctx) - that ctx slot IS the closure C otherwise lacks, so unlike
 * probe/attach above this needs only one trampoline, not one per driver:
 * each call to linux_request_irq hands irq_register a pointer to this
 * line's own table entry, and the entry already knows which Linux handler
 * and dev_id it means. */
struct lkpi_irq_entry {
    irq_handler_t handler;
    void         *dev_id;
    unsigned int  irq;
    int           in_use;
};

/* A POOL, not one entry per line. It was indexed by IRQ number, which made
 * the table itself the thing that enforced one Linux driver per line - so
 * IRQF_SHARED could not be honoured no matter what irq.c did underneath.
 * Now that irq.c chains handlers, this is the other half of the same fix. */
static struct lkpi_irq_entry lkpi_irq_table[16];

static int lkpi_irq_trampoline(void *ctx) {
    struct lkpi_irq_entry *e = (struct lkpi_irq_entry *)ctx;

    /* The return value is passed through now instead of being discarded.
     * IRQ_HANDLED is 1 and IRQ_NONE is 0, which is exactly irq.h's
     * convention - deliberately, so this is a pass-through and not a
     * translation that could invert. */
    return e->handler((int)e->irq, e->dev_id);
}

int linux_request_irq(unsigned int irq, irq_handler_t handler,
                       unsigned long flags, const char *name, void *dev_id) {
    int i, slot = -1;

    (void)flags; /* IRQF_SHARED is now the behaviour whether asked for or
                  * not: irq.c chains unconditionally, and a driver that did
                  * not ask to share is not harmed by another one being on
                  * the line - it just has to answer IRQ_NONE honestly. */
    (void)name;

    if (irq >= 16 || handler == NULL) {
        return -1;
    }
    for (i = 0; i < 16; i++) {
        if (!lkpi_irq_table[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    lkpi_irq_table[slot].handler = handler;
    lkpi_irq_table[slot].dev_id  = dev_id;
    lkpi_irq_table[slot].irq     = irq;
    lkpi_irq_table[slot].in_use  = 1;

    if (irq_register((uint8)irq, lkpi_irq_trampoline,
                     &lkpi_irq_table[slot]) != 0) {
        lkpi_irq_table[slot].in_use = 0;
        return -1;
    }
    return 0;
}

void linux_free_irq(unsigned int irq, void *dev_id) {
    int i;

    /* Matched on dev_id, which is what Linux's own free_irq uses to pick a
     * registration off a shared line - and the reason its signature takes a
     * dev_id at all rather than just an irq. This used to ignore it and
     * unregister the whole line. */
    for (i = 0; i < 16; i++) {
        if (lkpi_irq_table[i].in_use &&
            lkpi_irq_table[i].irq == irq &&
            lkpi_irq_table[i].dev_id == dev_id) {
            irq_unregister_handler((uint8)irq, lkpi_irq_trampoline,
                                   &lkpi_irq_table[i]);
            lkpi_irq_table[i].in_use = 0;
            return;
        }
    }
}
