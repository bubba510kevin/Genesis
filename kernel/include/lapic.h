#ifndef LAPIC_H
#define LAPIC_H

#include "typesk.h"

/* The Local APIC, brought up far enough for MSI to work.
 *
 * --- why this is here and not in Part 10 ---------------------------------
 * The plan put "Local APIC" under Part 10 (SMP bring-up) and said Part 5
 * (MSI) "depends on nothing else here". That was wrong, and it is worth
 * saying so rather than quietly reordering: an MSI is not a special kind of
 * interrupt line, it is a MEMORY WRITE to 0xFEE00000 that the Local APIC
 * decodes into a vector. With no LAPIC software-enabled, the write goes
 * nowhere and the interrupt is simply never delivered. There is no version
 * of MSI that works on an 8259 alone.
 *
 * So this file brings up the half of the LAPIC that receiving interrupts
 * needs - enable, spurious vector, EOI, and a self-IPI for testing - and
 * nothing else. Part 10 adds what SENDING to other CPUs needs: the APIC ID,
 * INIT-SIPI-SIPI, and the IPI machinery for TLB shootdown. That is additive
 * to this, not a rewrite of it.
 *
 * --- what stays on the 8259 ----------------------------------------------
 * Everything legacy. The PIC is still remapped to 32-47 and still delivers
 * the timer and the keyboard, through the LAPIC's LINT0 in virtual-wire
 * mode, which needs no LAPIC EOI. Only dynamically allocated vectors (48+,
 * see idt.h) are LAPIC-delivered and take lapic_eoi(). Moving legacy IRQs
 * onto an IOAPIC is a separate change with its own risk, and it buys
 * nothing until there is a second CPU to route them to.
 */

/* True if the CPU reports a Local APIC and it was successfully enabled and
 * mapped. Everything else in this file is a no-op when this is false, so a
 * machine without one degrades to "MSI unavailable" rather than faulting. */
int lapic_available(void);

/* Enable the Local APIC and map its register page. Call after paging is up
 * (it maps MMIO) and before anything allocates a dynamic vector. Safe to
 * call once; a second call is a no-op. */
void lapic_init(void);

/* End-of-interrupt for a LAPIC-delivered vector. NOT for the legacy 32-47
 * range - those are the PIC's and take pic_send_eoi. Sending the wrong one
 * leaves the source unacknowledged, which shows up as an interrupt that
 * fires exactly once and then never again. */
void lapic_eoi(void);

/* This CPU's APIC ID. Zero until Part 10 makes more than one meaningful,
 * but read from the hardware rather than assumed, because MSI's message
 * address encodes a destination and hardcoding zero there would be a lie
 * that works right up until it doesn't. */
uint32 lapic_id(void);

/* Send `vector` to this CPU. Real hardware delivery through the LAPIC - the
 * same path an MSI takes - which is what makes it worth testing with rather
 * than calling the handler directly. Returns 0 on success, -1 if no LAPIC. */
int lapic_send_self(uint8 vector);

/* The task-priority register. The LAPIC blocks any vector whose priority
 * class (vector >> 4) is at or below TPR >> 4, so writing it is real
 * masking, not bookkeeping. Part 11 maps NT's IRQL onto it - see
 * KeRaiseIrql in kernel/wdm.c. */
uint8 lapic_get_tpr(void);
void  lapic_set_tpr(uint8 value);

/* Enable the CALLING CPU's Local APIC. lapic_init does this for the BSP and
 * also maps the register page; an AP calls this instead, because the mapping
 * is shared but the SVR/TPR/enable registers are per-CPU. An AP that skips
 * it accepts no interrupts and looks alive while answering nothing. */
void lapic_init_ap(void);

/* --- the SEND half (Part 10) ---------------------------------------------
 * Additive to everything above, exactly as this file predicted. */

/* Send `vector` to the CPU with this APIC ID. Returns 0, or -1 if there is
 * no LAPIC or the ICR never came free. */
int lapic_send_ipi(uint32 apic_id, uint8 vector);

/* INIT: assert and de-assert, which is what resets an AP into the state a
 * STARTUP IPI can act on. */
int lapic_send_init(uint32 apic_id);

/* STARTUP. `page` is NOT a vector despite occupying the vector field - it is
 * the physical page number the AP begins executing at in real mode, so 8
 * means 0x8000. See the implementation. */
int lapic_send_sipi(uint32 apic_id, uint8 page);

/* The physical address MSI message-address registers must be programmed
 * with, for this CPU. See pci_msi_alloc in pci.h. */
uint64 lapic_msi_address(void);

/* One line for the boot log, the same posture as pci_report/e820_report. */
void lapic_report(uint8 color);

#endif
