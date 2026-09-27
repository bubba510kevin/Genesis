#include "kprintf.h"
#include "lapic.h"
#include "ksmp.h"
#include "typesk.h"
#include "ioapic.h"
#include "irq.h"
#include "bkl.h"
#include "netstack.h"
#include "timer.h"
#include "sched.h"

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

/* --- a device interrupt, moved ---------------------------------------------
 *
 * bus_bind_intr's mechanism end to end: the NIC's legacy line is pointed at
 * the last CPU, a ping to the gateway provokes a receive interrupt, and the
 * interrupt has to be counted on THAT CPU. Then the line goes back to the
 * BSP, where everything else expects it. */
int smp_irq_selftest(void) {
    int n = smp_cpu_count();
    int target = n - 1;
    uint8 line = 0;
    int i, failures = 0;
    uint64 before, start;
    uint32 home;

    if (n < 2 || !ioapic_active()) {
        kprintf_c(0x0E, "smp irq selftest: needs 2 CPUs and an IOAPIC - skipped\n");
        return 0;
    }
    /* The first shareable device line with a handler: not the clock, the
     * keyboard, the serial port or the ATA channels, whose interrupts are
     * not provoked by a ping. */
    for (i = 3; i < 16; i++) {
        if (i == 4 || i == 14 || i == 15) {
            continue;
        }
        if (irq_handler_count((uint8)i) > 0) {
            line = (uint8)i;
            break;
        }
    }
    if (line == 0) {
        kprintf_c(0x0E, "smp irq selftest: no device line to move - skipped\n");
        return 0;
    }
    home = ioapic_irq_destination(line);
    before = smp_cpu(target)->dev_irqs;
    if (ioapic_bind_irq(line, smp_cpu(target)->apic_id) != 0) {
        kprintf_c(0x0C, "smp irq selftest: could not bind irq %d\n", line);
        return 1;
    }
    if (ioapic_irq_destination(line) != smp_cpu(target)->apic_id) {
        kprintf_c(0x0C, "smp irq selftest: irq %d destination did not change\n",
                  line);
        failures++;
    }
    (void)net_ping(0x0202000AU);                  /* 10.0.2.2, the gateway */
    start = timer_ticks_now();
    while (smp_cpu(target)->dev_irqs == before &&
           timer_ticks_now() - start < 100) {
        bkl_wait_for_interrupt();
    }
    if (smp_cpu(target)->dev_irqs == before) {
        kprintf_c(0x0C, "smp irq selftest: irq %d bound to cpu%d never arrived "
                        "there\n", line, target);
        failures++;
    }
    (void)ioapic_bind_irq(line, home);            /* back where it was */
    if (failures == 0) {
        kprintf("smp: irq selftest passed - irq %d moved to cpu%d and was "
                "delivered there (%lx interrupts)\n", line, target,
                smp_cpu(target)->dev_irqs - before);
    }
    return failures;
}
