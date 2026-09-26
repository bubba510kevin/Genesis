#include "acpi.h"
#include "io.h"
#include "ioapic.h"
#include "kprintf.h"
#include "lapic.h"
#include "paging.h"
#include "pic.h"
#include "pmm.h"
#include "typesk.h"
#include "vmalloc.h"

/* See ioapic.h. */

/* The IOAPIC is not memory-mapped register-per-address the way the LAPIC is.
 * It exposes exactly two 32-bit windows: write a register NUMBER to IOREGSEL,
 * then read or write that register's value through IOWIN. Everything else is
 * indexed through those two. */
#define IOREGSEL 0x00
#define IOWIN    0x10

#define IOAPIC_REG_ID      0x00
#define IOAPIC_REG_VERSION 0x01
/* Redirection entries start at register 0x10 and take TWO registers each -
 * low half then high half - so entry n is at 0x10 + n*2. Treating them as one
 * register per entry is an easy and very confusing mistake: it half-programs
 * every other line. */
#define IOAPIC_REG_REDIR   0x10

/* Low half of a redirection entry. */
#define REDIR_MASKED        (1u << 16)
#define REDIR_TRIGGER_LEVEL (1u << 15)
#define REDIR_POLARITY_LOW  (1u << 13)
#define REDIR_DELIVERY_FIXED (0u << 8)
#define REDIR_DESTMODE_PHYS  (0u << 11)

/* MPS INTI flags, from the MADT override entry. */
#define INTI_POLARITY_MASK  0x3
#define INTI_POLARITY_LOW   0x3
#define INTI_TRIGGER_MASK   0xC
#define INTI_TRIGGER_LEVEL  0xC

/* Same page-table bits, and the same reasoning, as lapic.c's. Duplicated
 * rather than shared for the reason that file already gives: the tree prefers
 * a local definition to a header that exists for two users. Uncached is not
 * optional here - a cached redirection entry means a mask that appears to
 * take effect and does not. */
#define PAGE_PWT 0x8
#define PAGE_PCD 0x10

#define MAX_IOAPICS 4

struct ioapic {
    volatile uint8 *base;
    uint32          id;
    uint32          gsi_base;
    uint32          gsi_count;
};

static struct ioapic apics[MAX_IOAPICS];
static int           apic_count;
static int           active;

/* What ioapic_route_irq actually programmed, for the report. Indexed by ISA
 * IRQ. Vector 0 means "never routed" - vector 0 is the divide-error
 * exception and can never legitimately appear here. */
static uint8  routed_vector[16];
static uint32 routed_gsi[16];
/* Which lines this file left unmasked at handover, so the report can say
 * "live" rather than making the reader infer it from a redirection entry. */
static uint16 routed_live;

static uint32 ioapic_read(struct ioapic *io, uint32 reg) {
    *(volatile uint32 *)(io->base + IOREGSEL) = reg;
    return *(volatile uint32 *)(io->base + IOWIN);
}

static void ioapic_write(struct ioapic *io, uint32 reg, uint32 value) {
    *(volatile uint32 *)(io->base + IOREGSEL) = reg;
    *(volatile uint32 *)(io->base + IOWIN) = value;
}

/* Which IOAPIC owns this GSI, or NULL. A machine with one IOAPIC makes this
 * look pointless; the gsi_base field exists precisely because a machine may
 * have several, and picking apics[0] unconditionally would work on QEMU and
 * fail on the hardware the field was invented for. */
static struct ioapic *owner_of(uint32 gsi) {
    int i;

    for (i = 0; i < apic_count; i++) {
        if (gsi >= apics[i].gsi_base &&
            gsi < apics[i].gsi_base + apics[i].gsi_count) {
            return &apics[i];
        }
    }
    return NULL;
}

uint32 ioapic_gsi_for_irq(uint8 irq) {
    int i, n = acpi_override_count();

    for (i = 0; i < n; i++) {
        uint8  source;
        uint32 gsi;

        if (acpi_override(i, &source, &gsi, NULL) == 0 && source == irq) {
            return gsi;
        }
    }
    return irq;
}

/* The polarity and trigger bits for an ISA IRQ, as redirection-entry flags.
 *
 * The default when no override covers the line is ISA's: active high, edge
 * triggered. That is not the same as the PCI default (active low, level),
 * which is why a PCI line reaching here without an override would be
 * programmed wrong - see ioapic_route_irq's comment on that. */
static uint32 inti_flags_for_irq(uint8 irq) {
    int i, n = acpi_override_count();
    uint32 out = 0;

    for (i = 0; i < n; i++) {
        uint8  source;
        uint16 flags;

        if (acpi_override(i, &source, NULL, &flags) != 0 || source != irq) {
            continue;
        }
        if ((flags & INTI_POLARITY_MASK) == INTI_POLARITY_LOW) {
            out |= REDIR_POLARITY_LOW;
        }
        if ((flags & INTI_TRIGGER_MASK) == INTI_TRIGGER_LEVEL) {
            out |= REDIR_TRIGGER_LEVEL;
        }
        return out;
    }
    return 0;
}

static void set_masked(uint32 gsi, int masked) {
    struct ioapic *io = owner_of(gsi);
    uint32 reg, low;

    if (io == NULL) {
        return;
    }
    reg = IOAPIC_REG_REDIR + (gsi - io->gsi_base) * 2;
    low = ioapic_read(io, reg);
    if (masked) {
        low |= REDIR_MASKED;
    } else {
        low &= ~REDIR_MASKED;
    }
    ioapic_write(io, reg, low);
}

int ioapic_route_irq(uint8 irq, uint8 vector, uint32 apic_id) {
    uint32 gsi = ioapic_gsi_for_irq(irq);
    struct ioapic *io = owner_of(gsi);
    uint32 reg;

    if (io == NULL || irq >= 16) {
        return -1;
    }
    reg = IOAPIC_REG_REDIR + (gsi - io->gsi_base) * 2;

    /* High half FIRST, and while the entry is still masked. The destination
     * lives in the high half and the mask bit lives in the low half, so
     * writing the low half first opens a window in which the line is unmasked
     * and pointed at whatever destination the previous entry had. */
    ioapic_write(io, reg + 1, apic_id << 24);
    ioapic_write(io, reg,
                 REDIR_MASKED | REDIR_DELIVERY_FIXED | REDIR_DESTMODE_PHYS |
                 inti_flags_for_irq(irq) | vector);

    routed_vector[irq] = vector;
    routed_gsi[irq]    = gsi;
    return 0;
}

void ioapic_mask_irq(uint8 irq) {
    if (!active) {
        return;
    }
    set_masked(ioapic_gsi_for_irq(irq), 1);
}

void ioapic_unmask_irq(uint8 irq) {
    if (!active) {
        return;
    }
    set_masked(ioapic_gsi_for_irq(irq), 0);
}

int ioapic_active(void) {
    return active;
}

int ioapic_entry_for_irq(uint8 irq, uint32 *out_low, uint32 *out_high) {
    uint32 gsi = ioapic_gsi_for_irq(irq);
    struct ioapic *io = owner_of(gsi);
    uint32 reg;

    if (!active || io == NULL) {
        return -1;
    }
    reg = IOAPIC_REG_REDIR + (gsi - io->gsi_base) * 2;
    if (out_low != NULL) {
        *out_low = ioapic_read(io, reg);
    }
    if (out_high != NULL) {
        *out_high = ioapic_read(io, reg + 1);
    }
    return 0;
}

int ioapic_init(void) {
    int i;
    uint32 bsp;
    uint16 was_unmasked;

    if (active) {
        return 1;
    }

    /* An IOAPIC delivers into a Local APIC. With no LAPIC there is nothing
     * on the receiving end, and routing a line at it would silence the line
     * rather than move it. */
    if (!lapic_available()) {
        return 0;
    }

    /* acpi_enumerate_cpus is what walks the MADT; the IOAPIC entries are
     * collected on the same pass. Called for that side effect, and it is
     * idempotent, so this does not depend on smp.c having run first. */
    acpi_enumerate_cpus();
    if (acpi_ioapic_count() == 0) {
        return 0;
    }

    for (i = 0; i < acpi_ioapic_count() && apic_count < MAX_IOAPICS; i++) {
        uint32 id, gsi_base;
        uint64 phys, va;
        struct ioapic *io;
        uint32 ver;

        if (acpi_ioapic(i, &id, &phys, &gsi_base) != 0) {
            continue;
        }

        /* 0xFEC00000 by convention, above RAM, so not in the direct map -
         * the same situation lapic.c's register page is in, handled the same
         * way. */
        va = kvm_alloc_range(PMM_PAGE_SIZE, PMM_PAGE_SIZE);
        if (va == 0) {
            kprintf_c(0x0C, "ioapic: no kernel VA for the register page\n");
            return 0;
        }
        if (!vmm_map_page((virt_addr_t)va, (phys_addr_t)phys,
                          PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT)) {
            kprintf_c(0x0C, "ioapic: could not map the register page\n");
            kvm_free_range(va);
            return 0;
        }

        io = &apics[apic_count];
        io->base     = (volatile uint8 *)(va + (phys & 0xFFFULL));
        io->id       = id;
        io->gsi_base = gsi_base;

        /* Bits 16-23 of the version register are the index of the LAST
         * redirection entry, so the count is that plus one. Reading it rather
         * than assuming 24 - the count is genuinely part-specific, and an
         * IOAPIC with fewer entries would have owner_of() claiming GSIs that
         * do not exist. */
        ver = ioapic_read(io, IOAPIC_REG_VERSION);
        io->gsi_count = ((ver >> 16) & 0xFF) + 1;
        apic_count++;
    }

    if (apic_count == 0) {
        return 0;
    }

    /* Mask every entry before anything is routed. Firmware leaves these in
     * whatever state it used, and an inherited entry pointing at a vector
     * this kernel means something else by is worse than no entry at all. */
    for (i = 0; i < apic_count; i++) {
        uint32 e;

        for (e = 0; e < apics[i].gsi_count; e++) {
            ioapic_write(&apics[i], IOAPIC_REG_REDIR + e * 2, REDIR_MASKED);
        }
    }

    /* Which lines are live RIGHT NOW, before anything is disturbed.
     *
     * The 8259 mask is the kernel's actual record of which legacy lines are
     * in use: pic_remap inherits the BIOS's masks, which is why the timer and
     * the keyboard work without anyone unmasking them, and irq_register has
     * been clearing bits here for every driver that attached. Carrying that
     * state across is what makes the handover invisible.
     *
     * The alternative - unmask IRQ 0 and 1 by hand because those are the two
     * that bypass irq_register - would be correct today and silently wrong
     * the moment a third line is live before this point. A bit is set here
     * for a reason and the reason does not need to be re-derived. */
    was_unmasked = (uint16)(inb(PIC1_DATA) | (inb(PIC2_DATA) << 8));
    was_unmasked = (uint16)~was_unmasked;

    /* Now the 8259s. Fully masked rather than left alone: both controllers
     * are still wired to the BSP's LINT0 in virtual-wire mode, so a line left
     * unmasked there delivers the SAME device interrupt twice by two
     * different paths, with only one of them getting an EOI. */
    for (i = 0; i < 16; i++) {
        pic_set_mask((uint8)i);
    }

    active = 1;

    /* Route the legacy lines to the BSP at the vectors interrupt.c already
     * dispatches - 32 + irq. Keeping that mapping is what makes this change
     * additive: interrupt.c's vector arithmetic, irq.c's tables and every
     * driver's irq_register call are all unchanged, and only the wire the
     * interrupt travels on is different.
     *
     * Every line starts masked. irq.c unmasks on registration, exactly as it
     * did with the PIC, and arch/interrupt.c unmasks IRQ 0 and 1 explicitly
     * because the timer and keyboard bypass irq_register. */
    bsp = lapic_id();
    for (i = 0; i < 16; i++) {
        /* IRQ 2 is the 8259 cascade. It is not a device line and has no
         * meaning once the PICs are masked, so it is deliberately not
         * routed - on a machine whose timer is overridden onto GSI 2, routing
         * IRQ 2 as well would program the timer's entry twice, the second
         * time with the wrong vector. */
        if (i == 2) {
            continue;
        }
        ioapic_route_irq((uint8)i, (uint8)(32 + i), bsp);
        if (was_unmasked & (1u << i)) {
            ioapic_unmask_irq((uint8)i);
            routed_live |= (uint16)(1u << i);
        }
    }
    return 1;
}

void ioapic_report(uint8 color) {
    int i;

    if (!active) {
        kprintf_c(color, "ioapic: none - legacy IRQs stay on the 8259 pair\n");
        return;
    }
    for (i = 0; i < apic_count; i++) {
        kprintf_c(color, "ioapic: id %d, %d entries, gsi %d-%d\n",
                  apics[i].id, apics[i].gsi_count, apics[i].gsi_base,
                  apics[i].gsi_base + apics[i].gsi_count - 1);
    }
    kprintf_c(color, "ioapic: 8259 pair fully masked; legacy irqs routed "
                     "to apic id %d\n", lapic_id());
    for (i = 0; i < 16; i++) {
        if (routed_vector[i] == 0) {
            continue;
        }
        kprintf_c(color, "  irq %d -> gsi %d, vector %d  %s%s\n",
                  i, routed_gsi[i], routed_vector[i],
                  (routed_live & (1u << i)) ? "live" : "masked",
                  routed_gsi[i] != (uint32)i ? "  (overridden)" : "");
    }
}
