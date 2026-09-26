#include "kprintf.h"
#include "lapic.h"
#include "ksmp.h"
#include "typesk.h"

/* Part 10's check: prove a second CPU is EXECUTING, not merely counted.
 *
 * acpi_enumerate_cpus returning 2 proves the firmware described two CPUs.
 * smp_init returning 2 proves one of them set a flag. Neither proves it is
 * still running, still has interrupts on, or can do anything - an AP that
 * set `online` and then triple-faulted looks identical from the BSP.
 *
 * So the test makes the AP do work, twice, and reads the result: a TLB
 * shootdown IPI, which the AP can only answer if it is alive, has a valid
 * IDT, has interrupts enabled, and is running on a stack of its own.
 */

int smp_selftest(void) {
    int failures = 0;
    int i;
    uint64 before[SMP_MAX_CPUS];
    int    n = smp_cpu_count();

    if (n <= 1) {
        /* Not a failure. `-smp 1` is a supported and ordinary way to run
         * this kernel, and the whole point of the regression check is that
         * the single-CPU path is unaffected. Naming the flag saves the next
         * reader from concluding SMP is broken. */
        kprintf_c(0x0E, "smp selftest: one cpu - AP paths not exercised "
                        "(try -smp 2)\n");
        return 0;
    }

    for (i = 0; i < n; i++) {
        struct cpu_local *c = smp_cpu(i);

        if (c == NULL) {
            continue;
        }
        before[i] = c->tlb_shootdowns;
        if (i > 0 && !c->online) {
            kprintf_c(0x0C, "smp selftest: cpu%d counted but not online\n", i);
            failures++;
        }
        if (i > 0 && c->apic_id == smp_cpu(0)->apic_id) {
            kprintf_c(0x0C, "smp selftest: cpu%d has the BSP's apic id\n", i);
            failures++;
        }
    }

    /* Two shootdowns, not one. A single one cannot distinguish "the AP
     * answered" from "the counter was already non-zero" - and the address
     * differs between them so a handler that cached the first would show
     * up. */
    smp_tlb_shootdown(0xFFFFFE0000000000ULL);
    smp_tlb_shootdown(0xFFFFFE0000001000ULL);

    for (i = 1; i < n; i++) {
        struct cpu_local *c = smp_cpu(i);

        if (c == NULL) {
            continue;
        }
        if (c->tlb_shootdowns != before[i] + 2) {
            kprintf_c(0x0C, "smp selftest: cpu%d answered %lx of 2 "
                            "shootdowns\n", i, c->tlb_shootdowns - before[i]);
            failures++;
        }
    }

    /* The BSP must NOT have answered its own broadcast. Invalidating on the
     * sender is the caller's job and doing it here as well would be
     * harmless - but a BSP counter that moved means the "skip myself" test
     * in smp_tlb_shootdown is comparing the wrong thing, and that same
     * comparison is what stops a CPU waiting on itself. */
    if (smp_cpu(0)->tlb_shootdowns != before[0]) {
        kprintf_c(0x0C, "smp selftest: the BSP answered its own IPI\n");
        failures++;
    }

    if (failures == 0) {
        kprintf("smp: selftest passed - %d cpus, APs answered TLB "
                "shootdown IPIs\n", n);
    } else {
        kprintf_c(0x0C, "smp: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
