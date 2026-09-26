#include "typesk.h"

/* The real-mode trampoline every application processor starts in.
 *
 * An AP comes out of INIT-SIPI-SIPI in REAL MODE, at CS = (SIPI page << 8),
 * IP = 0. Not protected mode, not long mode - real mode, with no GDT, no
 * paging, and 16-bit registers, on a machine where the BSP has been running
 * 64-bit code for a while. So this code has to redo the entire boot
 * transition, per CPU.
 *
 * It is written here rather than in boot64.c despite doing nearly the same
 * thing, and the difference is what makes it its own file: boot64.c runs
 * ONCE, at a link-time-known address, with paging off. This runs many times,
 * must be COPIED to a page below 1MB at runtime (a SIPI's start address is a
 * page number in one byte - it cannot name the kernel's address), and the
 * page it is copied to has to be identity-mapped in the CR3 it installs,
 * because the instruction after `mov %eax, %cr0` still has to be fetchable.
 * That last point is the one that turns into a triple fault with nothing to
 * read if it is missed.
 *
 * Everything the BSP has to hand over - CR3, a stack, and where to go - is
 * patched into .quad slots below after the blob is copied. The alternative,
 * computing them in the trampoline, would mean parsing kernel structures in
 * 16-bit code.
 *
 * The GDT here is minimal and temporary: null, a 32-bit code/data pair to
 * get through protected mode, and a 64-bit code descriptor. The AP loads the
 * kernel's real GDT from C, once it can run C.
 */

/* --- why this is NOT in .lowtext ----------------------------------------
 *
 * boot64.c's pre-long-mode code lives in .lowtext, which linker.ld places at
 * KERNEL_LMA (0x7E00) with VMA == LMA, because it executes with paging off
 * at the address it was loaded to. This looks like the same kind of code and
 * it is not.
 *
 * Two reasons. The blunt one: .lowtext STARTS at 0x7E00 and the boot sector
 * jumps to exactly that address, so whichever object file contributes first
 * owns the entry point. `kernel_ap_trampoline.o` sorts before
 * `kernel_boot64.o` on the linker command line, so putting this there
 * silently moved _kernel_entry and the machine booted to nothing at all -
 * no output, no fault, QEMU just exited.
 *
 * The real one: this blob does not need a link address. It is COPIED to
 * 0x8000 before use, and every address inside it is written as
 * AP_BASE + (label - ap_tramp_start), which the assembler folds to a
 * constant. So it can be linked anywhere, and .text - high, with the rest of
 * the kernel - is where it belongs.
 */
__asm__(
".section .text.aptramp,\"ax\"\n"
".global ap_tramp_start\n"
".global ap_tramp_end\n"
".global ap_tramp_cr3\n"
".global ap_tramp_stack\n"
".global ap_tramp_entry\n"

/* Where this blob will be COPIED to. Every absolute address below is
 * computed against it, so changing it here is the only edit needed - but it
 * must match AP_TRAMPOLINE_PHYS in smp.c, and it must be a page number that
 * fits in the SIPI's single byte (so under 1MB, and page-aligned). */
".set AP_BASE, 0x8000\n"
/* label -> the address that label will have once copied.
 *
 * Written as AP_BASE + (label - ap_tramp_start) at every use rather than as
 * a single ".set AP_AT, AP_BASE - ap_tramp_start". The assembler refuses
 * that: ap_tramp_start is section-relative and AP_BASE is absolute, so their
 * difference is not a constant it can emit. The DIFFERENCE OF TWO SYMBOLS IN
 * THE SAME SECTION is absolute, though, which is why this form assembles. */

".code16\n"
"ap_tramp_start:\n"
"    cli\n"
"    cld\n"
"    xorw %ax, %ax\n"
"    movw %ax, %ds\n"
"    movw %ax, %es\n"
"    movw %ax, %ss\n"

/* lgdtl, not lgdt: the 32-bit form loads a 4-byte base. The 16-bit form
 * would silently truncate the base to 24 bits, and the GDT would be found
 * at the right address only by luck. */
"    lgdtl (AP_BASE + ap_gdt_ptr - ap_tramp_start)\n"

"    movl %cr0, %eax\n"
"    orl $1, %eax\n"
"    movl %eax, %cr0\n"
"    ljmpl $0x08, $(AP_BASE + ap_prot - ap_tramp_start)\n"

".code32\n"
"ap_prot:\n"
"    movw $0x10, %ax\n"
"    movw %ax, %ds\n"
"    movw %ax, %es\n"
"    movw %ax, %ss\n"
"    movw %ax, %fs\n"
"    movw %ax, %gs\n"

/* PAE. Long mode is PAE-only; setting LME without this gives a #GP on the
 * write to CR0.PG rather than anything that names the real problem. */
"    movl %cr4, %eax\n"
"    orl $(1 << 5), %eax\n"
"    movl %eax, %cr4\n"

"    movl (AP_BASE + ap_tramp_cr3 - ap_tramp_start), %eax\n"
"    movl %eax, %cr3\n"

/* EFER: LME (bit 8) to arm long mode, and NXE (bit 11) in the same write.
 *
 * NXE here is ROADMAP's Owed "second CPU needs its own write" - it is a
 * per-CPU MSR, so the BSP setting it does nothing for this core, and an AP
 * without it treats every NX page-table bit as reserved. That would fault on
 * the first kernel page it touched, since paging.c sets NX on data mappings.
 * Doing it in the same wrmsr as LME rather than later in C is deliberate:
 * there is no window where this CPU is in long mode without it. */
"    movl $0xC0000080, %ecx\n"
"    rdmsr\n"
"    orl $((1 << 8) | (1 << 11)), %eax\n"
"    wrmsr\n"

"    movl %cr0, %eax\n"
"    orl $(1 << 31), %eax\n"
"    movl %eax, %cr0\n"
"    ljmpl $0x18, $(AP_BASE + ap_long - ap_tramp_start)\n"

".code64\n"
"ap_long:\n"
"    movq (AP_BASE + ap_tramp_stack - ap_tramp_start), %rsp\n"
"    movq (AP_BASE + ap_tramp_entry - ap_tramp_start), %rax\n"
/* An indirect jump, not a call. There is nothing to return to - the entry
 * point never comes back - and the stack has only just been established. */
"    jmp *%rax\n"

".align 8\n"
"ap_tramp_cr3:   .quad 0\n"
"ap_tramp_stack: .quad 0\n"
"ap_tramp_entry: .quad 0\n"

".align 8\n"
"ap_gdt:\n"
"    .quad 0x0000000000000000\n"   /* null                                */
"    .quad 0x00CF9A000000FFFF\n"   /* 0x08: 32-bit code, base 0, 4GB      */
"    .quad 0x00CF92000000FFFF\n"   /* 0x10: 32-bit data                   */
/* 0x18: 64-bit code. L=1 (bit 53) is the bit that matters; the limit and
 * base are ignored in long mode, which is why this one looks so different
 * from the two above. */
"    .quad 0x00AF9A000000FFFF\n"
"ap_gdt_end:\n"

"ap_gdt_ptr:\n"
"    .word ap_gdt_end - ap_gdt - 1\n"
"    .long AP_BASE + ap_gdt - ap_tramp_start\n"

"ap_tramp_end:\n"
".previous\n"
);
