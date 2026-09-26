#include "acpi.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "kheap.h"
#include "kprintf.h"
#include "lapic.h"
#include "paging.h"
#include "pmm.h"
#include "ksmp.h"
#include "syscall.h"
#include "typesk.h"

/* See smp.h. */

/* Must match AP_BASE in ap_trampoline.c, and must be a page number that fits
 * in a SIPI's single byte - so page-aligned and below 1MB. 0x8000 is
 * conventional and is clear of the BIOS data area, the boot sector's load
 * address and the EBDA. */
#define AP_TRAMPOLINE_PHYS 0x8000ULL
#define AP_TRAMPOLINE_PAGE 0x08

/* The AP's stack. 16KB each, from the kernel heap - an AP that only idles
 * needs almost none, but an IPI handler runs on it and the fault path can
 * print a backtrace, so it is sized like a kernel stack rather than
 * minimally. */
#define AP_STACK_SIZE 0x4000

extern uint8 ap_tramp_start[];
extern uint8 ap_tramp_end[];
extern uint64 ap_tramp_cr3;
extern uint64 ap_tramp_stack;
extern uint64 ap_tramp_entry;

static struct cpu_local cpus[SMP_MAX_CPUS];
static int              cpu_count = 1;   /* the BSP is always CPU 0 */

/* Handshake between the BSP and the AP it is currently starting. One at a
 * time, deliberately: two APs coming up together would race on
 * ap_tramp_stack, which is a single word in a single trampoline page. Real
 * kernels start them serially for the same reason. */
static volatile int ap_started;

/* TLB shootdown state. The address to invalidate and a count of CPUs that
 * still have to acknowledge. */
static volatile uint64 shootdown_addr;
static volatile int    shootdown_pending;
static int             shootdown_vector = -1;

static void delay_loops(uint32 n) {
    volatile uint32 i;

    /* Not calibrated, and it does not need to be. The INIT-SIPI-SIPI
     * sequence requires a wait of roughly 10ms after INIT and 200us after
     * each SIPI, and every one of those is a MINIMUM - waiting too long
     * costs boot time and nothing else. A calibrated delay here would be
     * precision with no consumer. */
    for (i = 0; i < n; i++) {
        __asm__ volatile ("pause");
    }
}

struct cpu_local *smp_this_cpu(void) {
    uint32 id = lapic_id();
    int i;

    /* Matched on the APIC ID rather than read out of gs:0x10, and the first
     * version of this did the latter. GS is not a reliable answer to "which
     * CPU am I" on the BSP: syscall.c's convention puts the per-CPU block in
     * KERNEL_GS_BASE while in ring 3 and in GS_BASE while in the kernel, and
     * the two only swap on a swapgs at syscall entry. Before the first user
     * process has ever run - which is exactly when the boot-time selftests
     * execute - the BSP's GS_BASE is still 0, and gs:0x10 is a read of
     * address 0x10.
     *
     * That is the same "ask rather than assume" trap syscall_set_user_gs_base
     * already documents, arrived at from the other direction. An AP does not
     * share the problem (its GS_BASE is set once and never swapped), but a
     * function that is correct on one CPU and faults on another is worse than
     * one that is slightly slower on both.
     *
     * The LAPIC ID register is one MMIO read and the array is single digits
     * long, so the cost is nothing next to the IPI this is called around. */
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].apic_id == id) {
            return &cpus[i];
        }
    }
    return &cpus[0];
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

/* The IPI handler every CPU runs for a shootdown. */
static void shootdown_handler(void *ctx) {
    (void)ctx;

    __asm__ volatile ("invlpg (%0)" : : "r"(shootdown_addr) : "memory");
    smp_this_cpu()->tlb_shootdowns++;

    /* Decremented AFTER the invalidation, not before. The BSP treats this
     * reaching zero as "every CPU has dropped that translation", and it may
     * free the physical page immediately afterwards. */
    __asm__ volatile ("lock decl %0" : "+m"(shootdown_pending));
}

void smp_tlb_shootdown(uint64 addr) {
    int i;
    uint32 spin;
    uint32 self_id;

    if (cpu_count <= 1 || shootdown_vector < 0) {
        return;
    }

    self_id           = lapic_id();
    shootdown_addr    = addr;
    shootdown_pending = cpu_count - 1;

    for (i = 0; i < cpu_count; i++) {
        if (!cpus[i].online || cpus[i].apic_id == self_id) {
            continue;
        }
        lapic_send_ipi(cpus[i].apic_id, (uint8)shootdown_vector);
    }

    /* Bounded. A CPU that never answers is a real failure, but hanging the
     * machine here turns a lost TLB entry into a dead system - and the
     * unmapping caller is usually in the middle of something it can still
     * finish. */
    for (spin = 0; spin < 10000000u && shootdown_pending > 0; spin++) {
        __asm__ volatile ("pause");
    }
}

/* --- running work on the APs -------------------------------------------
 *
 * A single function pointer the idle loop checks after every wakeup, plus a
 * count of APs still working. See smp.h for why this is as small as it is.
 */
static void (*volatile ap_work)(void);
static volatile uint64 ap_work_generation;
static volatile int    ap_work_pending;

void smp_run_on_aps(void (*fn)(void)) {
    int i;

    if (cpu_count <= 1 || fn == NULL) {
        return;
    }
    ap_work         = fn;
    ap_work_pending = cpu_count - 1;
    /* A GENERATION counter rather than each AP claiming the pointer with an
     * xchg. The claim version was written first and is wrong for more than
     * two CPUs: one AP takes the pointer, every other AP sees NULL and runs
     * nothing, and ap_work_pending never reaches zero. Every AP has to run
     * this request, so what each one needs is "have I done THIS one", not
     * "is there one left". */
    ap_work_generation++;

    /* Published before the wakeup, so an AP that is already awake for some
     * other reason cannot see the new generation with a stale ap_work.
     *
     * The ORDER above is what guarantees that, not this barrier: ap_work and
     * ap_work_generation are both volatile, so the compiler may not reorder
     * them with respect to each other, and x86 makes stores visible in
     * program order. An AP that sees the new generation has therefore
     * already seen the new pointer. The barrier is here to stop the IPI
     * sends below being hoisted above either of them. */
    __asm__ volatile ("" : : : "memory");

    for (i = 1; i < cpu_count; i++) {
        if (cpus[i].online && shootdown_vector >= 0) {
            /* Reusing the shootdown vector as a generic "wake up and look"
             * signal: its handler invalidates one page and returns, which is
             * harmless, and the idle loop below is what actually notices the
             * work. A second vector would be tidier and would buy nothing -
             * the AP has to re-check ap_work on every wakeup regardless,
             * because a TLB shootdown wakes it too. */
            lapic_send_ipi(cpus[i].apic_id, (uint8)shootdown_vector);
        }
    }
}

void smp_wait_for_aps(void) {
    uint32 spin;

    for (spin = 0; spin < 200000000u && ap_work_pending > 0; spin++) {
        __asm__ volatile ("pause");
    }
}

/* --- the AP's first C ---------------------------------------------------
 *
 * Reached from the trampoline with a stack, long mode, paging on the
 * kernel's own tables, and EFER.NXE already set (the trampoline does it in
 * the same wrmsr as LME - see ap_trampoline.c for why it cannot wait until
 * here). Everything else is this function's job. */
void smp_ap_entry(void) {
    struct cpu_local *self = &cpus[cpu_count];
    uint64 seen_generation = ap_work_generation;

    /* Its own GDT and TSS before anything else can fault. */
    gdt_init_ap(self->index);

    /* And its own LAPIC. See lapic_init_ap - the register page is shared,
     * the enable bits are not. */
    lapic_init_ap();

    /* The IDT is genuinely shared - it is a table of code addresses, the
     * same on every CPU - so this is a load, not a build. The IST STACKS
     * behind it are not shared, and that is why the TSS above is per-CPU. */
    idt_load();

    /* GS points at this CPU's block. Every reference to "the current CPU"
     * from here on resolves through this, which is why it comes before the
     * CPU is marked online. */
    wrmsr(MSR_GS_BASE, (uint64)self);
    wrmsr(MSR_KERNEL_GS_BASE, 0);

    self->online = 1;
    __asm__ volatile ("" : : : "memory");
    ap_started = 1;

    /* Interrupts on: this CPU exists to answer IPIs. The legacy PIC is not
     * routed here - only the BSP takes IRQ 0-15 - so nothing else arrives. */
    __asm__ volatile ("sti");

    for (;;) {
        /* Checked after every wakeup, and hlt is what wakes on an interrupt.
         * A TLB shootdown IPI wakes this too, which is why the test is on
         * the generation rather than on ap_work being non-NULL. */
        if (ap_work_generation != seen_generation && ap_work != NULL) {
            seen_generation = ap_work_generation;
            ap_work();
            __asm__ volatile ("lock decl %0" : "+m"(ap_work_pending));
        }
        __asm__ volatile ("hlt");
    }
}

static int start_ap(uint32 apic_id, int index) {
    uint64 stack;
    uint32 spin;

    stack = (uint64)kmalloc(AP_STACK_SIZE);
    if (stack == 0) {
        return -1;
    }

    cpus[index].apic_id = apic_id;
    cpus[index].index   = (uint32)index;
    cpus[index].online  = 0;
    cpus[index].kernel_rsp = 0;
    cpus[index].user_rsp   = 0;

    /* Patch the copy in low memory, not the original: the original is in the
     * kernel image at a high address and the AP will never see it. */
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

    /* Twice, per the Intel startup algorithm. The second is a no-op on a CPU
     * that already started - it is there for the ones that miss the first,
     * which happens on real hardware and never on QEMU. Sending only one
     * works here and would be a latent failure elsewhere. */
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

    /* CPU 0 is this one, and it is already running. Its block has to be
     * described before anything reads smp_this_cpu(). */
    cpus[0].apic_id = lapic_id();
    cpus[0].index   = 0;
    cpus[0].online  = 1;
    cpu_count = 1;

    /* Every block points at itself, so smp_this_cpu() is one load through
     * GS rather than a search by APIC ID - which matters because it is
     * called from interrupt context. Done for all of them here, before any
     * AP runs, so an AP never reads its own self pointer before it is set. */
    for (i = 0; i < SMP_MAX_CPUS; i++) {
        cpus[i].self = &cpus[i];
    }

    if (!lapic_available()) {
        return 1;
    }

    found = acpi_enumerate_cpus();
    if (found <= 1) {
        return 1;
    }

    if (tramp_len > 0x1000) {
        kprintf_c(0x0C, "smp: trampoline is %d bytes, will not fit a page\n",
                  (int)tramp_len);
        return 1;
    }

    /* The trampoline page must be IDENTITY mapped, not just present: the AP
     * turns paging on while executing at 0x8000, and the very next
     * instruction fetch uses the new tables. If 0x8000 does not map to
     * physical 0x8000 there, the CPU triple-faults with nothing to read.
     *
     * paging_init dropped the identity map the bootloader left, so this puts
     * one page of it back, and only for as long as bring-up needs it. */
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

    /* The shootdown vector, allocated once and used by every CPU. Allocated
     * before any AP starts, so an AP is never asked to answer a vector it
     * has no handler for. */
    shootdown_vector = idt_alloc_vector(shootdown_handler, NULL);

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

    /* The identity mapping goes away now. Leaving it would mean a null-ish
     * kernel pointer near 0x8000 silently resolving instead of faulting -
     * and nothing needs it once every AP is past the trampoline. */
    vmm_unmap_page(AP_TRAMPOLINE_PHYS);

    return cpu_count;
}

void smp_report(uint8 color) {
    int i;

    kprintf_c(color, "smp: %d cpu%s online\n", cpu_count,
              cpu_count == 1 ? "" : "s");
    for (i = 0; i < cpu_count; i++) {
        kprintf_c(color, "  cpu%d  apic id %d  %s  tlb shootdowns %lx\n",
                  i, cpus[i].apic_id,
                  i == 0 ? "bsp" : "ap",
                  cpus[i].tlb_shootdowns);
    }
}
