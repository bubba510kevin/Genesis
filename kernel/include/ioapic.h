#ifndef IOAPIC_H
#define IOAPIC_H

#include "typesk.h"

/* The I/O APIC: device interrupts routed through the APIC fabric instead of
 * the 8259 pair.
 *
 * --- what this changes ----------------------------------------------------
 * Before this file, every device interrupt reached the CPU the same way it
 * did on a 1982 PC/AT: two cascaded 8259s wired to the BSP's LINT0 in
 * virtual-wire mode. That works, and lapic.h's own comment defended keeping
 * it - "it buys nothing until there is a second CPU to route them to". There
 * is a second CPU now (kernel/arch/smp.c), so it buys something.
 *
 * Three things specifically:
 *
 *   1. A destination. A redirection entry names which Local APIC gets the
 *      interrupt. The 8259 has no such field - its output goes to whichever
 *      CPU the wire reaches, which is the BSP and only the BSP.
 *   2. More than 15 lines. The 8259 pair has 15 usable inputs and one of
 *      them is spent on the cascade. An IOAPIC on QEMU has 24.
 *   3. Per-line polarity and trigger mode, which is what lets a level-
 *      triggered shared PCI line be told apart from an edge-triggered ISA
 *      one instead of being assumed.
 *
 * --- what it deliberately does not do -------------------------------------
 * No interrupt balancing. Every line is routed to one CPU and stays there.
 * Choosing a destination per line is now possible (ioapic_route_irq takes an
 * APIC ID) but deciding it dynamically is policy, and policy with no measured
 * problem behind it is the kind of thing ROADMAP item 11 exists to stop.
 *
 * No x2APIC. The redirection entry's destination field is 8 bits in xAPIC
 * mode, which addresses 255 CPUs, and this kernel's ACPI_MAX_CPUS is 32.
 */

/* Program the IOAPICs the MADT describes, mask every redirection entry, and
 * mask the 8259s. After this returns non-zero, the PIC is no longer the
 * delivery path for anything and pic_send_eoi must not be used - see
 * irq_eoi() in irq.h.
 *
 * Returns 1 if an IOAPIC was found and is now live, 0 if not (in which case
 * nothing was changed and the 8259 path is still the live one). Call after
 * lapic_init - an IOAPIC delivers to a Local APIC, so bringing it up first
 * would route interrupts at a controller that is not accepting them yet. */
int ioapic_init(void);

/* True once ioapic_init has succeeded. The mask/EOI paths branch on this,
 * so a machine with no IOAPIC keeps working exactly as it did. */
int ioapic_active(void);

/* The Global System Interrupt an ISA IRQ actually arrives on, applying the
 * MADT's interrupt source overrides. Returns `irq` unchanged when no
 * override covers it, which is the common case for everything except the
 * timer. See acpi.h's note on why assuming identity here is the classic
 * IOAPIC bring-up bug. */
uint32 ioapic_gsi_for_irq(uint8 irq);

/* Point one ISA IRQ at `vector` on the CPU with `apic_id`, applying the
 * override table for both the GSI number and the polarity/trigger bits.
 * The entry is left MASKED - unmask it with ioapic_unmask_irq once a handler
 * is actually registered, the same discipline irq.c already applies to
 * pic_clear_mask. Returns 0, or -1 if no IOAPIC owns that GSI. */
int ioapic_route_irq(uint8 irq, uint8 vector, uint32 apic_id);

/* Deliver legacy line `irq` to the CPU with APIC ID `apic_id` from now on,
 * keeping its vector and mask state. -1 for IRQ 0 (the clock stays on the
 * BSP), a line with no IOAPIC entry, or no IOAPIC at all. */
int    ioapic_bind_irq(uint8 irq, uint32 apic_id);
uint32 ioapic_irq_destination(uint8 irq);

/* Mask and unmask by ISA IRQ number, applying overrides. No-ops when no
 * IOAPIC is active, so irq.c can call them unconditionally. */
void ioapic_mask_irq(uint8 irq);
void ioapic_unmask_irq(uint8 irq);

/* Read a routed line's redirection entry back off the hardware. For the
 * selftest and for diagnosing a line that does not fire - a driver that
 * registered and hears nothing wants to know whether its entry is masked,
 * pointed at the wrong CPU, or programmed with a vector nothing dispatches.
 * Returns 0, or -1 if no IOAPIC owns that IRQ's GSI. */
int ioapic_entry_for_irq(uint8 irq, uint32 *out_low, uint32 *out_high);

/* What got routed where, for the boot log. */
void ioapic_report(uint8 color);

/* Confirm the handover actually happened rather than merely being programmed:
 * the 8259s are genuinely masked, the timer's entry reads back unmasked with
 * the vector interrupt.c dispatches, and ticks are still arriving. Returns
 * the number of failures. Must run AFTER sti - one of the checks waits for a
 * real interrupt. */
int ioapic_selftest(void);

#endif
