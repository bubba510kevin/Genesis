#ifndef SMP_H
#define SMP_H

#include "typesk.h"

/* SMP bring-up - ROADMAP item 13, and the hinge of the item 11 pass.
 *
 * Before this, `grep -rl apic kernel/` was empty and cpu.h had no per-CPU
 * concept beyond the single implicit BSP. Part 5 added the LAPIC's receive
 * half for MSI; this adds the send half's consumer, a real-mode trampoline,
 * a per-CPU array, and TLB shootdown.
 *
 * --- what an AP does, and what it deliberately does not ------------------
 *
 * Each AP reaches a C entry point, installs its own GDT/TSS/IST, sets its
 * own GS base to its own per-CPU block, sets EFER.NXE (Part 14, which is
 * this part's natural fallout rather than a separate exercise), registers
 * itself as online, and idles in hlt.
 *
 * It does NOT run user processes. sched.c still has one global run queue and
 * one global "current process", and making that per-CPU is a scheduler
 * change with its own correctness burden - the plan calls it "mechanism
 * only" for exactly this reason. What is here is the mechanism: a second CPU
 * that is really executing kernel code, really answering IPIs, and really
 * invalidating its own TLB when asked. Handing it a process is Part 13's
 * business.
 *
 * Saying that plainly matters, because "SMP bring-up" can be read as "the
 * kernel is now SMP", and it is not. It is a kernel with more than one CPU
 * running in it, which is the prerequisite.
 */

/* Maximum CPUs this build tracks. */
#define SMP_MAX_CPUS 8

/* Per-CPU block, reached through GS.
 *
 * The first two fields are at gs:0 and gs:8 and their offsets are hardcoded
 * in syscall.c's entry assembly - which is why this structure lives here
 * with the offsets stated rather than being grown casually. Anything added
 * goes AFTER them. */
struct cpu_local {
    uint64 kernel_rsp;    /* gs:0    - stack to switch to on syscall entry */
    uint64 user_rsp;      /* gs:8    - where the user's RSP is parked      */
    struct cpu_local *self; /* gs:0x10 - points at this same block          */

    uint32 apic_id;
    uint32 index;         /* 0 is always the BSP                        */
    uint8  online;
    uint64 tlb_shootdowns; /* IPIs answered, for the report              */
};

/* Enumerate CPUs, start every application processor, and wait for each to
 * report in. Call after paging, the LAPIC, the IDT and kheap are up, and
 * with interrupts still DISABLED - an AP's first act is to take its own
 * interrupts, and the BSP spinning for it does not need any.
 *
 * Returns the number of CPUs online including the BSP, so 1 means no AP
 * started (or none exists), which is a normal outcome under `-smp 1` and
 * not a failure. */
int smp_init(void);

/* CPUs online, and this CPU's block. */
int smp_cpu_count(void);
struct cpu_local *smp_this_cpu(void);
struct cpu_local *smp_cpu(int index);

/* Invalidate `addr` on every OTHER online CPU and wait for them to
 * acknowledge. A no-op with one CPU online, which is why calling it
 * unconditionally from the paging code is safe and cheap.
 *
 * Synchronous by design: an asynchronous shootdown means the caller can
 * reuse the physical page while another CPU still has a translation to it,
 * and that is a silent memory-corruption bug rather than a fault. */
void smp_tlb_shootdown(uint64 addr);

/* One line per CPU. */
void smp_report(uint8 color);

/* --- running something on the APs ---------------------------------------
 *
 * The smallest possible remote-execution facility, and deliberately so: it
 * exists because Part 11's lock test cannot mean anything without two CPUs
 * genuinely inside the same critical section at the same time, and an AP
 * that only idles cannot provide that.
 *
 * It is NOT a scheduler and must not grow into one. One function, no
 * arguments, no return value, no queue - every AP runs the same `fn` once
 * and goes back to idling. Part 13 is where real per-CPU scheduling goes.
 */
void smp_run_on_aps(void (*fn)(void));

/* Block until every AP started by the last smp_run_on_aps has finished.
 * Bounded - a CPU that never finishes must not hang the boot. */
void smp_wait_for_aps(void);

/* Prove a second CPU is really executing and really answering IPIs, rather
 * than merely having been counted. Returns the number of failures; zero
 * CPUs to test is zero failures with a printed note. */
int smp_selftest(void);

/* The AP entry point. Not called by C - the trampoline jumps to it once the
 * AP is in long mode. Declared here so the trampoline's address-of is
 * type-checked. */
void smp_ap_entry(void);

#endif
