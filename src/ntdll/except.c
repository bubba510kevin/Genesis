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

/* ===========================================================================
 * Structured exception handling - ROADMAP 14(c).
 *
 * The x64 model, in the order an exception meets it:
 *
 *   1. The kernel turns a fault into an EXCEPTION_RECORD and a CONTEXT on
 *      the thread's stack and enters KiUserExceptionDispatcher (or a
 *      program calls RtlRaiseException, which builds the same two itself).
 *   2. RtlDispatchException offers it to the vectored handlers, then walks
 *      the stack frame by frame: RtlLookupFunctionEntry finds each
 *      function's RUNTIME_FUNCTION in its image's .pdata, RtlVirtualUnwind
 *      replays its prologue backwards to reach the caller - and reports the
 *      frame's language handler if it has one.
 *   3. A handler says continue here (the CONTEXT is resumed), keep looking,
 *      or - for __except - unwind to me: RtlUnwindEx walks the same frames
 *      again, calling each one's handler as a TERMINATION handler (that is
 *      where __finally blocks run), and resumes in the target frame at the
 *      __except block.
 *   4. Nothing took it: the unhandled filter, then NtRaiseException's last
 *      chance, which ends the process.
 * ======================================================================== */

/* --- finding a function's unwind data -------------------------------------- */

#define PEB_MODULES_OFFSET 0xA00
#define MODULES_MAGIC      0x444F4D47u        /* kernel/include/teb.h */
#define MAX_MODULES        16

typedef struct {
    DWORD magic;
    DWORD count;
    struct {
        QWORD base;
        QWORD size;
    } mod[MAX_MODULES];
} module_table_t;

static const module_table_t *module_table(void) {
    const BYTE *peb = (const BYTE *)NtCurrentTeb()->ProcessEnvironmentBlock;
    const module_table_t *t = (const module_table_t *)(peb + PEB_MODULES_OFFSET);

    return (t->magic == MODULES_MAGIC) ? t : NULL_PTR;
}

/* The image's exception directory: IMAGE_DIRECTORY_ENTRY_EXCEPTION, read
 * from the headers the loader mapped with the image. */
static const RUNTIME_FUNCTION *image_pdata(QWORD base, DWORD *count) {
    DWORD nt = *(const DWORD *)(base + 0x3C);
    const BYTE *opt = (const BYTE *)(base + nt + 0x18);
    DWORD rva, size;

    if (*(const DWORD *)(base + nt) != 0x00004550u ||   /* "PE\0\0" */
        *(const WORD *)opt != 0x20Bu ||                  /* PE32+    */
        *(const DWORD *)(opt + 0x6C) <= 3) {             /* dir count */
        return NULL_PTR;
    }
    rva  = *(const DWORD *)(opt + 0x70 + 3 * 8);
    size = *(const DWORD *)(opt + 0x70 + 3 * 8 + 4);
    if (rva == 0 || size < sizeof(RUNTIME_FUNCTION)) {
        return NULL_PTR;
    }
    *count = size / sizeof(RUNTIME_FUNCTION);
    return (const RUNTIME_FUNCTION *)(base + rva);
}

PRUNTIME_FUNCTION RtlLookupFunctionEntry(QWORD pc, QWORD *image_base,
                                         PVOID history) {
    const module_table_t *t = module_table();
    DWORD i;

    (void)history;
    *image_base = 0;
    if (t == NULL_PTR) {
        return NULL_PTR;
    }
    for (i = 0; i < t->count && i < MAX_MODULES; i++) {
        QWORD base = t->mod[i].base;
        const RUNTIME_FUNCTION *rf;
        DWORD n = 0, lo, hi, rva;

        if (pc < base || pc >= base + t->mod[i].size) {
            continue;
        }
        *image_base = base;
        rf = image_pdata(base, &n);
        if (rf == NULL_PTR) {
            return NULL_PTR;
        }
        /* .pdata is sorted by BeginAddress: binary search. */
        rva = (DWORD)(pc - base);
        lo = 0;
        hi = n;
        while (lo < hi) {
            DWORD mid = lo + (hi - lo) / 2;

            if (rva < rf[mid].BeginAddress) {
                hi = mid;
            } else if (rva >= rf[mid].EndAddress) {
                lo = mid + 1;
            } else {
                return (PRUNTIME_FUNCTION)&rf[mid];
            }
        }
        return NULL_PTR;
    }
    return NULL_PTR;
}

/* --- RtlVirtualUnwind -------------------------------------------------------
 *
 * One frame, backwards. The UNWIND_INFO lists the prologue's operations,
 * last first; replaying them in that order undoes the prologue and leaves
 * the context as it was at the call - after which the return address is
 * popped, unless a machine frame (an interrupt/exception frame) supplied
 * RIP and RSP itself. The layout is Microsoft's, documented as "x64
 * exception handling"; the structure of this function follows Wine's. */

#define UWOP_PUSH_NONVOL     0
#define UWOP_ALLOC_LARGE     1
#define UWOP_ALLOC_SMALL     2
#define UWOP_SET_FPREG       3
#define UWOP_SAVE_NONVOL     4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_EPILOG          6
#define UWOP_SAVE_XMM128     8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME  10

typedef struct {
    BYTE offset;                 /* where in the prologue it happened */
    BYTE code_info;              /* low 4 bits: op, high 4 bits: info */
} unwind_code_t;

typedef struct {
    BYTE version_flags;          /* low 3: version, high 5: flags     */
    BYTE prolog;
    BYTE count;
    BYTE frame;                  /* low 4: register, high 4: offset/16 */
    unwind_code_t codes[1];
} unwind_info_t;

#define UI_VERSION(u)   ((u)->version_flags & 7)
#define UI_FLAGS(u)     ((u)->version_flags >> 3)
#define UC_OP(c)        ((c).code_info & 0xF)
#define UC_INFO(c)      ((c).code_info >> 4)

static QWORD *int_reg(PCONTEXT c, int reg) {
    return &c->Rax + reg;        /* Rax..R15 are contiguous, in encoding order */
}

static BYTE *xmm_reg(PCONTEXT c, int reg) {
    return c->FltSave + 0xA0 + reg * 16;
}

static void copy16(BYTE *dst, const BYTE *src) {
    int i;

    for (i = 0; i < 16; i++) {
        dst[i] = src[i];
    }
}

static int opcode_slots(unwind_code_t c) {
    switch (UC_OP(c)) {
    case UWOP_ALLOC_LARGE:     return 2 + (UC_INFO(c) != 0);
    case UWOP_SAVE_NONVOL:
    case UWOP_SAVE_XMM128:
    case UWOP_EPILOG:          return 2;
    case UWOP_SAVE_NONVOL_FAR:
    case UWOP_SAVE_XMM128_FAR: return 3;
    default:                   return 1;
    }
}

/* Is `pc` in an epilogue? An exception can land between the stack
 * deallocation and the ret, where the prologue's description no longer
 * matches the stack. Recognised by the only shapes an epilogue may take:
 * add/lea to RSP, pops of nonvolatiles, then ret (or a tail jump out). */
static int in_epilog(const BYTE *pc, QWORD base, const RUNTIME_FUNCTION *fn) {
    if ((pc[0] & 0xF8) == 0x48) {
        switch (pc[1]) {
        case 0x81:
            if (pc[0] == 0x48 && pc[2] == 0xC4) { pc += 7; break; }
            return 0;
        case 0x83:
            if (pc[0] == 0x48 && pc[2] == 0xC4) { pc += 4; break; }
            return 0;
        case 0x8D:
            if (pc[0] & 0x06) return 0;
            if (((pc[2] >> 3) & 7) != 4) return 0;   /* dest must be RSP */
            if ((pc[2] & 7) == 4) return 0;          /* no SIB           */
            if ((pc[2] >> 6) == 1) { pc += 4; break; }
            if ((pc[2] >> 6) == 2) { pc += 7; break; }
            return 0;
        default:
            break;
        }
    }
    for (;;) {
        if ((*pc & 0xF0) == 0x40) {
            pc++;                                    /* REX */
        }
        if (*pc >= 0x58 && *pc <= 0x5F) {
            pc++;                                    /* pop  */
            continue;
        }
        if (*pc == 0xC2 || *pc == 0xC3) {
            return 1;                                /* ret  */
        }
        if (*pc == 0xF3 && pc[1] == 0xC3) {
            return 1;                                /* rep ret */
        }
        if (*pc == 0xE9 || *pc == 0xEB) {
            /* A jmp is an epilogue only if it leaves the function - a
             * tail call. */
            QWORD target = (*pc == 0xE9)
                ? (QWORD)(pc + 5) + (QWORD)(long long)*(const LONG *)(pc + 1)
                : (QWORD)(pc + 2) + (QWORD)(long long)(signed char)pc[1];

            return target < base + fn->BeginAddress ||
                   target >= base + fn->EndAddress;
        }
        return 0;
    }
}

/* Execute the epilogue the thread was in the middle of. */
static void run_epilog(const BYTE *pc, PCONTEXT c) {
    for (;;) {
        BYTE rex = 0;

        if ((*pc & 0xF0) == 0x40) {
            rex = *pc++ & 0x0F;
        }
        switch (*pc) {
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            *int_reg(c, (*pc - 0x58) + ((rex & 1) ? 8 : 0)) = *(QWORD *)c->Rsp;
            c->Rsp += 8;
            pc++;
            continue;
        case 0x81:
            c->Rsp += (QWORD)(long long)*(const LONG *)(pc + 2);
            pc += 6;
            continue;
        case 0x83:
            c->Rsp += (QWORD)(long long)(signed char)pc[2];
            pc += 3;
            continue;
        case 0x8D:
            if ((pc[1] >> 6) == 1) {
                c->Rsp = *int_reg(c, (pc[1] & 7) + ((rex & 1) ? 8 : 0)) +
                         (QWORD)(long long)(signed char)pc[2];
                pc += 3;
            } else {
                c->Rsp = *int_reg(c, (pc[1] & 7) + ((rex & 1) ? 8 : 0)) +
                         (QWORD)(long long)*(const LONG *)(pc + 2);
                pc += 6;
            }
            continue;
        case 0xC2: case 0xC3: case 0xE9: case 0xEB:
        case 0xF3:
            /* ret, or the tail jump: either way the caller's address is on
             * top of the stack now. */
            c->Rip = *(QWORD *)c->Rsp;
            c->Rsp += 8;
            return;
        default:
            return;                                  /* not reached */
        }
    }
}

PEXCEPTION_ROUTINE RtlVirtualUnwind(DWORD type, QWORD base, QWORD pc,
                                    PRUNTIME_FUNCTION fn, PCONTEXT c,
                                    PVOID *handler_data, QWORD *frame_out,
                                    PVOID context_pointers) {
    const unwind_info_t *ui;
    const DWORD *tail;
    QWORD frame = c->Rsp;
    DWORD prolog_offset = ~0u;
    int machframe = 0, i;

    (void)context_pointers;
    *handler_data = NULL_PTR;
    for (;;) {
        ui = (const unwind_info_t *)(base + (fn->UnwindData & ~1u));
        tail = (const DWORD *)&ui->codes[(ui->count + 1) & ~1];
        if (UI_VERSION(ui) != 1 && UI_VERSION(ui) != 2) {
            break;
        }
        if (ui->frame & 0x0F) {
            frame = *int_reg(c, ui->frame & 0x0F) - (QWORD)(ui->frame >> 4) * 16;
        }
        if (pc >= base + fn->BeginAddress &&
            pc < base + fn->BeginAddress + ui->prolog) {
            prolog_offset = (DWORD)(pc - base - fn->BeginAddress);
        } else {
            prolog_offset = ~0u;
            if (!(UI_FLAGS(ui) & UNW_FLAG_CHAININFO) &&
                in_epilog((const BYTE *)pc, base, fn)) {
                run_epilog((const BYTE *)pc, c);
                *frame_out = frame;
                return NULL_PTR;
            }
        }
        for (i = 0; i < ui->count; i += opcode_slots(ui->codes[i])) {
            unwind_code_t op = ui->codes[i];
            QWORD at;

            if (prolog_offset < op.offset) {
                continue;                    /* not executed yet */
            }
            switch (UC_OP(op)) {
            case UWOP_PUSH_NONVOL:
                *int_reg(c, UC_INFO(op)) = *(QWORD *)c->Rsp;
                c->Rsp += 8;
                break;
            case UWOP_ALLOC_LARGE:
                if (UC_INFO(op)) {
                    c->Rsp += *(const DWORD *)&ui->codes[i + 1];
                } else {
                    c->Rsp += (QWORD)*(const WORD *)&ui->codes[i + 1] * 8;
                }
                break;
            case UWOP_ALLOC_SMALL:
                c->Rsp += (QWORD)(UC_INFO(op) + 1) * 8;
                break;
            case UWOP_SET_FPREG:
                c->Rsp = *int_reg(c, ui->frame & 0x0F) -
                         (QWORD)(ui->frame >> 4) * 16;
                break;
            case UWOP_SAVE_NONVOL:
                at = frame + (QWORD)*(const WORD *)&ui->codes[i + 1] * 8;
                *int_reg(c, UC_INFO(op)) = *(QWORD *)at;
                break;
            case UWOP_SAVE_NONVOL_FAR:
                at = frame + *(const DWORD *)&ui->codes[i + 1];
                *int_reg(c, UC_INFO(op)) = *(QWORD *)at;
                break;
            case UWOP_SAVE_XMM128:
                at = frame + (QWORD)*(const WORD *)&ui->codes[i + 1] * 16;
                copy16(xmm_reg(c, UC_INFO(op)), (const BYTE *)at);
                break;
            case UWOP_SAVE_XMM128_FAR:
                at = frame + *(const DWORD *)&ui->codes[i + 1];
                copy16(xmm_reg(c, UC_INFO(op)), (const BYTE *)at);
                break;
            case UWOP_PUSH_MACHFRAME:
                if (UC_INFO(op)) {
                    c->Rsp += 8;             /* an error code first */
                }
                c->Rip = *(QWORD *)c->Rsp;
                c->Rsp = *(QWORD *)(c->Rsp + 24);
                machframe = 1;
                break;
            default:
                break;                       /* UWOP_EPILOG: descriptive */
            }
        }
        if (!(UI_FLAGS(ui) & UNW_FLAG_CHAININFO)) {
            break;
        }
        fn = (PRUNTIME_FUNCTION)tail;        /* the parent's entry */
    }
    if (!machframe) {
        c->Rip = *(QWORD *)c->Rsp;
        c->Rsp += 8;
    }
    *frame_out = frame;
    if (!(UI_FLAGS(ui) & type) || prolog_offset != ~0u) {
        return NULL_PTR;                     /* no such handler, or in prolog */
    }
    *handler_data = (PVOID)(tail + 1);
    return (PEXCEPTION_ROUTINE)(base + *tail);
}

/* One frame up, whether or not the function has unwind data: a function
 * with none is a leaf, and its return address is on top of the stack. */
static PEXCEPTION_ROUTINE step(PCONTEXT c, DWORD type, QWORD *base,
                               PRUNTIME_FUNCTION *fn, PVOID *data,
                               QWORD *frame) {
    *fn = RtlLookupFunctionEntry(c->Rip, base, NULL_PTR);
    if (*fn == NULL_PTR) {
        *frame = c->Rsp;
        *data = NULL_PTR;
        c->Rip = *(QWORD *)c->Rsp;
        c->Rsp += 8;
        return NULL_PTR;
    }
    return RtlVirtualUnwind(type, *base, c->Rip, *fn, c, data, frame,
                            NULL_PTR);
}

static int on_stack(QWORD rsp) {
    PTEB teb = NtCurrentTeb();

    return rsp >= (QWORD)teb->StackLimit && rsp < (QWORD)teb->StackBase;
}

static void copy_context(PCONTEXT dst, const CONTEXT *src) {
    const QWORD *s = (const QWORD *)src;
    QWORD *d = (QWORD *)dst;
    unsigned i;

    for (i = 0; i < sizeof(CONTEXT) / 8; i++) {
        d[i] = s[i];
    }
}

/* --- vectored handlers ------------------------------------------------------ */

#define MAX_VECTORED 32

static struct {
    PVECTORED_EXCEPTION_HANDLER fn;
    int used;
} vectored[MAX_VECTORED];
static int vectored_count;                   /* in call order */
static RTL_SRWLOCK vectored_lock;

PVOID RtlAddVectoredExceptionHandler(DWORD first,
                                     PVECTORED_EXCEPTION_HANDLER fn) {
    PVOID handle = NULL_PTR;
    int i;

    if (fn == NULL_PTR) {
        return NULL_PTR;
    }
    RtlAcquireSRWLockExclusive(&vectored_lock);
    if (vectored_count < MAX_VECTORED) {
        if (first) {
            for (i = vectored_count; i > 0; i--) {
                vectored[i] = vectored[i - 1];
            }
            i = 0;
        } else {
            i = vectored_count;
        }
        vectored[i].fn = fn;
        vectored[i].used = 1;
        vectored_count++;
        handle = (PVOID)fn;
    }
    RtlReleaseSRWLockExclusive(&vectored_lock);
    return handle;
}

DWORD RtlRemoveVectoredExceptionHandler(PVOID handle) {
    DWORD removed = 0;
    int i, j;

    RtlAcquireSRWLockExclusive(&vectored_lock);
    for (i = 0; i < vectored_count; i++) {
        if ((PVOID)vectored[i].fn == handle) {
            for (j = i; j + 1 < vectored_count; j++) {
                vectored[j] = vectored[j + 1];
            }
            vectored_count--;
            removed = 1;
            break;
        }
    }
    RtlReleaseSRWLockExclusive(&vectored_lock);
    return removed;
}

static int call_vectored(PEXCEPTION_RECORD rec, PCONTEXT ctx) {
    PVECTORED_EXCEPTION_HANDLER list[MAX_VECTORED];
    EXCEPTION_POINTERS ep;
    int i, n;

    /* A snapshot, called without the lock held: a handler may add or
     * remove handlers, or raise an exception of its own. */
    RtlAcquireSRWLockShared(&vectored_lock);
    n = vectored_count;
    for (i = 0; i < n; i++) {
        list[i] = vectored[i].fn;
    }
    RtlReleaseSRWLockShared(&vectored_lock);

    ep.ExceptionRecord = rec;
    ep.ContextRecord = ctx;
    for (i = 0; i < n; i++) {
        if (list[i](&ep) == EXCEPTION_CONTINUE_EXECUTION) {
            return 1;
        }
    }
    return 0;
}

/* --- dispatch ---------------------------------------------------------------- */

static void raise_status(NTSTATUS status, PEXCEPTION_RECORD nested);

BOOLEAN RtlDispatchException(PEXCEPTION_RECORD rec, PCONTEXT ctx) {
    CONTEXT walk;
    DISPATCHER_CONTEXT dc;
    int frames;

    if (call_vectored(rec, ctx)) {
        return 1;
    }
    copy_context(&walk, ctx);
    for (frames = 0; frames < 1024; frames++) {
        QWORD base = 0, frame = 0, pc = walk.Rip;
        PRUNTIME_FUNCTION fn;
        PVOID data;
        PEXCEPTION_ROUTINE handler;

        if (pc == 0 || !on_stack(walk.Rsp)) {
            break;                             /* the top of the stack */
        }
        handler = step(&walk, UNW_FLAG_EHANDLER, &base, &fn, &data, &frame);
        if (handler == NULL_PTR) {
            continue;
        }
        if (!on_stack(frame)) {
            break;                             /* a corrupt frame: stop */
        }
        dc.ControlPc        = pc;
        dc.ImageBase        = base;
        dc.FunctionEntry    = fn;
        dc.EstablisherFrame = frame;
        dc.TargetIp         = 0;
        dc.ContextRecord    = &walk;
        dc.LanguageHandler  = handler;
        dc.HandlerData      = data;
        dc.HistoryTable     = NULL_PTR;
        dc.ScopeIndex       = 0;
        dc.Fill0            = 0;
        switch (handler(rec, (PVOID)frame, ctx, &dc)) {
        case ExceptionContinueExecution:
            if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) {
                raise_status(STATUS_NONCONTINUABLE_EXCEPTION, rec);
            }
            return 1;
        case ExceptionContinueSearch:
            break;
        case ExceptionNestedException:
        case ExceptionCollidedUnwind:
            break;                             /* keep looking */
        default:
            raise_status(STATUS_INVALID_DISPOSITION, rec);
        }
    }
    return 0;
}

/* --- unwinding ----------------------------------------------------------------
 *
 * Walk from here up to TargetFrame, calling every frame's handler as a
 * termination handler (EXCEPTION_UNWINDING set) - that is when __finally
 * blocks run - then resume IN the target frame at TargetIp, with RAX the
 * return value (the exception code, for __except). The frames in between,
 * the dispatcher's included, simply cease to exist. */
void RtlUnwindEx(PVOID target_frame, PVOID target_ip, PEXCEPTION_RECORD rec,
                 PVOID retval, PCONTEXT original, PVOID history) {
    EXCEPTION_RECORD local;
    CONTEXT ctx, prev;
    DISPATCHER_CONTEXT dc;
    int frames;

    (void)original;
    (void)history;
    if (rec == NULL_PTR) {
        local.ExceptionCode = STATUS_UNWIND;
        local.ExceptionFlags = 0;
        local.ExceptionRecord = NULL_PTR;
        local.ExceptionAddress = NULL_PTR;
        local.NumberParameters = 0;
        rec = &local;
    }
    rec->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (target_frame == NULL_PTR) {
        rec->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;
    }

    RtlCaptureContext(&ctx);
    for (frames = 0; frames < 1024; frames++) {
        QWORD base = 0, frame = 0;
        PRUNTIME_FUNCTION fn;
        PVOID data;
        PEXCEPTION_ROUTINE handler;

        if (ctx.Rip == 0 || !on_stack(ctx.Rsp)) {
            break;
        }
        copy_context(&prev, &ctx);
        handler = step(&ctx, UNW_FLAG_UHANDLER, &base, &fn, &data, &frame);
        if (target_frame != NULL_PTR && frame == (QWORD)target_frame) {
            rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
        }
        if (handler != NULL_PTR) {
            dc.ControlPc        = prev.Rip;
            dc.ImageBase        = base;
            dc.FunctionEntry    = fn;
            dc.EstablisherFrame = frame;
            dc.TargetIp         = (QWORD)target_ip;
            dc.ContextRecord    = &prev;
            dc.LanguageHandler  = handler;
            dc.HandlerData      = data;
            dc.HistoryTable     = NULL_PTR;
            dc.ScopeIndex       = 0;
            dc.Fill0            = 0;
            (void)handler(rec, (PVOID)frame, &prev, &dc);
        }
        if (target_frame != NULL_PTR && frame == (QWORD)target_frame) {
            /* Resume in the target frame itself: its context is the one
             * before this step unwound it. */
            prev.Rax = (QWORD)retval;
            prev.Rip = (QWORD)target_ip;
            NtContinue(&prev, 0);
            break;                             /* NtContinue failed */
        }
    }
    raise_status(STATUS_INVALID_DISPOSITION, rec);  /* target not found */
}

void RtlUnwind(PVOID target_frame, PVOID target_ip, PEXCEPTION_RECORD rec,
               PVOID retval) {
    RtlUnwindEx(target_frame, target_ip, rec, retval, NULL_PTR, NULL_PTR);
}

/* --- __C_specific_handler ------------------------------------------------------
 *
 * The language handler every __try in C names. Its data is a scope table:
 * for each __try, the code range it covers, the filter (or 1, meaning
 * EXCEPTION_EXECUTE_HANDLER outright) and the __except block to jump to - or,
 * for a __finally, a JumpTarget of 0 and the finally block as the "handler",
 * called during unwinding with AbnormalTermination TRUE. Innermost scopes come
 * first, which is what makes a linear search find the right one. */
typedef struct {
    DWORD count;
    struct {
        DWORD begin, end, handler, target;
    } scope[1];
} scope_table_t;

typedef LONG (*filter_fn)(PEXCEPTION_POINTERS info, PVOID frame);
typedef void (*finally_fn)(BOOLEAN abnormal, PVOID frame);

EXCEPTION_DISPOSITION __C_specific_handler(PEXCEPTION_RECORD rec,
                                           PVOID frame, PCONTEXT ctx,
                                           PDISPATCHER_CONTEXT dc) {
    const scope_table_t *t = (const scope_table_t *)dc->HandlerData;
    QWORD base = dc->ImageBase;
    DWORD pc = (DWORD)(dc->ControlPc - base);
    DWORD i;

    if (!(rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))) {
        /* Dispatch: find an __except whose filter accepts it. */
        for (i = dc->ScopeIndex; i < t->count; i++) {
            LONG verdict;

            if (pc < t->scope[i].begin || pc >= t->scope[i].end ||
                t->scope[i].target == 0) {
                continue;
            }
            if (t->scope[i].handler == 1) {
                verdict = EXCEPTION_EXECUTE_HANDLER;
            } else {
                EXCEPTION_POINTERS ep;

                ep.ExceptionRecord = rec;
                ep.ContextRecord = ctx;
                verdict = ((filter_fn)(base + t->scope[i].handler))(&ep, frame);
            }
            if (verdict < 0) {
                return ExceptionContinueExecution;
            }
            if (verdict > 0) {
                /* Ours. Unwind everything above, then land in the
                 * __except block with the code in EAX. Does not return. */
                RtlUnwindEx(frame, (PVOID)(base + t->scope[i].target), rec,
                            (PVOID)(QWORD)rec->ExceptionCode,
                            dc->ContextRecord, dc->HistoryTable);
            }
        }
        return ExceptionContinueSearch;
    }

    /* Unwind: run the __finally blocks of every scope the pc is inside,
     * innermost first, stopping at the __except being unwound TO. */
    for (i = dc->ScopeIndex; i < t->count; i++) {
        if (pc < t->scope[i].begin || pc >= t->scope[i].end) {
            continue;
        }
        if (t->scope[i].target == 0) {
            dc->ScopeIndex = i + 1;          /* not again, if re-entered */
            ((finally_fn)(base + t->scope[i].handler))(1, frame);
        } else if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) &&
                   dc->TargetIp == base + t->scope[i].target) {
            break;
        }
    }
    return ExceptionContinueSearch;
}

/* --- raising ------------------------------------------------------------------- */

static PTOP_LEVEL_EXCEPTION_FILTER unhandled_filter;

PTOP_LEVEL_EXCEPTION_FILTER RtlSetUnhandledExceptionFilter(
    PTOP_LEVEL_EXCEPTION_FILTER filter) {
    PTOP_LEVEL_EXCEPTION_FILTER old = unhandled_filter;

    unhandled_filter = filter;
    return old;
}

/* Nothing on the stack wanted it. The unhandled filter gets the last look
 * (kernel32's UnhandledExceptionFilter, and the program's own behind it):
 * it may fix things up and continue, or accept it - which ends the process
 * with the exception code, quietly, as a filter that "handled" it asked.
 * Otherwise the kernel's last chance ends it, loudly. */
static void unhandled(PEXCEPTION_RECORD rec, PCONTEXT ctx) {
    if (unhandled_filter != NULL_PTR) {
        EXCEPTION_POINTERS ep;
        LONG verdict;

        ep.ExceptionRecord = rec;
        ep.ContextRecord = ctx;
        verdict = unhandled_filter(&ep);
        if (verdict == EXCEPTION_CONTINUE_EXECUTION) {
            NtContinue(ctx, 0);
        } else if (verdict == EXCEPTION_EXECUTE_HANDLER) {
            NtTerminateProcess(NtCurrentProcess(), rec->ExceptionCode);
        }
    }
    NtRaiseException(rec, ctx, 0);
}

/* Where KiUserExceptionDispatcher lands, with the kernel's record and
 * context. Never returns. */
void KiUserExceptionDispatch(PEXCEPTION_RECORD rec, PCONTEXT ctx);
void KiUserExceptionDispatch(PEXCEPTION_RECORD rec, PCONTEXT ctx) {
    if (RtlDispatchException(rec, ctx)) {
        NtContinue(ctx, 0);
    }
    unhandled(rec, ctx);
    for (;;) {
    }
}

void RtlRaiseException(PEXCEPTION_RECORD rec) {
    CONTEXT ctx;
    QWORD base = 0, frame = 0;
    PRUNTIME_FUNCTION fn;
    PVOID data;

    /* The raiser's context, not ours: one frame up, so that continuing
     * returns from this call, and the address is the caller's. */
    RtlCaptureContext(&ctx);
    (void)step(&ctx, UNW_FLAG_NHANDLER, &base, &fn, &data, &frame);
    rec->ExceptionAddress = (PVOID)ctx.Rip;

    if (RtlDispatchException(rec, &ctx)) {
        NtContinue(&ctx, 0);
    }
    unhandled(rec, &ctx);
}

static void raise_status(NTSTATUS status, PEXCEPTION_RECORD nested) {
    EXCEPTION_RECORD rec;

    rec.ExceptionCode = status;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    rec.ExceptionRecord = nested;
    rec.ExceptionAddress = NULL_PTR;
    rec.NumberParameters = 0;
    RtlRaiseException(&rec);
    for (;;) {
    }
}

void RtlRaiseStatus(NTSTATUS status) {
    raise_status(status, NULL_PTR);
}

/* --- KiUserExceptionDispatcher --------------------------------------------------
 *
 * Entered from the kernel, never called (nt_exception_deliver). RSP points
 * at the faulting CONTEXT; the record is at +0x4D0 and a machine frame at
 * +0x570 (kernel/include/nt_context.h).
 *
 * The unwind info is the point of writing this in assembly. It says: the
 * nonvolatile registers are saved at their CONTEXT offsets, 0x570 bytes of
 * stack are "allocated", and below that is a machine frame. So when
 * RtlUnwindEx walks up through this frame it arrives in the faulting
 * function with the faulting function's RIP, RSP and every nonvolatile
 * register - exactly what landing in its __except block needs. The prologue
 * is empty: the kernel built all of it. */
__asm__(
".text\n"
".globl KiUserExceptionDispatcher\n"
".def KiUserExceptionDispatcher; .scl 2; .type 32; .endef\n"
".seh_proc KiUserExceptionDispatcher\n"
"KiUserExceptionDispatcher:\n"
"    .seh_pushframe\n"
"    .seh_stackalloc 0x570\n"
"    .seh_savereg %rbx, 0x90\n"
"    .seh_savereg %rbp, 0xA0\n"
"    .seh_savereg %rsi, 0xA8\n"
"    .seh_savereg %rdi, 0xB0\n"
"    .seh_savereg %r12, 0xD8\n"
"    .seh_savereg %r13, 0xE0\n"
"    .seh_savereg %r14, 0xE8\n"
"    .seh_savereg %r15, 0xF0\n"
"    .seh_savexmm %xmm6,  0x200\n"
"    .seh_savexmm %xmm7,  0x210\n"
"    .seh_savexmm %xmm8,  0x220\n"
"    .seh_savexmm %xmm9,  0x230\n"
"    .seh_savexmm %xmm10, 0x240\n"
"    .seh_savexmm %xmm11, 0x250\n"
"    .seh_savexmm %xmm12, 0x260\n"
"    .seh_savexmm %xmm13, 0x270\n"
"    .seh_savexmm %xmm14, 0x280\n"
"    .seh_savexmm %xmm15, 0x290\n"
"    .seh_endprologue\n"
"    cld\n"
"    leaq 0x4D0(%rsp), %rcx\n"
"    movq %rsp, %rdx\n"
"    call KiUserExceptionDispatch\n"
"    ud2\n"
".seh_endproc\n"
);
