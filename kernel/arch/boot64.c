#include "typesk.h"

#if !defined(__x86_64__)
#error "boot64.c is part of the long-mode kernel; build it with -m64"
#endif

/* ===========================================================================
 * Everything that runs before the kernel proper.
 * ---------------------------------------------------------------------------
 * Replaces stub.asm, lm_enter.c and longmode.c from the 32-bit tree. They
 * merged into one file for a hard reason: ld cannot link elf32 and elf64
 * objects together, so the pre-long-mode code cannot be a separate 32-bit
 * object. The .code32 directive puts 32-bit instructions inside this 64-bit
 * object instead, and .lowtext (VMA == LMA in linker.ld) puts them at an
 * address that is valid with paging off.
 *
 * boot.asm hands over here in 32-bit protected mode with paging OFF - which
 * makes this simpler than the 32-bit kernel's trampoline was. That one had to
 * turn paging off first, because CR4.PAE cannot be modified while CR0.PG is
 * set. Here it was never on:
 *
 *   lgdt              64-bit GDT, 6-byte operand form (still 32-bit mode)
 *   CR4.PAE = 1       long mode is PAE-only
 *   EFER.LME = 1      arms it; nothing observable happens yet
 *   CR3 = boot_pml4   the static hierarchy below
 *   CR0.PG = 1        long mode ACTIVATES. EFER.LMA becomes 1, and the CPU is
 *                     in compatibility mode - 64-bit paging, 32-bit code
 *   ljmp $0x08        loads CS with L=1. THIS is what makes it 64-bit
 *
 * If it dies, read EFER.LMA between the last two steps: 0 means the
 * PAE/LME/CR3 setup is wrong, 1 means the 64-bit code descriptor is. Without
 * that split it is one triple fault with no information in it.
 *
 * --- The page tables are static data ---
 * Built by the assembler, not at runtime, because there is no runtime here:
 * any C that could construct them would itself be 64-bit code that cannot
 * execute yet. Every address is known at link time, so .quad and .rept do the
 * whole job and there is nothing to get wrong at boot.
 *
 * Two mappings, sharing one PD:
 *   identity     0x0000000000000000 -> physical 0, first 1GB
 *   kernel half  0xFFFFFFFF80000000 -> physical 0, first 1GB
 * The identity map exists so the instruction after CR0.PG is still fetchable;
 * paging_init() later replaces all of this with 4KB-granular tables that omit
 * it. Indices: 0xFFFFFFFF80000000 is pml4[511], pdpt[510], pd[0].
 * ======================================================================== */

extern void kernel_start(void);

__asm__(
".section .lowtext,\"ax\"\n"
".code32\n"
".global _kernel_entry\n"
".align 16\n"

"_kernel_entry:\n"
"    cli\n"
"    lgdt boot_gdt64_ptr\n"          /* 6-byte form: still CS.L = 0        */

"    mov  %cr4, %eax\n"              /* CR4.PAE = bit 5                    */
"    or   $0x20, %eax\n"
"    mov  %eax, %cr4\n"

"    mov  $0xC0000080, %ecx\n"       /* EFER.LME = bit 8                   */
"    rdmsr\n"
"    or   $0x100, %eax\n"
"    wrmsr\n"

"    mov  $boot_pml4, %eax\n"        /* .lowtext is VMA == LMA, so this is */
"    mov  %eax, %cr3\n"              /* already the physical address       */

"    mov  %cr0, %eax\n"              /* CR0.PG -> compatibility mode       */
"    or   $0x80000000, %eax\n"
"    mov  %eax, %cr0\n"

"    ljmp $0x08, $entry64\n"         /* CS.L = 1 -> genuinely 64-bit       */

/* --- static paging hierarchy -------------------------------------------
 * In .lowdata, not .lowtext. A 4096-aligned input section forces the enclosing
 * OUTPUT section to 4096 alignment, and ld would then round .lowtext up off the
 * address the boot sector jumps to - which is what happened when KERNEL_LMA
 * was 0x7E00 and .lowtext moved to 0x8000. KERNEL_LMA is page-aligned now
 * (0x100000), so that rounding would be a no-op; the split stays because the
 * constant is allowed to move again. Keeping the tables in their own section
 * leaves .lowtext at its natural 16-byte alignment. */
".section .lowdata,\"a\"\n"
".align 4096\n"
"boot_pml4:\n"
"    .quad boot_pdpt + 0x3\n"        /* [0]   identity                     */
"    .fill 510, 8, 0\n"
"    .quad boot_pdpt + 0x3\n"        /* [511] kernel half                  */

".align 4096\n"
"boot_pdpt:\n"
"    .quad boot_pd + 0x3\n"          /* [0]   -> low 1GB                   */
"    .fill 509, 8, 0\n"
"    .quad boot_pd + 0x3\n"          /* [510] -> same 1GB, kernel view     */
"    .quad 0\n"

".align 4096\n"
"boot_pd:\n"
"    .set pd_addr, 0\n"
"    .rept 512\n"
"    .quad pd_addr + 0x83\n"         /* present | rw | PS (2MB page)       */
"    .set pd_addr, pd_addr + 0x200000\n"
"    .endr\n"

".section .lowtext,\"ax\"\n"
".code32\n"

/* --- minimal 64-bit GDT -------------------------------------------------
 * Only needs to survive until gdt_init() installs the real one with a TSS.
 * 0xAF = G=1, D/B=0, L=1. D/B must be 0 when L is 1; the pair is reserved and
 * faults on the far jump. */
".section .lowtext,\"ax\"\n"
".code32\n"
".align 16\n"
"boot_gdt64:\n"
"    .quad 0x0000000000000000\n"
"    .quad 0x00AF9A000000FFFF\n"     /* 0x08 code64                        */
"    .quad 0x00CF92000000FFFF\n"     /* 0x10 data                          */
"boot_gdt64_end:\n"
"boot_gdt64_ptr:\n"
"    .word boot_gdt64_end - boot_gdt64 - 1\n"
"    .long boot_gdt64\n"

/* --- first 64-bit instructions ----------------------------------------- */
".code64\n"
".align 16\n"
"entry64:\n"
"    mov  $0x10, %ax\n"
"    mov  %ax, %ds\n"
"    mov  %ax, %es\n"
"    mov  %ax, %ss\n"
"    mov  %ax, %fs\n"
"    mov  %ax, %gs\n"

/* Still executing from the identity map at a low address. Switch to the
 * kernel's real stack and jump to the high alias in one go - movabs is the
 * only way to load a full 64-bit address, and an absolute indirect jump is
 * the only way to reach it from here. */
"    movabs $kernel_stack_top, %rsp\n"
"    movabs $kernel_start, %rax\n"
"    jmp  *%rax\n"

".previous\n"
".code64\n"
);

/* .bss is NOBITS: it occupies no space in the flat binary, so nothing has
 * loaded or zeroed it. Every static the C standard promises starts at zero is
 * holding whatever was in RAM. Runs on the stack from .stack, which linker.ld
 * deliberately places OUTSIDE bss_start..bss_end - inside it, this would erase
 * its own return address partway through. */
extern uint8 bss_start;
extern uint8 bss_end;

extern void flk(void);

void kernel_start(void) {
    uint8 *p = &bss_start;

    while (p < &bss_end) {
        *p++ = 0;
    }

    flk();

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
