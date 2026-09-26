/* _start for the dynamic linker.
 *
 * Assembly rather than C, for one reason: the stack pointer the kernel built
 * IS the program's argument block, and it has to arrive at the program's
 * entry point unchanged. A C _start would push a frame, and there is no way
 * to write "call this, then jump elsewhere with RSP exactly as it was on
 * entry" in C that does not depend on what the compiler chose to do with the
 * frame pointer.
 *
 * The sequence:
 *   1. RSP on entry points at argc. Pass it to rtld_start as its argument.
 *   2. Align RSP down to 16 for the call - the SysV ABI requires it, and a
 *      misaligned call into anything using SSE faults on the first spill.
 *      The kernel's frame is already 16-aligned at argc, so the sub below is
 *      what UNaligns it in the ordinary case; the copy in R12 is what matters.
 *   3. Restore the original RSP and jump to whatever rtld_start returned.
 *
 * Nothing is pushed across the jump. The program finds argc, argv, envp and
 * the auxv exactly where the kernel put them - which is why this cannot
 * simply `ret`.
 */

    .text
    .globl _start
    .type  _start, @function
_start:
    /* Self-relocation, before anything else in the linker runs.
     *
     * This object's own RELATIVE relocations have not been applied yet, so
     * every global in rtld.c is currently a wrong address. self_relocate is
     * written to touch no globals at all (see the comment on it), and this is
     * the call that makes every OTHER global usable.
     *
     * The two arguments are computed here rather than read from the auxv,
     * because reading the auxv means walking envp, which means calling
     * something - and there is nothing safe to call yet. _DYNAMIC and the
     * GOT's first entry give the same two numbers with no memory access
     * outside this object:
     *
     *   base    = the difference between where _DYNAMIC actually is and where
     *             the link-time address says it should be
     *   dynamic = _DYNAMIC, biased
     *
     * lea gives the RUNTIME address (RIP-relative, no relocation involved);
     * the GOT slot holds the LINK-TIME one. Their difference is the bias. */
    leaq _DYNAMIC(%rip), %rsi        /* runtime address of _DYNAMIC        */
    movq _GLOBAL_OFFSET_TABLE_(%rip), %rdi   /* link-time address of it    */
    movq %rsi, %rax
    subq %rdi, %rax                  /* rax = load bias                    */
    movq %rax, %rdi                  /* arg 1: base                        */
                                     /* arg 2: dynamic, already in rsi     */
    movq %rsp, %r12                  /* the kernel's frame, saved          */
    andq $-16, %rsp
    call self_relocate

    /* From here every global in rtld.c is valid. */
    movq %r12, %rdi                  /* arg 1: the stack pointer           */
    andq $-16, %rsp
    call rtld_start                  /* returns the program's entry point  */

    /* RSP back to exactly what the kernel handed us, then go. The program's
     * _start reads argc at (%rsp) and it must be the kernel's frame, not
     * ours. */
    movq %r12, %rsp

    /* Cleared, because the program is entitled to assume a clean start.
     * RDX in particular: the ABI says it holds a function to register with
     * atexit, and a non-null leftover would be called at exit. */
    xorq %rdx, %rdx
    xorq %rbp, %rbp
    jmp  *%rax

    .size _start, . - _start

/* No executable stack. Without this the linker marks the stack RWX because a
 * .S file that says nothing is assumed to want a trampoline - and then every
 * program loaded through this interpreter gets an executable stack, which is
 * a property nobody would think to look for here. */
    .section .note.GNU-stack,"",@progbits
