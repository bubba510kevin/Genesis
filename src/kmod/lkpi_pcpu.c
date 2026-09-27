/* A LOADED module with per-CPU variables - the case the kernel's own
 * lkpi_smp_selftest cannot cover, because its DEFINE_PER_CPU lives in the
 * kernel image. Here the lkpi_percpu section is in a separately relocated
 * ELF object, and kldload has to place it in the per-CPU area and replicate
 * it (kernel/driver/kldload.c) for per_cpu() to mean anything.
 *
 * Loaded at boot, before the other CPUs join the scheduler, so the check
 * runs as a late_initcall - once they have. It prints one line either way;
 * the boot log is the result. */

#include "linux/module.h"
#include "linux/kernel.h"
#include "linux/smp.h"
#include "linux/percpu.h"
#include "linux/cpumask.h"

/* Initialised, so the replication has to carry data and not just zeroes. */
static DEFINE_PER_CPU(long, pmod_counter) = 5;
static DEFINE_PER_CPU(int, pmod_zeroed);

static void bump(void *info) {
    (void)info;
    this_cpu_add(pmod_counter, smp_processor_id() + 1);
    this_cpu_inc(pmod_zeroed);
}

static int pmod_check(void) {
    int cpu, bad = 0, n = 0;

    on_each_cpu(bump, NULL, 1);
    for_each_online_cpu(cpu) {
        n++;
        if (per_cpu(pmod_counter, cpu) != 5 + cpu + 1) {
            bad++;
        }
        if (per_cpu(pmod_zeroed, cpu) != 1) {
            bad++;
        }
        if (cpu > 0 && &per_cpu(pmod_counter, cpu) == &per_cpu(pmod_counter, 0)) {
            bad++;
        }
    }
    if (bad == 0) {
        printk("lkpi pcpu module: passed - %d cpus, each with its own copy\n", n);
    } else {
        printk("lkpi pcpu module: FAILED - %d wrong per-CPU values\n", bad);
    }
    return 0;
}

late_initcall(pmod_check);
