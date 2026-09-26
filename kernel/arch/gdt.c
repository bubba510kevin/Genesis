#include "gdt.h"
#include "typesk.h"

#if !defined(__x86_64__)
#error "gdt64.c is the long-mode implementation; build it with -m64"
#endif

/* ===========================================================================
 * 64-bit GDT and TSS.
 * ---------------------------------------------------------------------------
 * Segmentation is almost gone in long mode: base and limit are ignored for
 * code and data, so the descriptors carry essentially only privilege level and
 * the L bit. What the GDT is still FOR is three things - telling the CPU a
 * segment is 64-bit (L=1), carrying DPL so ring 3 exists, and holding the TSS,
 * which is how the CPU finds a kernel stack when an interrupt arrives while
 * user code is running.
 *
 * Descriptor order is not arbitrary. SYSRET reconstructs its selectors from
 * MSR_STAR by fixed offsets: user data must sit immediately after user code32
 * and immediately before user code64. Getting this wrong does not fail at
 * boot - it fails the first time a syscall returns to user space. The layout
 * below is the conventional one that satisfies it.
 * ======================================================================== */

#define GDT_NULL          0x00
#define GDT_KERNEL_CODE   0x08
#define GDT_KERNEL_DATA   0x10
#define GDT_USER_DATA     0x18   /* before user code: required by SYSRET */
#define GDT_USER_CODE     0x20
#define GDT_TSS           0x28   /* occupies TWO slots: 0x28 and 0x30 */

#define GDT_ENTRIES       7      /* 5 segments + 2 for the 16-byte TSS */

/* The TSS in long mode has no task-switching role at all - the CPU no longer
 * does hardware task switches. It survives purely as a place to put stack
 * pointers:
 *   rsp0   loaded automatically on a ring 3 -> ring 0 transition
 *   ist[]  seven alternate stacks, selectable per IDT gate. Worth using for
 *          the double fault handler: if the fault was a bad kernel stack, an
 *          IST entry is the only way the handler runs at all rather than
 *          triple-faulting.
 * Exactly 104 bytes; the static assert below holds you to it. */
struct tss64 {
    uint32 reserved0;
    uint64 rsp0;
    uint64 rsp1;
    uint64 rsp2;
    uint64 reserved1;
    uint64 ist[7];
    uint64 reserved2;
    uint16 reserved3;
    uint16 iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16 limit;
    uint64 base;      /* 64-bit now, not 32 */
} __attribute__((packed));

typedef char assert_tss_size[(sizeof(struct tss64) == 104) ? 1 : -1];
typedef char assert_ptr_size[(sizeof(struct gdt_ptr) == 10) ? 1 : -1];

/* One GDT and one TSS PER CPU.
 *
 * The GDT's descriptors are identical on every CPU and could in principle be
 * shared - but the TSS cannot be, and the TSS descriptor lives inside the
 * GDT. Two CPUs sharing a TSS share rsp0 and all seven IST stacks, so a
 * double fault on either one lands on the same emergency stack, and a ring
 * transition on either one loads the other's kernel stack. Both are silent
 * until they are catastrophic.
 *
 * Index 0 is the BSP, matching smp.h's per-CPU array. */
#define GDT_MAX_CPUS 8

static uint64 gdt[GDT_MAX_CPUS][GDT_ENTRIES];
static struct gdt_ptr gdtp[GDT_MAX_CPUS];
static struct tss64 tss[GDT_MAX_CPUS] __attribute__((aligned(16)));

/* A normal 8-byte descriptor. In long mode the CPU ignores base and limit for
 * code and data segments, so those fields are written only to keep the
 * encoding well-formed. */
static uint64 gdt_descriptor(uint8 access, uint8 flags) {
    uint64 d = 0;
    d |= 0x0000FFFFULL;                       /* limit 15:0  (ignored)   */
    d |= ((uint64)access) << 40;
    d |= ((uint64)(flags & 0x0F)) << 48;      /* limit 19:16 (ignored)   */
    d |= ((uint64)(flags & 0xF0)) << 48;      /* G / D-B / L / AVL       */
    return d;
}

/* Build and load CPU `cpu`'s GDT and TSS. gdt_init is this with cpu 0; an
 * AP calls gdt_init_ap with its own index from smp_ap_entry. */
void gdt_init_ap(int cpu) {
    uint64 tss_base;
    uint64 tss_limit = sizeof(struct tss64) - 1;
    uint64 lo, hi;
    int i;

    if (cpu < 0 || cpu >= GDT_MAX_CPUS) {
        return;
    }
    tss_base = (uint64)&tss[cpu];

    for (i = 0; i < GDT_ENTRIES; i++) {
        gdt[cpu][i] = 0;
    }

    /* access 0x9A = P|DPL0|S|exec|read,  0xF A = P|DPL3|...
     * flags  0xA0 = G=1, D/B=0, L=1.  D/B MUST be 0 when L is 1 - the
     * combination is reserved and faults on the far jump. */
    gdt[cpu][GDT_KERNEL_CODE / 8] = gdt_descriptor(0x9A, 0xA0);
    gdt[cpu][GDT_KERNEL_DATA / 8] = gdt_descriptor(0x92, 0xC0);
    gdt[cpu][GDT_USER_DATA   / 8] = gdt_descriptor(0xF2, 0xC0); /* DPL 3 data */
    gdt[cpu][GDT_USER_CODE   / 8] = gdt_descriptor(0xFA, 0xA0); /* DPL 3 code */

    /* The TSS descriptor is a SYSTEM descriptor and is 16 bytes wide, because
     * its base is 64-bit. It therefore consumes two GDT slots, and the
     * selector that follows it must skip both. Type 0x9 is "64-bit TSS,
     * available"; 0xB would mean busy, which faults on ltr. */
    lo  = (tss_limit & 0xFFFFULL);
    lo |= (tss_base & 0xFFFFULL) << 16;
    lo |= ((tss_base >> 16) & 0xFFULL) << 32;
    lo |= 0x89ULL << 40;                          /* P=1, DPL=0, type=9 */
    lo |= ((tss_limit >> 16) & 0xFULL) << 48;
    lo |= ((tss_base >> 24) & 0xFFULL) << 56;
    hi  = (tss_base >> 32) & 0xFFFFFFFFULL;

    gdt[cpu][GDT_TSS / 8]       = lo;
    gdt[cpu][(GDT_TSS / 8) + 1] = hi;

    for (i = 0; i < 7; i++) {
        tss[cpu].ist[i] = 0;
    }
    tss[cpu].rsp0 = 0;
    tss[cpu].rsp1 = 0;
    tss[cpu].rsp2 = 0;
    /* Past the end of the TSS means "no I/O bitmap", which denies port access
     * to ring 3 entirely. Leaving this zero instead would make the CPU read a
     * bitmap out of whatever follows the struct. */
    tss[cpu].iomap_base = sizeof(struct tss64);

    gdtp[cpu].limit = (uint16)(sizeof(gdt[cpu]) - 1);
    gdtp[cpu].base  = (uint64)&gdt[cpu];

    __asm__ volatile ("lgdt (%0)" : : "r"(&gdtp[cpu]) : "memory");

    /* Reload the segment registers so the CPU reads the new table now, at a
     * line you can find, rather than at the next interrupt. CS needs a far
     * transfer, and long mode has no ljmp to an absolute address - so push the
     * selector and target and lretq into it. */
    __asm__ volatile (
        "mov %0, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        : : "i"(GDT_KERNEL_DATA), "i"(GDT_KERNEL_CODE) : "rax", "memory"
    );

    __asm__ volatile ("ltr %w0" : : "r"((uint16)GDT_TSS) : "memory");
}

/* Set the stack the CPU switches to on a ring 3 -> ring 0 transition. Call
 * this on every context switch with the incoming task's kernel stack, or the
 * first syscall from the new task lands on the previous task's stack. */
void gdt_init(void) {
    gdt_init_ap(0);
}

/* BSP only, and that is a real restriction rather than an oversight: only
 * the BSP runs user processes in this pass, so only the BSP's TSS is ever
 * consulted for a ring transition. An AP never leaves smp_ap_entry's idle
 * loop. When a scheduler starts placing processes on APs (Part 13), this
 * has to become per-CPU - and it will need the current CPU's index, which
 * means reading it through GS rather than taking it as an argument, since
 * every existing caller is a context switch that does not know which CPU it
 * is on. */
void gdt_set_kernel_stack(uint64 rsp0) {
    tss[0].rsp0 = rsp0;
}

/* Install an alternate stack for a given IST index (1-7). An IDT gate with a
 * matching ist field switches to it unconditionally, even from ring 0 - which
 * is what makes a double fault handler survivable when the kernel stack is
 * what broke. */
void gdt_set_ist(int index, uint64 stack_top) {
    gdt_set_ist_cpu(0, index, stack_top);
}

void gdt_set_ist_cpu(int cpu, int index, uint64 stack_top) {
    if (cpu >= 0 && cpu < GDT_MAX_CPUS && index >= 1 && index <= 7) {
        tss[cpu].ist[index - 1] = stack_top;
    }
}
