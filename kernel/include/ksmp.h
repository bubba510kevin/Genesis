#ifndef SMP_H
#define SMP_H

#include "typesk.h"

/* Symmetric multiprocessing - ROADMAP item 13.
 *
 * --- what this kernel does with more than one CPU --------------------------
 *
 * Every CPU runs user processes. Each has its own current thread, its own
 * idle thread, its own run-state (resched flag, time slice), its own LAPIC
 * timer for preemption, its own TSS and kernel-stack slot, and its own
 * notion of which address space is loaded. Threads of one process run on
 * several CPUs at once; the scheduler honours a per-thread affinity mask.
 *
 * The KERNEL is serialised by one lock - the big kernel lock, bkl.h - which
 * is taken on every entry from ring 3 and released on every exit to it. That
 * is the Giant / BKL model FreeBSD 5 and Linux 2.0-2.4 shipped SMP with, and
 * it was chosen for the same reason they chose it: the kernel (and every
 * vendored subsystem in it) was written for one CPU, and one lock makes all
 * of that code correct on N CPUs at once, while user code - where the time
 * of a compute-bound workload goes - runs in parallel. The lock is passed to
 * the next thread across a context switch rather than released, so the
 * scheduler's own state is covered by it too.
 *
 * What runs WITHOUT the lock, and why each is safe, is short and listed:
 *   - user code, on every CPU;
 *   - an idle CPU halted in sched_idle_loop;
 *   - the IPI handlers below (TLB shootdown, remote call, reschedule), which
 *     touch only their own per-CPU mailboxes - they must, because the CPU
 *     that sent them is holding the lock and waiting for the answer;
 *   - a CPU spinning to acquire the lock, which services those same
 *     mailboxes while it spins, for the same reason.
 *
 * --- IPIs ------------------------------------------------------------------
 *
 * Three vectors, allocated at smp_init:
 *   shootdown   invalidate one page or the whole TLB, then acknowledge;
 *   call        run a function on this CPU (smp_call_on / smp_call_all);
 *   resched     "look at the run queue" - wakes an idle CPU and makes a busy
 *               one enter the kernel so a pending signal or a kill lands.
 */

/* Maximum CPUs this build tracks. */
#define SMP_MAX_CPUS 8

struct process;
struct address_space;

/* Per-CPU block, reached through GS.
 *
 * The first three fields are at gs:0, gs:8 and gs:0x10 and their offsets are
 * hardcoded in syscall.c's entry assembly and in smp_this_cpu() - which is
 * why this structure lives here with the offsets stated rather than being
 * grown casually. Anything added goes AFTER them. */
struct cpu_local {
    uint64 kernel_rsp;    /* gs:0    - stack to switch to on syscall entry */
    uint64 user_rsp;      /* gs:8    - where the user's RSP is parked      */
    struct cpu_local *self; /* gs:0x10 - points at this same block          */

    uint32 apic_id;
    uint32 index;         /* 0 is always the BSP                        */
    uint8  online;
    uint8  scheduling;    /* has joined the scheduler (runs processes)  */
    uint64 tlb_shootdowns; /* IPIs answered, for the report              */

    /* --- scheduling state, all of it this CPU's own ----------------------- */
    struct process *current;       /* the thread this CPU is running       */
    struct process *idle;          /* this CPU's idle thread               */
    volatile int    resched;       /* switch at the next return to ring 3  */
    int             quantum_left;  /* ticks left in the current slice      */
    struct address_space *cur_space; /* the space loaded in this CR3       */

    /* --- the big kernel lock ---------------------------------------------- */
    int             bkl_depth;     /* >0: this CPU holds it                */
    int             irq_depth;     /* >0: inside a hardware interrupt      */

    /* --- mailboxes the IPI handlers (and a BKL spinner) service ----------- */
    volatile uint64 tlb_addr;      /* page to drop, or SMP_TLB_ALL         */
    volatile int    tlb_pending;   /* 1 while a request is outstanding     */
    void          (*volatile call_fn)(void *);
    void           *volatile call_arg;
    volatile int    call_pending;

    /* --- statistics --------------------------------------------------------- */
    uint64 ticks;          /* timer interrupts taken on this CPU         */
    uint64 idle_ticks;     /* ...of which found it idle                  */
    uint64 switches;       /* context switches performed here            */
    uint64 ipis_received;  /* resched + call IPIs                        */
    uint64 bkl_spins;      /* acquisitions that had to wait              */
    uint64 user_entries;   /* times this CPU entered the kernel from ring 3 */

    uint64 ist1_top;       /* this CPU's double-fault stack              */
    uint64 cow_last_addr;  /* page this CPU last resolved or flushed as a
                            * copy-on-write fault - see paging.c           */
    uint64 cow_last_tick;
};

/* smp_tlb_shootdown's "every translation", rather than one page. */
#define SMP_TLB_ALL (~0ULL)

/* The very first call, before anything else asks which CPU it is on: makes
 * CPU 0's block reachable through GS, so smp_this_cpu() is one load from
 * the first instruction of kernel_main onward. */
void smp_early_init(void);

/* Enumerate CPUs, start every application processor, and wait for each to
 * report in. Call after paging, the LAPIC, the IDT and kheap are up, and
 * with interrupts still DISABLED. The APs come up idle; they start taking
 * processes only after smp_start_scheduling.
 *
 * Returns the number of CPUs online including the BSP, so 1 means no AP
 * started (or none exists), which is a normal outcome under `-smp 1`. */
int smp_init(void);

/* Calibrate the LAPIC timer against the PIT, give every CPU an idle thread,
 * and release the APs into the scheduler. Call after sti and after the
 * scheduler and kernel threads exist. */
void smp_start_scheduling(void);

/* CPUs online, and this CPU's block. */
int smp_cpu_count(void);
struct cpu_local *smp_this_cpu(void);
struct cpu_local *smp_cpu(int index);
int smp_cpu_index(void);                /* smp_this_cpu()->index */
uint64 smp_online_mask(void);           /* bit i set: CPU i online */

/* Invalidate `addr` (or SMP_TLB_ALL) on every OTHER online CPU and wait for
 * them to acknowledge. Kernel-half addresses need this: every space shares
 * them. A no-op with one CPU online. */
void smp_tlb_shootdown(uint64 addr);

/* The same, on only the CPUs whose loaded address space is `as` - which is
 * the only set that can have a cached translation for a USER address in it.
 * Synchronous: when this returns, no other CPU can reach the old mapping,
 * so the caller may free the frame. */
void smp_tlb_shootdown_space(struct address_space *as, uint64 addr);

/* Run fn(arg) on CPU `cpu` (which may be this one) and wait for it to
 * finish. Returns 0, or -1 if that CPU is not online. The function runs in
 * interrupt context on the target, WITHOUT the big kernel lock: it may touch
 * only per-CPU state and its own argument. This is the primitive under
 * smp_call_function_single, KeIpiGenericCall and smp_rendezvous. */
int smp_call_on(int cpu, void (*fn)(void *), void *arg);

/* Run fn(arg) on every online CPU, this one included, and wait for all of
 * them. */
void smp_call_all(void (*fn)(void *), void *arg);

/* Ask CPU `cpu` to enter the kernel and look at the run queue. */
void smp_kick(int cpu);

/* Service this CPU's IPI mailboxes by hand - what a CPU spinning with
 * interrupts off (waiting for the big kernel lock) must do so that the CPU
 * holding the lock, which may be waiting for exactly this answer, can make
 * progress. */
void smp_poll_mailboxes(void);

/* One line per CPU. */
void smp_report(uint8 color);

/* --- deferred work ------------------------------------------------------------
 *
 * Work queued to run on a particular CPU "soon", with the big kernel lock,
 * at a point where nothing on that CPU's stack is mid-operation: NT's DPCs
 * are the client (kernel/driver/wdm_smp.c registers them). The points are
 * the tail of an interrupt, the return to ring 3 and the idle loop - every
 * place a CPU holds the lock with nothing in progress beneath it. */
void smp_set_deferred_hook(int (*pending)(void), void (*run)(void));
int  smp_deferred_pending(void);        /* this CPU has some */
void smp_run_deferred(void);            /* run it (lock held) */

/* The vector the per-CPU LAPIC timer fires, and whether a vector is one of
 * the SMP IPIs (which run without the big kernel lock). -1 / 0 before
 * smp_init. */
int smp_timer_vector(void);
int smp_is_ipi_vector(int vector);

/* --- running something on the APs ---------------------------------------
 *
 * The oldest remote-execution facility here, from before the scheduler ran
 * on the APs; kept because lock_selftest uses it to put two CPUs genuinely
 * inside the same critical section. Every AP runs the same `fn` once, from
 * its idle loop, without the big kernel lock. */
void smp_run_on_aps(void (*fn)(void));

/* Block until every AP started by the last smp_run_on_aps has finished.
 * Bounded - a CPU that never finishes must not hang the boot. */
void smp_wait_for_aps(void);

/* Called by an idle CPU with the big kernel lock DROPPED: runs any work
 * smp_run_on_aps left for it. */
void smp_idle_poll_work(void);

/* Prove a second CPU is really executing and really answering IPIs, rather
 * than merely having been counted. Returns the number of failures; zero
 * CPUs to test is zero failures with a printed note. */
int smp_selftest(void);

/* Prove processes really run on more than one CPU at once. After
 * smp_start_scheduling. */
int smp_sched_selftest(void);

/* The AP entry point. Not called by C - the trampoline jumps to it once the
 * AP is in long mode. Declared here so the trampoline's address-of is
 * type-checked. */
void smp_ap_entry(void);

#endif
