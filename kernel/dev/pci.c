#include "bus.h"
#include "idt.h"
#include "io.h"
#include "kprintf.h"
#include "lapic.h"
#include "pci.h"
#include "screen.h"
#include "typesk.h"

/* See pci.h for the config-space mechanism and the bus-0-only scope. */

static pci_ivars_t pci_funcs[PCI_MAX_FUNCS];
static int         pci_func_count;

/* Anchor node for pci_add_child's parent - never itself probed (no
 * devclass, see bus_add_child's NULL-dc note in bus.h), it exists only so
 * every found function has a bus_dev_t parent to hang off. */
static bus_dev_t *root;

/* Defined at the bottom of this file; probe_function calls it during the
 * scan to record each function's MSI capability offset once. */
uint8 pci_find_capability(const pci_ivars_t *f, uint8 cap_id);

static uint32 cfg_address(uint8 bus, uint8 slot, uint8 func, uint8 reg) {
    return 0x80000000u
         | ((uint32)bus  << 16)
         | ((uint32)slot << 11)
         | ((uint32)func << 8)
         | ((uint32)reg  & 0xFCu);
}

uint32 pci_cfg_read32(uint8 bus, uint8 slot, uint8 func, uint8 reg) {
    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, reg));
    return inl(PCI_CONFIG_DATA);
}

uint16 pci_cfg_read16(uint8 bus, uint8 slot, uint8 func, uint8 reg) {
    uint32 v = pci_cfg_read32(bus, slot, func, (uint8)(reg & 0xFC));
    return (uint16)(v >> ((reg & 2) * 8));
}

uint8 pci_cfg_read8(uint8 bus, uint8 slot, uint8 func, uint8 reg) {
    uint32 v = pci_cfg_read32(bus, slot, func, (uint8)(reg & 0xFC));
    return (uint8)(v >> ((reg & 3) * 8));
}

void pci_cfg_write32(uint8 bus, uint8 slot, uint8 func, uint8 reg, uint32 value) {
    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, reg));
    outl(PCI_CONFIG_DATA, value);
}

/* Config space is only addressable a dword at a time through the 0xCF8/0xCFC
 * mechanism, so a 16-bit write is a read-modify-write of the containing
 * dword. Worth having as a helper rather than open-coded: the MSI message-
 * control register and the command register are both 16-bit, and getting the
 * shift wrong writes the neighbouring register instead - on the command
 * register that neighbour is the status register, whose bits are
 * write-1-to-clear. */
void pci_cfg_write16(uint8 bus, uint8 slot, uint8 func, uint8 reg,
                     uint16 value) {
    uint8  aligned = (uint8)(reg & 0xFC);
    uint32 shift   = (uint32)(reg & 2) * 8;
    uint32 dword   = pci_cfg_read32(bus, slot, func, aligned);

    dword &= ~(0xFFFFu << shift);
    dword |= ((uint32)value) << shift;
    pci_cfg_write32(bus, slot, func, aligned, dword);
}

static void probe_function(uint8 bus, uint8 slot, uint8 func) {
    pci_ivars_t *f;
    uint32 id, cls;
    int bar;

    id = pci_cfg_read32(bus, slot, func, 0x00);
    if ((id & 0xFFFF) == PCI_VENDOR_NONE) {
        return;
    }
    if (pci_func_count >= PCI_MAX_FUNCS) {
        return;
    }

    f = &pci_funcs[pci_func_count++];
    f->bus       = bus;
    f->slot      = slot;
    f->func      = func;
    f->vendor_id = (uint16)(id & 0xFFFF);
    f->device_id = (uint16)(id >> 16);

    cls = pci_cfg_read32(bus, slot, func, 0x08);
    f->revision_id = (uint8)(cls & 0xFF);
    f->prog_if     = (uint8)((cls >> 8) & 0xFF);
    f->subclass    = (uint8)((cls >> 16) & 0xFF);
    f->class_code  = (uint8)((cls >> 24) & 0xFF);

    f->hdr_type = pci_cfg_read8(bus, slot, func, 0x0E);
    f->int_line = pci_cfg_read8(bus, slot, func, 0x3C);
    f->int_pin  = pci_cfg_read8(bus, slot, func, 0x3D);

    for (bar = 0; bar < 6; bar++) {
        f->bar_raw[bar] = pci_cfg_read32(bus, slot, func, (uint8)(0x10 + bar * 4));
    }

    f->msi_cap    = pci_find_capability(f, PCI_CAP_ID_MSI);
    f->msi_vector = 0;

    bus_add_child(root, devclass_find("pci"), f);
}

/* Which buses have already been scanned, so a malformed or looping bridge
 * topology cannot recurse forever. One bit per bus number, and 256 bits is
 * 32 bytes - cheaper than a depth limit and it catches the real case
 * (two bridges claiming the same secondary bus) rather than only the
 * pathological one. */
static uint8 bus_seen[32];

static int bus_already_scanned(uint8 bus) {
    return (bus_seen[bus >> 3] >> (bus & 7)) & 1;
}

static void mark_bus_scanned(uint8 bus) {
    bus_seen[bus >> 3] |= (uint8)(1u << (bus & 7));
}

static void scan_bus(uint8 bus);

/* A type-1 header is a PCI-to-PCI bridge. Its secondary bus number - the bus
 * on the far side - is at config offset 0x19, and every device down there is
 * invisible to a scan of bus 0 alone.
 *
 * ROADMAP item 11 names this as deferred, and pci.c's own comment named the
 * register to read. QEMU's default i440fx machine exposes no P2P bridge, so
 * this recursion finds nothing there and is exercised with `-device
 * pci-bridge` - see src/verif.c. That is worth stating plainly: on the
 * default machine this code path is correct and untaken, which is a weaker
 * claim than "verified" and should not be dressed up as one.
 *
 * Note the type check is on the HEADER TYPE, not on the class code. A
 * device can report class 0x06 subclass 0x04 and still be a type-0 header
 * (some integrated bridges do), and it is the header layout - not the class
 * - that decides whether offset 0x19 means a secondary bus number or is part
 * of a BAR. Reading 0x19 off a type-0 header would recurse into a bus number
 * fabricated from BAR bits. */
static void scan_bridge(uint8 bus, uint8 slot, uint8 func, uint8 hdr_type) {
    uint8 secondary;

    if ((hdr_type & 0x7F) != 0x01) {
        return;
    }

    secondary = pci_cfg_read8(bus, slot, func, 0x19);
    if (secondary == 0 || secondary == bus) {
        /* Secondary 0 means the bridge has not been configured by firmware;
         * secondary == bus is a bridge pointing at itself. Neither is worth
         * recursing into and both would loop. */
        return;
    }
    scan_bus(secondary);
}

static void scan_bus(uint8 bus) {
    uint16 slot;

    if (bus_already_scanned(bus)) {
        return;
    }
    mark_bus_scanned(bus);

    for (slot = 0; slot < 32; slot++) {
        uint32 id0 = pci_cfg_read32(bus, (uint8)slot, 0, 0x00);
        uint8 hdr0;
        uint8 func;

        if ((id0 & 0xFFFF) == PCI_VENDOR_NONE) {
            continue;
        }

        probe_function(bus, (uint8)slot, 0);
        hdr0 = pci_cfg_read8(bus, (uint8)slot, 0, 0x0E);
        scan_bridge(bus, (uint8)slot, 0, hdr0);

        if (hdr0 & PCI_HDRTYPE_MULTIFUNC) {
            for (func = 1; func < 8; func++) {
                uint32 idf = pci_cfg_read32(bus, (uint8)slot, func, 0x00);
                uint8 hdrf;

                if ((idf & 0xFFFF) == PCI_VENDOR_NONE) {
                    continue;
                }
                probe_function(bus, (uint8)slot, func);
                hdrf = pci_cfg_read8(bus, (uint8)slot, func, 0x0E);
                scan_bridge(bus, (uint8)slot, func, hdrf);
            }
        }
    }
}

void pci_init(void) {
    int i;

    pci_func_count = 0;
    for (i = 0; i < (int)sizeof(bus_seen); i++) {
        bus_seen[i] = 0;
    }
    if (root == NULL) {
        root = bus_add_child(NULL, NULL, NULL);
    }

    scan_bus(0);
}

static const char *class_name(uint8 class_code) {
    switch (class_code) {
        case 0x00: return "unclassified";
        case 0x01: return "mass storage";
        case 0x02: return "network";
        case 0x03: return "display";
        case 0x04: return "multimedia";
        case 0x05: return "memory";
        case 0x06: return "bridge";
        case 0x07: return "communication";
        case 0x08: return "system peripheral";
        case 0x09: return "input";
        case 0x0C: return "serial bus";
        default:   return "other";
    }
}

void pci_report(uint8 color) {
    int i;

    print_string("PCI devices:\n", color);
    for (i = 0; i < pci_func_count; i++) {
        const pci_ivars_t *f = &pci_funcs[i];

        print_string("  ", color);
        print_hex(f->bus, color);
        print_string(":", color);
        print_hex(f->slot, color);
        print_string(".", color);
        print_hex(f->func, color);
        print_string("  ", color);
        print_hex(f->vendor_id, color);
        print_string(":", color);
        print_hex(f->device_id, color);
        print_string("  class ", color);
        print_hex(f->class_code, color);
        print_string(".", color);
        print_hex(f->subclass, color);
        print_string("  ", color);
        print_string(class_name(f->class_code), color);
        print_string("\n", color);
    }
}

int pci_bar_size(const pci_ivars_t *f, int bar, uint64 *base, uint64 *size,
                 int *is_mem) {
    uint8  reg;
    uint32 orig, probe;
    uint32 orig_hi = 0, probe_hi = 0;
    int    is_mem64;

    if (bar < 0 || bar > 5) {
        return -1;
    }
    /* The high dword of a 64-bit memory BAR is not independently
     * addressable - reject asking for it directly rather than silently
     * returning half an address (see pci.h). */
    if (bar > 0) {
        uint32 prev = f->bar_raw[bar - 1];
        if (!(prev & 0x1) && ((prev >> 1) & 0x3) == 0x2) {
            return -1;
        }
    }

    reg      = (uint8)(0x10 + bar * 4);
    orig     = f->bar_raw[bar];
    is_mem64 = !(orig & 0x1) && (((orig >> 1) & 0x3) == 0x2);

    pci_cfg_write32(f->bus, f->slot, f->func, reg, 0xFFFFFFFFu);
    probe = pci_cfg_read32(f->bus, f->slot, f->func, reg);
    pci_cfg_write32(f->bus, f->slot, f->func, reg, orig);

    if (is_mem64) {
        uint8 reg_hi = (uint8)(reg + 4);

        orig_hi = f->bar_raw[bar + 1];
        pci_cfg_write32(f->bus, f->slot, f->func, reg_hi, 0xFFFFFFFFu);
        probe_hi = pci_cfg_read32(f->bus, f->slot, f->func, reg_hi);
        pci_cfg_write32(f->bus, f->slot, f->func, reg_hi, orig_hi);
    }

    /* Size math stays in 32-bit arithmetic for a 32-bit BAR (I/O or
     * 32-bit memory) - there is no real high dword to fold in, and
     * widening a 32-bit ~x+1 into a 64-bit result without masking would
     * sign-extend the inverted high bits into garbage. Only the genuine
     * 64-bit-memory case combines both dwords before inverting. */
    if (orig & 0x1) {
        /* I/O space: bit 0 set, bit 1 reserved, size lives above bit 1. */
        uint32 masked = probe & 0xFFFFFFFCu;

        *is_mem = 0;
        *base   = orig & 0xFFFFFFFCu;
        *size   = (masked == 0) ? 0 : (uint64)(~masked + 1);
    } else if (!is_mem64) {
        uint32 masked = probe & 0xFFFFFFF0u;

        *is_mem = 1;
        *base   = orig & 0xFFFFFFF0u;
        *size   = (masked == 0) ? 0 : (uint64)(~masked + 1);
    } else {
        uint64 probe64 = ((uint64)probe_hi << 32) | (probe & 0xFFFFFFF0u);

        *is_mem = 1;
        *base   = ((uint64)orig_hi << 32) | (orig & 0xFFFFFFF0u);
        *size   = (probe64 == 0) ? 0 : (~probe64 + 1);
    }
    return 0;
}

bus_dev_t *pci_root(void) {
    return root;
}

/* --- capabilities and MSI ------------------------------------------------
 * See pci.h for the API and for why MSI-X is deliberately left out.
 */

#define PCI_STATUS_CAP_LIST  0x10   /* status register bit 4 */
#define PCI_COMMAND_INTX_DIS 0x400  /* command register bit 10 */

/* MSI capability layout, offsets from the capability's own base:
 *   +0x00  u8  capability ID (0x05)
 *   +0x01  u8  next-capability pointer
 *   +0x02  u16 message control
 *   +0x04  u32 message address (low)
 *   +0x08  u32 message address (high)   - 64-bit capable only
 *   +0x08  u16 message data             - 32-bit form
 *   +0x0C  u16 message data             - 64-bit form
 * The 64-bit form shifts everything after the address, which is why the
 * data offset below is computed from the control register rather than
 * assumed. Getting that wrong writes the vector into the address's high
 * dword, and the interrupt is then delivered to a physical address nobody
 * decodes - silently, with no fault. */
#define MSI_CTRL          0x02
#define MSI_ADDR_LO       0x04
#define MSI_ADDR_HI       0x08
#define MSI_CTRL_ENABLE   0x0001
#define MSI_CTRL_MMC_MASK 0x000E   /* bits 3:1, log2 of vectors requested */
#define MSI_CTRL_MME_MASK 0x0070   /* bits 6:4, log2 of vectors granted   */
#define MSI_CTRL_64BIT    0x0080
#define MSI_CTRL_MASKING  0x0100

uint8 pci_find_capability(const pci_ivars_t *f, uint8 cap_id) {
    uint16 status;
    uint8  ptr;
    int    guard;

    status = pci_cfg_read16(f->bus, f->slot, f->func, 0x06);
    if ((status & PCI_STATUS_CAP_LIST) == 0) {
        return 0;
    }

    /* A bridge (type 1) keeps its capability pointer at the same offset as a
     * type 0 header, so no special case is needed here - but a type 2
     * (CardBus) header does not, and nothing in this tree enumerates one. */
    ptr = pci_cfg_read8(f->bus, f->slot, f->func, 0x34) & 0xFC;

    /* Bounded walk. A malformed device whose next-pointer loops back on
     * itself would otherwise hang the boot, and 48 links is already more
     * than the config space can hold at four bytes each. */
    for (guard = 0; ptr != 0 && guard < 48; guard++) {
        uint8 id   = pci_cfg_read8(f->bus, f->slot, f->func, ptr);
        uint8 next = pci_cfg_read8(f->bus, f->slot, f->func, (uint8)(ptr + 1));

        if (id == cap_id) {
            return ptr;
        }
        if (next == ptr) {
            break;              /* self-referential list */
        }
        ptr = next & 0xFC;
    }
    return 0;
}

int pci_msi_alloc(pci_ivars_t *f, void (*handler)(void *ctx), void *ctx) {
    uint16 ctrl;
    uint16 cmd;
    uint8  data_off;
    uint64 addr;
    int    vector;

    if (f == NULL || f->msi_cap == 0) {
        return -1;
    }
    /* No Local APIC means the message write at 0xFEE00000 is decoded by
     * nothing. Refusing here rather than programming the device and letting
     * the interrupt vanish is the difference between a failure that is
     * reported and one that is debugged. */
    if (!lapic_available()) {
        return -1;
    }
    if (f->msi_vector != 0) {
        return -1;              /* already enabled */
    }

    vector = idt_alloc_vector(handler, ctx);
    if (vector < 0) {
        return -1;
    }

    ctrl = pci_cfg_read16(f->bus, f->slot, f->func,
                          (uint8)(f->msi_cap + MSI_CTRL));

    /* Disable first, then program, then enable. Writing the address and data
     * of a capability that is already enabled is a race against the device:
     * it may raise an interrupt with the old data and the new address, or
     * the reverse. Every one of those combinations is a vector nobody
     * bound. */
    pci_cfg_write16(f->bus, f->slot, f->func,
                    (uint8)(f->msi_cap + MSI_CTRL),
                    (uint16)(ctrl & ~MSI_CTRL_ENABLE));

    addr = lapic_msi_address();
    pci_cfg_write32(f->bus, f->slot, f->func,
                    (uint8)(f->msi_cap + MSI_ADDR_LO), (uint32)addr);

    if (ctrl & MSI_CTRL_64BIT) {
        pci_cfg_write32(f->bus, f->slot, f->func,
                        (uint8)(f->msi_cap + MSI_ADDR_HI),
                        (uint32)(addr >> 32));
        data_off = (uint8)(f->msi_cap + 0x0C);
    } else {
        data_off = (uint8)(f->msi_cap + 0x08);
    }

    /* The message data IS the vector, for edge-triggered fixed delivery to
     * the destination in the address. Bits 15:8 select delivery mode and
     * trigger; all zero is "fixed, edge", which is the only combination that
     * makes sense for MSI - MSI has no level to deassert. */
    pci_cfg_write16(f->bus, f->slot, f->func, data_off, (uint16)vector);

    /* Grant exactly one vector: clear the multiple-message-enable field.
     * A device that asked for more (MMC > 0) gets one anyway, which is
     * legal - the spec requires a device to work with fewer than it
     * requested, and it is the caller's job not to need more. */
    ctrl = (uint16)((ctrl & ~MSI_CTRL_MME_MASK) | MSI_CTRL_ENABLE);
    pci_cfg_write16(f->bus, f->slot, f->func,
                    (uint8)(f->msi_cap + MSI_CTRL), ctrl);

    /* And take the function off its legacy line - see pci.h for why this is
     * not optional. */
    cmd = pci_cfg_read16(f->bus, f->slot, f->func, 0x04);
    pci_cfg_write16(f->bus, f->slot, f->func, 0x04,
                    (uint16)(cmd | PCI_COMMAND_INTX_DIS));

    f->msi_vector = (uint8)vector;
    return vector;
}

void pci_msi_release(pci_ivars_t *f) {
    uint16 ctrl;
    uint16 cmd;

    if (f == NULL || f->msi_cap == 0 || f->msi_vector == 0) {
        return;
    }

    /* Silence the device before releasing the vector, not after: freeing
     * first leaves a window where the device can still raise a vector that
     * has just become unbound, or worse, one that has been handed to
     * somebody else. */
    ctrl = pci_cfg_read16(f->bus, f->slot, f->func,
                          (uint8)(f->msi_cap + MSI_CTRL));
    pci_cfg_write16(f->bus, f->slot, f->func,
                    (uint8)(f->msi_cap + MSI_CTRL),
                    (uint16)(ctrl & ~MSI_CTRL_ENABLE));

    cmd = pci_cfg_read16(f->bus, f->slot, f->func, 0x04);
    pci_cfg_write16(f->bus, f->slot, f->func, 0x04,
                    (uint16)(cmd & ~PCI_COMMAND_INTX_DIS));

    idt_free_vector((int)f->msi_vector);
    f->msi_vector = 0;
}

void pci_msi_report(uint8 color) {
    int i;
    int any = 0;

    for (i = 0; i < pci_func_count; i++) {
        const pci_ivars_t *f = &pci_funcs[i];
        uint16 ctrl;

        if (f->msi_cap == 0) {
            continue;
        }
        if (!any) {
            print_string("PCI MSI-capable functions:\n", color);
            any = 1;
        }
        ctrl = pci_cfg_read16(f->bus, f->slot, f->func,
                              (uint8)(f->msi_cap + MSI_CTRL));
        kprintf_c(color, "  %x:%x.%x  %x:%x  cap at %x  %s  %s  vectors %d\n",
                  f->bus, f->slot, f->func, f->vendor_id, f->device_id,
                  f->msi_cap,
                  (ctrl & MSI_CTRL_64BIT) ? "64-bit" : "32-bit",
                  (ctrl & MSI_CTRL_ENABLE) ? "enabled" : "disabled",
                  1 << ((ctrl & MSI_CTRL_MMC_MASK) >> 1));
    }
    if (!any) {
        print_string("PCI MSI-capable functions: none\n", color);
    }
}

/* --- MSI programming selftest --------------------------------------------
 *
 * Lives in pci.c rather than beside bus_selftest.c because it needs this
 * file's capability-register offsets and its function table, and copying
 * either somewhere else would mean the test could pass against offsets that
 * no longer match the ones pci_msi_alloc actually uses.
 *
 * What this proves and what it does not. It writes the MSI capability, then
 * reads every register back OFF THE DEVICE and checks the device latched
 * exactly what was written - which is the half that catches a wrong offset
 * for the 64-bit layout, a register the device implements read-only, or an
 * enable bit that never took. It does NOT prove an MSI ever arrives: that
 * would need a driver programming a device into raising one, which is item
 * 12's work and explicitly out of scope this pass.
 *
 * The delivery half is not untested, though - it is tested somewhere else.
 * idt_selftest raises a LAPIC self-IPI on an allocated vector and watches it
 * run the bound handler, and that is the identical chain an MSI takes from
 * the moment the message hits the LAPIC. What is untested is strictly the
 * device-writes-the-message step. Saying that plainly is better than a test
 * named "msi works".
 */
/* Never expected to run: nothing programs this device into raising an MSI,
 * and the test releases the vector before returning. It exists because
 * idt_alloc_vector refuses a NULL handler - correctly, since a vector bound
 * to nothing counts every arrival as unhandled - and passing NULL here is
 * what the first draft of this test did, which made pci_msi_alloc return -1
 * and the double-alloc check below pass for the wrong reason. */
static void msi_test_handler(void *ctx) {
    (void)ctx;
}

int pci_msi_selftest(void) {
    int          failures = 0;
    int          i;
    int          vector;
    int          before;
    pci_ivars_t *f = NULL;
    uint16       ctrl;
    uint16       cmd;
    uint32       lo;
    uint16       data;
    uint8        data_off;
    uint64       want;

    for (i = 0; i < pci_func_count; i++) {
        if (pci_funcs[i].msi_cap != 0) {
            f = &pci_funcs[i];
            break;
        }
    }
    if (f == NULL) {
        /* Not a failure. QEMU's default i440fx machine exposes no
         * MSI-capable function at all, so on that machine this path is
         * correct and untaken - a weaker claim than "verified", and the
         * command that makes it stronger is named here rather than left for
         * a reader to guess. */
        kprintf_c(0x0E, "pci msi selftest: no MSI-capable function - "
                        "programming not exercised (try -device ich9-ahci)\n");
        return 0;
    }
    if (!lapic_available()) {
        kprintf_c(0x0E, "pci msi selftest: no local APIC - MSI cannot work "
                        "on this machine\n");
        return 0;
    }

    before = idt_vector_count();

    vector = pci_msi_alloc(f, msi_test_handler, 0);
    if (vector < IDT_DYNAMIC_FIRST || vector > IDT_DYNAMIC_LAST) {
        kprintf_c(0x0C, "pci msi selftest: alloc returned %d\n", vector);
        return 1;
    }
    if (idt_vector_count() != before + 1) {
        kprintf_c(0x0C, "pci msi selftest: alloc did not take a vector\n");
        failures++;
    }
    /* A second enable on the same function must be refused. Letting it
     * through would reprogram a live capability and strand the first
     * vector bound but unreachable. */
    if (pci_msi_alloc(f, msi_test_handler, 0) != -1) {
        kprintf_c(0x0C, "pci msi selftest: double alloc succeeded\n");
        failures++;
    }

    ctrl = pci_cfg_read16(f->bus, f->slot, f->func,
                          (uint8)(f->msi_cap + MSI_CTRL));

    /* --- the readbacks -------------------------------------------------- */
    want = lapic_msi_address();
    lo   = pci_cfg_read32(f->bus, f->slot, f->func,
                          (uint8)(f->msi_cap + MSI_ADDR_LO));
    if (lo != (uint32)want) {
        kprintf_c(0x0C, "pci msi selftest: address lo read back %x, "
                        "wrote %x\n", lo, (uint32)want);
        failures++;
    }
    if (ctrl & MSI_CTRL_64BIT) {
        uint32 hi = pci_cfg_read32(f->bus, f->slot, f->func,
                                   (uint8)(f->msi_cap + MSI_ADDR_HI));
        if (hi != (uint32)(want >> 32)) {
            kprintf_c(0x0C, "pci msi selftest: address hi read back %x, "
                            "wrote %x\n", hi, (uint32)(want >> 32));
            failures++;
        }
        data_off = (uint8)(f->msi_cap + 0x0C);
    } else {
        data_off = (uint8)(f->msi_cap + 0x08);
    }

    /* The whole 16-bit data register, not just the low byte: bits 15:8 pick
     * delivery mode and trigger, and all-zero is the "fixed, edge" that MSI
     * requires. A stray bit there is a delivery mode nothing handles. */
    data = pci_cfg_read16(f->bus, f->slot, f->func, data_off);
    if (data != (uint16)vector) {
        kprintf_c(0x0C, "pci msi selftest: data read back %x, wrote %x\n",
                  data, vector);
        failures++;
    }

    if (!(ctrl & MSI_CTRL_ENABLE)) {
        kprintf_c(0x0C, "pci msi selftest: enable bit did not take\n");
        failures++;
    }
    /* Exactly one vector granted. A device that asked for more and got its
     * request back would raise vector+1..vector+n, none of them bound. */
    if (ctrl & MSI_CTRL_MME_MASK) {
        kprintf_c(0x0C, "pci msi selftest: granted %d vectors, wanted 1\n",
                  1 << ((ctrl & MSI_CTRL_MME_MASK) >> 4));
        failures++;
    }

    cmd = pci_cfg_read16(f->bus, f->slot, f->func, 0x04);
    if (!(cmd & PCI_COMMAND_INTX_DIS)) {
        kprintf_c(0x0C, "pci msi selftest: INTx not disabled - the function "
                        "can deliver the same event twice\n");
        failures++;
    }

    /* --- release ---------------------------------------------------------
     * Checked as its own half: a release that frees the vector but leaves
     * the device enabled is worse than no release at all, because the
     * device then raises a vector that has been handed to somebody else. */
    pci_msi_release(f);

    ctrl = pci_cfg_read16(f->bus, f->slot, f->func,
                          (uint8)(f->msi_cap + MSI_CTRL));
    if (ctrl & MSI_CTRL_ENABLE) {
        kprintf_c(0x0C, "pci msi selftest: still enabled after release\n");
        failures++;
    }
    cmd = pci_cfg_read16(f->bus, f->slot, f->func, 0x04);
    if (cmd & PCI_COMMAND_INTX_DIS) {
        kprintf_c(0x0C, "pci msi selftest: INTx left disabled after "
                        "release\n");
        failures++;
    }
    if (f->msi_vector != 0) {
        kprintf_c(0x0C, "pci msi selftest: msi_vector not cleared\n");
        failures++;
    }
    if (idt_vector_count() != before) {
        kprintf_c(0x0C, "pci msi selftest: release leaked a vector\n");
        failures++;
    }

    if (failures == 0) {
        kprintf("pci msi: selftest passed - %x:%x programmed on vector %d "
                "and released\n", f->vendor_id, f->device_id, vector);
    } else {
        kprintf_c(0x0C, "pci msi: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
