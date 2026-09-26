#include "io.h"
#include "ioapic.h"
#include "kprintf.h"
#include "pic.h"
#include "timer.h"
#include "typesk.h"

/* Does the IOAPIC actually deliver, or is it merely programmed?
 *
 * That distinction is the whole point of this file. "The kernel booted with
 * an IOAPIC" is close to no evidence at all on its own: if the 8259s had been
 * left live, every legacy interrupt would still arrive by the old path and
 * the boot would look identical whether or not a single redirection entry was
 * correct. A test that cannot tell those two apart is not testing the change.
 *
 * So the checks below are built around one idea - prove the OLD path is shut
 * off, then prove interrupts still arrive.
 */

#define REDIR_MASKED        (1u << 16)
#define REDIR_TRIGGER_LEVEL (1u << 15)
#define REDIR_VECTOR_MASK   0xFFu

static int fail(const char *what) {
    kprintf_c(0x0C, "ioapic selftest: %s\n", what);
    return 1;
}

int ioapic_selftest(void) {
    int failures = 0;
    uint32 low = 0, high = 0;
    uint64 before, after;
    uint64 spins;

    if (!ioapic_active()) {
        /* Not a failure. A machine with no IOAPIC is a supported
         * configuration and the 8259 path is still correct on it - saying so
         * beats a silent zero, which would read exactly like a pass. */
        kprintf_c(0x0E, "ioapic: no IOAPIC on this machine - selftest "
                        "skipped, legacy IRQs are still on the 8259\n");
        return 0;
    }

    /* 1. The 8259s are genuinely out of the picture.
     *
     * Read back off the hardware rather than trusting that the masking loop
     * ran. This is the check that makes the rest of the test mean something:
     * with both masks at 0xFF the PIC cannot deliver anything, so any
     * interrupt that arrives after this point arrived through the IOAPIC. */
    {
        uint8 m1 = inb(PIC1_DATA);
        uint8 m2 = inb(PIC2_DATA);

        if (m1 != 0xFF || m2 != 0xFF) {
            kprintf_c(0x0C, "ioapic selftest: 8259 masks are %x/%x, not "
                            "ff/ff - both controllers are live and the same "
                            "interrupt has two paths to the CPU\n",
                      (uint32)m1, (uint32)m2);
            failures++;
        }
    }

    /* 2. The timer's entry reads back the way it was programmed.
     *
     * Read from the IOAPIC, not from a variable this file wrote - the
     * failure this catches is a redirection entry that was computed
     * correctly and written to the wrong register, which no amount of
     * checking our own bookkeeping would find. Register 0x10 + n*2 is the
     * easy one to get wrong (two registers per entry, not one), and getting
     * it wrong half-programs every other line. */
    if (ioapic_entry_for_irq(0, &low, &high) != 0) {
        failures += fail("no IOAPIC owns the timer's GSI");
    } else {
        if ((low & REDIR_VECTOR_MASK) != 32) {
            kprintf_c(0x0C, "ioapic selftest: timer entry has vector %d, "
                            "wanted 32 - interrupt.c dispatches 32+irq\n",
                      low & REDIR_VECTOR_MASK);
            failures++;
        }
        if (low & REDIR_MASKED) {
            failures += fail("timer entry is masked - the tick cannot arrive");
        }
        /* The ISA timer is edge triggered. A level-triggered entry for an
         * edge-triggered source re-delivers forever after the first EOI. */
        if (low & REDIR_TRIGGER_LEVEL) {
            failures += fail("timer entry is level triggered, wanted edge");
        }
        /* Destination is the high half's top byte. Zero here would also be a
         * plausible value for "never written", which is why the check is
         * that it matches the BSP rather than merely that it is small. */
        if ((high >> 24) != 0) {
            kprintf_c(0x0C, "ioapic selftest: timer routed to apic id %d, "
                            "wanted the BSP (0)\n", high >> 24);
            failures++;
        }
    }

    /* 3. Ticks are actually arriving.
     *
     * With check 1 having established the 8259s are masked, this is the
     * discriminating one: the counter can only advance if the IOAPIC
     * delivered IRQ 0 to the LAPIC, the LAPIC raised vector 32, and irq_eoi
     * sent the LAPIC's EOI rather than the PIC's. Get any of those three
     * wrong and this hangs at exactly one tick.
     *
     * Bounded by a spin count and not by a second tick, so a broken handover
     * REPORTS rather than wedging the boot - the whole reason to have the
     * test is to be told, and a test that hangs tells you nothing until
     * somebody notices the machine stopped. */
    before = timer_ticks_now();
    for (spins = 0; spins < 200000000ULL; spins++) {
        after = timer_ticks_now();
        if (after != before) {
            break;
        }
        __asm__ volatile ("pause");
    }
    if (after == before) {
        failures += fail("the tick never advanced - IRQ 0 is not being "
                         "delivered, or its EOI went to the wrong controller");
    }

    if (failures == 0) {
        kprintf_c(0x0A, "ioapic: selftest passed - 8259 pair masked, timer "
                        "on gsi %d, ticks arriving through the APIC\n",
                  ioapic_gsi_for_irq(0));
    }
    return failures;
}
