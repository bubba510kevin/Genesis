#include "io.h"
#include "kprintf.h"
#include "lapic.h"
#include "paging.h"
#include "pmm.h"
#include "typesk.h"
#include "vmalloc.h"

/* See lapic.h for what this does and does not bring up, and for why it is
 * in Part 5 rather than Part 10. */

#define MSR_IA32_APIC_BASE  0x1Bu
#define APIC_BASE_BSP       (1u << 8)     /* this CPU is the bootstrap one  */
#define APIC_BASE_ENABLE    (1u << 11)    /* xAPIC global enable            */
#define APIC_BASE_ADDR_MASK 0xFFFFF000ULL

/* Register offsets from the LAPIC base, all 32-bit and all requiring an
 * aligned 32-bit access - a byte or word write to any of these is undefined,
 * which is why every access below goes through the two helpers. */
#define LAPIC_ID        0x020
#define LAPIC_VERSION   0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICR_LOW   0x300
#define LAPIC_ICR_HIGH  0x310

#define LAPIC_SVR_ENABLE    (1u << 8)     /* software enable                */

/* ICR fields. Only what a self-IPI needs. */
#define ICR_DELIVERY_FIXED  (0u << 8)
#define ICR_LEVEL_ASSERT    (1u << 14)
#define ICR_DEST_SELF       (1u << 18)    /* destination shorthand 01       */
#define ICR_DELIVERY_STATUS (1u << 12)

/* Part 10's additions - the SEND half. lapic.h's own comment said Part 10
 * would be additive to Part 5's receive half, and this is that addition. */
#define ICR_DELIVERY_INIT   (5u << 8)
#define ICR_DELIVERY_STARTUP (6u << 8)
#define ICR_LEVEL_DEASSERT  (0u << 14)
#define ICR_TRIGGER_LEVEL   (1u << 15)

/* Page-table bits for MMIO. Not in paging.h because nothing else in the
 * tree maps a device register yet - the first caller defines them, the way
 * this codebase already prefers a local duplicate to a shared header that
 * exists for one user.
 *
 * Both are load-bearing, not belt-and-braces: a cached mapping of the LAPIC
 * means a read can be served from a cache line that predates the interrupt
 * that changed it, and a write can sit in a write-back buffer past the point
 * the hardware was supposed to see it. The classic symptom is an EOI that
 * appears to work and an interrupt that never fires again. */
#define PAGE_PWT  0x8
#define PAGE_PCD  0x10

/* The spurious-interrupt vector.
 *
 * 0xFF for two reasons. It is the highest vector, so it loses every priority
 * arbitration, and some early P5/P6 parts ignore the low four bits of this
 * field entirely - a value whose low nibble is not 0xF would be silently
 * rounded there. Both argue for the same number and neither costs anything
 * here. It needs a real IDT gate, which it has: idt.c fills all 256. */
#define LAPIC_SPURIOUS_VECTOR 0xFF

static volatile uint8 *lapic_base;
static int             available;
static uint32          version_reg;

static uint32 lapic_read(uint32 reg) {
    return *(volatile uint32 *)(lapic_base + reg);
}

static void lapic_write(uint32 reg, uint32 value) {
    *(volatile uint32 *)(lapic_base + reg) = value;
}

int lapic_available(void) {
    return available;
}

void lapic_init(void) {
    uint32 eax, ebx, ecx, edx;
    uint64 base_msr;
    phys_addr_t phys;
    uint64 va;

    if (available) {
        return;
    }

    /* CPUID leaf 1, EDX bit 9. Asking rather than assuming: an emulator
     * configured without an APIC is a real configuration, and writing
     * IA32_APIC_BASE on a CPU that has no APIC is a #GP. */
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if ((edx & (1u << 9)) == 0) {
        kprintf_c(0x0E, "lapic: CPU reports no local APIC - MSI "
                        "unavailable\n");
        return;
    }

    base_msr = rdmsr(MSR_IA32_APIC_BASE);
    phys = (phys_addr_t)(base_msr & APIC_BASE_ADDR_MASK);
    if (phys == 0) {
        kprintf_c(0x0E, "lapic: IA32_APIC_BASE reads zero\n");
        return;
    }

    /* The hardware enable. Distinct from the SVR's software enable below and
     * both are needed: clearing this one takes the APIC off the bus
     * entirely, and on most parts it cannot be set again without a reset. */
    wrmsr(MSR_IA32_APIC_BASE, base_msr | APIC_BASE_ENABLE);

    /* The register page is at 0xFEE00000 on every part this kernel runs on -
     * above RAM, so it is NOT in paging.c's direct map (physmap_init only
     * covers up to e820's highest usable address). It needs its own kernel
     * VA, and Part 1's range allocator is exactly what stops that from being
     * a fourth hand-picked constant. */
    va = kvm_alloc_range(PMM_PAGE_SIZE, PMM_PAGE_SIZE);
    if (va == 0) {
        kprintf_c(0x0C, "lapic: no kernel VA for the register page\n");
        return;
    }
    if (!vmm_map_page((virt_addr_t)va, phys,
                      PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT)) {
        kprintf_c(0x0C, "lapic: could not map the register page\n");
        kvm_free_range(va);
        return;
    }
    lapic_base = (volatile uint8 *)va;

    /* Accept every priority. The TPR blocks vectors at or below its class,
     * and it is not architecturally guaranteed to be zero out of reset - a
     * stale non-zero value here silently drops low-numbered MSI vectors,
     * which is a miserable thing to debug because higher ones still work.
     *
     * Part 11 makes this register mean something: NT's IRQL levels map onto
     * it, which is what turns wdm.c's KeRaiseIrql from a variable into real
     * masking. */
    lapic_write(LAPIC_TPR, 0);

    /* The software enable, plus the spurious vector. Until this is written
     * the LAPIC accepts nothing, which is precisely why MSI cannot work on
     * an 8259-only kernel however correctly the device is programmed. */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);

    version_reg = lapic_read(LAPIC_VERSION);
    available = 1;
}

uint8 lapic_get_tpr(void) {
    return available ? (uint8)(lapic_read(LAPIC_TPR) & 0xFF) : 0;
}

void lapic_set_tpr(uint8 value) {
    if (available) {
        lapic_write(LAPIC_TPR, value);
    }
}

void lapic_init_ap(void) {
    uint64 base_msr;

    /* Every CPU has its OWN Local APIC, with its own IA32_APIC_BASE, its own
     * SVR and its own TPR. The BSP enabling its LAPIC does nothing for this
     * one - and an AP whose LAPIC is not software-enabled accepts no
     * interrupts at all, so it sits in hlt forever while IPIs are delivered
     * to nothing.
     *
     * That is exactly how this was found: the AP started, reported online,
     * and answered zero of two TLB shootdown IPIs. Nothing faulted, because
     * nothing was wrong on the sending side.
     *
     * The MAPPING is shared - all CPUs' LAPICs answer at the same physical
     * address, each seeing its own registers - so this does not repeat
     * lapic_init's kvm_alloc_range/vmm_map_page. Only the per-CPU register
     * writes. */
    if (!available) {
        return;
    }
    base_msr = rdmsr(MSR_IA32_APIC_BASE);
    wrmsr(MSR_IA32_APIC_BASE, base_msr | APIC_BASE_ENABLE);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
}

void lapic_eoi(void) {
    if (available) {
        /* Write value is ignored; the write itself is the acknowledgement. */
        lapic_write(LAPIC_EOI, 0);
    }
}

uint32 lapic_id(void) {
    /* Bits 31:24 in xAPIC mode. */
    return available ? (lapic_read(LAPIC_ID) >> 24) : 0;
}

int lapic_send_self(uint8 vector) {
    uint32 spin;

    if (!available) {
        return -1;
    }

    /* Wait for any previous IPI to be accepted before overwriting the ICR.
     * Bounded rather than a bare `while`: a wedged delivery-status bit
     * should show up as a failed test, not a hung boot. */
    for (spin = 0; spin < 100000u; spin++) {
        if ((lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_STATUS) == 0) {
            break;
        }
    }
    if ((lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_STATUS) != 0) {
        return -1;
    }

    /* The self shorthand makes the destination field irrelevant, but the
     * high half is written anyway: leaving a stale destination in a register
     * the next caller may use without a shorthand is how a Part 10 IPI ends
     * up on the wrong CPU. */
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,
                ICR_DEST_SELF | ICR_LEVEL_ASSERT | ICR_DELIVERY_FIXED |
                (uint32)vector);
    return 0;
}

/* Wait for the ICR to be free. Shared by every send below. Bounded for the
 * same reason lapic_send_self's is: a wedged delivery-status bit must show
 * up as a failed send, not a hung boot. */
static int icr_wait(void) {
    uint32 spin;

    for (spin = 0; spin < 1000000u; spin++) {
        if ((lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_STATUS) == 0) {
            return 0;
        }
    }
    return -1;
}

/* Writing ICR_HIGH then ICR_LOW is not a style choice - the write to
 * ICR_LOW is what triggers the send, so the destination must already be
 * there. Reversing these two lines sends to whatever the previous
 * destination was. */
static int icr_send(uint32 apic_id, uint32 low) {
    if (!available || icr_wait() != 0) {
        return -1;
    }
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, low);
    return 0;
}

int lapic_send_ipi(uint32 apic_id, uint8 vector) {
    return icr_send(apic_id,
                    ICR_LEVEL_ASSERT | ICR_DELIVERY_FIXED | (uint32)vector);
}

int lapic_send_init(uint32 apic_id) {
    /* Assert, then de-assert. The de-assert is a level-triggered INIT with
     * the level bit CLEAR, and it is required by the startup sequence in
     * the Intel manual even though most modern CPUs no longer need it -
     * omitting it works on QEMU and fails on some real hardware, which is
     * the worst possible way for it to be wrong. */
    if (icr_send(apic_id, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT |
                          ICR_TRIGGER_LEVEL) != 0) {
        return -1;
    }
    if (icr_send(apic_id, ICR_DELIVERY_INIT | ICR_LEVEL_DEASSERT |
                          ICR_TRIGGER_LEVEL) != 0) {
        return -1;
    }
    return 0;
}

int lapic_send_sipi(uint32 apic_id, uint8 page) {
    /* The vector field of a STARTUP IPI is not a vector - it is the PAGE
     * NUMBER the AP starts executing at, in real mode. Page 8 means the AP
     * begins at physical 0x8000 with CS=0x0800, IP=0. That reinterpretation
     * of the same field is the single most confusing thing about SIPI. */
    return icr_send(apic_id,
                    ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT | (uint32)page);
}

uint64 lapic_msi_address(void) {
    /* The MSI message address is not the LAPIC's register base - it is a
     * separate architectural window at 0xFEE00000 that the northbridge
     * decodes, with the destination APIC ID in bits 19:12. They happen to be
     * the same number on a default configuration, which is exactly why this
     * is written out rather than reusing the mapped base: they are different
     * things and relocating the LAPIC would separate them.
     *
     * Bits 3 (redirection hint) and 2 (destination mode) left clear: fixed
     * delivery to a physical APIC ID, which is the only mode that means
     * anything with one CPU. */
    return 0xFEE00000ULL | ((uint64)lapic_id() << 12);
}

void lapic_report(uint8 color) {
    if (!available) {
        kprintf_c(color, "lapic: not available\n");
        return;
    }
    kprintf_c(color, "lapic: id %d  version %x  max lvt %d  at %lx\n",
              lapic_id(), version_reg & 0xFF,
              (version_reg >> 16) & 0xFF, (uint64)(uintptr)lapic_base);
}
