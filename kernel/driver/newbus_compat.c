#include "bus.h"
#include "newbus_compat.h"
#include "pci.h"
#include "typesk.h"
#include "io.h"
#include "hints.h"
#include "irq.h"
#include "kheap.h"
#include "kprintf.h"
#include "ioapic.h"
#include "ksmp.h"
#include <stdarg.h>

/* The FreeBSD Newbus source-compat adapter - see kernel/include/
 * newbus_compat.h and kernel/include/sys/bus.h for the design (device_t
 * IS bus_dev_t, so DEVMETHOD-extracted probe/attach/detach function
 * pointers need no runtime trampoline, unlike lkpi.c/wdm.c's slot-based
 * adapters - the whole reason this file is small). Real driver source
 * never includes this file or kernel/include/bus.h directly; it only
 * ever sees kernel/include/sys/bus.h's method-table-shaped driver_t. */

#define NEWBUS_COMPAT_MAX_DRIVERS 8

/* Real, distinct addresses - only their IDENTITY matters, never their
 * contents (see newbus_compat.h). */
const int device_probe_desc;
const int device_attach_desc;
const int device_detach_desc;
const int device_suspend_desc;
const int device_resume_desc;
const int device_shutdown_desc;

static driver_t newbus_driver_pool[NEWBUS_COMPAT_MAX_DRIVERS];
static int      newbus_driver_count;

/* Add `drv` to the "pci" devclass and, if the boot-time attach pass has
 * already run, probe it against the devices that pass left unclaimed.
 *
 * Both compat layers do exactly this, so it is written once. The reason it
 * is not just devclass_add_driver is that a driver arriving in a loadable
 * module registers long after bus_attach_children - see bus_driver_added in
 * kernel/driver/bus.c, which is upstream's BUS_DRIVER_ADDED and which is
 * what makes a module's driver actually reach hardware rather than silently
 * matching nothing. */
static int register_and_probe(const char *busname, driver_t *drv) {
    devclass_t dc = devclass_find(busname);
    int rc = devclass_add_driver(dc, drv);

    if (rc == 0) {
        bus_driver_added(dc);
    }
    return rc;
}

int newbus_register_driver(struct freebsd_driver *drv) {
    return newbus_register_driver_on("pci", drv);
}

/* DRIVER_MODULE(name, busname, ...) names the bus, and until now that
 * argument was DISCARDED - every driver went onto the "pci" devclass
 * whatever it said. Harmless while every driver here was a PCI driver, and
 * immediately wrong on the first real one: if_rl.c has DRIVER_MODULE(rl,
 * pci, ...), DRIVER_MODULE(rl, cardbus, ...) and DRIVER_MODULE(miibus, rl,
 * ...), so the same driver was added to the PCI devclass twice and the
 * miibus driver was added to it as a third PCI driver. Three slots consumed
 * to register one device driver, and two of them wrong. */
int newbus_register_driver_on(const char *busname, struct freebsd_driver *drv) {
    device_method_t *methods = (device_method_t *)drv->methods;
    int (*probe_fn)(bus_dev_t *)  = NULL;
    int (*attach_fn)(bus_dev_t *) = NULL;
    void (*detach_fn)(bus_dev_t *) = NULL;
    driver_t *out;
    int i;

    /* probe/attach/detach are still pulled out by identity, because bus.c
     * calls those three through real function-pointer fields on every
     * device and a table walk per call would be pure cost.
     *
     * What changed is what happens to the REST. This loop used to discard
     * every other entry - device_suspend, device_resume, device_shutdown
     * and any custom KOBJ interface a real FreeBSD driver declares
     * (virtio_bus_*, bus_read_ivar, ...) were silently unreachable, which
     * is the KOBJ gap item 11 names. The whole array is now kept on the
     * driver_t (see below) and newbus_method_get dispatches any of them. */
    for (i = 0; methods[i].desc != NULL; i++) {
        if (methods[i].desc == &device_probe_desc) {
            probe_fn = (int (*)(bus_dev_t *))methods[i].func;
        } else if (methods[i].desc == &device_attach_desc) {
            attach_fn = (int (*)(bus_dev_t *))methods[i].func;
        } else if (methods[i].desc == &device_detach_desc) {
            detach_fn = (void (*)(bus_dev_t *))methods[i].func;
        }
    }

    if (newbus_driver_count >= NEWBUS_COMPAT_MAX_DRIVERS) {
        return -1;
    }
    out = &newbus_driver_pool[newbus_driver_count++];
    out->name       = drv->name;
    out->probe      = probe_fn;
    out->attach     = attach_fn;
    out->detach     = detach_fn;
    out->softc_size = (uint32)drv->size;
    out->pass       = drv->pass;
    out->methods    = drv->methods;

    return register_and_probe(busname, out);
}

void *newbus_method_get(bus_dev_t *dev, const void *desc) {
    driver_t *drv = bus_get_driver(dev);

    return drv == NULL ? NULL : newbus_driver_method_get(drv, desc);
}

void *newbus_driver_method_get(driver_t *drv, const void *desc) {
    const device_method_t *methods;
    int i;

    if (drv == NULL || desc == NULL) {
        return NULL;
    }

    /* NULL for a Genesis-native driver_t, which has no method table at all.
     * Returning NULL rather than asserting is the right answer: "this
     * driver does not implement that interface" is a normal result, and it
     * is the same one a KOBJ lookup gives for an unimplemented method. */
    methods = (const device_method_t *)drv->methods;
    if (methods == NULL) {
        return NULL;
    }

    /* Identity comparison on the desc token, which is what real KOBJ does
     * underneath its codegen - the generated descriptor structs are matched
     * by address too, not by name. */
    for (i = 0; methods[i].desc != NULL; i++) {
        if (methods[i].desc == desc) {
            return methods[i].func;
        }
    }
    return NULL;
}

/* device_get_parent (sys/bus.h) - another pass-through, for the same reason
 * device_get_softc is one: device_t and bus_dev_t are the same pointer. */
bus_dev_t *device_get_parent(bus_dev_t *dev) {
    return bus_get_parent(dev);
}

/* Upstream returns the DEVCLASS name here, which for a device somebody named
 * explicitly is that name, and for one the bus merely enumerated is the name
 * of whatever driver attached. Part 15 made the first of those possible, so
 * this now answers it in that order rather than only ever the driver's. */
const char *device_get_name(bus_dev_t *dev) {
    const char *explicit_name = bus_get_devname(dev);
    driver_t *drv;

    if (explicit_name != NULL) {
        return explicit_name;
    }
    drv = bus_get_driver(dev);
    return drv == NULL ? "" : drv->name;
}

/* "foo0" is two pieces here rather than one string, because Genesis has no
 * per-device buffer to compose it into and every caller wants the parts. */
int device_get_unit(bus_dev_t *dev) {
    return bus_get_devunit(dev);
}

/* rman_get_* (sys/rman.h) - the names real driver source reads a resource
 * back through. bus_alloc_resource/bus_alloc_resource_any/
 * bus_release_resource themselves need no wrapper: sys/bus.h declares the
 * same three symbols bus.c already defines, over the same struct resource
 * (see bus.h's typedef comment for why the tag matters). Only the accessor
 * NAMES differ, so only they land here. */
unsigned long long rman_get_start(bus_resource_t *r) {
    return bus_get_resource_start(r);
}

unsigned long long rman_get_size(bus_resource_t *r) {
    return bus_get_resource_size(r);
}

unsigned long long rman_get_end(bus_resource_t *r) {
    return bus_get_resource_end(r);
}

int rman_get_rid(bus_resource_t *r) {
    return bus_get_resource_rid(r);
}

/* device_get_softc/device_get_ivars (sys/bus.h) - device_t and bus_dev_t
 * are the same pointer type, so these are pass-through renames, not
 * translations. */
/* Create the child device a miibus handle needs. See kernel/bsd/miibus.c:
 * the MAC device cannot double as the miibus handle because device_get_softc
 * has to answer differently for each.
 *
 * bus_add_child with a NULL devclass: nothing will ever probe this child - it
 * exists only to be a distinct identity with its own softc slot. */
bus_dev_t *newbus_add_miibus_child(bus_dev_t *parent) {
    return bus_add_child(parent, devclass_find("miibus"), NULL);
}

/* The mii_data a miibus handle stands for, if this device is one. See
 * kernel/bsd/miibus.c for why this hook exists - Genesis has no child-device
 * creation, so a MAC driver's sc->rl_miibus is the MAC device itself and
 * device_get_softc on it would otherwise hand back the MAC's own softc. */
struct mii_data;
struct mii_data *mii_softc_for(bus_dev_t *dev);

void *device_get_softc(bus_dev_t *dev) {
    struct mii_data *mii;

    if (dev == NULL) {
        /* A driver asking for the softc of a device it never got. Upstream
         * this faults; reporting it names the caller instead. */
        return NULL;
    }
    mii = mii_softc_for(dev);
    if (mii != NULL) {
        return mii;
    }
    return bus_get_softc(dev);
}

void *device_get_ivars(bus_dev_t *dev) {
    return bus_get_ivars(dev);
}

/* pci_get_vendor/pci_get_device (dev/pci/pcivar.h) - deliberately
 * implemented here rather than as inline functions in that header: the
 * header must stay includable alongside <sys/bus.h> in real driver
 * source without pulling in Genesis's own pci.h (which includes bus.h
 * and would reintroduce the driver_t collision sys/bus.h's comment
 * explains). This file already has pci.h for pci_ivars_t. */
unsigned short pci_get_vendor(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    return f->vendor_id;
}

unsigned short pci_get_device(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    return f->device_id;
}

/* --- the rest of the PCI accessors ---------------------------------------
 *
 * Everything below is declared in dev/pci/pcivar.h and sys/bus.h and comes
 * straight off the pci_ivars_t the bus filled in at enumeration, or through
 * pci_cfg_* for the registers that struct does not carry.
 */

unsigned short pci_get_subvendor(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    return (unsigned short)pci_cfg_read16(f->bus, f->slot, f->func, 0x2C);
}

unsigned short pci_get_subdevice(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    return (unsigned short)pci_cfg_read16(f->bus, f->slot, f->func, 0x2E);
}

unsigned char pci_get_revid(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    return f->revision_id;
}

unsigned char pci_get_class(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    return f->class_code;
}

unsigned char pci_get_subclass(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    return f->subclass;
}

unsigned char pci_get_progif(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    return f->prog_if;
}

/* DEVICE in the high half, VENDOR in the low. Upstream's order, and the
 * opposite of how the pair is normally written - which is why a driver
 * copying an ID table out of FreeBSD source must use this rather than
 * composing the value by hand. */
unsigned int pci_get_devid(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    return ((unsigned int)f->device_id << 16) | f->vendor_id;
}

unsigned int pci_read_config(bus_dev_t *dev, int reg, int width) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);

    /* `width` is BYTES. A driver passing bits gets a refusal rather than a
     * plausible wrong answer. */
    switch (width) {
        case 1: return pci_cfg_read8(f->bus, f->slot, f->func, (uint8)reg);
        case 2: return pci_cfg_read16(f->bus, f->slot, f->func, (uint8)reg);
        case 4: return pci_cfg_read32(f->bus, f->slot, f->func, (uint8)reg);
        default: return 0;
    }
}

void pci_write_config(bus_dev_t *dev, int reg, unsigned int val, int width) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    uint8  aligned = (uint8)(reg & ~3);
    uint32 shift   = (uint32)((reg & 3) * 8);
    uint32 cur;

    /* Read-modify-write for the sub-dword widths: config space is dword
     * addressed in hardware, and writing the whole dword with the other
     * bytes zeroed clears whatever shared that register - which for
     * PCIR_COMMAND means disabling the device being configured. */
    switch (width) {
        case 4:
            pci_cfg_write32(f->bus, f->slot, f->func, (uint8)reg, val);
            return;
        case 2:
            cur = pci_cfg_read32(f->bus, f->slot, f->func, aligned);
            cur &= ~(0xFFFFu << shift);
            cur |= (val & 0xFFFFu) << shift;
            pci_cfg_write32(f->bus, f->slot, f->func, aligned, cur);
            return;
        case 1:
            cur = pci_cfg_read32(f->bus, f->slot, f->func, aligned);
            cur &= ~(0xFFu << shift);
            cur |= (val & 0xFFu) << shift;
            pci_cfg_write32(f->bus, f->slot, f->func, aligned, cur);
            return;
        default:
            return;
    }
}

void pci_enable_busmaster(bus_dev_t *dev) {
    unsigned int cmd = pci_read_config(dev, 0x04, 2);
    pci_write_config(dev, 0x04, cmd | 0x4u, 2);
}

void pci_disable_busmaster(bus_dev_t *dev) {
    unsigned int cmd = pci_read_config(dev, 0x04, 2);
    pci_write_config(dev, 0x04, cmd & ~0x4u, 2);
}

void pci_enable_io(bus_dev_t *dev, int space) {
    unsigned int cmd = pci_read_config(dev, 0x04, 2);

    /* SYS_RES_IOPORT is 4 and SYS_RES_MEMORY is 3 (bus.h). Mapped to the
     * command register's decode-enable bits rather than assumed equal. */
    if (space == 4) {
        cmd |= 0x1u;
    } else if (space == 3) {
        cmd |= 0x2u;
    }
    pci_write_config(dev, 0x04, cmd, 2);
}

/* --- register access ------------------------------------------------------
 *
 * bus_read_N / bus_write_N dispatch on what the resource actually IS. That
 * dispatch is the whole value of the abstraction: the same driver source
 * works against a device whose registers appear as memory on one board and
 * as I/O ports on another, which is common on exactly the kind of older PCI
 * hardware this kernel runs against.
 *
 * A memory resource is mapped on first use and the mapping is cached on the
 * resource, because a driver reads a register in a loop and ioremapping per
 * access would be absurd. An I/O resource needs no mapping at all - the port
 * number IS the address.
 */

/* Declared here rather than by including linux/io.h: that header spells
 * outb's arguments in the opposite order from Genesis's io.h, and having
 * both visible in one file is how a port number ends up written into a
 * device register. Only ioremap is wanted, and it has no such twin. */
void *ioremap(unsigned long phys_addr, unsigned long size);

/* The compat-side types and constants, declared HERE rather than by
 * including sys/bus.h.
 *
 * That is not an oversight - sys/bus.h's own header comment says the two
 * headers are never included together, because sys/bus.h defines a driver_t
 * with a method-table shape and bus.h defines one with probe/attach/detach
 * fields. This file is the adapter and needs bus.h.
 *
 * So these must be kept IDENTICAL to sys/bus.h's by hand. They are
 * declarations of the same symbols, and the compiler cannot check that
 * across two headers that never meet. Any change to one is a change to
 * both. */
typedef int  driver_filter_t(void *);
typedef void driver_intr_t(void *);

#define M_ZERO_COMPAT   0x0100
#define SYS_RES_MEMORY_COMPAT  3
const char *device_get_nameunit(bus_dev_t *dev);

static void *res_mapping(bus_resource_t *r) {
    void *va = bus_get_resource_mapping(r);

    if (va == NULL) {
        va = ioremap((unsigned long)bus_get_resource_start(r),
                     (unsigned long)bus_get_resource_size(r));
        bus_set_resource_mapping(r, va);
    }
    return va;
}

static int res_is_memory(bus_resource_t *r) {
    return bus_get_resource_type(r) == SYS_RES_MEMORY_COMPAT;
}

unsigned char bus_read_1(bus_resource_t *r, unsigned long off) {
    if (res_is_memory(r)) {
        volatile unsigned char *p = (volatile unsigned char *)res_mapping(r);
        return p != NULL ? p[off] : 0xFF;
    }
    return inb((uint16)(bus_get_resource_start(r) + off));
}

unsigned short bus_read_2(bus_resource_t *r, unsigned long off) {
    if (res_is_memory(r)) {
        volatile unsigned char *p = (volatile unsigned char *)res_mapping(r);
        return p != NULL ? *(volatile unsigned short *)(p + off) : 0xFFFF;
    }
    return inw((uint16)(bus_get_resource_start(r) + off));
}

unsigned int bus_read_4(bus_resource_t *r, unsigned long off) {
    if (res_is_memory(r)) {
        volatile unsigned char *p = (volatile unsigned char *)res_mapping(r);
        return p != NULL ? *(volatile unsigned int *)(p + off) : 0xFFFFFFFFu;
    }
    return inl((uint16)(bus_get_resource_start(r) + off));
}

void bus_write_1(bus_resource_t *r, unsigned long off, unsigned char v) {
    if (res_is_memory(r)) {
        volatile unsigned char *p = (volatile unsigned char *)res_mapping(r);
        if (p != NULL) {
            p[off] = v;
        }
        return;
    }
    outb((uint16)(bus_get_resource_start(r) + off), v);
}

void bus_write_2(bus_resource_t *r, unsigned long off, unsigned short v) {
    if (res_is_memory(r)) {
        volatile unsigned char *p = (volatile unsigned char *)res_mapping(r);
        if (p != NULL) {
            *(volatile unsigned short *)(p + off) = v;
        }
        return;
    }
    outw((uint16)(bus_get_resource_start(r) + off), v);
}

void bus_write_4(bus_resource_t *r, unsigned long off, unsigned int v) {
    if (res_is_memory(r)) {
        volatile unsigned char *p = (volatile unsigned char *)res_mapping(r);
        if (p != NULL) {
            *(volatile unsigned int *)(p + off) = v;
        }
        return;
    }
    outl((uint16)(bus_get_resource_start(r) + off), v);
}

/* --- interrupts -----------------------------------------------------------
 *
 * Upstream splits a handler into a `filter` that runs at interrupt level and
 * decides whether the interrupt was ours, and a `handler` that runs later in
 * an interrupt thread. Genesis has no interrupt threads, so both run at
 * interrupt level here.
 *
 * That is STRICTER than the driver asked for, not looser - a filter is
 * already required to be interrupt-safe. What a driver may not do here that
 * it could upstream is sleep in its handler.
 */

#define NEWBUS_INTR_MAX 8

struct newbus_intr {
    driver_filter_t *filter;
    driver_intr_t   *handler;
    void            *arg;
    uint8            irq;
    int              in_use;
    int              bound_cpu;      /* -1: wherever boot routed it */
    char             descr[24];
};

static struct newbus_intr newbus_intrs[NEWBUS_INTR_MAX];

static int newbus_intr_trampoline(void *ctx) {
    struct newbus_intr *e = (struct newbus_intr *)ctx;

    /* The filter's return value decides whether the line was ours, which is
     * what irq.c's shared-line dispatch needs to count the interrupt as
     * claimed. FILTER_HANDLED is 2 upstream and FILTER_STRAY is 1; anything
     * non-stray means we take it. */
    if (e->filter != NULL) {
        int rc = e->filter(e->arg);

        if (rc == 1) {
            return 0;               /* FILTER_STRAY - not ours */
        }
    }
    if (e->handler != NULL) {
        e->handler(e->arg);
    }
    return 1;
}

int bus_setup_intr(bus_dev_t *dev, bus_resource_t *irq, int flags,
                   driver_filter_t *filter, driver_intr_t *handler,
                   void *arg, void **cookiep) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    struct newbus_intr *e = NULL;
    uint8 line;
    int i;

    (void)flags;
    if (filter == NULL && handler == NULL) {
        return -1;
    }
    for (i = 0; i < NEWBUS_INTR_MAX; i++) {
        if (!newbus_intrs[i].in_use) {
            e = &newbus_intrs[i];
            break;
        }
    }
    if (e == NULL) {
        return -1;
    }

    /* The IRQ resource if the driver allocated one, otherwise the line the
     * bus recorded at enumeration. Both are legitimate - upstream drivers
     * commonly allocate SYS_RES_IRQ first, but not all do. */
    line = (irq != NULL) ? (uint8)bus_get_resource_start(irq)
                         : (f != NULL ? f->int_line : 0);

    e->filter  = filter;
    e->handler = handler;
    e->arg     = arg;
    e->irq     = line;
    e->in_use  = 1;
    e->bound_cpu = -1;
    e->descr[0]  = '\0';

    if (irq_register(line, newbus_intr_trampoline, e) != 0) {
        e->in_use = 0;
        return -1;
    }
    if (cookiep != NULL) {
        *cookiep = e;
    }
    return 0;
}

int bus_bind_intr(bus_dev_t *dev, bus_resource_t *irq, int cpu) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    struct cpu_local *c = smp_cpu(cpu);
    uint8 line;
    int i;

    if (c == NULL || !c->online) {
        return 22;                                    /* EINVAL */
    }
    line = (irq != NULL) ? (uint8)bus_get_resource_start(irq)
                         : (f != NULL ? f->int_line : 0);
    if (ioapic_bind_irq(line, c->apic_id) != 0) {
        return 45;                                    /* EOPNOTSUPP */
    }
    for (i = 0; i < NEWBUS_INTR_MAX; i++) {
        if (newbus_intrs[i].in_use && newbus_intrs[i].irq == line) {
            newbus_intrs[i].bound_cpu = cpu;
        }
    }
    return 0;
}

int bus_describe_intr(bus_dev_t *dev, bus_resource_t *irq, void *cookie,
                      const char *fmt, ...) {
    struct newbus_intr *e = (struct newbus_intr *)cookie;
    int i;

    (void)dev; (void)irq;
    if (e == NULL || fmt == NULL) {
        return 22;
    }
    /* The format is taken literally: drivers describe with a plain name
     * ("rx", "tx0") far more often than with conversions. */
    for (i = 0; i < (int)sizeof(e->descr) - 1 && fmt[i] != '\0'; i++) {
        e->descr[i] = fmt[i];
    }
    e->descr[i] = '\0';
    return 0;
}

int bus_teardown_intr(bus_dev_t *dev, bus_resource_t *irq, void *cookie) {
    struct newbus_intr *e = (struct newbus_intr *)cookie;

    (void)dev;
    (void)irq;
    if (e == NULL || !e->in_use) {
        return -1;
    }
    /* The targeted form, not irq_unregister - the line may be shared, and
     * unregistering it wholesale would silence every other device on it.
     * That is the specific bug shared IRQs make easy, and irq.c grew
     * irq_unregister_handler for exactly this. */
    irq_unregister_handler(e->irq, newbus_intr_trampoline, e);
    e->in_use = 0;
    return 0;
}

/* --- the rest -------------------------------------------------------------- */

void device_printf(bus_dev_t *dev, const char *fmt, ...) {
    va_list ap;

    /* The name-and-unit prefix is the whole reason a driver calls this
     * instead of printf: "em0: link up" rather than an unattributed line. */
    kprintf_c(0x0F, "%s: ", device_get_nameunit(dev));
    va_start(ap, fmt);
    kvprintf(0x0F, fmt, ap);
    va_end(ap);
}

/* One description per device, in a small side table rather than on
 * bus_dev_t - this is FreeBSD-compat state and bus.c's own device model has
 * no notion of it, so it does not belong in the shared struct. */
#define NEWBUS_DESC_MAX 16

static struct {
    bus_dev_t  *dev;
    const char *desc;
} newbus_descs[NEWBUS_DESC_MAX];

void device_set_desc(bus_dev_t *dev, const char *desc) {
    int i, free_slot = -1;

    for (i = 0; i < NEWBUS_DESC_MAX; i++) {
        if (newbus_descs[i].dev == dev) {
            newbus_descs[i].desc = desc;
            return;
        }
        if (newbus_descs[i].dev == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        newbus_descs[free_slot].dev  = dev;
        newbus_descs[free_slot].desc = desc;
    }
}

const char *device_get_desc(bus_dev_t *dev) {
    int i;

    for (i = 0; i < NEWBUS_DESC_MAX; i++) {
        if (newbus_descs[i].dev == dev) {
            return newbus_descs[i].desc;
        }
    }
    return "";
}

/* device_get_unit already exists earlier in this file. */

const char *device_get_nameunit(bus_dev_t *dev) {
    const char *n = bus_get_devname(dev);
    driver_t *drv;

    /* The explicitly-set name first, then the attached DRIVER's name.
     *
     * Falling back to a literal "device" was the first version and it made
     * every device_printf line read "device: ..." - which defeats the whole
     * reason a driver calls device_printf instead of printf. Most devices
     * here are enumerated rather than named (see bus.c's hasclass note), so
     * that fallback was the common case, not the rare one. */
    if (n != NULL && n[0] != '\0') {
        return n;
    }
    drv = bus_get_driver(dev);
    if (drv != NULL && drv->name != NULL && drv->name[0] != '\0') {
        return drv->name;
    }
    return "device";
}

/* malloc(9)/free(9) are NOT defined here.
 *
 * kernel/bsd/mbuf.c already defines them, with exactly FreeBSD's signature,
 * because the vendored mbuf and UMA code needed them first. A second
 * definition is a duplicate symbol at link time - which is how this was
 * found. sys/bus.h declares them so driver source compiles; this comment
 * exists so the next reader does not add them back. */

/* --- the last few a real vendored driver reaches for --------------------- */

int device_is_attached(bus_dev_t *dev) {
    return bus_get_driver(dev) != NULL;
}

/* No child devices are created by this compat layer - mii_attach associates
 * a mii_data with the parent directly rather than adding a child (see
 * kernel/bsd/miibus.c) - so there is nothing to detach and this succeeds.
 * Success rather than an error: a driver calls it on its own detach path
 * and an error would abort a teardown that is otherwise fine. */
int bus_generic_detach(bus_dev_t *dev) {
    (void)dev;
    return 0;
}

int bus_generic_attach(bus_dev_t *dev) {
    (void)dev;
    return 0;
}

bus_dev_t *device_add_child(bus_dev_t *dev, const char *name, int unit) {
    (void)dev; (void)name; (void)unit;
    /* NULL, and a caller that needs a real child device will notice. Adding
     * one means giving it its own ivars and a place in the bus topology,
     * which bus_add_child can do - but nothing that reaches here today wants
     * a child that is enumerated rather than synthesised. */
    return NULL;
}

int device_delete_child(bus_dev_t *dev, bus_dev_t *child) {
    (void)dev; (void)child;
    return 0;
}

/* --- PCI power management ------------------------------------------------
 *
 * Capability ID 0x01 is PCI-PM. pci_find_capability already walks the
 * capability list (it is how MSI is found), so this is a lookup rather than
 * new machinery. */
#define PCIY_PMG 0x01

int pci_has_pm(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);

    return f != NULL && pci_find_capability(f, PCIY_PMG) != 0;
}

int pci_get_powerstate(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    uint8 cap;

    if (f == NULL) {
        return 0;   /* PCI_POWERSTATE_D0 */
    }
    cap = pci_find_capability(f, PCIY_PMG);
    if (cap == 0) {
        /* A device with no power-management capability is always D0 - it has
         * no other state to be in. Reporting "unknown" would make a driver
         * think it had to do something about it. */
        return 0;
    }
    /* The power state is the low two bits of the PMCSR, at capability
     * offset 4. */
    return pci_cfg_read16(f->bus, f->slot, f->func, (uint8)(cap + 4)) & 0x3;
}

int pci_set_powerstate(bus_dev_t *dev, int state) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    uint8 cap;
    uint16 pmcsr;

    if (f == NULL) {
        return -1;
    }
    cap = pci_find_capability(f, PCIY_PMG);
    if (cap == 0) {
        /* Only D0 is reachable without the capability, and asking for it is
         * trivially satisfied. Asking for anything else cannot be done. */
        return state == 0 ? 0 : -1;
    }
    pmcsr = pci_cfg_read16(f->bus, f->slot, f->func, (uint8)(cap + 4));
    pmcsr = (uint16)((pmcsr & ~0x3u) | ((uint32)state & 0x3u));
    pci_write_config(dev, cap + 4, pmcsr, 2);
    return 0;
}

void pci_enable_pme(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    uint8 cap;
    uint16 pmcsr;

    if (f == NULL) {
        return;
    }
    cap = pci_find_capability(f, PCIY_PMG);
    if (cap == 0) {
        return;
    }
    /* PME_EN is bit 8 of the PMCSR. Programmed even though nothing here ever
     * suspends: a driver that enabled Wake-on-LAN and found the bit clear
     * would have no way to tell that the request was dropped. */
    pmcsr = pci_cfg_read16(f->bus, f->slot, f->func, (uint8)(cap + 4));
    pci_write_config(dev, cap + 4, (uint16)(pmcsr | 0x100), 2);
}

/* --- rman's bus_space view of a resource ---------------------------------
 *
 * Forward-declared because rman_get_bushandle calls it, and ioremap comes
 * from kernel/driver/lkpi_kernel.c - declared by hand here for the reason
 * given at res_mapping above (linux/io.h's outb has the opposite argument
 * order from Genesis's and must not be in scope in this file). */
void *rman_get_virtual(bus_resource_t *r);

int rman_get_bustag(bus_resource_t *r) {
    /* 1 is BUS_SPACE_TAG_MEM and 0 is BUS_SPACE_TAG_IO (machine/bus.h).
     * Spelled numerically because that header is on the vendored-driver
     * include path and this file is on Genesis's - they never meet. */
    return bus_get_resource_type(r) == SYS_RES_MEMORY ? 1 : 0;
}

uintptr rman_get_bushandle(bus_resource_t *r) {
    /* For MEMORY the handle must be a MAPPED kernel virtual address, not the
     * physical one - bus_space_read_4 dereferences it directly. For PORTS it
     * is the port number, which needs no mapping. Getting this backwards
     * gives a driver a pointer to whatever happens to live at that physical
     * address in the direct map. */
    if (bus_get_resource_type(r) == SYS_RES_MEMORY) {
        return (uintptr)rman_get_virtual(r);
    }
    return (uintptr)bus_get_resource_start(r);
}

void *rman_get_virtual(bus_resource_t *r) {
    void *va = bus_get_resource_mapping(r);

    if (va == NULL && bus_get_resource_type(r) == SYS_RES_MEMORY) {
        va = ioremap((unsigned long)bus_get_resource_start(r),
                     (unsigned long)bus_get_resource_size(r));
        bus_set_resource_mapping(r, va);
    }
    return va;
}

/* --- MSI/MSI-X counts, capability lookup, and tunables -------------------
 *
 * The errno values are spelled out here rather than by including an errno
 * header: this file is on GENESIS's include path, not the vendored-driver
 * one, so kernel/bsd/compat/sys/errno.h is not reachable from it. They must
 * match that file's values exactly - a driver compares what it gets back
 * against the constant IT saw. */
#define NB_ENOENT  2
#define NB_ENXIO   6
#define ENOENT     NB_ENOENT
#define ENXIO      NB_ENXIO

int pci_msi_count(bus_dev_t *dev) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);

    /* One. Genesis allocates a single vector per function (pci_msi_alloc),
     * so reporting the device's advertised count would promise more than can
     * be delivered and a driver would size a vector array for it. */
    return (f != NULL && f->msi_cap != 0) ? 1 : 0;
}

int pci_msix_count(bus_dev_t *dev) {
    (void)dev;
    /* Zero - there is no MSI-X support here, and its vector table lives in a
     * BAR rather than config space so it is real work rather than a lookup.
     * Zero is also what a device WITHOUT the capability reports, which every
     * driver already handles by falling back. */
    return 0;
}

int pci_alloc_msi(bus_dev_t *dev, int *count) {
    if (pci_msi_count(dev) == 0) {
        return ENXIO;
    }
    /* IN/OUT: the driver asked for *count and is told what it got. Writing
     * back 1 rather than leaving the request untouched is the whole contract
     * - a driver that asked for 4 and is not corrected will index four
     * vectors it does not have. */
    *count = 1;
    return 0;
}

int pci_alloc_msix(bus_dev_t *dev, int *count) {
    (void)dev; (void)count;
    return ENXIO;
}

int pci_release_msi(bus_dev_t *dev) {
    pci_ivars_t *f = (pci_ivars_t *)bus_get_ivars(dev);

    if (f != NULL && f->msi_vector != 0) {
        pci_msi_release(f);
    }
    return 0;
}

int pci_find_cap(bus_dev_t *dev, int capability, int *capreg) {
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    uint8 off;

    if (f == NULL) {
        return ENXIO;
    }
    off = pci_find_capability(f, (uint8)capability);
    if (off == 0) {
        return ENOENT;
    }
    if (capreg != NULL) {
        *capreg = off;
    }
    return 0;
}

int pci_find_extcap(bus_dev_t *dev, int capability, int *capreg) {
    (void)dev; (void)capability; (void)capreg;
    /* PCIe EXTENDED capabilities live at config offset 0x100 and up, which
     * needs memory-mapped (ECAM) config access - Genesis uses the legacy
     * 0xCF8/0xCFC port pair, which cannot reach past 0xFF. Refused rather
     * than answered wrongly. */
    return ENXIO;
}

int pci_set_max_read_req(bus_dev_t *dev, int size) {
    int cap = 0;

    /* PCIe device control register, bits 14-12. A device with no PCIe
     * capability has no such register and is left at the 512-byte default,
     * which is reported rather than echoing back what was asked for. */
    if (pci_find_cap(dev, 0x10, &cap) != 0) {
        return 512;
    }
    if (size < 128) {
        size = 128;
    }
    if (size > 4096) {
        size = 4096;
    }
    {
        /* Round DOWN to a power of two: the field encodes 128 << n, and
         * rounding up would ask the device for longer completions than the
         * caller was willing to accept. */
        int enc = 0, v = size >> 8;

        while (v > 0) {
            enc++;
            v >>= 1;
        }
        {
            uint16 ctl = (uint16)pci_read_config(dev, cap + 8, 2);

            ctl = (uint16)((ctl & ~0x7000u) | ((uint32)enc << 12));
            pci_write_config(dev, cap + 8, ctl, 2);
        }
    }
    return size;
}

/* resource_int_value - a driver's tunable lookup, over the REAL hint source
 * kernel/driver/hints.c already provides (a compiled-in device.hints in
 * upstream's exact format). Not a stub: a hint line genuinely reaches a
 * driver through this. */
int resource_int_value(const char *name, int unit, const char *resname,
                       int *result) {
    const char *v = hint_get(name, unit, resname);
    int val = 0, i = 0, neg = 0;

    if (v == NULL) {
        return ENOENT;      /* "no hint says otherwise" - use the default */
    }
    if (v[0] == '-') {
        neg = 1;
        i = 1;
    }
    for (; v[i] >= '0' && v[i] <= '9'; i++) {
        val = val * 10 + (v[i] - '0');
    }
    *result = neg ? -val : val;
    return 0;
}

int resource_string_value(const char *name, int unit, const char *resname,
                          const char **result) {
    const char *v = hint_get(name, unit, resname);

    if (v == NULL) {
        return ENOENT;
    }
    *result = v;
    return 0;
}

/* --- bus_get_domain -----------------------------------------------------
 *
 * Which NUMA memory domain a device's DMA is closest to. net/if.c's
 * if_alloc_dev() asks, so that an interface's softc can be allocated near the
 * hardware that will DMA into it.
 *
 * Genesis has one memory domain, and the honest answer to "which is closest"
 * on such a machine is "there is no preference" - which upstream spells as a
 * NON-ZERO return, leaving the caller to allocate from the general pool.
 * Filling in domain 0 and returning success would tell the caller it had been
 * given a real answer.
 */
int bus_get_domain(bus_dev_t *dev, int *domain) {
    (void)dev;
    (void)domain;
    return (ENOENT);
}
