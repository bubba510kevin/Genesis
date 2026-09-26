#include "idt.h"
#include "kprintf.h"
#include "lapic.h"
#include "typesk.h"

/* Exercises Part 5's dynamic vector allocator and its delivery path.
 *
 * The interesting choice here is HOW the interrupt is raised. Calling the
 * handler directly would test a function pointer. `int $N` would test the
 * IDT gate but not the controller, and N has to be a compile-time constant
 * so the test could not use whatever vector the allocator actually returned.
 *
 * A LAPIC self-IPI tests the whole chain: allocator -> IDT gate -> stub ->
 * interrupt_dispatch's >47 branch -> idt_dispatch_vector -> handler ->
 * lapic_eoi. That is the same path an MSI takes, minus the device writing
 * the message - which is the part QEMU's default machine gives no way to
 * exercise (see pci_msi_report and src/verif.c).
 */

static volatile int fired;
static volatile int fired_ctx_ok;
static int           ctx_token = 0x5A5A;

static void test_handler(void *ctx) {
    fired++;
    if (ctx == &ctx_token) {
        fired_ctx_ok = 1;
    }
}

static void other_handler(void *ctx) {
    (void)ctx;
    fired += 100;
}

int idt_selftest(void) {
    int failures = 0;
    int v1;
    int v2;
    int spin;
    int baseline;

    /* What is ALREADY allocated before this test starts.
     *
     * This used to assume nothing was: it required the first allocation to
     * return exactly IDT_DYNAMIC_LAST and the count to be zero after
     * cleanup. Part 10 broke both by allocating a permanent vector for TLB
     * shootdown IPIs, and the test failed on a kernel that was working
     * correctly - which is the worst kind of test failure, because the
     * obvious reading is that the new code is broken.
     *
     * Measuring relative to a baseline keeps the property actually worth
     * checking (top-down order, and no leak) without pinning a number that
     * legitimately moves whenever another subsystem takes a vector. */
    baseline = idt_vector_count();

    /* --- 1. allocation --------------------------------------------------- */
    v1 = idt_alloc_vector(test_handler, &ctx_token);
    if (v1 < IDT_DYNAMIC_FIRST || v1 > IDT_DYNAMIC_LAST) {
        kprintf_c(0x0C, "idt selftest: alloc returned %d, out of range\n", v1);
        return failures + 1;
    }
    /* Top-down allocation, documented in idt.h - so the first one out must be
     * the last usable vector. A test that only checked "in range" would not
     * notice the allocator quietly changing direction, and direction is what
     * decides LAPIC priority class. */
    if (v1 != IDT_DYNAMIC_LAST - baseline) {
        kprintf_c(0x0C, "idt selftest: first alloc gave %d, expected %d "
                        "(%d already held)\n",
                  v1, IDT_DYNAMIC_LAST - baseline, baseline);
        failures++;
    }

    v2 = idt_alloc_vector(other_handler, 0);
    if (v2 == v1) {
        kprintf_c(0x0C, "idt selftest: allocator handed out %d twice\n", v1);
        failures++;
    }

    /* The spurious vector must never be handed out - it belongs to the LAPIC
     * and binding a handler to it would run that handler on every withdrawn
     * interrupt. */
    if (v1 == IDT_SPURIOUS_VECTOR || v2 == IDT_SPURIOUS_VECTOR) {
        kprintf_c(0x0C, "idt selftest: allocator handed out the spurious "
                        "vector\n");
        failures++;
    }

    /* Taking an already-taken vector must fail rather than silently
     * overwrite the handler bound to it. */
    if (idt_alloc_vector_at(v1, other_handler, 0) == 0) {
        kprintf_c(0x0C, "idt selftest: re-allocated a live vector\n");
        failures++;
    }
    /* Out of range in both directions - 47 is the last PIC vector and 255 is
     * spurious; neither is the allocator's to give. */
    if (idt_alloc_vector_at(47, other_handler, 0) == 0 ||
        idt_alloc_vector_at(255, other_handler, 0) == 0) {
        kprintf_c(0x0C, "idt selftest: allocated outside 48-254\n");
        failures++;
    }

    /* --- 2. real delivery ------------------------------------------------- */
    if (!lapic_available()) {
        kprintf_c(0x0E, "idt selftest: no local APIC - delivery not tested, "
                        "and MSI cannot work on this machine\n");
        idt_free_vector(v1);
        idt_free_vector(v2);
        return failures;
    }

    fired = 0;
    fired_ctx_ok = 0;

    if (lapic_send_self((uint8)v1) != 0) {
        kprintf_c(0x0C, "idt selftest: self-IPI would not send\n");
        failures++;
    } else {
        /* Bounded wait. The IPI is delivered asynchronously and this runs
         * with interrupts enabled, so a handful of pause iterations is
         * plenty - but a hang here would be a boot that never finishes,
         * which is worse than a failed test. */
        for (spin = 0; spin < 1000000 && fired == 0; spin++) {
            __asm__ volatile ("pause");
        }
        if (fired != 1) {
            kprintf_c(0x0C, "idt selftest: self-IPI on vector %d fired %d "
                            "times\n", v1, fired);
            failures++;
        }
        if (!fired_ctx_ok) {
            kprintf_c(0x0C, "idt selftest: handler got the wrong context\n");
            failures++;
        }
    }

    /* --- 3. release -------------------------------------------------------
     *
     * The self-IPI below lands on a vector nothing is bound to, on purpose -
     * that is what is being tested. interrupt.c counts it, so a clean boot
     * ends with exactly ONE unhandled dynamic vector and that one is this.
     * Said out loud because interrupt_report prints the count right after
     * this runs, and an unexplained non-zero there should otherwise be
     * treated as a real defect. */
    kprintf("idt selftest: the unhandled-vector count below will be 1 - "
            "the next self-IPI targets a deliberately freed vector\n");
    idt_free_vector(v1);
    fired = 0;
    if (lapic_send_self((uint8)v1) == 0) {
        for (spin = 0; spin < 100000 && fired == 0; spin++) {
            __asm__ volatile ("pause");
        }
        if (fired != 0) {
            kprintf_c(0x0C, "idt selftest: freed vector still dispatches\n");
            failures++;
        }
    }
    /* And it must be re-allocatable, which is the half that catches a free
     * that only clears the handler and not the allocation bit. */
    if (idt_alloc_vector_at(v1, test_handler, &ctx_token) != 0) {
        kprintf_c(0x0C, "idt selftest: freed vector could not be "
                        "re-allocated\n");
        failures++;
    }
    idt_free_vector(v1);
    idt_free_vector(v2);

    if (idt_vector_count() != baseline) {
        kprintf_c(0x0C, "idt selftest: %d vectors bound after cleanup, "
                        "started with %d\n", idt_vector_count(), baseline);
        failures++;
    }

    if (failures == 0) {
        kprintf("idt: selftest passed\n");
    } else {
        kprintf_c(0x0C, "idt: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
