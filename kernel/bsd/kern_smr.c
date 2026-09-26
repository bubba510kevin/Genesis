/* SMR - Safe Memory Reclamation.
 *
 * --- what problem this solves -------------------------------------------
 * netinet/in_pcb.c looks up a socket by 4-tuple WITHOUT taking a lock: it
 * walks the PCB hash inside smr_enter()/smr_exit(), reads the entry it wants,
 * and only then tries to lock it. That is the hot path of every arriving UDP
 * datagram and every TCP segment, and taking a lock on it would serialise the
 * whole receive path behind one hash bucket.
 *
 * The unlocked read is safe only if a PCB's MEMORY cannot be reused while a
 * reader still holds a pointer into it. SMR is the mechanism: a free does not
 * return the memory to the allocator immediately, it stamps it with a
 * sequence number, and the allocator withholds it until every reader that
 * could have been running at that moment has left its read section.
 *
 * --- what this file is, and where it is coarser -------------------------
 * Upstream's is per-CPU and tracks a sequence number per reader, so a writer
 * waits only for readers that started BEFORE its free. This one tracks a
 * per-CPU read-section DEPTH and waits for every CPU to be at zero.
 *
 * That is conservative in the safe direction: it waits for at least as long
 * as upstream would, never less. What it costs is a writer that occasionally
 * waits for a reader it did not have to. With two CPUs and a read section
 * that is a hash-bucket walk, that is a handful of instructions.
 *
 * It replaces a shim that PANICKED - "smr_create reached, UMA_ZONE_SMR needs
 * real SMR" - which was the right thing to have while nothing created such a
 * zone, and became a halted boot the moment in_pcb.c did.
 *
 * --- the one hazard, stated ---------------------------------------------
 * A writer must not call smr_wait() from inside its own read section: it
 * would wait for itself. Upstream forbids that too, and this cannot detect it
 * cheaply, so the spin below is BOUNDED and reports rather than hanging. A
 * bounded wait that is exceeded means either that rule was broken or that a
 * reader is stuck, and both are worth a line on the console.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/smr.h>

#include "ksmp.h"
#include "kheap.h"
#include "kprintf.h"

/* One per SMR domain. UMA creates one per SMR zone; in_pcb.c creates one for
 * the TCP and UDP PCB tables. */
struct smr {
    volatile smr_seq_t s_seq;       /* advanced on every deferred free */
    const char        *s_name;
};

/* Read-section depth per CPU. Not a count of readers - a count of nested
 * ENTRIES, because smr_enter/smr_exit nest and a decrement to zero is what
 * says this CPU is quiescent.
 *
 * Indexed by smp_this_cpu()->index rather than by APIC id: the index is dense
 * and bounded by SMP_MAX_CPUS, which is what makes the scan below cheap. */
static volatile int smr_depth[SMP_MAX_CPUS];

/* How long a writer waits for readers to drain, in spins. Generous: a read
 * section here is a hash-bucket walk, so anything approaching this means
 * something is wrong rather than slow. */
#define SMR_WAIT_SPINS  10000000

static int smr_stuck_reported;

smr_t smr_create(const char *name, int limit, int flags) {
    struct smr *s;

    (void)limit;
    (void)flags;
    s = (struct smr *)kmalloc(sizeof(*s));
    if (s == NULL) {
        return (NULL);
    }
    /* Sequence numbers start at 1, not 0: SMR_SEQ_INVALID is 0 and UMA stores
     * it in a bucket to mean "this bucket carries no deferred frees". A
     * domain whose first advance produced 0 would be indistinguishable from
     * one that had never been used. */
    s->s_seq = 1;
    s->s_name = name;
    return (s);
}

void smr_destroy(smr_t smr) {
    kfree(smr);
}

void smr_enter_impl(smr_t smr) {
    (void)smr;
    /* Increment BEFORE the caller reads anything, and let the compiler know:
     * a reader whose depth increment is reordered after its first load is a
     * reader the writer cannot see. */
    smr_depth[smp_this_cpu()->index]++;
    __asm__ volatile ("" ::: "memory");
}

void smr_exit_impl(smr_t smr) {
    (void)smr;
    __asm__ volatile ("" ::: "memory");
    smr_depth[smp_this_cpu()->index]--;
}

/* Stamp a free. The returned sequence is what the caller stores with the
 * memory it is deferring; smr_poll() against it later says whether the memory
 * may be reused. */
smr_seq_t smr_advance(smr_t smr) {
    smr_seq_t seq;

    if (smr == NULL) {
        return (SMR_SEQ_INVALID);
    }
    __asm__ volatile ("" ::: "memory");
    seq = ++smr->s_seq;
    if (seq == SMR_SEQ_INVALID) {
        seq = ++smr->s_seq;     /* skip the reserved value on wrap */
    }
    return (seq);
}

/* Is every CPU out of its read section?
 *
 * The goal sequence is IGNORED, and that is the coarsening this file's header
 * describes: upstream compares each CPU's observed sequence against the goal
 * so a reader that started after the free does not delay it. Here a reader is
 * a reader. Waiting longer than necessary is safe; waiting less is not. */
static int all_quiescent(void) {
    int i;

    for (i = 0; i < SMP_MAX_CPUS; i++) {
        if (smr_depth[i] != 0) {
            return (0);
        }
    }
    return (1);
}

int smr_poll(smr_t smr, smr_seq_t goal, bool wait) {
    long spins;

    (void)smr;
    (void)goal;

    if (all_quiescent()) {
        return (1);
    }
    if (!wait) {
        return (0);
    }
    for (spins = 0; spins < SMR_WAIT_SPINS; spins++) {
        __asm__ volatile ("pause" ::: "memory");
        if (all_quiescent()) {
            return (1);
        }
    }
    /* Bounded, and loud. Reported once rather than every time, because if it
     * happens at all it will happen on every free afterwards and a flood
     * would bury the line that says why. */
    if (!smr_stuck_reported) {
        smr_stuck_reported = 1;
        kprintf_c(0x0C, "smr: readers did not drain - either a writer waited "
                        "inside its own read section, or a read section is "
                        "stuck. Deferred frees are proceeding UNSAFELY.\n");
    }
    return (1);
}

void smr_wait(smr_t smr, smr_seq_t goal) {
    (void)smr_poll(smr, goal, true);
}

void smr_synchronize(smr_t smr) {
    smr_wait(smr, smr_advance(smr));
}

void smr_init(void) {
    int i;

    for (i = 0; i < SMP_MAX_CPUS; i++) {
        smr_depth[i] = 0;
    }
}
