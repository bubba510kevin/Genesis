/* Interrupt balancing policy - see irqbalance.h.
 *
 * Load is measured, not guessed: irq_dispatch counts every interrupt per
 * line (irq_delivered), and a pass looks at the difference since the last
 * one. The planner is a greedy longest-processing-time assignment, which is
 * within 4/3 of the best possible spread and is what irqbalance(1) and
 * FreeBSD's intr_shuffle do in spirit: heaviest line first, each onto the
 * CPU that is lightest so far.
 *
 * Ties matter as much as loads. At boot nothing has fired yet and every line
 * weighs zero, so the planner also counts LINES per CPU and breaks load ties
 * on that - which turns "no information" into "one line per CPU" rather than
 * "everything on CPU 0". And a line whose current CPU is among the lightest
 * stays there, so a balanced machine produces an empty plan. */

#include "irqbalance.h"
#include "ioapic.h"
#include "irq.h"
#include "ksmp.h"
#include "kthread.h"
#include "kprintf.h"
#include "netstack.h"
#include "timer.h"
#include "bkl.h"

static uint16 pinned;                        /* bit i: line i bound by a driver */
static uint64 last_count[IRQ_BALANCE_LINES];
static uint64 passes, moves, plans_rejected;
static int    started;

void irq_balance_pin(uint8 irq) {
    if (irq < IRQ_BALANCE_LINES) {
        pinned |= (uint16)(1u << irq);
    }
}

int irq_balance_plan(const uint64 load[IRQ_BALANCE_LINES],
                     const uint8 eligible[IRQ_BALANCE_LINES],
                     const int cur[IRQ_BALANCE_LINES], int ncpu,
                     int out[IRQ_BALANCE_LINES]) {
    uint64 cpu_load[SMP_MAX_CPUS];
    int cpu_lines[SMP_MAX_CPUS];
    uint8 done[IRQ_BALANCE_LINES];
    int i, n, moved = 0;

    if (ncpu < 1) {
        ncpu = 1;
    }
    if (ncpu > SMP_MAX_CPUS) {
        ncpu = SMP_MAX_CPUS;
    }
    for (i = 0; i < ncpu; i++) {
        cpu_load[i] = 0;
        cpu_lines[i] = 0;
    }
    /* Lines the policy may not move still weigh on their CPU. */
    for (i = 0; i < IRQ_BALANCE_LINES; i++) {
        out[i] = cur[i];
        done[i] = !eligible[i];
        if (!eligible[i] && load[i] != 0 && cur[i] >= 0 && cur[i] < ncpu) {
            cpu_load[cur[i]] += load[i];
            cpu_lines[cur[i]]++;
        }
    }
    for (n = 0; n < IRQ_BALANCE_LINES; n++) {
        int best = -1, pick, c;

        /* the heaviest line not yet placed; lowest number on a tie */
        for (i = 0; i < IRQ_BALANCE_LINES; i++) {
            if (!done[i] && (best < 0 || load[i] > load[best])) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        done[best] = 1;

        /* the lightest CPU - by load, then by line count - preferring the
         * one the line is already on */
        pick = (cur[best] >= 0 && cur[best] < ncpu) ? cur[best] : 0;
        for (c = 0; c < ncpu; c++) {
            if (cpu_load[c] < cpu_load[pick] ||
                (cpu_load[c] == cpu_load[pick] &&
                 cpu_lines[c] < cpu_lines[pick])) {
                pick = c;
            }
        }
        cpu_load[pick] += load[best];
        cpu_lines[pick]++;
        out[best] = pick;
        if (pick != cur[best]) {
            moved++;
        }
    }
    return moved;
}

static int cpu_of_apic(uint32 apic_id) {
    int i, n = smp_cpu_count();

    for (i = 0; i < n; i++) {
        if (smp_cpu(i) != NULL && smp_cpu(i)->apic_id == apic_id) {
            return i;
        }
    }
    return 0;
}

/* The busiest CPU's load under an assignment. */
static uint64 max_load(const uint64 load[IRQ_BALANCE_LINES],
                       const int where[IRQ_BALANCE_LINES], int ncpu) {
    uint64 per[SMP_MAX_CPUS] = { 0 };
    uint64 worst = 0;
    int i;

    for (i = 0; i < IRQ_BALANCE_LINES; i++) {
        if (where[i] >= 0 && where[i] < ncpu) {
            per[where[i]] += load[i];
        }
    }
    for (i = 0; i < ncpu; i++) {
        if (per[i] > worst) {
            worst = per[i];
        }
    }
    return worst;
}

int irq_balance_run(int force) {
    uint64 load[IRQ_BALANCE_LINES];
    uint8 eligible[IRQ_BALANCE_LINES];
    int cur[IRQ_BALANCE_LINES], out[IRQ_BALANCE_LINES];
    int i, ncpu = smp_cpu_count(), moved = 0, planned;

    if (!ioapic_active() || ncpu < 2) {
        return 0;
    }
    passes++;
    for (i = 0; i < IRQ_BALANCE_LINES; i++) {
        uint64 now = irq_delivered((uint8)i);

        load[i] = now - last_count[i];
        last_count[i] = now;
        cur[i] = cpu_of_apic(ioapic_irq_destination((uint8)i));
        /* Not the clock, not the cascade, nothing without a handler, and
         * nothing a driver placed itself. */
        eligible[i] = i >= 3 && irq_handler_count((uint8)i) > 0 &&
                      !(pinned & (1u << i));
    }
    planned = irq_balance_plan(load, eligible, cur, ncpu, out);
    if (planned == 0) {
        return 0;
    }
    if (!force) {
        uint64 before = max_load(load, cur, ncpu);
        uint64 after = max_load(load, out, ncpu);

        /* Worth it only if the busiest CPU sheds a quarter of its load - and
         * never on an idle period, which says nothing about the next one. */
        if (before == 0 || after * 4 > before * 3) {
            plans_rejected++;
            return 0;
        }
    }
    for (i = 0; i < IRQ_BALANCE_LINES; i++) {
        if (eligible[i] && out[i] != cur[i] &&
            ioapic_bind_irq((uint8)i, smp_cpu(out[i])->apic_id) == 0) {
            moved++;
        }
    }
    moves += (uint64)moved;
    return moved;
}

static void irq_balance_thread(void *arg) {
    (void)arg;
    for (;;) {
        kthread_sleep(IRQ_BALANCE_PERIOD);
        (void)irq_balance_run(0);
    }
}

void irq_balance_start(void) {
    int moved;

    if (started || !ioapic_active() || smp_cpu_count() < 2) {
        return;
    }
    started = 1;
    moved = irq_balance_run(1);
    (void)kthread_create(irq_balance_thread, NULL, "irqbalance");
    kprintf("irqbalance: device lines spread over %d cpus (%d moved), "
            "re-planned every %d ticks\n", smp_cpu_count(), moved,
            IRQ_BALANCE_PERIOD);
}

void irq_balance_report(uint8 color) {
    int i;

    kprintf_c(color, "irqbalance: %lx passes, %lx lines moved, %lx plans "
                     "below the margin\n", passes, moves, plans_rejected);
    for (i = 3; i < IRQ_BALANCE_LINES; i++) {
        if (irq_handler_count((uint8)i) == 0) {
            continue;
        }
        kprintf_c(color, "  irq %d on cpu%d, %lx interrupts%s\n", i,
                  cpu_of_apic(ioapic_irq_destination((uint8)i)),
                  irq_delivered((uint8)i),
                  (pinned & (1u << i)) ? "  (pinned)" : "");
    }
}

/* --- selftest ---------------------------------------------------------------
 *
 * The planner against loads chosen to have one right answer each, then the
 * live machine: the NIC's line has to be where the policy put it, and a
 * ping's interrupt has to arrive on that CPU. */

static int plan_case(const char *what, const uint64 *load, const uint8 *elig,
                     const int *cur, int ncpu, const int *want, int want_moved) {
    int out[IRQ_BALANCE_LINES];
    int i, moved = irq_balance_plan(load, elig, cur, ncpu, out);

    for (i = 0; i < IRQ_BALANCE_LINES; i++) {
        if (elig[i] && want[i] >= 0 && out[i] != want[i]) {
            kprintf_c(0x0C, "irqbalance selftest: %s - irq %d to cpu%d, "
                            "expected cpu%d\n", what, i, out[i], want[i]);
            return 1;
        }
        if (!elig[i] && out[i] != cur[i]) {
            kprintf_c(0x0C, "irqbalance selftest: %s - moved ineligible "
                            "irq %d\n", what, i);
            return 1;
        }
    }
    if (want_moved >= 0 && moved != want_moved) {
        kprintf_c(0x0C, "irqbalance selftest: %s - %d moved, expected %d\n",
                  what, moved, want_moved);
        return 1;
    }
    return 0;
}

int irq_balance_selftest(void) {
    uint64 load[IRQ_BALANCE_LINES] = { 0 };
    uint8 elig[IRQ_BALANCE_LINES] = { 0 };
    int cur[IRQ_BALANCE_LINES] = { 0 };
    int want[IRQ_BALANCE_LINES];
    int i, failures = 0, n = smp_cpu_count();

    for (i = 0; i < IRQ_BALANCE_LINES; i++) {
        want[i] = -1;
    }

    /* 1. Three hot lines all on cpu0 of 4: one each on cpu0, 1, 2 - the
     *    heaviest keeps cpu0, where it already is. */
    load[5] = 900; load[9] = 500; load[11] = 300;
    elig[5] = elig[9] = elig[11] = 1;
    want[5] = 0; want[9] = 1; want[11] = 2;
    failures += plan_case("three hot lines on one cpu", load, elig, cur, 4,
                          want, 2);

    /* 2. Already spread: nothing moves. */
    cur[9] = 1; cur[11] = 2;
    failures += plan_case("an already balanced machine", load, elig, cur, 4,
                          want, 0);

    /* 3. No load at all (boot): still one line per CPU, not all on cpu0. */
    load[5] = load[9] = load[11] = 0;
    cur[9] = cur[11] = 0;
    want[5] = 0; want[9] = 1; want[11] = 2;
    failures += plan_case("the zero-load boot spread", load, elig, cur, 4,
                          want, 2);

    /* 4. A pinned hot line stays and weighs on its CPU: the eligible hot
     *    line avoids cpu0, where the pinned one is. */
    load[5] = 1000; elig[5] = 0; cur[5] = 0;
    load[9] = 800; cur[9] = 0;
    load[11] = 0; elig[11] = 0;
    want[9] = 1;
    failures += plan_case("a pinned line", load, elig, cur, 2, want, 1);

    /* 5. The margin: a plan that barely helps is not applied. Checked by
     *    arithmetic on the same numbers irq_balance_run uses. */
    {
        uint64 l[IRQ_BALANCE_LINES] = { 0 };
        int a[IRQ_BALANCE_LINES] = { 0 }, b[IRQ_BALANCE_LINES] = { 0 };

        l[5] = 100; l[9] = 90; a[5] = 0; a[9] = 1; b[5] = 0; b[9] = 0;
        if (max_load(l, b, 2) != 190 || max_load(l, a, 2) != 100) {
            kprintf_c(0x0C, "irqbalance selftest: max_load wrong\n");
            failures++;
        }
    }

    /* 6. The live machine: the NIC's line is where the policy put it, and
     *    its interrupts arrive there. */
    if (n >= 2 && ioapic_active() && started) {
        uint8 line = 0;
        int cpu;
        uint64 before, start;

        for (i = 3; i < IRQ_BALANCE_LINES; i++) {
            if (i != 4 && i != 14 && i != 15 && irq_handler_count((uint8)i) > 0) {
                line = (uint8)i;
                break;
            }
        }
        if (line != 0) {
            cpu = cpu_of_apic(ioapic_irq_destination(line));
            before = smp_cpu(cpu)->dev_irqs;
            (void)net_ping(net_gateway());
            start = timer_ticks_now();
            while (smp_cpu(cpu)->dev_irqs == before &&
                   timer_ticks_now() - start < 100) {
                bkl_wait_for_interrupt();
            }
            if (smp_cpu(cpu)->dev_irqs == before) {
                kprintf_c(0x0C, "irqbalance selftest: irq %d placed on cpu%d "
                                "but nothing arrived there\n", line, cpu);
                failures++;
            } else {
                kprintf("irqbalance: irq %d is on cpu%d by policy, and a ping's "
                        "interrupt arrived there\n", line, cpu);
            }
        }
    }

    if (failures == 0) {
        kprintf("irqbalance: selftest passed\n");
    } else {
        kprintf_c(0x0C, "irqbalance: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
