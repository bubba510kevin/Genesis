#ifndef IDT_H
#define IDT_H

#include "typesk.h"

/* Builds a 256-entry IDT of 16-byte gates, filling ALL of them from the stub
 * table in idt.c. Vector 8 is placed on IST slot 1, so call
 * gdt_set_ist(1, ...) with a real stack before enabling interrupts. */
void idt_init(void);

/* Load the already-built IDT on the calling CPU. idt_init does this for the
 * BSP; an application processor calls it directly (see smp.h). */
void idt_load(void);

/* --- dynamic interrupt vectors (48-255) ---------------------------------
 * ROADMAP item 11: idt.c hard-wired exactly 48 vectors - 0-31 exceptions,
 * 32-47 the remapped PIC IRQs - with zero allocation machinery for the other
 * 208. MSI needs one: an MSI message tells the device which vector to raise,
 * so something has to hand out a free one and remember what to call when it
 * arrives. There was nothing to build on, so this is new infrastructure
 * rather than a stub being unstuck.
 *
 * Layered ABOVE irq.h, not beside it. irq.h owns the 16 legacy 8259 lines
 * and their PIC masking; this owns raw IDT vectors with no controller behind
 * them. A driver using MSI never touches irq.h, and a driver on a legacy
 * INTx line never touches this.
 */

/* Called from interrupt context with the ctx given at allocation time. Keep
 * it short - interrupts are disabled for the duration (the gates are
 * interrupt gates, so IF is clear on entry). */
typedef void (*idt_vector_fn)(void *ctx);

/* Reserve a free vector in 48-255 and bind a handler to it. Returns the
 * vector, or -1 if none are free.
 *
 * Allocation is from the TOP down. Deliberate: the highest vectors have the
 * highest interrupt priority class on the LAPIC (priority is vector >> 4),
 * so a device that gets in first should not silently outrank everything
 * added later. Going downwards means early allocations end up in the highest
 * class, which is the same direction of bias FreeBSD's own allocator takes,
 * and either choice is arbitrary as long as it is written down. */
int idt_alloc_vector(idt_vector_fn handler, void *ctx);

/* Bind a handler to one SPECIFIC vector. Returns 0, or -1 if the vector is
 * out of range or already taken. For a caller that needs a fixed number -
 * a test that has to name the vector in an `int $N` instruction, or a future
 * IPI vector that other CPUs must agree on. */
int idt_alloc_vector_at(int vector, idt_vector_fn handler, void *ctx);

/* Release a vector. Safe on one that was never allocated. */
void idt_free_vector(int vector);

/* Run the handler bound to `vector`, if any. Returns 1 if one ran, 0 if the
 * vector is unbound - which the caller uses to decide whether the interrupt
 * was real. Called from interrupt.c's dispatch, not by drivers. */
int idt_dispatch_vector(uint8 vector);

/* How many of 48-255 are currently bound. For the boot report and the
 * selftest; there is no other reason to ask. */
int idt_vector_count(void);

/* The LAPIC's spurious-interrupt vector, which is never handed out by
 * idt_alloc_vector. See kernel/lapic.c for why it is this number. */
#define IDT_SPURIOUS_VECTOR 0xFF

/* First and last dynamically allocatable vector. 48 because 0-47 are the
 * exceptions and the remapped PIC; 254 because 255 is the spurious vector. */
#define IDT_DYNAMIC_FIRST   48
#define IDT_DYNAMIC_LAST    254

/* Exercise allocation, range refusal, real LAPIC-delivered dispatch, and
 * release. Must run with interrupts ENABLED - it raises a self-IPI and waits
 * for it. Returns the number of failures. */
int idt_selftest(void);

#endif
