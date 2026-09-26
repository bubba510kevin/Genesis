#include "irq.h"
#include "kprintf.h"
#include "typesk.h"

/* Part 8's check: two handlers on one legacy line, both run.
 *
 * irq_dispatch is called DIRECTLY rather than by provoking a device, and
 * that is a real limitation of this test rather than a shortcut. Nothing in
 * this tree drives a shared legacy line - QEMU's default machine routes the
 * few devices it has onto lines that either nothing claims or that
 * interrupt.c handles directly - so waiting for a real shared interrupt
 * would be waiting forever. What this covers is the chain: registration
 * order, that EVERY handler runs, claim accounting, and that removing one
 * device does not silence the other. What it does not cover is the PIC
 * actually delivering to two drivers, which needs hardware this tree does
 * not have a driver for yet (item 12).
 *
 * IRQ 11 because it is the conventional PCI line, is not driven by anything
 * here, and irq_register unmasks whatever it is given - picking a line some
 * real device sat on would arm that device mid-boot.
 */

#define TEST_IRQ 11

static int a_calls, b_calls, c_calls;

/* Claims the interrupt. */
static int handler_a(void *ctx) {
    (void)ctx;
    a_calls++;
    return 1;
}

/* Does NOT claim it - the "not my device" case, which is the whole reason
 * the handler signature returns int. */
static int handler_b(void *ctx) {
    (void)ctx;
    b_calls++;
    return 0;
}

/* Shares handler_b's function pointer in the sense that matters for
 * unregistration: a distinct ctx. */
static int handler_c(void *ctx) {
    (void)ctx;
    c_calls++;
    return 0;
}

static int ctx_one = 1;
static int ctx_two = 2;

int irq_selftest(void) {
    int failures = 0;

    a_calls = b_calls = c_calls = 0;

    /* --- 1. two handlers on one line ------------------------------------ */
    if (irq_register(TEST_IRQ, handler_a, &ctx_one) != 0) {
        kprintf_c(0x0C, "irq selftest: first register failed\n");
        return 1;
    }
    if (irq_register(TEST_IRQ, handler_b, &ctx_two) != 0) {
        kprintf_c(0x0C, "irq selftest: SECOND register on the same line was "
                        "refused - sharing is not on\n");
        irq_unregister(TEST_IRQ);
        return 1;
    }
    if (irq_handler_count(TEST_IRQ) != 2) {
        kprintf_c(0x0C, "irq selftest: expected 2 handlers, have %d\n",
                  irq_handler_count(TEST_IRQ));
        failures++;
    }

    /* --- 2. both run on one interrupt ------------------------------------
     * The a-only version of this test passes against the OLD one-handler
     * table too, because handler_a is the one that would have been kept. It
     * is b's count that discriminates. */
    irq_dispatch(TEST_IRQ);
    if (a_calls != 1 || b_calls != 1) {
        kprintf_c(0x0C, "irq selftest: one interrupt gave a=%d b=%d, "
                        "wanted 1 and 1\n", a_calls, b_calls);
        failures++;
    }

    /* --- 3. dispatch does not stop at the first claimer -------------------
     * handler_a claims it and is registered FIRST, so a dispatch that
     * stopped on the first non-zero return would never reach b. That is a
     * plausible-looking optimisation and it is wrong on a line where two
     * devices can assert at once. */
    if (b_calls == 0) {
        kprintf_c(0x0C, "irq selftest: dispatch stopped at the first "
                        "claiming handler\n");
        failures++;
    }

    /* --- 4. targeted unregistration -------------------------------------
     * Add a third with the same ctx-distinguishing shape, then remove the
     * middle one and check the other two survive. Removing the HEAD or the
     * TAIL would not exercise the relink. */
    if (irq_register(TEST_IRQ, handler_c, &ctx_one) != 0) {
        kprintf_c(0x0C, "irq selftest: third register failed\n");
        failures++;
    }
    irq_unregister_handler(TEST_IRQ, handler_b, &ctx_two);
    if (irq_handler_count(TEST_IRQ) != 2) {
        kprintf_c(0x0C, "irq selftest: after removing one of three, %d "
                        "remain\n", irq_handler_count(TEST_IRQ));
        failures++;
    }

    a_calls = b_calls = c_calls = 0;
    irq_dispatch(TEST_IRQ);
    if (b_calls != 0) {
        kprintf_c(0x0C, "irq selftest: an unregistered handler still ran\n");
        failures++;
    }
    if (a_calls != 1 || c_calls != 1) {
        kprintf_c(0x0C, "irq selftest: removing one handler silenced the "
                        "others (a=%d c=%d)\n", a_calls, c_calls);
        failures++;
    }

    /* --- 5. mismatched ctx must not unregister ---------------------------
     * handler_c is registered with &ctx_one. Asking to remove (handler_c,
     * &ctx_two) names a registration that does not exist, and must do
     * nothing - this is the case that protects one trampoline shared by
     * several devices, which is exactly how lkpi.c registers. */
    irq_unregister_handler(TEST_IRQ, handler_c, &ctx_two);
    if (irq_handler_count(TEST_IRQ) != 2) {
        kprintf_c(0x0C, "irq selftest: unregister matched on the handler "
                        "alone, ignoring ctx\n");
        failures++;
    }

    /* --- 6. cleanup ------------------------------------------------------ */
    irq_unregister(TEST_IRQ);
    if (irq_handler_count(TEST_IRQ) != 0) {
        kprintf_c(0x0C, "irq selftest: %d handlers survived unregister\n",
                  irq_handler_count(TEST_IRQ));
        failures++;
    }

    if (failures == 0) {
        kprintf("irq: selftest passed\n");
    } else {
        kprintf_c(0x0C, "irq: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
