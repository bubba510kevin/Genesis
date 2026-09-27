#ifndef IRQ_H
#define IRQ_H

#include "typesk.h"

/* Table-driven dispatch for legacy PIC IRQs 2-15, with SHARING.
 *
 * interrupt.c's dispatch has always hardcoded IRQ 0 (timer) and IRQ 1
 * (keyboard) directly - boot-critical, already proven, and not what this
 * file touches. Every OTHER IRQ used to have no registration mechanism at
 * all: a PCI-attached driver had nowhere to hook in short of adding a
 * third hardcoded branch to interrupt_dispatch, and a fourth, and a fifth.
 * This is that hook, deferred from the PCI/bus.c pass until a real
 * interrupt-driven driver (the Linux-shaped and WDM-shaped shims' request_
 * irq/IoConnectInterrupt-equivalent) made it load-bearing rather than
 * speculative. See ROADMAP items 4/6/11.
 *
 * --- sharing (ROADMAP item 11) ------------------------------------------
 *
 * This file used to hold exactly one handler per line and refuse a second.
 * That is not how legacy PCI works: INTA#-INTD# are four physical wires per
 * slot routed onto a handful of PIC inputs, so several unrelated functions
 * genuinely share one IRQ, and the only way to find out which of them
 * actually raised it is to ask each in turn.
 *
 * Which is why the handler signature returns int now instead of void. On a
 * shared line "did anything claim this?" is not bookkeeping - it is the
 * only defence against a device whose interrupt nobody acknowledges, which
 * on a LEVEL-triggered line (and INTx is level-triggered) re-asserts the
 * instant the PIC is EOI'd and livelocks the machine. Genesis cannot fix
 * that yet - see the masking note on irq_report - but it can tell you it is
 * happening instead of appearing to hang for no reason.
 */

/* Called with the ctx given at registration. MUST return non-zero if this
 * handler's own device raised the interrupt and it has been serviced, and
 * zero if it did not. Returning non-zero unconditionally on a shared line
 * defeats the unclaimed-interrupt detection for every other device on it,
 * so a handler that cannot tell should return zero and let a device that
 * can speak up. */
typedef int (*irq_handler_fn)(void *ctx);

/* Add `handler` to legacy IRQ `irq` (2-15; 0 and 1 are reserved - see
 * above). Returns 0, or -1 if `irq` is out of range or the handler pool is
 * full. A line may carry several handlers; they run in registration order.
 *
 * Unmasks the line via pic_clear_mask, so the device starts interrupting as
 * soon as this returns - register only once the handler is ready to run. */
int irq_register(uint8 irq, irq_handler_fn handler, void *ctx);

/* Interrupts dispatched on `irq` since boot, claimed or not. */
uint64 irq_delivered(uint8 irq);

/* Remove ONE registration, matched on the (handler, ctx) pair it was added
 * with. Both, not just the handler: one trampoline function shared by
 * several devices - which is exactly what kernel/lkpi.c does - is one
 * handler pointer and several distinct ctxs, and matching on the function
 * alone would unregister somebody else's device. Re-masks the line only
 * when the last handler on it goes. Safe on a pair that was never
 * registered. */
void irq_unregister_handler(uint8 irq, irq_handler_fn handler, void *ctx);

/* Remove EVERY handler on `irq` and re-mask the line. Safe to call on an
 * IRQ that was never registered. */
void irq_unregister(uint8 irq);

/* Called from interrupt_dispatch for irq >= 2. Runs every handler
 * registered on the line, in registration order, and counts the interrupt
 * as unclaimed if none of them returned non-zero.
 *
 * Every handler runs, not just up to the first that claims it. Two devices
 * on one line can raise it at the same time, and stopping early would leave
 * the second one asserting a level-triggered wire forever. */
void irq_dispatch(uint8 irq);

/* Acknowledge a legacy line, to whichever controller actually delivered it.
 *
 * This exists because "which EOI" stopped being a constant when the IOAPIC
 * landed. A PIC-delivered interrupt needs pic_send_eoi and an APIC-delivered
 * one needs lapic_eoi, and the two are not interchangeable in either
 * direction: sending the PIC's for an APIC-delivered interrupt leaves the
 * LAPIC's in-service bit set, so nothing at or below that priority is ever
 * delivered again, and sending the LAPIC's for a PIC-delivered one leaves the
 * 8259 holding the line.
 *
 * One function rather than the branch written out at each of interrupt.c's
 * call sites, because the answer is a property of the machine and not of the
 * call site, and three copies of it is three chances to update two. */
void irq_eoi(uint8 irq);

/* How many handlers are registered on `irq`. For the report and the
 * selftest. */
int irq_handler_count(uint8 irq);

/* How many registrations - across every line - have their handler function
 * OR their ctx inside [base, base+size)? Asked by kldload.c before it unmaps
 * a module image; see irq.c for why the ctx counts. */
int irq_count_handlers_in_range(uint64 base, uint64 size);

/* One line per line that has any handler, plus the unclaimed-interrupt
 * counts. A non-zero unclaimed count is a real signal: some device on that
 * line is raising interrupts nobody owns.
 *
 * What this deliberately does NOT do is mask a line that is producing
 * unclaimed interrupts. Linux does exactly that after 99900 of 100000
 * unhandled, and it is the right eventual answer - but it needs a rate
 * measurement over real time, and doing it on a count alone would mask a
 * line for a device that is merely slow to attach. Reported, not acted on,
 * and said out loud here rather than left as an absence. */
void irq_report(uint8 color);

/* Exercise sharing: two handlers on one line, both run, claim accounting,
 * targeted unregistration, and that the line stays live until the last one
 * goes. Returns the number of failures. Uses a line no hardware in this
 * tree drives, and calls irq_dispatch directly rather than waiting for a
 * device - see the comment in the implementation. */
int irq_selftest(void);

#endif
