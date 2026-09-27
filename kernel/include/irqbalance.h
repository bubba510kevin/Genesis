#ifndef IRQBALANCE_H
#define IRQBALANCE_H

#include "typesk.h"

/* Interrupt balancing: which CPU each device line is delivered to.
 *
 * ioapic_bind_irq is the mechanism; this is the policy. At boot every line
 * with a handler is spread across the online CPUs, and afterwards a kernel
 * thread ("irqbalance") re-plans every IRQ_BALANCE_PERIOD ticks from how many
 * interrupts each line took since the last look: the hottest line goes first,
 * each to the CPU carrying the least interrupt load so far. A plan is applied
 * only when it cuts the busiest CPU's load by at least a quarter, so two lines
 * of similar rate do not swap back and forth every period.
 *
 * A line a driver bound on purpose (bus_bind_intr) is PINNED and the policy
 * never moves it - an explicit request beats a heuristic. The clock (IRQ 0)
 * stays on the BSP, which keeps time. */

#define IRQ_BALANCE_LINES  16
#define IRQ_BALANCE_PERIOD 200            /* ticks - two seconds at 100Hz */

/* The planner, pure: `load[i]` interrupts on line i this period (lines with
 * `eligible[i]` zero are ignored and keep `cur[i]`), `cur[i]` the CPU index
 * each line is on now. Writes the chosen CPU into `out[i]` for every line and
 * returns how many eligible lines would move. `ncpu` CPUs, indices 0..ncpu-1. */
int irq_balance_plan(const uint64 load[IRQ_BALANCE_LINES],
                     const uint8 eligible[IRQ_BALANCE_LINES],
                     const int cur[IRQ_BALANCE_LINES], int ncpu,
                     int out[IRQ_BALANCE_LINES]);

/* One pass over the live lines. `force` applies the plan even without the
 * hysteresis margin (the boot-time spread). Returns the lines moved. */
int  irq_balance_run(int force);

/* Boot: spread the lines, then start the rebalancing thread. */
void irq_balance_start(void);

/* A driver's explicit binding: never moved by the policy afterwards. */
void irq_balance_pin(uint8 irq);

int  irq_balance_selftest(void);
void irq_balance_report(uint8 color);

#endif
