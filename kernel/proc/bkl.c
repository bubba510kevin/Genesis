#include "bkl.h"
#include "ksmp.h"
#include "typesk.h"

/* The big kernel lock. See bkl.h for the rules and ksmp.h for why the kernel
 * is serialised by one lock at all.
 *
 * A ticket lock: `next` is the ticket dispenser, `serving` the one being
 * served. A CPU takes a ticket with one atomic increment and waits until its
 * number comes up, so the order of entry is the order of arrival. A plain
 * test-and-set lock lets the CPU that just released it win the next race
 * almost every time (its cache line is hot), and a thread making syscalls in
 * a loop would then keep every other CPU out of the kernel indefinitely.
 *
 * `owner` is for reports and assertions only; correctness rests on the
 * ticket pair. */
static volatile uint32 bkl_next;
static volatile uint32 bkl_serving;
static volatile int    bkl_owner_cpu = -1;
static uint64          bkl_waits;

void bkl_init(void) {
    bkl_next = 0;
    bkl_serving = 0;
    bkl_owner_cpu = -1;
}

static uint64 flags_save_cli(void) {
    uint64 f;

    __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(f) : : "memory");
    return f;
}

static void flags_restore(uint64 f) {
    __asm__ volatile ("pushq %0\n\tpopfq" : : "r"(f) : "memory", "cc");
}

void bkl_acquire(void) {
    struct cpu_local *c = smp_this_cpu();
    uint32 ticket;
    uint64 f;

    if (c->bkl_depth > 0) {
        c->bkl_depth++;
        return;
    }
    /* Interrupts off for the wait, whatever the caller had: an interrupt
     * taken while this CPU is between "have a ticket" and "being served"
     * would itself try to acquire (depth is still 0), take a SECOND ticket
     * behind its own first one, and wait forever. */
    f = flags_save_cli();
    ticket = __atomic_fetch_add(&bkl_next, 1, __ATOMIC_ACQ_REL);
    if (bkl_serving != ticket) {
        bkl_waits++;
        c->bkl_spins++;
        while (__atomic_load_n(&bkl_serving, __ATOMIC_ACQUIRE) != ticket) {
            /* The holder may be waiting on THIS CPU - a TLB shootdown it
             * cannot finish until every CPU acknowledges, a remote call it
             * is waiting to see complete. With interrupts off the IPI for it
             * cannot be taken, so the mailbox is answered from here. */
            smp_poll_mailboxes();
            /* And the pre-scheduler work facility (smp_run_on_aps): an AP
             * spends the whole of boot here, queued behind the BSP, and the
             * BSP's lock selftest is waiting for it to run exactly that. */
            smp_idle_poll_work();
            __asm__ volatile ("pause");
        }
    }
    bkl_owner_cpu = (int)c->index;
    c->bkl_depth = 1;
    flags_restore(f);
}

void bkl_release(void) {
    struct cpu_local *c = smp_this_cpu();

    if (c->bkl_depth <= 0) {
        return;                 /* not held: releasing is a no-op, not a crash */
    }
    if (--c->bkl_depth == 0) {
        bkl_owner_cpu = -1;
        __atomic_fetch_add(&bkl_serving, 1, __ATOMIC_RELEASE);
    }
}

int bkl_release_all(void) {
    struct cpu_local *c = smp_this_cpu();
    int depth = c->bkl_depth;

    if (depth > 0) {
        c->bkl_depth = 1;
        bkl_release();
    }
    return depth;
}

void bkl_restore(int depth) {
    struct cpu_local *c = smp_this_cpu();

    if (depth <= 0) {
        return;
    }
    bkl_acquire();
    c->bkl_depth = depth;
}

int bkl_held(void) {
    return smp_this_cpu()->bkl_depth > 0;
}

void bkl_exit_to_user(void) {
    struct cpu_local *c = smp_this_cpu();

    if (c->bkl_depth > 0) {
        c->bkl_depth = 1;
        bkl_release();
    }
}

void bkl_wait_for_interrupt(void) {
    uint64 f = flags_save_cli();
    int depth = bkl_release_all();

    /* sti; hlt as one pair: sti's one-instruction shadow means an interrupt
     * already pending is taken AFTER the hlt starts, so it wakes the hlt
     * instead of being consumed in the gap and leaving the CPU halted with
     * the wakeup already spent. */
    __asm__ volatile ("sti; hlt; cli" : : : "memory");
    bkl_restore(depth);
    flags_restore(f);
}

uint64 bkl_contended(void) {
    return bkl_waits;
}

int bkl_owner(void) {
    return bkl_owner_cpu;
}
