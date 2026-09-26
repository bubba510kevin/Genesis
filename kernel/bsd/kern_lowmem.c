/* Memory pressure, and the thing that tells a cache to shrink.
 *
 * --- ROADMAP item 7's fourth blocker, and what it actually was ------------
 *
 * The entry reads: "A REAL kmem_cache WITH RECLAIM. UMA is vendored so the
 * allocator exists, but nothing raises vm_lowmem - and the ARC is a cache
 * that is only correct if something can tell it to shrink."
 *
 * The reclaim machinery was never the missing part. uma_core.c ships all of
 * it: uma_reclaim_wakeup sets a flag, uma_reclaim_worker invokes
 * EVENTHANDLER_INVOKE(vm_lowmem, VM_LOW_KMEM) and then drains the zones. What
 * was missing was a NOTICER - nothing in this kernel ever looked at how much
 * memory was left and said so. uma_vendor.c's own comment had narrowed it to
 * exactly that: "What was missing is now the trigger rather than the context."
 *
 * This is the trigger.
 *
 * --- why a thread and not a hook in the allocator -------------------------
 *
 * The obvious design is to fire from pmm_alloc_frame when it starts running
 * short. That is wrong here for a reason worth writing down: reclaim CALLS
 * FREE, and the vm_lowmem handlers are arbitrary subsystem code (UMA's zone
 * drain today, the ARC later) that allocates and frees while it runs. Firing
 * from inside the allocator means re-entering it from its own failure path,
 * with whatever lock it holds still held.
 *
 * Upstream has exactly this split for the same reason: the page daemon is a
 * thread, and the allocator only ever wakes it. A thread also means reclaim
 * can BLOCK, which a handler draining a zone with a sleepable lock must be
 * allowed to do - and kernel threads did not exist when this item was
 * written, which is why it is only buildable now.
 *
 * --- the threshold, and why it is a fraction rather than a number ---------
 *
 * An absolute floor ("fire below 1024 free frames") is wrong on both ends: on
 * a small machine it never stops firing, on a large one it fires far too
 * late. A fraction of total is scale-free, and one eighth is the point where
 * there is still enough left to run the reclaim itself - which allocates.
 *
 * Firing is RATE LIMITED to once a second, upstream's number and for
 * upstream's reason: a handler that frees nothing (because there is genuinely
 * nothing cached) must not turn a low-memory condition into a spin that
 * consumes the CPU the machine needs to recover.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/eventhandler.h>
#include <vm/uma.h>

#include "kprintf.h"
#include "pmm.h"
#include "kheap.h"

/* Declared rather than included: kernel/include/kthread.h reaches process.h
 * and its `struct thread`, which collides with <sys/proc.h>'s. The same
 * arrangement kern_taskqueue.c uses. */
int  genesis_kthread_spawn(void (*fn)(void *), void *arg, const char *name);
void kthread_sleep(uint64_t ticks);

/* kernel/fs/pcache.c. The file page cache is the other cache in this kernel
 * that has to be told to shrink, and it is not a UMA zone, so uma_reclaim
 * does not reach it. Declared rather than included for the usual reason. */
int  pcache_reclaim(void);

/* Counted so the selftest can tell "the trigger fired" from "the trigger was
 * called and decided not to", which are different answers and look the same
 * from outside. */
static uint64_t lowmem_fires;
static uint64_t lowmem_last_tick;
static int      lowmem_thread_pid;

uint64_t genesis_lowmem_fire_count(void) {
    return lowmem_fires;
}

/* Is this machine under memory pressure?
 *
 * A pure function of two numbers, separated from everything else in this file
 * SO THAT IT CAN BE TESTED. Driving the real allocator to one eighth free in
 * a selftest means allocating most of the machine's memory, which is a test
 * that either fails to reproduce or takes the kernel down with it; the
 * decision is the part worth checking, and this is it. */
int genesis_lowmem_should_fire(uint64_t free_frames, uint64_t total_frames) {
    if (total_frames == 0) {
        return 0;                        /* nothing known, nothing to say */
    }
    return free_frames < (total_frames / 8);
}

/* Tell every cache in the kernel to shrink, now.
 *
 * NOT rate limited - the limiting lives in the poll loop below, so that this
 * entry point stays usable as "reclaim right now" by a caller that has a
 * reason (the selftest, and eventually an allocation that has genuinely just
 * failed). Splitting them that way is what lets the rate limit be tested
 * separately from the reclaim. */
void genesis_lowmem_fire(void) {
    lowmem_fires++;

    /* The event first, then UMA's own drain. Order matters: a handler may
     * free objects back INTO uma zones - that is what the ARC's eviction
     * does - and draining the zones first would leave exactly those objects
     * cached. Upstream's uma_reclaim_worker does the same two steps in the
     * same order. */
    EVENTHANDLER_INVOKE(vm_lowmem, VM_LOW_KMEM);
    uma_reclaim(UMA_RECLAIM_DRAIN);

    /* The page cache last, and not through the event, because it is not a
     * vm_lowmem subscriber - it is part of the kernel rather than a
     * subsystem that registered. Last because the handlers above may free
     * objects that were read through it, and dropping its pages first would
     * mean re-reading them to satisfy work that is about to be discarded. */
    (void)pcache_reclaim();
}

/* One pass of the watcher. Split out so the selftest can drive it without a
 * thread and without waiting a second for the rate limit.
 *
 * TWO POOLS ARE WATCHED, not one, and that is not belt-and-braces. The
 * machine's memory is physical frames, but UMA's slabs come from the KERNEL
 * HEAP (genesis_kmem_malloc is kmalloc_a - see uma_vendor.c), and so will the
 * ARC's buffers. The heap commits frames from the PMM as it grows, so heap
 * exhaustion arrives FIRST and is the pressure a cache can actually do
 * something about; frame exhaustion is the machine-wide one that arrives
 * later and matters more. Watching only frames would mean never firing until
 * the heap had already failed an allocation. */
void genesis_lowmem_poll(void) {
    uint64_t now = (uint64_t)ticks;
    kh_size  used = 0, freeb = 0, committed = 0;
    int      low;

    kheap_stats(&used, &freeb, &committed);

    low = genesis_lowmem_should_fire(pmm_free_frames(), pmm_total_frames()) ||
          genesis_lowmem_should_fire((uint64_t)freeb,
                                     (uint64_t)(used + freeb));

    if (!low) {
        return;
    }
    /* Once a second at most. `ticks` is signed and wraps; the subtraction is
     * done in unsigned and compared against the interval, which is the form
     * that survives the wrap - `now > last + hz` is the form that does not. */
    if (lowmem_fires != 0 && (now - lowmem_last_tick) < (uint64_t)hz) {
        return;
    }
    lowmem_last_tick = now;
    genesis_lowmem_fire();
}

static void lowmem_thread(void *arg) {
    (void)arg;
    for (;;) {
        /* Ten times a second. Often enough that a burst of allocation is
         * noticed before it exhausts the machine, rare enough that the check
         * itself - two loads and a compare - costs nothing measurable. */
        kthread_sleep((uint64_t)(hz / 10 > 0 ? hz / 10 : 1));
        genesis_lowmem_poll();
    }
}

void genesis_lowmem_init(void) {
    lowmem_thread_pid = genesis_kthread_spawn(lowmem_thread, NULL, "lowmem");
    if (lowmem_thread_pid == 0) {
        /* Reported, not silent. Without the thread the kernel still runs and
         * still reclaims when something calls genesis_lowmem_fire by hand -
         * it just stops noticing on its own, which is precisely the state
         * this file exists to leave behind. */
        kprintf_c(0x0C, "lowmem: no kernel thread - pressure will NOT be "
                        "noticed automatically\n");
    }
}

void genesis_lowmem_report(unsigned char color) {
    uint64_t total = pmm_total_frames();
    uint64_t freef = pmm_free_frames();

    kprintf_c(color, "lowmem: %lx of %lx frames free, threshold %lx, "
                     "fired %lx time%s\n",
              freef, total, total / 8, lowmem_fires,
              lowmem_fires == 1 ? "" : "s");
}

/* --- is the trigger real? -------------------------------------------------
 *
 * Three things to establish, and the third is the only one that says reclaim
 * WORKS rather than that it RAN.
 *
 * THE DECISION, both directions. A threshold function that answered "yes"
 * always would pass a test that only checked the low case, and would turn the
 * watcher into a permanent reclaim loop.
 *
 * THE EVENT REACHES A HANDLER. Registered through the ordinary
 * eventhandler_register path, so what is tested is the path the ARC will use
 * rather than a direct call this file could make to itself.
 *
 * IT ACTUALLY RETURNS MEMORY. A UMA zone is filled and emptied - which leaves
 * its slabs cached, not freed, because that is what a cache does - and then
 * reclaim is fired and the machine's FREE FRAME COUNT must go up. That is the
 * measurement, and it is the one an implementation that fires the event and
 * forgets to drain would fail while passing everything else.
 */

static int  lowmem_test_calls;
static void lowmem_test_handler(void *arg, int flags) {
    (void)arg;
    (void)flags;
    lowmem_test_calls++;
}

int genesis_lowmem_selftest(void) {
    int failures = 0;
    uint64_t before, after;
    uma_zone_t z;
    void *items[256];
    int i;

#define LMT(cond, what)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            kprintf_c(0x0C, "lowmem: %s\n", (what));                        \
            failures++;                                                     \
        }                                                                   \
    } while (0)

    /* --- the decision ---------------------------------------------------- */
    LMT(genesis_lowmem_should_fire(1000, 1000) == 0,
        "a machine with everything free is reported as under pressure");
    LMT(genesis_lowmem_should_fire(500, 1000) == 0,
        "half free is not pressure");
    LMT(genesis_lowmem_should_fire(100, 1000) == 1,
        "one tenth free is NOT reported as pressure");
    LMT(genesis_lowmem_should_fire(0, 0) == 0,
        "an unknown total is treated as pressure");

    /* --- the event reaches a handler ------------------------------------- */
    lowmem_test_calls = 0;
    if (EVENTHANDLER_REGISTER(vm_lowmem, lowmem_test_handler, NULL,
                              EVENTHANDLER_PRI_ANY) == NULL) {
        LMT(0, "could not register a vm_lowmem handler");
    }
    LMT(lowmem_test_calls == 0, "the handler ran before anything fired");

    /* --- and reclaim actually returns memory ----------------------------- */
    z = uma_zcreate("lowmem-test", 512, NULL, NULL, NULL, NULL,
                    UMA_ALIGN_PTR, 0);
    if (z == NULL) {
        LMT(0, "could not create a zone to reclaim");
        goto done;
    }
    for (i = 0; i < 256; i++) {
        items[i] = uma_zalloc(z, M_NOWAIT);
    }
    for (i = 0; i < 256; i++) {
        if (items[i] != NULL) {
            uma_zfree(z, items[i]);
        }
    }

    /* MEASURED ON THE HEAP, not on physical frames, and getting that wrong
     * is what made the first version of this check toothless. UMA's slabs
     * come from kmalloc_a (uma_vendor.c's genesis_kmem_malloc), so reclaim
     * hands memory back to the KERNEL HEAP - the frame count does not move
     * at all, and the check reported "nothing cached" while sitting next to
     * a zone that had 128KB cached. A measurement aimed at the wrong pool
     * cannot fail, which is worse than no measurement. */
    {
        kh_size used_before = 0, used_after = 0, f = 0, c = 0;

        kheap_stats(&used_before, &f, &c);
        genesis_lowmem_fire();
        kheap_stats(&used_after, &f, &c);

        before = (uint64_t)used_before;
        after  = (uint64_t)used_after;

        LMT(lowmem_test_calls == 1, "the handler did not run exactly once");
        LMT(after <= before, "reclaim made the heap BIGGER");
        LMT(after < before,
            "reclaim freed nothing - the zone had 256 items cached");
        if (after < before) {
            kprintf("lowmem: reclaim returned %lx heap bytes\n",
                    before - after);
        }
    }

done:
    if (failures == 0) {
        kprintf("lowmem: selftest passed\n");
    } else {
        kprintf_c(0x0C, "lowmem: selftest FAILED (%d)\n", failures);
    }
    return failures;
#undef LMT
}
