OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_kernel_entry)

/* Long-mode linker script.
 *
 * Three differences from the 32-bit version, beyond the obvious base address:
 *
 * 1. The boot sector is NOT linked here anymore. It is assembled standalone
 *    with `nasm -f bin` and concatenated onto the front of the image by
 *    build.py. 16-bit code with absolute relocations inside an elf64 link
 *    ranges from awkward to impossible, and there is nothing to gain from it -
 *    the boot sector shares no symbols with the kernel.
 *
 *    The one thing it did need, kernel_sectors, now comes from build.py via
 *    `nasm -Dkernel_sectors=N` computed from the built image size.
 *
 * 2. .lowtext holds every instruction that executes before long mode: the
 *    protected-mode entry, the page table setup, and the trampoline. It is
 *    32-bit code inside a 64-bit object, emitted by the .code32 directive in
 *    lm_enter.c. VMA == LMA, because paging is off or mid-transition
 *    throughout and addresses must be physical.
 *
 * 3. KERNEL_VMA is 0xFFFFFFFF80000000 - the top 2GB. Not an arbitrary choice:
 *    -mcmodel=kernel generates 32-bit sign-extended offsets on the assumption
 *    the kernel lives there. Move the base and the code model breaks.
 */
KERNEL_VMA = 0xFFFFFFFF80000000;

/* Where the flat image is loaded, and where boot.asm jumps.
 *
 * 0x100000, not the 0x7E00 this was for most of the tree's life. Loading
 * immediately after the boot sector meant the image grew upward toward the
 * 32-bit stack boot.asm sets up at 0x90000, giving a hard 557568-byte ceiling
 * that the kernel had reached 98% of. Above 1MB there is no such neighbour:
 * the only remaining bound is KERNEL_MAP_SIZE in kernel/include/paging.h,
 * which is a page-table constant costing .bss rather than image bytes.
 *
 * The move is what boot.asm's unreal-mode copy exists for - INT 13h cannot
 * write above 1MB. Everything else was already independent of this value:
 * the boot page tables map the low 1GB with 2MB pages either way, the PMM
 * reserves 0 through kernel_phys_end whatever that is, and the AP trampoline
 * is copied to a fixed low page at runtime rather than linked at one. */
KERNEL_LMA = 0x100000;

SECTIONS
{
    . = KERNEL_LMA;

    /* Runs with paging off or mid-switch: linked where it is loaded. */
    .lowtext : {
        /* The image head (kernel/arch/vbe_boot.c) FIRST: a jump to
           _kernel_entry at 0x100000, where boot.asm calls, then the VBE mode
           request words and the real-mode stub boot.asm far-calls at
           0xFFFF:0x0020. KEEP because nothing references the stub by name. */
        KEEP(*(.lowtext.head))
        *(.lowtext*)
    }

    /* Page tables live here rather than in .lowtext: a 4096-aligned input
       section forces the enclosing output section to 4096 alignment, which
       would round _kernel_entry up off the address the boot sector jumps to.

       That was a live bug when KERNEL_LMA was 0x7E00 - .lowtext would have
       moved to 0x8000. KERNEL_LMA is page-aligned now, so the rounding would
       be a no-op and the split is no longer load-bearing. It stays because
       the separation is correct on its own terms and because the constant
       above is allowed to change again. */
    . = ALIGN(4096);
    .lowdata : {
        *(.lowdata*)
    }
    . = ALIGN(16);

    /* Everything from here is linked high and loaded low. The VMA/LMA delta
       stays exactly KERNEL_VMA - do not ALIGN in one space without the other,
       or virt_to_phys() silently stops being a subtraction. */
    . += KERNEL_VMA;

    .text : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text*)
    }
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata*)
    }
    .data : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data*)

        /* FreeBSD LINK SETS.
         *
         * Every SYSINIT(), MTX_SYSINIT(), SYSCTL_NODE() and DOMAIN_SET() in
         * the vendored tree emits a pointer into a section named set_<name>
         * (see kernel/bsd/compat/sys/linker_set.h), and the code that walks
         * one asks ld for __start_set_<name> and __stop_set_<name>. ld
         * generates that pair automatically for any section whose name is a
         * valid C identifier - but only if the section is actually PLACED.
         *
         * Without this line they are ORPHAN sections. ld still places them
         * somewhere and the __start_/__stop_ symbols still resolve, so
         * nothing fails to link - it just puts them wherever it decides,
         * which here means potentially after `load_end` and therefore outside
         * the bytes the bootloader reads. The failure mode is a set that
         * appears empty at runtime, which reads as "no initialisers were
         * registered" rather than as a linker-script problem.
         *
         * KEEP(), because --gc-sections would otherwise discard a set entry:
         * nothing references it by name, which is the entire point of a link
         * set. This build does not use --gc-sections today; the KEEP is what
         * stops that from becoming a silent regression if it ever does.
         *
         * SORT() so that entries from different objects land in a stable
         * order. kernel/bsd/sysinit.c sorts by (subsystem, order) anyway and
         * its sort is stable, so this is what makes the tie-break between two
         * equal-priority initialisers reproducible from one build to the
         * next. */
        . = ALIGN(8);
        __start_set_sysinit_set = .;
        KEEP(*(SORT(set_sysinit_set)))
        __stop_set_sysinit_set = .;

        . = ALIGN(8);
        __start_set_sysuninit_set = .;
        KEEP(*(SORT(set_sysuninit_set)))
        __stop_set_sysuninit_set = .;

        . = ALIGN(8);
        __start_set_sysctl_set = .;
        KEEP(*(SORT(set_sysctl_set)))
        __stop_set_sysctl_set = .;

        /* LinuxKPI per-CPU variables (<linux/percpu.h>): the section IS
         * CPU 0's copy of every DEFINE_PER_CPU in the kernel, plus the
         * reserve alloc_percpu and loaded modules carve from. Placed, with
         * its bounds named, for the same orphan-section reason as the link
         * sets above; kernel/driver/lkpi_smp.c copies it once per CPU. */
        . = ALIGN(64);
        __start_lkpi_percpu = .;
        KEEP(*(lkpi_percpu))
        . = ALIGN(64);
        __stop_lkpi_percpu = .;

        /* Any other set a future vendored file declares. Placed after the
         * three named above so that adding one does not need a linker-script
         * edit - only a SET_DECLARE in the C file. */
        . = ALIGN(8);
        KEEP(*(SORT(set_*)))
    }

    /* Reserved space for the kernel's own symbol table (ROADMAP item 11 -
       see kernel/ksyms_data.c, kernel/ksyms.c and build.py's gen_ksyms()).
       kernel/ksyms_data.c defines the actual reservation as a real,
       non-all-zero const array (`section(".ksyms")`) - a plain
       ". += 262144;" here with no input section is classified NOBITS by
       ld (same as .bss) and objcopy then emits NO bytes for it at all,
       which was tried first and confirmed the hard way. Picking up a real
       input section keeps this PROGBITS, so objcopy actually writes
       262144 real bytes into kernel.bin for build.py to patch afterwards -
       no second link. 8-byte aligned so ksyms.c can cast the region
       straight to its packed {u64 addr; u32 name_offset; u32 pad;} entry
       array without a manual byte-copy. */
    . = ALIGN(8);
    .ksyms : AT(ADDR(.ksyms) - KERNEL_VMA) {
        *(.ksyms)
    }

    /* Last byte present in the flat binary; .bss is NOBITS and emits nothing,
       so the loader must not be told to read past here. */
    load_end = .;

    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA) {
        bss_start = .;
        *(COMMON)
        *(.bss*)
        bss_end = .;
    }

    /* Outside bss_start..bss_end deliberately: the entry code sets RSP to
       kernel_stack_top before calling into C, so zero_bss() runs ON this
       stack. Inside the zeroed range it would erase its own return address
       partway through the loop. 64KB rather than 16 - the 64-bit ABI has no
       red zone available to us (-mno-red-zone) and frames are wider. */
    .stack (NOLOAD) : {
        . = ALIGN(16);
        kernel_stack_bottom = .;
        . += 64K;
        kernel_stack_top = .;
    }

    /* Guaranteed-good stack for the double fault handler, installed via
       gdt_set_ist(1, ist1_stack_top). If a double fault means the main kernel
       stack is unusable, a handler running on it faults again and the machine
       triple-faults with nothing printed. */
    .ist_stacks (NOLOAD) : {
        . = ALIGN(16);
        ist1_stack_bottom = .;
        . += 16K;
        ist1_stack_top = .;
    }

    kernel_end = .;
    end = kernel_end;

    /* Physical counterparts. `end` is a 64-bit virtual address now, so the PMM
       - which works in physical frames - must use kernel_phys_end or it will
       try to mark most of a 64-bit address space. */
    kernel_phys_start = KERNEL_LMA;
    kernel_phys_end   = kernel_end - KERNEL_VMA;

    /* Measured in physical space, and no longer includes the boot sector:
       build.py concatenates that separately and adds 1. */
    kernel_size    = (load_end - KERNEL_VMA) - KERNEL_LMA;
    kernel_sectors = (kernel_size + 511) / 512;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
