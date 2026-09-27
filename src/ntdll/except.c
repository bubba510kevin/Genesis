/* Contexts, APCs and (ROADMAP 14(c)) exceptions - the user-mode half of
 * kernel/exec/nt_context.c.
 *
 * The kernel's side of both is the same move: capture the thread's
 * registers into a CONTEXT, push it on the thread's own stack, and send the
 * thread into a dispatcher here with RSP pointing at it. What the dispatcher
 * does differs; how it ends does not - NtContinue(Context), which puts every
 * register back and never returns.
 */

#include "ntdll.h"

typedef char context_layout[(sizeof(CONTEXT) == 0x4D0 &&
    __builtin_offsetof(CONTEXT, ContextFlags) == 0x30 &&
    __builtin_offsetof(CONTEXT, EFlags) == 0x44 &&
    __builtin_offsetof(CONTEXT, Rax) == 0x78 &&
    __builtin_offsetof(CONTEXT, Rsp) == 0x98 &&
    __builtin_offsetof(CONTEXT, Rip) == 0xF8 &&
    __builtin_offsetof(CONTEXT, FltSave) == 0x100 &&
    __builtin_offsetof(CONTEXT, VectorRegister) == 0x300) ? 1 : -1];

/* --- KiUserApcDispatcher -------------------------------------------------
 *
 * Entered from the kernel (nt_apc_deliver), never called. RSP points at the
 * CONTEXT to resume, 16-aligned; its four home slots carry the APC, as on
 * NT: P1Home the NormalContext (first argument), P2Home and P3Home the two
 * system arguments, P4Home the routine. The routine gets the CONTEXT as a
 * fourth argument, which is also what NT passes.
 *
 * The `call` leaves the routine exactly as if it had been called normally:
 * RSP 8 mod 16, and the 32-byte home area above its return address is the
 * CONTEXT's P1-P4Home - which the routine may scribble on, harmlessly, since
 * NtContinue does not read them.
 *
 * Then NtContinue(Context, TRUE): the interrupted wait's return, or the next
 * APC first. If NtContinue ever fails, the context is unusable and there is
 * nowhere to go; the thread ends with the status rather than running on in
 * a frame that does not exist. */
__asm__(
".text\n"
".globl KiUserApcDispatcher\n"
"KiUserApcDispatcher:\n"
"    movq 0x00(%rsp), %rcx\n"
"    movq 0x08(%rsp), %rdx\n"
"    movq 0x10(%rsp), %r8\n"
"    movq 0x18(%rsp), %rax\n"
"    movq %rsp, %r9\n"
"    call *%rax\n"
"    movq %rsp, %rcx\n"
"    movl $1, %edx\n"
"    call NtContinue\n"
"    movl %eax, %edx\n"
"    movq $-2, %rcx\n"
"    call NtTerminateThread\n"
"1:  jmp 1b\n"
);

/* --- RtlCaptureContext ---------------------------------------------------
 *
 * The caller's registers as they will be when this returns: RIP is the
 * return address, RSP the caller's RSP after the return pops it. Every
 * general register is stored before any is used as scratch, RFLAGS is
 * pushed first so nothing below disturbs it, and the FPU/SSE state goes
 * into FltSave with fxsave - which needs the CONTEXT 16-aligned, as
 * CONTEXT's declaration guarantees. */
__asm__(
".text\n"
".globl RtlCaptureContext\n"
"RtlCaptureContext:\n"
"    pushfq\n"
"    movq %rax, 0x78(%rcx)\n"
"    movq %rcx, 0x80(%rcx)\n"
"    movq %rdx, 0x88(%rcx)\n"
"    movq %rbx, 0x90(%rcx)\n"
"    movq %rbp, 0xA0(%rcx)\n"
"    movq %rsi, 0xA8(%rcx)\n"
"    movq %rdi, 0xB0(%rcx)\n"
"    movq %r8,  0xB8(%rcx)\n"
"    movq %r9,  0xC0(%rcx)\n"
"    movq %r10, 0xC8(%rcx)\n"
"    movq %r11, 0xD0(%rcx)\n"
"    movq %r12, 0xD8(%rcx)\n"
"    movq %r13, 0xE0(%rcx)\n"
"    movq %r14, 0xE8(%rcx)\n"
"    movq %r15, 0xF0(%rcx)\n"
"    leaq 16(%rsp), %rax\n"          /* past RFLAGS and the return address */
"    movq %rax, 0x98(%rcx)\n"
"    movq 8(%rsp), %rax\n"
"    movq %rax, 0xF8(%rcx)\n"
"    movq (%rsp), %rax\n"
"    movl %eax, 0x44(%rcx)\n"
"    movw %cs, 0x38(%rcx)\n"
"    movw %ds, 0x3A(%rcx)\n"
"    movw %es, 0x3C(%rcx)\n"
"    movw %fs, 0x3E(%rcx)\n"
"    movw %gs, 0x40(%rcx)\n"
"    movw %ss, 0x42(%rcx)\n"
"    fxsave 0x100(%rcx)\n"
"    stmxcsr 0x34(%rcx)\n"
"    movl $0x10000F, 0x30(%rcx)\n"   /* CONTEXT_FULL | CONTEXT_SEGMENTS */
"    movq 0x78(%rcx), %rax\n"
"    popfq\n"
"    ret\n"
);
