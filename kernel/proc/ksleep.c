#include "ksleep.h"
#include "ksmp.h"
#include "kthread.h"
#include "process.h"
#include "sched.h"
#include "timer.h"
#include "typesk.h"
#include "waitq.h"

/* See kernel/include/ksleep.h for why this file exists on this side of the
 * header split. What follows is the block itself.
 *
 * One wait queue per channel slot, sharing kern_synch.c's hash. Two unrelated
 * channels can land in the same slot, which produces SPURIOUS wakeups and
 * never lost ones - the caller re-tests its generation, which is exactly what
 * makes sharing safe, and is the same argument the hash was chosen under.
 */
static wait_queue_t chan_q[KSLEEP_SLOTS];

void ksleep_init(void) {
    int i;

    for (i = 0; i < KSLEEP_SLOTS; i++) {
        waitq_init(&chan_q[i]);
    }
}

/* Save RFLAGS and disable interrupts; restore. Duplicated from mtx.c rather
 * than shared, matching this tree's preference for a trivial helper per file
 * over a header every file has to know about. */
static uint64 intr_disable(void) {
    uint64 flags;

    __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static void intr_restore(uint64 flags) {
    __asm__ volatile ("pushq %0\n\tpopfq" : : "r"(flags) : "memory", "cc");
}

int ksleep_can_block(void) {
    struct cpu_local *c = smp_this_cpu();
    process_t *me = c->current;

    if (me == NULL || me->is_idle || c->irq_depth > 0) {
        return 0;
    }
    /* A kernel thread can always be switched away from. A process in a
     * syscall can once its CPU has an idle thread to fall back to - before
     * smp_start_scheduling a block with nothing else runnable would come
     * straight back out of schedule() still marked blocked. */
    return me->is_kthread || c->idle != NULL;
}

struct gen_ctx {
    const volatile uint32 *gen;
    uint32                 seen;
};

static int gen_moved(void *ctx) {
    struct gen_ctx *c = (struct gen_ctx *)ctx;

    return *c->gen != c->seen;
}

int ksleep_wait(unsigned int slot, const volatile uint32 *gen, uint32 seen,
                uint32 timo) {
    struct gen_ctx ctx;
    uint64 flags;
    uint64 deadline;
    int r;

    if (slot >= KSLEEP_SLOTS || gen == NULL) {
        return 0;
    }
    if (!ksleep_can_block()) {
        /* Not blockable. Say so by answering "nothing changed" rather than
         * blocking a context with nothing to deschedule; the caller's loop
         * then falls through to its own idle. */
        return 0;
    }
    ctx.gen  = gen;
    ctx.seen = seen;
    deadline = (timo > 0) ? timer_ticks_now() + timo : 0;

    /* --- why this runs with interrupts off ---------------------------------
     *
     * A kernel thread runs with IF SET - it has to, since its only wakers are
     * interrupts. That is what opens a window every other waiter in this
     * kernel is closed against by accident: a syscall path enters through
     * SYSCALL with IF already clear (MSR_SFMASK), so between "test the
     * condition" and "go on the queue and block" nothing can run. Here it
     * can, and the sequence that loses is:
     *
     *   1. waitq_wait_until calls gen_moved()          - unchanged
     *   2. the NIC interrupt fires, wakeup() bumps the
     *      generation and calls ksleep_wake(slot)      - queue is empty
     *   3. waitq_add() puts this thread on the queue
     *   4. sched_block() blocks it, on a wakeup that has already been
     *      delivered to nobody
     *
     * The thread then sleeps to its deadline. Not a hang - kern_synch.c caps
     * every sleep - but a ten-second stall on a packet that arrived, which is
     * exactly the class of bug that is invisible on an idle machine and
     * ruinous under load.
     *
     * Disabling interrupts across the whole wait closes it. That is only
     * legal because switch_context saves and restores RFLAGS (see
     * kernel/proc/syscall.c): the thread this switches to resumes with its
     * OWN interrupt state, not with the IF=0 established here, so blocking
     * with interrupts off does not disable interrupts for the machine. It
     * disables them for this thread, which is what was wanted.
     *
     * waitq_wait_until's internal `sti; hlt; cli` - the path taken when
     * nothing else was runnable - is unaffected: it restores IF=0 on the way
     * out, which is the state it found. */
    flags = intr_disable();
    r = waitq_wait_until(&chan_q[slot], gen_moved, &ctx, deadline);
    intr_restore(flags);

    /* WAITQ_READY(1) means the generation moved. WAITQ_TIMEOUT(-1) and
     * WAITQ_SIGNAL(0) both mean it did not, and the caller's own loop is what
     * distinguishes them - it re-reads the generation and re-checks its own
     * deadline, so there is nothing here that needs to. */
    return r == WAITQ_READY;
}

void ksleep_wake(unsigned int slot) {
    if (slot >= KSLEEP_SLOTS) {
        return;
    }
    /* waitq_wake_all also wakes the readiness queue, so every wakeup(9) in
     * the kernel now wakes every poll(2) waiter as well. That is the cost
     * waitq.h describes and accepts - a spurious wake is a scan of at most
     * MAX_HANDLES entries, a missed one is a hang - and it is the correct
     * side to err on here rather than an oversight: a socket wakeup very
     * often IS a readiness change for a descriptor, and there is no socket
     * fd yet (ROADMAP item 6) for this to be measured against. */
    waitq_wake_all(&chan_q[slot]);
}
