/* CONTEXT capture, delivery and restore; user-mode APCs. See nt_context.h. */

#include "bkl.h"
#include "kheap.h"
#include "nt.h"
#include "nt_context.h"
#include "paging.h"
#include "process.h"
#include "sched.h"
#include "screen.h"
#include "syscall.h"
#include "typesk.h"
#include "waitq.h"

#define OFF(f) __builtin_offsetof(nt_context_t, f)
typedef char nt_context_layout[(sizeof(nt_context_t) == NT_CONTEXT_SIZE &&
    OFF(context_flags) == 0x30 && OFF(seg_cs) == 0x38 && OFF(eflags) == 0x44 &&
    OFF(dr0) == 0x48 && OFF(rax) == 0x78 && OFF(rsp) == 0x98 &&
    OFF(r8) == 0xB8 && OFF(rip) == 0xF8 && OFF(flt_save) == 0x100 &&
    OFF(vector_register) == 0x300 && OFF(vector_control) == 0x4A0) ? 1 : -1];
#undef OFF

#define USER_CS   0x23
#define USER_SS   0x1B
#define USER_TOP  0x0000800000000000ULL

/* RFLAGS bits ring 3 may set: CF PF AF ZF SF TF DF OF AC ID. Not IOPL, not
 * NT, not VM, not IF - IF is forced on. */
#define USER_RFLAGS 0x240DD5ULL

static void copy(void *dst, const void *src, uint64 n) {
    uint8 *d = (uint8 *)dst;
    const uint8 *s = (const uint8 *)src;

    while (n--) {
        *d++ = *s++;
    }
}

static void zero(void *dst, uint64 n) {
    uint8 *d = (uint8 *)dst;

    while (n--) {
        *d++ = 0;
    }
}

/* Is [va, va+len) mapped in the calling thread's address space? The kernel
 * writes user memory directly, so a page that is not there would fault in
 * ring 0 - checked first, page by page. */
static int user_mapped(uint64 va, uint64 len) {
    process_t *me = proc_current();
    uint64 page;

    if (len == 0 || va < 0x1000ULL || va >= USER_TOP || len > USER_TOP - va) {
        return 0;
    }
    for (page = va & ~0xFFFULL; page < va + len; page += 0x1000ULL) {
        if (vmm_get_phys_in(me->space, page) == 0) {
            return 0;
        }
    }
    return 1;
}

static void capture_fpu(nt_context_t *c) {
    uint8 area[512] __attribute__((aligned(16)));

    __asm__ volatile ("fxsave (%0)" : : "r"(area) : "memory");
    copy(c->flt_save, area, sizeof(area));
    c->mxcsr = *(const uint32 *)(area + 24);
}

static void capture_common(nt_context_t *c) {
    zero(c, sizeof(*c));
    c->context_flags = NT_CONTEXT_FULL | NT_CONTEXT_SEGMENTS;
    c->seg_cs = USER_CS;
    c->seg_ss = c->seg_ds = c->seg_es = USER_SS;
    capture_fpu(c);
}

void ntctx_capture_syscall(nt_context_t *c, const struct syscall_frame *f,
                           uint64 user_rsp, uint64 rax) {
    capture_common(c);
    c->rax = rax;
    /* What SYSRET leaves: RCX the return address, R11 the flags. */
    c->rcx = f->rip;
    c->r11 = f->rflags;
    c->rdx = f->rdx;
    c->rsi = f->rsi;
    c->rdi = f->rdi;
    c->r8  = f->r8;
    c->r9  = f->r9;
    c->r10 = f->r10;
    c->rbx = f->rbx;
    c->rbp = f->rbp;
    c->r12 = f->r12;
    c->r13 = f->r13;
    c->r14 = f->r14;
    c->r15 = f->r15;
    c->rsp = user_rsp;
    c->rip = f->rip;
    c->eflags = (uint32)f->rflags;
}

void ntctx_capture_intr(nt_context_t *c, const struct interrupt_frame *f) {
    capture_common(c);
    c->rax = f->rax;  c->rcx = f->rcx;  c->rdx = f->rdx;  c->rbx = f->rbx;
    c->rsp = f->rsp;  c->rbp = f->rbp;  c->rsi = f->rsi;  c->rdi = f->rdi;
    c->r8  = f->r8;   c->r9  = f->r9;   c->r10 = f->r10;  c->r11 = f->r11;
    c->r12 = f->r12;  c->r13 = f->r13;  c->r14 = f->r14;  c->r15 = f->r15;
    c->rip = f->rip;
    c->eflags = (uint32)f->rflags;
}

uint64 ntctx_push(const nt_context_t *c, uint64 rsp, uint64 extra) {
    uint64 total = NT_CONTEXT_SIZE + ((extra + 15) & ~15ULL);
    uint64 at;

    /* Win64 has no red zone, but a little slack below the interrupted RSP
     * costs nothing and keeps a thread that was mid-push intact. */
    if (rsp < total + 0x1000ULL + 64) {
        return 0;
    }
    at = (rsp - 64 - total) & ~15ULL;
    if (!user_mapped(at, total)) {
        return 0;
    }
    copy((void *)at, c, NT_CONTEXT_SIZE);
    zero((void *)(at + NT_CONTEXT_SIZE), total - NT_CONTEXT_SIZE);
    return at;
}

int ntctx_sanitize(nt_context_t *c) {
    uint8 area[512] __attribute__((aligned(16)));
    uint32 mask;

    if (c->rip >= USER_TOP || c->rsp >= USER_TOP) {
        return 0;
    }
    c->seg_cs = USER_CS;
    c->seg_ss = USER_SS;
    c->eflags = (uint32)(((uint64)c->eflags & USER_RFLAGS) | 0x202ULL);
    /* MXCSR_MASK from this CPU's own FXSAVE image: a reserved bit set in the
     * saved MXCSR would make fxrstor raise #GP - in ring 0. */
    __asm__ volatile ("fxsave (%0)" : : "r"(area) : "memory");
    mask = *(const uint32 *)(area + 28);
    if (mask == 0) {
        mask = 0xFFBFu;
    }
    *(uint32 *)(c->flt_save + 24) &= mask;
    c->mxcsr &= mask;
    return 1;
}

/* The exit: RDI names an interrupt_frame on this kernel stack. The big
 * kernel lock goes first, as in syscall_return, with interrupts off from
 * then until iretq; then the frame is popped exactly as isr_common pops
 * one, and swapgs puts the user's GS base back. */
void nt_iret_exit(struct interrupt_frame *f) __attribute__((noreturn));
__asm__(
".text\n"
".global nt_iret_exit\n"
"nt_iret_exit:\n"
"    cli\n"
"    pushq %rdi\n"
"    call bkl_exit_to_user\n"
"    popq %rdi\n"
"    movq %rdi, %rsp\n"
"    popq %r15\n   popq %r14\n   popq %r13\n   popq %r12\n"
"    popq %r11\n   popq %r10\n   popq %r9\n    popq %r8\n"
"    popq %rbp\n   popq %rdi\n   popq %rsi\n"
"    popq %rdx\n   popq %rcx\n   popq %rbx\n   popq %rax\n"
"    addq $16, %rsp\n"
"    swapgs\n"
"    iretq\n"
);

void ntctx_resume(nt_context_t *c) {
    struct interrupt_frame f;
    uint8 area[512] __attribute__((aligned(16)));

    f.rax = c->rax;  f.rcx = c->rcx;  f.rdx = c->rdx;  f.rbx = c->rbx;
    f.rbp = c->rbp;  f.rsi = c->rsi;  f.rdi = c->rdi;
    f.r8  = c->r8;   f.r9  = c->r9;   f.r10 = c->r10;  f.r11 = c->r11;
    f.r12 = c->r12;  f.r13 = c->r13;  f.r14 = c->r14;  f.r15 = c->r15;
    f.vector = 0;
    f.error_code = 0;
    f.rip = c->rip;
    f.cs = USER_CS;
    f.rflags = c->eflags;
    f.rsp = c->rsp;
    f.ss = USER_SS;
    copy(area, c->flt_save, sizeof(area));

    /* Everything a return to ring 3 owes first: a suspension parks here, a
     * pending reschedule happens here, a zombie never comes back. After
     * this nothing may schedule, so the FPU image loaded next is the one
     * that reaches ring 3. */
    return_to_user(1);
    __asm__ volatile ("cli");
    __asm__ volatile ("fxrstor (%0)" : : "r"(area) : "memory");
    nt_iret_exit(&f);
}

/* --- APCs ------------------------------------------------------------------ */

struct nt_apc {
    struct nt_apc *next;
    uint64 routine, ctx, arg1, arg2;
};

int nt_apc_queue(process_t *t, uint64 routine, uint64 ctx, uint64 arg1,
                 uint64 arg2) {
    struct nt_apc *a = (struct nt_apc *)kmalloc(sizeof(*a));

    if (a == NULL) {
        return -12;                         /* -ENOMEM */
    }
    a->next = NULL;
    a->routine = routine;
    a->ctx = ctx;
    a->arg1 = arg1;
    a->arg2 = arg2;
    if (t->nt_apc_tail != NULL) {
        t->nt_apc_tail->next = a;
    } else {
        t->nt_apc_head = a;
    }
    t->nt_apc_tail = a;
    /* A thread in an alertable wait parks on the readiness queue (the
     * dispatcher's multi-object wait, and the alertable delay), and
     * re-tests for a pending APC when woken. A thread in any other wait is
     * not on that queue and is not disturbed. */
    waitq_wake_all(waitq_readiness());
    return 0;
}

int nt_apc_pending(const process_t *t) {
    return t != NULL && t->nt_apc_head != NULL;
}

static struct nt_apc *apc_take(process_t *t) {
    struct nt_apc *a = t->nt_apc_head;

    if (a != NULL) {
        t->nt_apc_head = a->next;
        if (t->nt_apc_head == NULL) {
            t->nt_apc_tail = NULL;
        }
    }
    return a;
}

void nt_apc_flush(process_t *t) {
    struct nt_apc *a;

    while ((a = apc_take(t)) != NULL) {
        kfree(a);
    }
}

/* Put the first queued APC's routine and arguments where KiUserApcDispatcher
 * reads them - the CONTEXT's four home slots, which are also the callee's
 * home area, as on NT - and push the whole thing below `c->rsp`. Returns the
 * new RSP, or 0 (nothing dequeued) if there is no room. */
static uint64 apc_frame(process_t *me, nt_context_t *c) {
    struct nt_apc *a = me->nt_apc_head;
    uint64 sp;

    if (a == NULL || me->nt_apc_dispatcher == 0) {
        return 0;
    }
    c->p1_home = a->ctx;
    c->p2_home = a->arg1;
    c->p3_home = a->arg2;
    c->p4_home = a->routine;
    sp = ntctx_push(c, c->rsp, 0);
    if (sp != 0) {
        kfree(apc_take(me));
    }
    return sp;
}

uint64 nt_apc_deliver(struct syscall_frame *f, uint64 status) {
    process_t *me = proc_current();
    nt_context_t c;
    uint64 sp;

    if (!nt_apc_pending(me) || me->nt_apc_dispatcher == 0) {
        return status;
    }
    ntctx_capture_syscall(&c, f, syscall_get_user_rsp(), status);
    sp = apc_frame(me, &c);
    if (sp == 0) {
        return status;
    }
    /* Out through the ordinary SYSRET exit, into the dispatcher, with RSP
     * at the CONTEXT. The dispatcher's NtContinue brings `status` back as
     * the interrupted call's result. */
    f->rip = me->nt_apc_dispatcher;
    syscall_set_user_rsp(sp);
    return status;
}

uint64 nt_test_alert(struct syscall_frame *f) {
    return nt_apc_deliver(f, STATUS_SUCCESS);
}

uint64 nt_continue(struct syscall_frame *f, uint64 ctx_ptr,
                   uint64 test_alert) {
    process_t *me = proc_current();
    nt_context_t in, c;
    uint32 flags;

    if (!user_mapped(ctx_ptr, NT_CONTEXT_SIZE)) {
        return STATUS_ACCESS_VIOLATION;
    }
    copy(&in, (const void *)ctx_ptr, NT_CONTEXT_SIZE);

    /* What the context does not mention stays as it is now - ContextFlags
     * selects the groups, as on NT. */
    ntctx_capture_syscall(&c, f, syscall_get_user_rsp(), STATUS_SUCCESS);
    flags = in.context_flags;
    if ((flags & NT_CONTEXT_CONTROL) == NT_CONTEXT_CONTROL) {
        c.rip = in.rip;
        c.rsp = in.rsp;
        c.eflags = in.eflags;
    }
    if ((flags & NT_CONTEXT_INTEGER) == NT_CONTEXT_INTEGER) {
        c.rax = in.rax;  c.rcx = in.rcx;  c.rdx = in.rdx;  c.rbx = in.rbx;
        c.rbp = in.rbp;  c.rsi = in.rsi;  c.rdi = in.rdi;
        c.r8  = in.r8;   c.r9  = in.r9;   c.r10 = in.r10;  c.r11 = in.r11;
        c.r12 = in.r12;  c.r13 = in.r13;  c.r14 = in.r14;  c.r15 = in.r15;
    }
    if ((flags & NT_CONTEXT_FLOATING_POINT) == NT_CONTEXT_FLOATING_POINT) {
        copy(c.flt_save, in.flt_save, sizeof(c.flt_save));
        c.mxcsr = in.mxcsr;
    }
    if (!ntctx_sanitize(&c)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* The next APC runs on top of the context being restored, so when IT
     * continues, this one is what comes back - one APC per trip, and the
     * queue drains through repeated NtContinue(..., TRUE). */
    if (test_alert && nt_apc_pending(me)) {
        uint64 sp = apc_frame(me, &c);

        if (sp != 0) {
            c.rip = me->nt_apc_dispatcher;
            c.rsp = sp;
        }
    }
    ntctx_resume(&c);
}

/* --- exceptions ------------------------------------------------------------ */

typedef struct {
    uint32 code;
    uint32 flags;
    uint64 record;                   /* a nested EXCEPTION_RECORD, or 0   */
    uint64 address;
    uint32 nparams;
    uint32 pad;
    uint64 info[15];
} nt_exception_record_t;

typedef char nt_exception_record_layout[(sizeof(nt_exception_record_t) == 0x98 &&
    __builtin_offsetof(nt_exception_record_t, address) == 0x10 &&
    __builtin_offsetof(nt_exception_record_t, info) == 0x20) ? 1 : -1];

#define EXCEPTION_NONCONTINUABLE 0x1u

/* Put CONTEXT + record + machine frame on the stack below c->rsp (see the
 * layout in nt_context.h). Returns the dispatcher's RSP, or 0. */
static uint64 exception_frame(const nt_context_t *c,
                              const nt_exception_record_t *rec) {
    uint64 sp = ntctx_push(c, c->rsp, NT_EXC_FRAME_EXTRA);
    uint64 *mf;

    if (sp == 0) {
        return 0;
    }
    copy((void *)(sp + NT_EXC_RECORD_OFFSET), rec, sizeof(*rec));
    mf = (uint64 *)(sp + NT_EXC_MACHFRAME_OFFSET);
    mf[0] = c->rip;
    mf[1] = USER_CS;
    mf[2] = c->eflags;
    mf[3] = c->rsp;
    mf[4] = USER_SS;
    return sp;
}

static int deliver(struct interrupt_frame *f, nt_exception_record_t *recp);

int nt_exception_deliver(struct interrupt_frame *f, uint64 cr2) {
    process_t *me = proc_current();
    nt_exception_record_t rec;

    if (me == NULL || me->personality != PERSONALITY_WINDOWS ||
        me->nt_exc_dispatcher == 0) {
        return 0;
    }
    zero(&rec, sizeof(rec));
    switch (f->vector) {
    case 0:  rec.code = 0xC0000094u; break;   /* INTEGER_DIVIDE_BY_ZERO     */
    case 1:  rec.code = 0x80000004u; break;   /* SINGLE_STEP                */
    case 3:  rec.code = 0x80000003u; break;   /* BREAKPOINT                 */
    case 4:  rec.code = 0xC0000095u; break;   /* INTEGER_OVERFLOW           */
    case 5:  rec.code = 0xC000008Cu; break;   /* ARRAY_BOUNDS_EXCEEDED      */
    case 6:  rec.code = 0xC000001Du; break;   /* ILLEGAL_INSTRUCTION        */
    case 16: rec.code = 0xC0000090u; break;   /* FLOAT_INVALID_OPERATION    */
    case 17: rec.code = 0x80000002u; break;   /* DATATYPE_MISALIGNMENT      */
    case 19: rec.code = 0xC00002B5u; break;   /* FLOAT_MULTIPLE_TRAPS       */
    case 13:
        /* A protection fault reports as an access violation to an unknown
         * address, which is what NT does: all ones. */
        rec.code = 0xC0000005u;
        rec.nparams = 2;
        rec.info[0] = 0;
        rec.info[1] = ~0ULL;
        break;
    case 14:
        rec.code = 0xC0000005u;               /* ACCESS_VIOLATION           */
        rec.nparams = 2;
        /* 0 read, 1 write, 8 execute (DEP) - from the #PF error code. */
        rec.info[0] = (f->error_code & 0x10) ? 8 : (f->error_code & 0x2) ? 1 : 0;
        rec.info[1] = cr2;
        break;
    default:
        return 0;
    }
    return deliver(f, &rec);
}

/* A fault the kernel has already classified, raised as exception `code`
 * with two parameters - a guard page's STATUS_GUARD_PAGE_VIOLATION, whose
 * parameters are an access violation's (read/write/execute, address). */
int nt_exception_deliver_status(struct interrupt_frame *f, uint32 code,
                                uint64 info0, uint64 info1) {
    process_t *me = proc_current();
    nt_exception_record_t rec;

    if (me == NULL || me->personality != PERSONALITY_WINDOWS ||
        me->nt_exc_dispatcher == 0) {
        return 0;
    }
    zero(&rec, sizeof(rec));
    rec.code = code;
    rec.nparams = 2;
    rec.info[0] = info0;
    rec.info[1] = info1;
    return deliver(f, &rec);
}

static int deliver(struct interrupt_frame *f, nt_exception_record_t *recp) {
    process_t *me = proc_current();
    nt_exception_record_t rec = *recp;
    nt_context_t c;
    uint64 sp;

    ntctx_capture_intr(&c, f);
    if (f->vector == 3) {
        /* int3 is a trap - RIP is past it. NT reports the breakpoint AT
         * the instruction, in the record and in the context; a handler that
         * wants to go on steps past it itself. */
        c.rip -= 1;
    }
    rec.address = c.rip;

    sp = exception_frame(&c, &rec);
    if (sp == 0) {
        return 0;                  /* nowhere to put it: die as before */
    }
    f->rip = me->nt_exc_dispatcher;
    f->rsp = sp;
    /* Into the dispatcher without the trap flag - single-stepping ntdll is
     * not what a debuggee's TF asked for - and with DF clear, as the ABI
     * requires on entry to any function. */
    f->rflags &= ~(0x100ULL | 0x400ULL);
    return 1;
}

uint64 nt_raise_exception(struct syscall_frame *f, uint64 rec_ptr,
                          uint64 ctx_ptr, uint64 first_chance) {
    process_t *me = proc_current();
    nt_exception_record_t rec;
    nt_context_t c;
    uint64 sp;

    (void)f;
    if (!user_mapped(rec_ptr, sizeof(rec)) ||
        !user_mapped(ctx_ptr, NT_CONTEXT_SIZE)) {
        return STATUS_ACCESS_VIOLATION;
    }
    copy(&rec, (const void *)rec_ptr, sizeof(rec));
    if (rec.nparams > 15) {
        return STATUS_INVALID_PARAMETER;
    }
    copy(&c, (const void *)ctx_ptr, NT_CONTEXT_SIZE);

    if (!(uint8)first_chance) {
        /* Last chance, and nobody took it: the process dies with the
         * exception code as its exit status, and says why - an unhandled
         * exception with no report is the hardest kind of crash to find. */
        print_string("\n[process ", 0x0E);
        print_hex((uint32)me->pid, 0x0E);
        print_string(": unhandled exception ", 0x0E);
        print_hex(rec.code, 0x0E);
        print_string(" at ", 0x0E);
        print_hex64(rec.address, 0x0E);
        print_string("]\n", 0x0E);
        return syscall_exit_group(rec.code & 0xFF, f);
    }
    if (me->nt_exc_dispatcher == 0 || !ntctx_sanitize(&c)) {
        return STATUS_INVALID_PARAMETER;
    }
    sp = exception_frame(&c, &rec);
    if (sp == 0) {
        return STATUS_ACCESS_VIOLATION;
    }
    c.rip = me->nt_exc_dispatcher;
    c.rsp = sp;
    c.eflags &= ~(0x100u | 0x400u);
    ntctx_resume(&c);
}
