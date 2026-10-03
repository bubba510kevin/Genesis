#include "dispatch.h"
#include "kprintf.h"
#include "ksleep.h"
#include "kstack.h"
#include "kthread.h"
#include "paging.h"
#include "process.h"
#include "sched.h"
#include "syscall.h"
#include "timer.h"
#include "typesk.h"
#include "waitq.h"
#include "bkl.h"

/* Kernel threads. See kernel/include/kthread.h for what one is, what it
 * unblocks, and the one rule that comes with it (they are not preemptible).
 * What follows is the mechanism.
 *
 * There are exactly three pieces of machinery here and nothing else:
 *
 *   1. A FABRICATED STACK, so that the first switch_context into a thread
 *      that has never run lands in C instead of returning to whatever the
 *      previous occupant of the slot left behind.
 *   2. AN ADDRESS SPACE that is the kernel's, so a kernel thread does not
 *      borrow a user process's page tables and outlive them.
 *   3. A REAP, because a thread cannot free the stack it is standing on.
 *
 * Everything else - readiness, blocking, waking, the tick accounting, the
 * wait queues - is the scheduler's, unchanged, because a kernel thread is a
 * process_t and the scheduler already knew how to schedule one of those.
 */

static int live_count;

/* --- the fabricated stack ------------------------------------------------
 *
 * Mirrors thread_bootstrap_stack (kernel/proc/syscall.c), which does the
 * same job for a thread that will return to ring 3. The difference is the
 * whole difference between the two kinds of thread: that one lands in
 * syscall_return with a user context to pop, this one lands in a C
 * trampoline and never leaves ring 0.
 *
 * From the top down:
 *
 *   [ 0                    ]  <- where kthread_entry would `ret` to
 *   [ &kthread_entry       ]  <- the `ret` in switch_context jumps here
 *   [ RFLAGS = 0x202       ]  <- popped by switch_context's popfq
 *   [ six callee-saved     ]  <- popped by switch_context, all zero
 *   ^ the returned RSP
 *
 * Two details that are easy to get wrong and silent when wrong:
 *
 * RFLAGS IS 0x202, NOT 0x002. Bit 1 is architecturally always set and bit 9
 * is IF. A kernel thread MUST start with interrupts enabled: its only wakers
 * are interrupts, the timer included, so one that starts with IF clear does
 * not run slowly, it stops the machine. This is the opposite choice from
 * thread_bootstrap_stack's 0x002 and for the opposite reason - that stack
 * resumes inside a syscall, which by SFMASK's definition has IF clear.
 *
 * THE TOP WORD IS ALIGNMENT, not just poison. kstack_top is page-aligned, so
 * pushing the return address alone would leave RSP 16-aligned at
 * kthread_entry's first instruction - whereas the ABI says a function entered
 * by `call` sees RSP == 8 (mod 16), because the call pushed eight bytes. The
 * dead word above the return address restores that off-by-eight. It is also a
 * usable poison value: a kthread_entry that somehow returned would fault at
 * RIP 0, which is at least a diagnosable address rather than a jump into the
 * previous occupant's leftovers. */
static void kthread_entry(void);

static uint64 kthread_bootstrap_stack(uint64 kstack_top) {
    uint64 sp = kstack_top;
    int i;

    sp -= 8;
    *(uint64 *)sp = 0;

    sp -= 8;
    *(uint64 *)sp = (uint64)&kthread_entry;

    sp -= 8;
    *(uint64 *)sp = 0x202;

    for (i = 0; i < 6; i++) {
        sp -= 8;
        *(uint64 *)sp = 0;
    }
    return sp;
}

/* The same layout, for proc_create_idle - an idle thread starts exactly the
 * way any other kernel thread does. */
uint64 kthread_boot_stack(uint64 kstack_top) {
    return kthread_bootstrap_stack(kstack_top);
}

/* Where every kernel thread starts.
 *
 * Reached by switch_context's `ret`, not by a call, so it takes no arguments
 * and must not return - the stack above it holds a deliberate zero rather
 * than a caller. The entry point and its argument come off the process
 * instead, which is why they are fields on process_t: a C function can read
 * a struct member and cannot read the %rbx that a fabricated callee-saved
 * slot would have carried them in. */
static void kthread_entry(void) {
    process_t *me = proc_current();

    /* Redundant with the 0x202 in the fabricated RFLAGS above, and kept.
     *
     * The two mechanisms fail differently: if switch_context ever loses its
     * popfq, the flags word becomes dead data and every kernel thread starts
     * with interrupts off - which is a hang with no output, in a file nobody
     * would think to look in. One instruction buys immunity to that. */
    __asm__ volatile ("sti");

    if (me != NULL && me->kentry != NULL) {
        me->kentry(me->karg);
    }
    kthread_exit();
}

/* --- creation ------------------------------------------------------------ */

void kthread_init(void) {
    live_count = 0;
    ksleep_init();
}

process_t *kthread_create(void (*fn)(void *), void *arg, const char *name) {
    process_t *p;

    if (fn == NULL) {
        return NULL;
    }
    if (live_count >= KTHREAD_MAX) {
        /* Refused rather than allowed to eat the last process slot. See
         * kthread.h: kernel threads and user processes share MAX_PROCESSES,
         * and a leak here would otherwise surface as "fork failed" in a
         * shell, which points at exactly the wrong subsystem. */
        kprintf_c(0x0C, "kthread: refusing '%s' - KTHREAD_MAX (%d) reached\n",
                  name != NULL ? name : "?", KTHREAD_MAX);
        return NULL;
    }

    /* ppid 0 on purpose. Nothing has pid 0, so a kernel thread is nobody's
     * child - which is what keeps wait4 from ever seeing one. process.c
     * asserts the same thing from the other side by skipping kernel threads
     * in proc_reap_child and proc_has_children; both, because "no process
     * has pid 0" is a property of the current allocator rather than a
     * guarantee, and the failure it would produce - a shell reaping the ZFS
     * txg thread - is not one you would trace back here. */
    p = proc_alloc(0);
    if (p == NULL) {
        return NULL;
    }

    p->is_kthread = 1;
    /* Anywhere, whoever the creator is: the boot context is pinned to the
     * BSP (process.c), and its kernel threads must not inherit that. A
     * thread that wants a CPU binds itself. */
    p->affinity = ~0ULL;
    p->kentry     = fn;
    p->karg       = arg;
    p->kname      = name != NULL ? name : "kthread";

    /* The KERNEL's address space, named explicitly rather than left NULL to
     * inherit whatever CR3 happened to be loaded.
     *
     * Inheriting would work and would even save a CR3 load per visit - the
     * kernel half is mapped in every space, which is the whole reason a
     * kernel stack is reachable from any of them. What it would also do is
     * leave a kernel thread running on a USER process's page tables, and the
     * day something frees that process while the thread is between two
     * scheduling points is the day the machine faults on its own stack. One
     * CR3 write is cheaper than owning that argument.
     *
     * It also makes proc_free's teardown correct for free: it refuses to
     * destroy vmm_kernel_space(), so a dying kernel thread cannot take the
     * kernel's page tables with it. */
    p->space = vmm_kernel_space();

    /* No user side at all. proc_alloc already zeroed these; written again
     * because a kernel thread having no user context is the point, and the
     * next person to add a field here should see where it belongs. */
    p->entry          = 0;
    p->user_rsp       = 0;
    p->saved_user_rsp = 0;
    p->brk_base       = 0;
    p->brk_current    = 0;
    p->mmap_next      = 0;

    p->thread.saved_rsp = kthread_bootstrap_stack(p->thread.kstack_top);

    /* PROC_READY, so the scheduler can pick it - but the caller keeps the
     * CPU until it reaches a scheduling point of its own. That ordering is
     * what makes "create three threads, then let them run" expressible, and
     * it is why this function does not yield. */
    p->state = PROC_READY;
    sched_enqueue(p);

    live_count++;
    return p;
}

/* --- running ------------------------------------------------------------- */

int kthread_running(void) {
    process_t *me = proc_current();

    return me != NULL && me->is_kthread;
}

process_t *kthread_current(void) {
    process_t *me = proc_current();

    return (me != NULL && me->is_kthread) ? me : NULL;
}

int kthread_count(void) {
    return live_count;
}

void kthread_yield(void) {
    schedule();
}

void kthread_sleep(uint64 ticks) {
    process_t *me = proc_current();

    if (me == NULL) {
        return;
    }
    if (ticks == 0) {
        kthread_yield();
        return;
    }
    sched_sleep_until(me, timer_ticks_now() + ticks);
}

/* --- exit and reclaim ---------------------------------------------------- */

void kthread_exit(void) {
    process_t *me = proc_current();

    if (me != NULL) {
        /* Off any wait queue first, for proc_retire's reason: everything
         * below can schedule, and the window where a dead thread is still on
         * a queue is exactly the window something can wake it. */
        waitq_leave(me);
        /* Mutants it still holds are abandoned now; the reaper's
         * proc_free would get there too, but only when it next runs. */
        dispatch_owner_exited(me->pid);
        if (me->owns_files) {
            handle_close_all(me->handles);
        }
        me->state = PROC_ZOMBIE;
        sched_dequeue(me);
        if (live_count > 0) {
            live_count--;
        }
    }

    /* A ZOMBIE is not runnable, so schedule() will not come back for this
     * thread - but it CAN return immediately if nothing else is runnable
     * right now, which is not the same thing as "never again". Halting with
     * interrupts on is what makes the difference: a timer tick will wake a
     * sleeper, and the loop then hands the CPU over for good.
     *
     * The slot and the stack are still live at this point and stay live
     * until somebody else's schedule() calls kthread_reap - a thread cannot
     * unmap the stack it is standing on. */
    for (;;) {
        schedule();
        bkl_wait_for_interrupt();
    }
}

void kthread_reap(void) {
    process_t *me = proc_current();
    int i;

    for (i = 0; i < proc_slots_used(); i++) {
        process_t *p = proc_at(i);

        if (p == NULL || p == me) {
            continue;
        }
        /* oncpu: a CPU that has not yet switched away from it. With the
         * big kernel lock passed across every switch that window is closed
         * before anyone can get here - but the check is what proc_free
         * relies on, and clearing is_kthread first on a slot proc_free then
         * refused would leave a zombie that no reaper recognises. */
        if (p->is_kthread && !p->is_idle && p->state == PROC_ZOMBIE &&
            !p->oncpu) {
            /* Cleared BEFORE proc_free, because proc_free hands the slot
             * back and the next kthread_create can be handed the same one.
             * A flag left set on a recycled slot would make a user process
             * look like a kernel thread to kern_synch.c, which would then
             * let it deschedule from an interrupt handler. */
            p->is_kthread = 0;
            p->kentry     = NULL;
            p->karg       = NULL;
            p->kname      = NULL;
            proc_free(p);
        }
    }
}

/* --- the bridge for kernel/bsd/ ------------------------------------------
 *
 * Declared in kernel/bsd/compat/sys/systm.h, and int-returning rather than
 * process_t-returning for the reason stated there: a file compiled against
 * the vendored FreeBSD headers cannot see process_t at all. A pid is the
 * narrowest thing that identifies a thread across that boundary.
 *
 * The pid is what a caller compares against genesis_kthread_id(), so the two
 * have to answer in the same currency - which is why neither of them hands
 * back a pointer even though both have one. */
int genesis_kthread_spawn(void (*fn)(void *), void *arg, const char *name) {
    process_t *p = kthread_create(fn, arg, name);

    return (p != NULL) ? p->pid : 0;
}

int genesis_kthread_id(void) {
    process_t *me = kthread_current();

    return (me != NULL) ? me->pid : 0;
}

/* --- report -------------------------------------------------------------- */

void kthread_report(uint8 color) {
    int i;
    int shown = 0;

    for (i = 0; i < proc_slots_used(); i++) {
        process_t *p = proc_at(i);

        if (p == NULL || !p->is_kthread) {
            continue;
        }
        kprintf_c(color, "kthread: [%d] %s  state=%d run=%u sleep=%u\n",
                  p->pid, p->kname != NULL ? p->kname : "?", (int)p->state,
                  (uint32)p->run_ticks, (uint32)p->sleep_ticks);
        shown++;
    }
    if (shown == 0) {
        kprintf_c(color, "kthread: none live (%d/%d slots)\n",
                  live_count, KTHREAD_MAX);
    }
}
