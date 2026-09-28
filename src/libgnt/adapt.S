/* gnt_sysv_to_win64 - enter a Win64 function from a System V call.
 *
 * Reached from an adapter stub (gnt.c) with the target in R11 and the
 * caller's arguments exactly where System V put them:
 *
 *     integer args 1-6   RDI RSI RDX RCX R8 R9
 *     integer args 7+    [rsp+8], [rsp+16], ...   (rsp at entry)
 *     float args         XMM0-XMM7, in order among the float args only
 *
 * and makes the Win64 call:
 *
 *     args 1-4           RCX RDX R8 R9  (or XMM0-3, by POSITION)
 *     args 5+            [rsp+32], [rsp+40], ... after the 32-byte home area
 *
 * Integer arguments move exactly. Eight stack arguments are copied whether
 * or not the caller passed them - reading a few words of the caller's frame
 * that are not arguments is harmless, and they arrive as arguments the
 * callee ignores - so functions of up to 14 integer arguments work. XMM0-3
 * are left alone, which is exact when the float arguments come first.
 *
 * Nothing needs saving for the caller: every register System V expects a
 * callee to preserve (RBX, RBP, R12-R15) Win64 preserves too, and the ones
 * Win64 additionally preserves (RSI, RDI, XMM6-15) System V never relied
 * on. RAX and XMM0 carry the result back unchanged. */

    .text
    .globl  gnt_sysv_to_win64
    .type   gnt_sysv_to_win64, @function
gnt_sysv_to_win64:
    push    %rbp
    mov     %rsp, %rbp
    /* rsp is 16-aligned here (entry was 8 mod 16, then one push). 32 bytes
     * of home area plus ten argument slots is 112, which keeps it so. */
    sub     $112, %rsp

    mov     %r8,  32(%rsp)              /* arg 5 */
    mov     %r9,  40(%rsp)              /* arg 6 */
    mov     16(%rbp), %rax              /* args 7-14: the caller's stack */
    mov     %rax, 48(%rsp)
    mov     24(%rbp), %rax
    mov     %rax, 56(%rsp)
    mov     32(%rbp), %rax
    mov     %rax, 64(%rsp)
    mov     40(%rbp), %rax
    mov     %rax, 72(%rsp)
    mov     48(%rbp), %rax
    mov     %rax, 80(%rsp)
    mov     56(%rbp), %rax
    mov     %rax, 88(%rsp)
    mov     64(%rbp), %rax
    mov     %rax, 96(%rsp)
    mov     72(%rbp), %rax
    mov     %rax, 104(%rsp)

    mov     %rcx, %r9                   /* arg 4 - before RCX is reused */
    mov     %rdx, %r8                   /* arg 3 */
    mov     %rsi, %rdx                  /* arg 2 */
    mov     %rdi, %rcx                  /* arg 1 */
    call    *%r11

    leave
    ret
    .size   gnt_sysv_to_win64, .-gnt_sysv_to_win64

    .section .note.GNU-stack,"",@progbits
