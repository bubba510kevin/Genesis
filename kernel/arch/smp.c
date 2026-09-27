#include "acpi.h"
#include "bkl.h"
#include "cpu.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "kheap.h"
#include "kprintf.h"
#include "lapic.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "ksmp.h"
#include "syscall.h"
#include "timer.h"
#include "typesk.h"
#include "wdm.h"

/* See ksmp.h for what this kernel does with more than one CPU. */

/* Must match AP_BASE in ap_trampoline.c, and must be a page number that fits
 * in a SIPI's single byte - so page-aligned and below 1MB. */
#define AP_TRAMPOLINE_PHYS 0x8000ULL
#define AP_TRAMPOLINE_PAGE 0x08

/* The AP's boot stack. It becomes nothing once the AP switches to its idle
 * thread; until then it runs bring-up and the pre-scheduling idle loop. */
#define AP_STACK_SIZE 0x4000

/* The double-fault stack, one per CPU: an IST index in the shared IDT names
 * a slot in the CURRENT CPU's TSS, so every CPU needs its own stack in it. */
#define AP_IST_SIZE 0x2000

extern uint8 ap_tramp_start[];
extern uint8 ap_tramp_end[];
extern uint64 ap_tramp_cr3;
extern uint64 ap_tramp_stack;
extern uint64 ap_tramp_entry;

static struct cpu_local cpus[SMP_MAX_CPUS];
static int              cpu_count = 1;   /* the BSP is always CPU 0 */
static volatile int     gs_ready;

/* Handshake between the BSP and the AP it is currently starting. One at a
 * time: two APs coming up together would race on ap_tramp_stack. */
static volatile int ap_started;

/* Released by smp_start_scheduling: until then the APs idle without taking
 * processes, because there are no idle threads and no timer calibration. */
static volatile int    sched_go;
static uint32          lapic_timer_count;

static int shootdown_vector = -1;
static int call_vector      = -1;
static int resched_vector   = -1;
static int timer_vector     = -1;

/* One initiator of cross-CPU requests at a time. Every initiator holds the
 * big kernel lock today; this makes the mailboxes correct without leaning on
 * that, and costs nothing uncontended. */
static volatile uint32 xcall_lock;

static void delay_loops(uint32 n) {
    volatile uint32 i;

    /* Not calibrated, and it does not need to be: every INIT-SIPI-SIPI wait
     * is a MINIMUM. */
    for (i = 0; i < n; i++) {
        __asm__ volatile ("pause");
    }
}

/* --- which CPU am I -------------------------------------------------------
 *
 * One load through GS. The convention (syscall.c) keeps GS_BASE pointing at
 * the executing CPU's block whenever the kernel runs - the BSP from
 * smp_early_init onward, each AP from its first C instruction - and swapgs
 * at the ring boundary puts the user's value there only while ring 3 runs.
 * So gs:0x10, the block's self pointer, is always this CPU's. */
void smp_early_init(void) {
    int i;

    for (i = 0; i < SMP_MAX_CPUS; i++) {
        cpus[i].self = &cpus[i];
        cpus[i].index = (uint32)i;
    }
    cpus[0].online = 1;
    wrmsr(MSR_GS_BASE, (uint64)&cpus[0]);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    gs_ready = 1;

    /* The BSP holds the big kernel lock from here until the first return to
     * ring 3: all of boot is kernel code, and anything an AP does in the
     * kernel must wait for it. */
    bkl_init();
    bkl_acquire();
}

struct cpu_local *smp_this_cpu(void) {
    struct cpu_local *c;

    if (!gs_ready) {
        return &cpus[0];
    }
    __asm__ volatile ("movq %%gs:0x10, %0" : "=r"(c));
    return c;
}

int smp_cpu_index(void) {
    return (int)smp_this_cpu()->index;
}

int smp_cpu_count(void) {
    return cpu_count;
}

struct cpu_local *smp_cpu(int index) {
    if (index < 0 || index >= SMP_MAX_CPUS) {
        return NULL;
    }
    return &cpus[index];
}

uint64 smp_online_mask(void) {
    uint64 m = 0;
    int i;

    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].online) {
            m |= 1ULL << i;
        }
    }
    return m;
}

/* --- the mailboxes -----------------------------------------------------------
 *
 * Each CPU has one TLB request slot and one call slot. The sender fills the
 * slot, raises the pending flag, and sends the IPI; the target does the work
 * and CLEARS the flag, which is the acknowledgement the sender waits for.
 * The work is done either by the IPI handler or - when the target is
 * spinning with interrupts off for the big kernel lock the sender holds - by
 * the spin loop itself, through smp_poll_mailboxes. */
static void local_flush(uint64 addr) {
    if (addr == SMP_TLB_ALL) {
        uint64 cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    } else {
        __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
    }
}

static void tlb_service(struct cpu_local *c) {
    if (__atomic_load_n(&c->tlb_pending, __ATOMIC_ACQUIRE)) {
        local_flush(c->tlb_addr);
        c->tlb_shootdowns++;
        /* Cleared AFTER the invalidation: the sender treats this as "this
         * CPU has dropped that translation" and may free the frame. */
        __atomic_store_n(&c->tlb_pending, 0, __ATOMIC_RELEASE);
    }
}

static void call_service(struct cpu_local *c) {
    if (__atomic_load_n(&c->call_pending, __ATOMIC_ACQUIRE)) {
        void (*fn)(void *) = c->call_fn;
        void *arg = c->call_arg;

        if (fn != NULL) {
            fn(arg);
        }
        __atomic_store_n(&c->call_pending, 0, __ATOMIC_RELEASE);
    }
}

void smp_poll_mailboxes(void) {
    struct cpu_local *c = smp_this_cpu();

    tlb_service(c);
    call_service(c);
}

static void shootdown_handler(void *ctx) {
    (void)ctx;
    tlb_service(smp_this_cpu());
}

static void call_handler(void *ctx) {
    struct cpu_local *c = smp_this_cpu();

    (void)ctx;
    c->ipis_received++;
    call_service(c);
}

/* The resched IPI's handler does nothing but count: the flag was set by the
 * sender, and the WORK is taking the interrupt at all - it breaks an idle
 * CPU out of hlt, and makes a CPU in ring 3 enter the kernel, whose return
 * path (interrupt_dispatch) then sees the flag. */
static void resched_handler(void *ctx) {
    (void)ctx;
    smp_this_cpu()->ipis_received++;
}

static void xcall_enter(void) {
    while (__atomic_exchange_n(&xcall_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        smp_poll_mailboxes();
        __asm__ volatile ("pause");
    }
}

static void xcall_exit(void) {
    __atomic_store_n(&xcall_lock, 0, __ATOMIC_RELEASE);
}

/* Wait for `flag` to clear, answering our own mailboxes meanwhile (another
 * CPU may be waiting on us at the same moment). Bounded: a CPU that never
 * answers is a real failure, but hanging here turns it into a dead machine.
 * Returns 0 if it cleared. */
static int wait_clear(volatile int *flag) {
    uint64 spin;

    for (spin = 0; spin < 400000000ULL; spin++) {
        if (__atomic_load_n(flag, __ATOMIC_ACQUIRE) == 0) {
            return 0;
        }
        smp_poll_mailboxes();
        __asm__ volatile ("pause");
    }
    return -1;
}

static void shootdown_mask(uint64 mask, uint64 addr) {
    struct cpu_local *self = smp_this_cpu();
    int i;

    mask &= ~(1ULL << self->index);
    mask &= smp_online_mask();
    if (mask == 0 || shootdown_vector < 0) {
        return;
    }
    xcall_enter();
    for (i = 0; i < cpu_count; i++) {
        if (mask & (1ULL << i)) {
            cpus[i].tlb_addr = addr;
            __atomic_store_n(&cpus[i].tlb_pending, 1, __ATOMIC_RELEASE);
            lapic_send_ipi(cpus[i].apic_id, (uint8)shootdown_vector);
        }
    }
    for (i = 0; i < cpu_count; i++) {
        if ((mask & (1ULL << i)) && wait_clear(&cpus[i].tlb_pending) != 0) {
            kprintf_c(0x0C, "smp: cpu%d did not answer a TLB shootdown\n", i);
            cpus[i].tlb_pending = 0;
        }
    }
    xcall_exit();
}

void smp_tlb_shootdown(uint64 addr) {
    if (cpu_count <= 1) {
        return;
    }
    shootdown_mask(~0ULL, addr);
}

void smp_tlb_shootdown_space(struct address_space *as, uint64 addr) {
    uint64 mask = 0;
    int i;

    if (cpu_count <= 1 || as == NULL) {
        return;
    }
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].online && cpus[i].cur_space == as) {
            mask |= 1ULL << i;
        }
    }
    shootdown_mask(mask, addr);
}

int smp_call_on(int cpu, void (*fn)(void *), void *arg) {
    struct cpu_local *self = smp_this_cpu();
    uint64 f;

    if (cpu < 0 || cpu >= cpu_count || !cpus[cpu].online || fn == NULL) {
        return -1;
    }
    if (cpu == (int)self->index) {
        __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(f) : : "memory");
        fn(arg);
        __asm__ volatile ("pushq %0\n\tpopfq" : : "r"(f) : "memory", "cc");
        return 0;
    }
    if (call_vector < 0) {
        return -1;
    }
    xcall_enter();
    cpus[cpu].call_fn  = fn;
    cpus[cpu].call_arg = arg;
    __atomic_store_n(&cpus[cpu].call_pending, 1, __ATOMIC_RELEASE);
    lapic_send_ipi(cpus[cpu].apic_id, (uint8)call_vector);
    if (wait_clear(&cpus[cpu].call_pending) != 0) {
        kprintf_c(0x0C, "smp: cpu%d did not run a remote call\n", cpu);
        cpus[cpu].call_pending = 0;
    }
    xcall_exit();
    return 0;
}

void smp_call_all(void (*fn)(void *), void *arg) {
    struct cpu_local *self = smp_this_cpu();
    uint64 f;
    int i;

    if (fn == NULL) {
        return;
    }
    if (cpu_count > 1 && call_vector >= 0) {
        xcall_enter();
        for (i = 0; i < cpu_count; i++) {
            if (i == (int)self->index || !cpus[i].online) {
                continue;
            }
            cpus[i].call_fn  = fn;
            cpus[i].call_arg = arg;
            __atomic_store_n(&cpus[i].call_pending, 1, __ATOMIC_RELEASE);
            lapic_send_ipi(cpus[i].apic_id, (uint8)call_vector);
        }
    }
    __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(f) : : "memory");
    fn(arg);
    __asm__ volatile ("pushq %0\n\tpopfq" : : "r"(f) : "memory", "cc");
    if (cpu_count > 1 && call_vector >= 0) {
        for (i = 0; i < cpu_count; i++) {
            if (i == (int)self->index || !cpus[i].online) {
                continue;
            }
            if (wait_clear(&cpus[i].call_pending) != 0) {
                kprintf_c(0x0C, "smp: cpu%d did not run a remote call\n", i);
                cpus[i].call_pending = 0;
            }
        }
        xcall_exit();
    }
}

void smp_kick(int cpu) {
    if (cpu < 0 || cpu >= cpu_count || !cpus[cpu].online) {
        return;
    }
    cpus[cpu].resched = 1;
    if (cpu != (int)smp_this_cpu()->index && resched_vector >= 0) {
        lapic_send_ipi(cpus[cpu].apic_id, (uint8)resched_vector);
    }
}

/* --- the per-CPU timer -------------------------------------------------------
 *
 * The LAPIC timer on every AP, at the PIT's rate. The BSP keeps the PIT:
 * its tick is the machine's clock (ticks, callouts, sleep deadlines), and
 * two clocks would disagree. What the AP's tick drives is only its own
 * slice - see interrupt_dispatch, which runs this without the big kernel
 * lock when the CPU was in ring 3 and takes the lock only if the slice ran
 * out. */
static void timer_handler(void *ctx) {
    (void)ctx;
    sched_tick_local();
}

int smp_timer_vector(void) {
    return timer_vector;
}

int smp_is_ipi_vector(int vector) {
    return vector >= 0 && (vector == shootdown_vector || vector == call_vector ||
                           vector == resched_vector || vector == timer_vector);
}

/* --- running work on the APs (pre-scheduler facility) ---------------------- */
static void (*volatile ap_work)(void);
static volatile uint64 ap_work_generation;
static volatile int    ap_work_pending;
static uint64          ap_seen_generation[SMP_MAX_CPUS];

void smp_run_on_aps(void (*fn)(void)) {
    int i;

    if (cpu_count <= 1 || fn == NULL) {
        return;
    }
    ap_work         = fn;
    ap_work_pending = cpu_count - 1;
    /* A GENERATION counter rather than each AP claiming the pointer: every
     * AP has to run this request, so what each needs is "have I done THIS
     * one", not "is there one left". Stores are ordered (volatile, x86), so
     * an AP that sees the new generation already sees the new pointer. */
    ap_work_generation++;
    __asm__ volatile ("" : : : "memory");

    for (i = 1; i < cpu_count; i++) {
        if (cpus[i].online && resched_vector >= 0) {
            lapic_send_ipi(cpus[i].apic_id, (uint8)resched_vector);
        }
    }
}

void smp_wait_for_aps(void) {
    uint32 spin;

    for (spin = 0; spin < 200000000u && ap_work_pending > 0; spin++) {
        __asm__ volatile ("pause");
    }
}

void smp_idle_poll_work(void) {
    struct cpu_local *c = smp_this_cpu();

    if (c->index == 0) {
        return;
    }
    if (ap_work_generation != ap_seen_generation[c->index] && ap_work != NULL) {
        ap_seen_generation[c->index] = ap_work_generation;
        /* Run with interrupts as the caller has them - off, from both the
         * idle loop and the big-kernel-lock spin. An interrupt taken here
         * would try to acquire the lock itself from the middle of a spin
         * for it. */
        ap_work();
        __asm__ volatile ("lock decl %0" : "+m"(ap_work_pending));
    }
}

/* --- the AP's first C ---------------------------------------------------
 *
 * Reached from the trampoline with a stack, long mode, paging on the
 * kernel's own tables, and EFER.NXE already set. Everything else that makes
 * a CPU able to run processes is this function's job - and every item is
 * per-CPU hardware state the BSP's setup did nothing for: its GDT and TSS,
 * its LAPIC, the SYSCALL MSRs, the FPU/SSE enables in CR0/CR4, its GS. */
void smp_ap_entry(void) {
    struct cpu_local *self = &cpus[cpu_count];
    static uint64 no_outgoing[SMP_MAX_CPUS];
    process_t *idle;

    gdt_init_ap((int)self->index);
    gdt_set_ist_cpu((int)self->index, 1, self->ist1_top);
    lapic_init_ap();
    idt_load();

    wrmsr(MSR_GS_BASE, (uint64)self);
    wrmsr(MSR_KERNEL_GS_BASE, 0);

    /* SSE and the FPU: without CR4.OSFXSR every SSE instruction in a user
     * program is #UD on this CPU - libc's memcpy is the first. */
    fpu_init_ap();

    /* SYSCALL: its MSRs are per CPU too. A process scheduled here that made
     * a system call without these would #UD on the instruction itself. */
    syscall_init_ap();

    self->cur_space = vmm_kernel_space();
    self->online = 1;
    __asm__ volatile ("" : : : "memory");
    ap_started = 1;

    /* Phase one: idle, without processes, until smp_start_scheduling. The
     * pre-scheduler work facility (smp_run_on_aps) is answered here. */
    __asm__ volatile ("sti");
    while (!sched_go) {
        __asm__ volatile ("cli");
        smp_idle_poll_work();
        if (!sched_go) {
            __asm__ volatile ("sti; hlt" : : : "memory");
        } else {
            __asm__ volatile ("sti");
        }
    }

    /* Phase two: join the scheduler. The LAPIC timer first, so the CPU can
     * preempt; then the big kernel lock, because becoming a scheduler
     * participant means reading and writing the run queue; then onto the
     * idle thread's own stack, which is where this CPU lives from now on -
     * this boot stack is never returned to. */
    __asm__ volatile ("cli");
    if (lapic_timer_count != 0 && timer_vector >= 0) {
        lapic_timer_periodic((uint8)timer_vector, lapic_timer_count);
    }
    idle = self->idle;
    if (idle == NULL) {
        /* No idle thread could be made for this CPU (the process table was
         * full): it stays out of the scheduler rather than running without
         * one. smp_start_scheduling already marked it offline. */
        lapic_timer_stop();
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    bkl_acquire();
    self->scheduling   = 1;
    self->current      = idle;
    self->quantum_left = 5;
    idle->state = PROC_RUNNING;
    idle->oncpu = 1;
    idle->cpu   = (int)self->index;
    proc_activate_stack(idle);
    vmm_switch_to(vmm_kernel_space());
    fpu_restore(idle->thread.fpu_state);
    switch_context(&no_outgoing[self->index], idle->thread.saved_rsp);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static int start_ap(uint32 apic_id, int index) {
    uint64 stack, ist;
    uint32 spin;

    stack = (uint64)kmalloc(AP_STACK_SIZE);
    ist   = (uint64)kmalloc(AP_IST_SIZE);
    if (stack == 0 || ist == 0) {
        return -1;
    }

    cpus[index].apic_id = apic_id;
    cpus[index].index   = (uint32)index;
    cpus[index].online  = 0;
    cpus[index].kernel_rsp = 0;
    cpus[index].user_rsp   = 0;
    /* IST1 - the double-fault stack - in THIS CPU's TSS, before the AP can
     * take one. gdt_init_ap zeroes the TSS, so it is set again from the AP
     * side too; recorded here so that side knows the value. */
    cpus[index].ist1_top = (ist + AP_IST_SIZE) & ~0xFULL;

    {
        uint64 off_cr3   = (uint64)&ap_tramp_cr3   - (uint64)ap_tramp_start;
        uint64 off_stack = (uint64)&ap_tramp_stack - (uint64)ap_tramp_start;
        uint64 off_entry = (uint64)&ap_tramp_entry - (uint64)ap_tramp_start;
        uint8 *low = (uint8 *)phys_to_virt(AP_TRAMPOLINE_PHYS);

        *(uint64 *)(low + off_cr3)   = (uint64)vmm_kernel_space()->root;
        *(uint64 *)(low + off_stack) = stack + AP_STACK_SIZE;
        *(uint64 *)(low + off_entry) = (uint64)smp_ap_entry;
    }

    ap_started = 0;

    if (lapic_send_init(apic_id) != 0) {
        return -1;
    }
    delay_loops(100000);

    /* Twice, per the Intel startup algorithm. */
    if (lapic_send_sipi(apic_id, AP_TRAMPOLINE_PAGE) != 0) {
        return -1;
    }
    delay_loops(20000);
    for (spin = 0; spin < 200000u && !ap_started; spin++) {
        __asm__ volatile ("pause");
    }
    if (!ap_started) {
        lapic_send_sipi(apic_id, AP_TRAMPOLINE_PAGE);
        for (spin = 0; spin < 2000000u && !ap_started; spin++) {
            __asm__ volatile ("pause");
        }
    }

    return ap_started ? 0 : -1;
}

int smp_init(void) {
    int found, i;
    uint64 tramp_len = (uint64)ap_tramp_end - (uint64)ap_tramp_start;

    cpus[0].apic_id = lapic_id();
    cpus[0].index   = 0;
    cpus[0].online  = 1;
    cpus[0].cur_space = vmm_current_space();
    cpu_count = 1;

    if (!lapic_available()) {
        return 1;
    }

    /* The IPI vectors, allocated before any AP starts so an AP is never
     * asked to answer a vector it has no handler for - and allocated even on
     * one CPU, so the code paths that name them are the same. */
    shootdown_vector = idt_alloc_vector(shootdown_handler, NULL);
    call_vector      = idt_alloc_vector(call_handler, NULL);
    resched_vector   = idt_alloc_vector(resched_handler, NULL);
    timer_vector     = idt_alloc_vector(timer_handler, NULL);

    found = acpi_enumerate_cpus();
    if (found <= 1) {
        return 1;
    }

    if (tramp_len > 0x1000) {
        kprintf_c(0x0C, "smp: trampoline is %d bytes, will not fit a page\n",
                  (int)tramp_len);
        return 1;
    }

    /* The trampoline page must be IDENTITY mapped: the AP turns paging on
     * while executing at 0x8000, and the next fetch uses the new tables. */
    if (!vmm_map_page(AP_TRAMPOLINE_PHYS, AP_TRAMPOLINE_PHYS,
                      PAGE_PRESENT | PAGE_RW)) {
        kprintf_c(0x0C, "smp: could not identity-map the trampoline page\n");
        return 1;
    }

    {
        uint8 *dst = (uint8 *)phys_to_virt(AP_TRAMPOLINE_PHYS);
        uint64 b;

        for (b = 0; b < tramp_len; b++) {
            dst[b] = ap_tramp_start[b];
        }
    }

    for (i = 0; i < found && cpu_count < SMP_MAX_CPUS; i++) {
        uint32 id = acpi_cpu_apic_id(i);

        if (id == cpus[0].apic_id) {
            continue;              /* that is us */
        }
        if (start_ap(id, cpu_count) == 0) {
            cpu_count++;
        } else {
            kprintf_c(0x0C, "smp: cpu with apic id %d did not start\n", id);
        }
    }

    vmm_unmap_page(AP_TRAMPOLINE_PHYS);
    return cpu_count;
}

/* Give every CPU an idle thread, calibrate the LAPIC timer, and let the APs
 * start taking processes. */
void smp_start_scheduling(void) {
    int i;

    for (i = 0; i < cpu_count; i++) {
        if (!cpus[i].online) {
            continue;
        }
        cpus[i].idle = proc_create_idle(i);
        if (cpus[i].idle == NULL) {
            kprintf_c(0x0C, "smp: no process slot for cpu%d's idle thread\n", i);
            if (i == 0) {
                return;
            }
            cpus[i].online = 0;
        }
    }
    cpus[0].scheduling = 1;

    /* The TSC first, on every configuration - a uniprocessor boot needs a
     * performance counter as much as an SMP one. */
    timer_calibrate_tsc(10);

    if (cpu_count > 1) {
        /* Ten PIT ticks: 100ms at 100Hz, long enough that the tick-edge
         * uncertainty is a percent, short enough not to be noticed. The BSP
         * holds the big kernel lock through this, and the PIT interrupt
         * nests on it. */
        lapic_timer_count = lapic_timer_calibrate(timer_ticks_now, 10);
        if (lapic_timer_count == 0) {
            kprintf_c(0x0C, "smp: LAPIC timer did not calibrate - the APs "
                            "will run processes but cannot preempt them\n");
        }
    }
    __asm__ volatile ("" : : : "memory");
    wdm_smp_started();
    sched_go = 1;
    for (i = 1; i < cpu_count; i++) {
        if (cpus[i].online && resched_vector >= 0) {
            lapic_send_ipi(cpus[i].apic_id, (uint8)resched_vector);
        }
    }
    kprintf_c(0x0A, "smp: scheduling on %d cpu%s (LAPIC timer %d units/tick, "
                    "TSC %d MHz)\n",
              cpu_count, cpu_count == 1 ? "" : "s", (int)lapic_timer_count,
              (int)(timer_tsc_hz() / 1000000));
}

void smp_report(uint8 color) {
    int i;

    kprintf_c(color, "smp: %d cpu%s online, big kernel lock waited %lx times\n",
              cpu_count, cpu_count == 1 ? "" : "s", bkl_contended());
    for (i = 0; i < cpu_count; i++) {
        kprintf_c(color, "  cpu%d  apic %d  %s  ticks %lx idle %lx  switches %lx"
                         "  ipis %lx  tlb %lx  from-user %lx\n",
                  i, cpus[i].apic_id, i == 0 ? "bsp" : "ap ",
                  cpus[i].ticks, cpus[i].idle_ticks, cpus[i].switches,
                  cpus[i].ipis_received, cpus[i].tlb_shootdowns,
                  cpus[i].user_entries);
    }
}

/* --- deferred work (see ksmp.h) ------------------------------------------------ */
static int  (*deferred_pending_fn)(void);
static void (*deferred_run_fn)(void);

void smp_set_deferred_hook(int (*pending)(void), void (*run)(void)) {
    deferred_pending_fn = pending;
    deferred_run_fn     = run;
}

int smp_deferred_pending(void) {
    return deferred_pending_fn != NULL && deferred_pending_fn();
}

void smp_run_deferred(void) {
    if (deferred_run_fn != NULL && deferred_pending_fn != NULL &&
        deferred_pending_fn()) {
        deferred_run_fn();
    }
}
