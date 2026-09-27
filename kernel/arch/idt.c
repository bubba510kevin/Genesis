#include "idt.h"
#include "typesk.h"

#if !defined(__x86_64__)
#error "idt64.c is the long-mode implementation; build it with -m64"
#endif

/* ===========================================================================
 * 64-bit IDT and interrupt stubs.
 * ---------------------------------------------------------------------------
 * Three things change from the 32-bit version, and the third is the one that
 * bites:
 *
 * 1. Gates are 16 bytes, not 8. The handler offset is 64-bit, split across
 *    three fields, and there is a new 3-bit IST index.
 *
 * 2. The interrupt frame is uniform. In 32-bit, the CPU pushes SS:ESP only on
 *    a privilege change, so a kernel-mode handler saw a different frame from a
 *    user-mode one. In long mode SS:RSP are ALWAYS pushed. One layout, no
 *    special case - a genuine simplification.
 *
 * 3. There is no pusha. Fifteen general-purpose registers get saved by hand,
 *    and the System V AMD64 ABI puts the first argument in RDI rather than on
 *    the stack. Both are handled in isr_common below.
 *
 * Stack alignment is worth checking rather than assuming. The CPU aligns RSP
 * to 16 before pushing the frame. For a vector with no error code: 5 qwords
 * pushed by the CPU (40 bytes) leaves RSP at 8 mod 16, our dummy error code
 * plus vector adds 16 (still 8), and the 15 register pushes add 120, which is
 * 8 mod 16 - landing back at 0. For a vector WITH an error code: 6 qwords (48,
 * so 0 mod 16), plus vector (8), plus 120 -> also 0. Both paths reach `call`
 * 16-byte aligned, which the ABI requires and which SSE-using code in the
 * compiler's output depends on.
 * ======================================================================== */

#define IDT_ENTRIES        256
#define KERNEL_CODE_SEL    0x08

#define IDT_TYPE_INTERRUPT 0x0E   /* clears IF on entry */
#define IDT_TYPE_TRAP      0x0F   /* leaves IF alone    */
#define IDT_PRESENT        0x80
#define IDT_DPL3           0x60   /* needed only for an int gate ring 3 may
                                   * raise deliberately, e.g. a syscall vector */

struct idt64_entry {
    uint16 offset_low;
    uint16 selector;
    uint8  ist;         /* bits 2:0 select a TSS.ist[] stack; 0 = use current */
    uint8  type_attr;
    uint16 offset_mid;
    uint32 offset_high;
    uint32 reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16 limit;
    uint64 base;
} __attribute__((packed));

typedef char assert_gate_size[(sizeof(struct idt64_entry) == 16) ? 1 : -1];
typedef char assert_ptr_size[(sizeof(struct idt_ptr) == 10) ? 1 : -1];

static struct idt64_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

/* What isr_common hands to the C handler. Order is the reverse of the pushes,
 * because the stack grows down: the last thing pushed is at the lowest
 * address and therefore first in the struct. */
struct interrupt_frame {
    uint64 r15, r14, r13, r12, r11, r10, r9, r8;
    uint64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64 vector;
    uint64 error_code;
    uint64 rip, cs, rflags, rsp, ss;   /* pushed by the CPU */
};

extern void *isr_stub_table[];

static void idt_set_gate(int vec, uint64 handler, uint8 type_attr, uint8 ist) {
    idt[vec].offset_low  = (uint16)(handler & 0xFFFF);
    idt[vec].selector    = KERNEL_CODE_SEL;
    idt[vec].ist         = (uint8)(ist & 0x7);
    idt[vec].type_attr   = type_attr;
    idt[vec].offset_mid  = (uint16)((handler >> 16) & 0xFFFF);
    idt[vec].offset_high = (uint32)(handler >> 32);
    idt[vec].reserved    = 0;
}

void idt_init(void) {
    int i;

    for (i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low = 0; idt[i].selector = 0; idt[i].ist = 0;
        idt[i].type_attr = 0;  idt[i].offset_mid = 0;
        idt[i].offset_high = 0; idt[i].reserved = 0;
    }

    /* All 256, not the first 48.
     *
     * ROADMAP item 11: there was no vector-allocation machinery at all above
     * 47, and MSI needs one - an MSI message carries the vector the device
     * will raise, and that vector has to have a gate before the device is
     * told about it. Filling the whole table here rather than lazily in
     * idt_alloc_vector keeps the stubs a build-time fact: the alternative is
     * a window where a device has been programmed with a vector whose gate
     * is not present yet, and the fault that produces is a #GP with an error
     * code that names the vector - readable, but only if you already know to
     * look for it.
     *
     * Vectors 48-255 all use the no-error-code stub form. That is not a
     * simplification: no architecturally defined exception lives above 31,
     * and an external interrupt never pushes an error code. */
    for (i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, (uint64)isr_stub_table[i],
                     IDT_PRESENT | IDT_TYPE_INTERRUPT, 0);
    }

    /* Vector 8, double fault, on IST slot 1. This is the one gate where the
     * IST genuinely matters: a double fault often means the kernel stack is
     * unusable, and without a guaranteed-good stack the handler faults again
     * and the machine triple-faults with nothing printed. Requires
     * gdt_set_ist(1, ...) to have been called with a real stack. */
    idt_set_gate(8, (uint64)isr_stub_table[8],
                 IDT_PRESENT | IDT_TYPE_INTERRUPT, 1);

    /* int3 from ring 3 is a breakpoint, not a protection fault: DebugBreak()
     * and __debugbreak() must raise EXCEPTION_BREAKPOINT, which is only
     * possible if the gate lets ring 3 through. With DPL 0 the CPU turns
     * the instruction into a #GP instead. */
    idt_set_gate(3, (uint64)isr_stub_table[3],
                 IDT_PRESENT | IDT_TYPE_INTERRUPT | IDT_DPL3, 0);

    idtp.limit = (uint16)(sizeof(idt) - 1);
    idtp.base  = (uint64)&idt;

    idt_load();
}

/* Load the already-built IDT on THIS CPU. The table itself is genuinely
 * shared - it holds code addresses, identical on every CPU - so an AP loads
 * it rather than building one. What is not shared is the IST stacks the
 * gates name, which live in the per-CPU TSS (see gdt.h). */
void idt_load(void) {
    __asm__ volatile ("lidt (%0)" : : "r"(&idtp) : "memory");
}

/* --- the stubs ----------------------------------------------------------
 * Written as top-level asm so the assembler generates them from macros and
 * the encodings are checkable, rather than hand-writing 48 near-identical
 * blocks. Vectors 8, 10-14, 17, 21, 29 and 30 push an error code; the rest do
 * not, so the no-error stubs push a dummy zero to keep one frame layout. */
__asm__(
".section .text\n"

".macro ISR_NOERR vec\n"
"isr_stub_\\vec:\n"
"    pushq $0\n"
"    pushq $\\vec\n"
"    jmp isr_common\n"
".endm\n"

".macro ISR_ERR vec\n"
"isr_stub_\\vec:\n"
"    pushq $\\vec\n"
"    jmp isr_common\n"
".endm\n"

"isr_common:\n"

/* --- swapgs, and why an interrupt needs it too ---------------------------
 *
 * The convention syscall_init sets up: in ring 3, GS_BASE is the user's (zero)
 * and KERNEL_GS_BASE holds the per-CPU block; syscall_entry swaps on the way
 * in and syscall_return swaps back. This stub used to do neither, on the
 * reasoning that nothing in the interrupt path reads GS - which was true of
 * the path itself and false of where it ends up.
 *
 * interrupt_dispatch calls return_to_user, which calls schedule(), which
 * switches to another thread. That thread leaves the kernel through
 * syscall_return, whose second-to-last instruction is `movq %gs:8, %rsp`. It
 * gets whatever GS_BASE the CPU happens to be holding - and when the switch
 * was initiated from an interrupt taken in ring 3, that is still the user's
 * zero. The load reads address 8 and the machine dies with a page fault
 * inside syscall_return, blaming a thread that did nothing wrong.
 *
 * That is reachable the moment two things are true at once: one process
 * blocked inside a syscall, and another running in user mode long enough to
 * be preempted by the timer. Pipes are what made it routine - a shell
 * pipeline has a reader parked in read(2) by construction - but nothing about
 * it is specific to pipes, and a forked child first scheduled from an
 * interrupt would have hit it just as hard.
 *
 * So: swap on entry from ring 3, swap back on exit to ring 3, and GS_BASE is
 * the per-CPU block for the whole time the kernel is running, whichever door
 * it came in by. Conditional on the saved CS because an interrupt taken while
 * already in the kernel has GS_BASE correct already, and swapping there would
 * install the user's.
 *
 * The frame at this point is: vector, error code, RIP, CS - so CS is at
 * 24(%rsp), and its low two bits are the privilege level being returned to. */
"    testb $3, 24(%rsp)\n"
"    jz 1f\n"
"    swapgs\n"
"1:\n"

"    pushq %rax\n  pushq %rbx\n  pushq %rcx\n  pushq %rdx\n"
"    pushq %rsi\n  pushq %rdi\n  pushq %rbp\n"
"    pushq %r8\n   pushq %r9\n   pushq %r10\n  pushq %r11\n"
"    pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
"    cld\n"                       /* ABI requires DF clear on entry to C */
"    movq %rsp, %rdi\n"           /* SysV: first argument in RDI         */
"    call interrupt_dispatch\n"
"    popq %r15\n   popq %r14\n   popq %r13\n   popq %r12\n"
"    popq %r11\n   popq %r10\n   popq %r9\n    popq %r8\n"
"    popq %rbp\n   popq %rdi\n   popq %rsi\n"
"    popq %rdx\n   popq %rcx\n   popq %rbx\n   popq %rax\n"
"    addq $16, %rsp\n"            /* discard vector + error code         */

/* And back. RSP now points at the CPU-pushed frame, so CS is at 8(%rsp).
 *
 * The test is on the frame rather than on anything remembered from entry
 * because this may not be the thread that entered: schedule() can have run in
 * between. Each thread's own frame is the one it arrived with, so reading the
 * decision back out of it is what keeps entry and exit paired per thread
 * rather than per pass through this code. */
"    testb $3, 8(%rsp)\n"
"    jz 2f\n"
"    swapgs\n"
"2:\n"
"    iretq\n"

/* 0-31 exceptions, 32-47 the remapped PIC IRQs */
"ISR_NOERR 0\n  ISR_NOERR 1\n  ISR_NOERR 2\n  ISR_NOERR 3\n"
"ISR_NOERR 4\n  ISR_NOERR 5\n  ISR_NOERR 6\n  ISR_NOERR 7\n"
"ISR_ERR   8\n  ISR_NOERR 9\n  ISR_ERR   10\n ISR_ERR   11\n"
"ISR_ERR   12\n ISR_ERR   13\n ISR_ERR   14\n ISR_NOERR 15\n"
"ISR_NOERR 16\n ISR_ERR   17\n ISR_NOERR 18\n ISR_NOERR 19\n"
"ISR_NOERR 20\n ISR_ERR   21\n ISR_NOERR 22\n ISR_NOERR 23\n"
"ISR_NOERR 24\n ISR_NOERR 25\n ISR_NOERR 26\n ISR_NOERR 27\n"
"ISR_NOERR 28\n ISR_ERR   29\n ISR_ERR   30\n ISR_NOERR 31\n"
"ISR_NOERR 32\n ISR_NOERR 33\n ISR_NOERR 34\n ISR_NOERR 35\n"
"ISR_NOERR 36\n ISR_NOERR 37\n ISR_NOERR 38\n ISR_NOERR 39\n"
"ISR_NOERR 40\n ISR_NOERR 41\n ISR_NOERR 42\n ISR_NOERR 43\n"
"ISR_NOERR 44\n ISR_NOERR 45\n ISR_NOERR 46\n ISR_NOERR 47\n"

/* 48-255: the dynamically allocatable range (kernel/idt_alloc.c), where
 * MSI vectors and the LAPIC spurious vector live. Generated by a counted
 * .rept rather than 208 more hand-written lines - which is the same reason
 * the 0-47 block above is macro-generated, just applied where writing them
 * out would be actively unreadable.
 *
 * .altmacro is what makes `%vecnum` expand to the counter's VALUE in a
 * macro argument instead of being passed as the literal string "vecnum".
 * Turned back off immediately after: it also changes how string arguments
 * are parsed, and leaving it on would quietly alter any macro added below. */
".altmacro\n"
".set vecnum, 48\n"
".rept 208\n"
"    ISR_NOERR %vecnum\n"
"    .set vecnum, vecnum+1\n"
".endr\n"
".noaltmacro\n"

".macro STUB_PTR n\n"
"    .quad isr_stub_\\n\n"
".endm\n"

".section .rodata\n"
".global isr_stub_table\n"
".align 8\n"
"isr_stub_table:\n"
".altmacro\n"
".set vecnum, 0\n"
".rept 256\n"
"    STUB_PTR %vecnum\n"
"    .set vecnum, vecnum+1\n"
".endr\n"
".noaltmacro\n"
".section .text\n"
);
