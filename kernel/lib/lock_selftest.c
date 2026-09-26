#include "kprintf.h"
#include "ksmp.h"
#include "klock.h"
#include "typesk.h"

/* Part 11's check.
 *
 * The uncomfortable fact about testing a lock is that on one CPU, with
 * interrupts disabled, a completely empty implementation passes every
 * single-threaded test you can write. lock/unlock/trylock/owned all behave
 * identically whether the xchg is there or not. So this file has two halves:
 *
 *   1. Single-CPU behaviour, which catches API mistakes - a trylock that
 *      leaves interrupts disabled on failure, an unlock that restores the
 *      wrong flags, a reader lock that lets a writer in.
 *
 *   2. A REAL contention test, which only runs with a second CPU and is the
 *      only part that can distinguish a working lock from a no-op.
 *
 * Half 2 needs the AP to execute something. smp.c's AP idles in hlt with no
 * mechanism to hand it work, so this installs one: a function pointer the AP
 * checks, driven by an IPI. That is deliberately the smallest possible
 * remote-execution facility - it is not a scheduler and is not trying to be.
 */

static mtx_t    test_mtx;
static rwlock_t test_rw;

/* The contended counter. Deliberately NOT volatile and NOT atomic: the lock
 * is what makes the increment safe, so if the lock does not work this loses
 * increments, which is exactly the failure being detected. */
static uint64 shared_counter;

#define HAMMER_ITERATIONS 20000

static void hammer(void) {
    int i;

    for (i = 0; i < HAMMER_ITERATIONS; i++) {
        kmtx_lock(&test_mtx);
        shared_counter++;
        kmtx_unlock(&test_mtx);
    }
}

int lock_selftest(void) {
    int failures = 0;
    uint64 flags_before, flags_after;

    kmtx_init(&test_mtx, "selftest");
    krw_init(&test_rw, "selftest_rw");

    /* --- 1. basic exclusion --------------------------------------------- */
    kmtx_lock(&test_mtx);
    if (!kmtx_owned(&test_mtx)) {
        kprintf_c(0x0C, "lock selftest: kmtx_owned false while held\n");
        failures++;
    }
    if (kmtx_trylock(&test_mtx) != 0) {
        kprintf_c(0x0C, "lock selftest: trylock succeeded on a held mtx\n");
        failures++;
    }
    kmtx_unlock(&test_mtx);
    if (kmtx_owned(&test_mtx)) {
        kprintf_c(0x0C, "lock selftest: still owned after unlock\n");
        failures++;
    }
    if (kmtx_trylock(&test_mtx) != 1) {
        kprintf_c(0x0C, "lock selftest: trylock failed on a free mtx\n");
        failures++;
    } else {
        kmtx_unlock(&test_mtx);
    }

    /* --- 2. interrupt state survives ------------------------------------
     *
     * The subtle one. A lock that ends with sti instead of restoring the
     * caller's flags passes every exclusion test and silently enables
     * interrupts inside somebody else's critical section. Checked in both
     * directions, because restoring is only correct if it preserves BOTH
     * states - a version that always disabled would pass a test that only
     * checked the disabled case. */
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(flags_before));
    if (flags_before & 0x200) {
        /* Interrupts were on. They must be on again afterwards. */
        kmtx_lock(&test_mtx);
        kmtx_unlock(&test_mtx);
        __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(flags_after));
        if (!(flags_after & 0x200)) {
            kprintf_c(0x0C, "lock selftest: unlock left interrupts "
                            "DISABLED\n");
            failures++;
        }
    } else {
        /* Interrupts were off. They must stay off - this is the direction
         * an unconditional sti gets wrong. */
        kmtx_lock(&test_mtx);
        kmtx_unlock(&test_mtx);
        __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(flags_after));
        if (flags_after & 0x200) {
            kprintf_c(0x0C, "lock selftest: unlock ENABLED interrupts that "
                            "the caller had off\n");
            failures++;
        }
    }
    /* A failed trylock must restore too, and this is its own case: the
     * failure path is a different branch and is the one that gets forgotten,
     * because a lock that leaks interrupt-disabled state only on contention
     * looks fine until it is contended. */
    kmtx_lock(&test_mtx);
    {
        uint64 f;
        kmtx_trylock(&test_mtx);           /* must fail */
        __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(f));
        (void)f;
    }
    kmtx_unlock(&test_mtx);
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(flags_after));
    if ((flags_before & 0x200) != (flags_after & 0x200)) {
        kprintf_c(0x0C, "lock selftest: a FAILED trylock changed the "
                        "interrupt state\n");
        failures++;
    }

    /* --- 3. reader/writer rules ------------------------------------------ */
    krw_rlock(&test_rw);
    krw_rlock(&test_rw);
    if (test_rw.readers != 2) {
        kprintf_c(0x0C, "lock selftest: two rlocks gave readers=%d\n",
                  test_rw.readers);
        failures++;
    }
    krw_runlock(&test_rw);
    krw_runlock(&test_rw);
    if (test_rw.readers != 0) {
        kprintf_c(0x0C, "lock selftest: readers=%d after unlocking both\n",
                  test_rw.readers);
        failures++;
    }
    krw_wlock(&test_rw);
    if (test_rw.readers != -1) {
        kprintf_c(0x0C, "lock selftest: a writer did not take it "
                        "exclusively\n");
        failures++;
    }
    krw_wunlock(&test_rw);

    /* --- 4. real contention ---------------------------------------------
     *
     * Both CPUs increment the same non-atomic counter through the lock. With
     * a working lock the total is exact; with a broken one increments are
     * lost, and the count is the evidence.
     *
     * The BSP hammers while the AP hammers, so the windows genuinely
     * overlap - starting the AP and then waiting for it would test nothing
     * but sequencing. */
    shared_counter = 0;

    if (smp_cpu_count() > 1) {
        smp_run_on_aps(hammer);
        hammer();
        smp_wait_for_aps();

        {
            uint64 want = (uint64)HAMMER_ITERATIONS * (uint64)smp_cpu_count();

            if (shared_counter != want) {
                kprintf_c(0x0C, "lock selftest: counter is %lx, wanted %lx - "
                                "%lx increments were LOST\n",
                          shared_counter, want, want - shared_counter);
                failures++;
            } else {
                kprintf("lock: %lx contended increments across %d cpus, "
                        "none lost\n", want, smp_cpu_count());
            }
        }
        if (test_mtx.contended == 0) {
            /* The counter can come out right by luck if the two CPUs never
             * actually overlapped. A zero contention count means the test
             * ran but proved nothing, and saying so is better than a green
             * line that means less than it appears to. */
            kprintf_c(0x0E, "lock selftest: the lock was never contended - "
                            "the count above proves less than it looks\n");
        }
    } else {
        hammer();
        if (shared_counter != HAMMER_ITERATIONS) {
            kprintf_c(0x0C, "lock selftest: uncontended count is wrong\n");
            failures++;
        }
        kprintf_c(0x0E, "lock selftest: one cpu - contention NOT tested, "
                        "which is the only half that can tell a working "
                        "lock from a no-op (try -smp 2)\n");
    }

    if (failures == 0) {
        kprintf("lock: selftest passed\n");
    } else {
        kprintf_c(0x0C, "lock: selftest FAILED (%d)\n", failures);
    }
    return failures;
}

void lock_report(uint8 color) {
    kprintf_c(color, "locks: selftest mtx  acquisitions %lx  contended %lx\n",
              test_mtx.acquisitions, test_mtx.contended);
}
