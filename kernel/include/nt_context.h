#ifndef NT_CONTEXT_H
#define NT_CONTEXT_H

#include "interrupt.h"
#include "process.h"
#include "syscall.h"
#include "typesk.h"

/* A thread's user-mode register state as Win64 writes it down - CONTEXT -
 * and the two things the kernel does with one: hand it to ntdll on the user
 * stack (an APC, an exception) and put it back (NtContinue).
 *
 * --- why the real layout ----------------------------------------------------
 * CONTEXT is not private to Genesis's ntdll. Every exception handler gets a
 * pointer to one, and compiled code reads it through windows.h's offsets -
 * MinGW's C++ unwinder, __C_specific_handler, a debugger. A layout that is
 * merely self-consistent would work for our own ntdll and corrupt everything
 * else, so the offsets below are Microsoft's and are checked at compile time.
 *
 * --- why NtContinue leaves by iretq ----------------------------------------
 * SYSRET takes RIP from RCX and RFLAGS from R11, so the ordinary syscall exit
 * cannot restore a context whose RCX and R11 hold anything else - which is
 * every context captured at a fault. NtContinue therefore leaves through a
 * fabricated interrupt frame and iretq, which restores all sixteen
 * registers, RIP, RSP and RFLAGS from memory. */

typedef struct nt_context {
    uint64 p1_home, p2_home, p3_home, p4_home, p5_home, p6_home;   /* 0x000 */
    uint32 context_flags;                                           /* 0x030 */
    uint32 mxcsr;                                                   /* 0x034 */
    uint16 seg_cs, seg_ds, seg_es, seg_fs, seg_gs, seg_ss;          /* 0x038 */
    uint32 eflags;                                                  /* 0x044 */
    uint64 dr0, dr1, dr2, dr3, dr6, dr7;                            /* 0x048 */
    uint64 rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;                  /* 0x078 */
    uint64 r8, r9, r10, r11, r12, r13, r14, r15;                    /* 0x0B8 */
    uint64 rip;                                                     /* 0x0F8 */
    uint8  flt_save[512];            /* XMM_SAVE_AREA32 = FXSAVE    0x100 */
    uint8  vector_register[26 * 16];                                /* 0x300 */
    uint64 vector_control;                                          /* 0x4A0 */
    uint64 debug_control;
    uint64 last_branch_to_rip, last_branch_from_rip;
    uint64 last_exception_to_rip, last_exception_from_rip;
} __attribute__((aligned(16))) nt_context_t;

#define NT_CONTEXT_SIZE           0x4D0

#define NT_CONTEXT_AMD64          0x00100000u
#define NT_CONTEXT_CONTROL        (NT_CONTEXT_AMD64 | 0x1u)
#define NT_CONTEXT_INTEGER        (NT_CONTEXT_AMD64 | 0x2u)
#define NT_CONTEXT_SEGMENTS       (NT_CONTEXT_AMD64 | 0x4u)
#define NT_CONTEXT_FLOATING_POINT (NT_CONTEXT_AMD64 | 0x8u)
#define NT_CONTEXT_FULL           (NT_CONTEXT_CONTROL | NT_CONTEXT_INTEGER | \
                                   NT_CONTEXT_FLOATING_POINT)

/* The running thread's state at a syscall (rax is the value the call will
 * return, rcx/r11 what SYSRET would leave in them) or at an interrupt from
 * ring 3. Either way the live FPU/SSE registers go into flt_save - the
 * kernel never touches them, so while it runs they are still the thread's. */
void ntctx_capture_syscall(nt_context_t *c, const struct syscall_frame *f,
                           uint64 user_rsp, uint64 rax);
void ntctx_capture_intr(nt_context_t *c, const struct interrupt_frame *f);

/* Copy `c` onto the user stack below `rsp`, with `extra` more bytes of
 * zeroed room above it (for an EXCEPTION_RECORD), 16-aligned. Returns where
 * the CONTEXT landed - the new RSP - or 0 if those pages are not mapped
 * (a thread that has run off its stack cannot be told about it this way). */
uint64 ntctx_push(const nt_context_t *c, uint64 rsp, uint64 extra);

/* Resume the calling thread in ring 3 exactly as `c` describes: every
 * general register, RIP, RSP, RFLAGS and the FPU/SSE state. Never returns.
 * `c` must have been through ntctx_sanitize. */
void ntctx_resume(nt_context_t *c) __attribute__((noreturn));

/* Make a context from user memory safe to resume: user code and stack
 * selectors whatever it says, RFLAGS limited to the bits ring 3 may own
 * (and IF forced on), MXCSR limited to what this CPU accepts so fxrstor
 * cannot fault. 0 if RIP or RSP is not a canonical user address - iretq to
 * one would fault in the kernel, not in the thread. */
int ntctx_sanitize(nt_context_t *c);

/* --- APCs (ROADMAP 14(b)) ---------------------------------------------------
 *
 * A user-mode APC: NormalRoutine(NormalContext, SystemArgument1,
 * SystemArgument2), run on the target thread when it next waits alertably
 * (or calls NtTestAlert). Queued per thread, first in first out. */
int  nt_apc_queue(process_t *t, uint64 routine, uint64 ctx,
                  uint64 arg1, uint64 arg2);
int  nt_apc_pending(const process_t *t);
void nt_apc_flush(process_t *t);          /* thread exit: drop them all */

/* Deliver the calling thread's first queued APC on its way out of the
 * current syscall: `status` is what the interrupted call returns once the
 * APC has run (NtContinue puts it back). Rewrites `f` to enter ntdll's
 * KiUserApcDispatcher with the CONTEXT on the user stack. Returns the value
 * for RAX, or `status` unchanged when nothing was delivered. */
uint64 nt_apc_deliver(struct syscall_frame *f, uint64 status);

/* --- exceptions (ROADMAP 14(c)) ------------------------------------------
 *
 * A fault in ring 3 of a Windows process becomes an exception the process
 * handles itself: an EXCEPTION_RECORD and the faulting CONTEXT on the user
 * stack, and the thread sent into ntdll!KiUserExceptionDispatcher. The
 * layout at the dispatcher's RSP (all of it described by the dispatcher's
 * own unwind info, so an unwind passes through it to the faulting frame):
 *
 *   +0x000  CONTEXT                (0x4D0)
 *   +0x4D0  EXCEPTION_RECORD       (0x98, padded to 0xA0)
 *   +0x570  machine frame          RIP, CS, RFLAGS, RSP, SS
 *
 * Returns 1 if the fault was handed over (the frame is rewritten; iretq
 * goes to the dispatcher), 0 if it cannot be - not a Windows process, no
 * dispatcher, or no stack left to put it on - and the caller kills the
 * thread as before. */
#define NT_EXC_RECORD_OFFSET    0x4D0
#define NT_EXC_MACHFRAME_OFFSET 0x570
#define NT_EXC_FRAME_EXTRA      (0x570 + 0x30 - NT_CONTEXT_SIZE)
int nt_exception_deliver(struct interrupt_frame *f, uint64 cr2);

/* NtRaiseException(PEXCEPTION_RECORD, PCONTEXT, BOOLEAN FirstChance).
 * FirstChance TRUE dispatches it like a fault; FALSE means nothing in the
 * process handled it, and the process ends with its exception code. */
uint64 nt_raise_exception(struct syscall_frame *f, uint64 rec_ptr,
                          uint64 ctx_ptr, uint64 first_chance);

/* NtContinue(PCONTEXT, BOOLEAN TestAlert) and NtTestAlert(). */
uint64 nt_continue(struct syscall_frame *f, uint64 ctx_ptr, uint64 test_alert);
uint64 nt_test_alert(struct syscall_frame *f);

#endif
