/* Win64 -> System V adapters: how a Windows program calls into a Linux .so.
 * ROADMAP item 19, stage 3; handed out by GetProcAddress (loader.c).
 *
 * GNT_ADAPTERS fixed stubs in kernel32's own .text, each 16 bytes:
 *
 *     mov  gnt_elf_targets+8*i(%rip), %r11
 *     jmp  gnt_win64_to_sysv
 *
 * The target is DATA (gnt_elf_targets, filled in by GetProcAddress), so no
 * code is ever written at run time and nothing needs an executable heap.
 *
 * gnt_win64_to_sysv, entered with the target in R11 and a Win64 call's
 * arguments in place:
 *
 *     Win64 args 1-4   RCX RDX R8 R9       args 5+   [entry rsp + 40 + 8k]
 *     SysV  args 1-6   RDI RSI RDX RCX R8 R9         args 7+ [call rsp + 8k]
 *
 * and what each side may clobber: Win64 promises its caller RSI, RDI and
 * XMM6-XMM15 survive a call, System V lets a callee destroy all three -
 * so they are saved here around the call. RBX, RBP, R12-R15 are callee-saved
 * under both. XMM0-XMM3 pass through (exact for leading float arguments),
 * AL is set to 4 as the varargs vector-register count, and RAX/XMM0 carry
 * the result back. Eight stack arguments are copied whether or not they
 * exist - reading a little of the caller's frame is harmless - so up to 14
 * integer arguments arrive. */

#define GNT_ADAPTERS 256

    .data
    .p2align 3
    .globl  gnt_elf_targets
gnt_elf_targets:
    .fill   GNT_ADAPTERS, 8, 0

    .text
    .p2align 4
    .globl  gnt_elf_adapters
gnt_elf_adapters:
    .set    i, 0
    .rept   GNT_ADAPTERS
    .p2align 4
    movq    gnt_elf_targets + 8 * i(%rip), %r11
    jmp     gnt_win64_to_sysv
    .set    i, i + 1
    .endr

    .p2align 4
gnt_win64_to_sysv:
    push    %rbp
    mov     %rsp, %rbp
    push    %rsi
    push    %rdi
    /* 8 mod 16 at entry; three pushes make it 0 mod 16, and 224 keeps it
     * there for the call: 64 bytes of outgoing stack arguments at the
     * bottom, then 160 for XMM6-15. */
    sub     $224, %rsp
    movdqu  %xmm6,   64(%rsp)
    movdqu  %xmm7,   80(%rsp)
    movdqu  %xmm8,   96(%rsp)
    movdqu  %xmm9,  112(%rsp)
    movdqu  %xmm10, 128(%rsp)
    movdqu  %xmm11, 144(%rsp)
    movdqu  %xmm12, 160(%rsp)
    movdqu  %xmm13, 176(%rsp)
    movdqu  %xmm14, 192(%rsp)
    movdqu  %xmm15, 208(%rsp)

    /* Win64 args 7-14 (at entry rsp + 56.., which is rbp + 64..) become the
     * System V stack arguments. */
    mov     64(%rbp), %rax
    mov     %rax, 0(%rsp)
    mov     72(%rbp), %rax
    mov     %rax, 8(%rsp)
    mov     80(%rbp), %rax
    mov     %rax, 16(%rsp)
    mov     88(%rbp), %rax
    mov     %rax, 24(%rsp)
    mov     96(%rbp), %rax
    mov     %rax, 32(%rsp)
    mov     104(%rbp), %rax
    mov     %rax, 40(%rsp)
    mov     112(%rbp), %rax
    mov     %rax, 48(%rsp)
    mov     120(%rbp), %rax
    mov     %rax, 56(%rsp)

    mov     %rcx, %rdi                  /* arg 1 */
    mov     %rdx, %rsi                  /* arg 2 */
    mov     %r8,  %rdx                  /* arg 3 */
    mov     %r9,  %rcx                  /* arg 4 */
    mov     48(%rbp), %r8               /* arg 5: Win64 [entry rsp + 40] */
    mov     56(%rbp), %r9               /* arg 6 */
    mov     $4, %eax                    /* varargs: at most XMM0-3 in use */
    call    *%r11

    movdqu   64(%rsp), %xmm6
    movdqu   80(%rsp), %xmm7
    movdqu   96(%rsp), %xmm8
    movdqu  112(%rsp), %xmm9
    movdqu  128(%rsp), %xmm10
    movdqu  144(%rsp), %xmm11
    movdqu  160(%rsp), %xmm12
    movdqu  176(%rsp), %xmm13
    movdqu  192(%rsp), %xmm14
    movdqu  208(%rsp), %xmm15
    lea     -16(%rbp), %rsp
    pop     %rdi
    pop     %rsi
    pop     %rbp
    ret
