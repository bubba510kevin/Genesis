#include "cpu.h"
#include "interrupt.h"
#include "process.h"
#include "sched.h"
#include "screen.h"
#include "signal.h"
#include "syscall.h"
#include "typesk.h"
#include "waitq.h"

/* What happens to a signal nobody handles.
 *
 * Getting the defaults right matters more than it looks: a shell relies on
 * SIGCHLD being ignored by default and on SIGINT killing the foreground job.
 * Treating every unhandled signal as fatal would make the first SIGCHLD kill
 * the shell. */
typedef enum { DFL_TERM, DFL_IGNORE, DFL_STOP } default_action_t;

static default_action_t default_action(int signo) {
    switch (signo) {
        case SIGCHLD:
        case SIGCONT:
        case SIGURG:
        case SIGWINCH:
            /* SIGWINCH and SIGURG are ignored by default on Linux. Falling
             * through to "terminate" would kill every program that did not
             * install a handler the first time the terminal is resized. */
            return DFL_IGNORE;
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            /* Stop the whole process until SIGCONT (job_stop below). These
             * used to be IGNORED, because nothing could resume a stopped
             * process and stopping one would have wedged it for good. */
            return DFL_STOP;
        default:
            return DFL_TERM;
    }
}

/* --- job control -----------------------------------------------------------
 *
 * A stop is a property of the PROCESS - every thread of it stops - and is
 * taken in the stopping thread's own context (take_signal, at its next
 * return to ring 3), which is where POSIX puts the default action. The
 * threads then park in return_to_user until SIGCONT. The parent is told the
 * way it is told about an exit: SIGCHLD (unless it asked for SA_NOCLDSTOP)
 * and a wakeup for a wait4 in progress, which reports it under WUNTRACED.
 *
 * NOT DONE: POSIX's orphaned-process-group rule (SIGTSTP/TTIN/TTOU sent to a
 * group with no parent in the session outside it are discarded), and the
 * transparent restart of a system call a stop interrupted - a read that was
 * blocked when SIGSTOP arrived returns -EINTR after SIGCONT, where Linux
 * would resume it. Both are recorded in ROADMAP item 15. */
#define STOP_SIGNALS (sigmask_of(SIGSTOP) | sigmask_of(SIGTSTP) | \
                      sigmask_of(SIGTTIN) | sigmask_of(SIGTTOU))

static int is_stop_signal(int signo) {
    return (sigmask_of(signo) & STOP_SIGNALS) != 0;
}

static struct process *leader_of(struct process *p) {
    struct process *l = proc_find(p->tgid);

    return l != NULL ? l : p;
}

/* SIGCHLD for a stop or a continue, and a wake for any thread of the parent
 * sitting in wait4 - the same two things an exit does (proc_retire). */
static void notify_parent_job(struct process *leader) {
    struct process *parent = proc_find(leader->ppid);
    int i;

    if (parent == NULL) {
        return;
    }
    if (!(parent->sig_handlers[SIGCHLD].flags & SA_NOCLDSTOP)) {
        signal_send(parent, SIGCHLD);
    }
    for (i = 0; i < proc_slots_used(); i++) {
        struct process *t = proc_at(i);

        if (t != NULL && t->state == PROC_BLOCKED && t->waiting_for_child &&
            t->tgid == parent->tgid) {
            sched_wake(t);
        }
    }
}

static void job_stop(struct process *p, int signo) {
    struct process *leader = leader_of(p);
    int i;

    for (i = 0; i < proc_slots_used(); i++) {
        struct process *t = proc_at(i);

        if (t == NULL || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE ||
            t->tgid != p->tgid) {
            continue;
        }
        t->job_stopped = 1;
        /* A sibling running ring-3 code on another CPU is kicked into the
         * kernel so it parks now, not at its next system call. */
        if (t != p && t->state == PROC_RUNNING && t->oncpu) {
            sched_poke(t);
        }
    }
    leader->stop_report = signo;
    leader->cont_report = 0;
    notify_parent_job(leader);
}

/* SIGCONT's effect, which happens when it is SENT - whether or not it is
 * blocked, ignored or caught - as POSIX requires: resume every thread, and
 * throw away stop signals still pending. */
static void job_continue(struct process *p) {
    int i, was_stopped = 0;

    for (i = 0; i < proc_slots_used(); i++) {
        struct process *t = proc_at(i);

        if (t == NULL || t->state == PROC_UNUSED || t->tgid != p->tgid) {
            continue;
        }
        t->sig_pending &= ~STOP_SIGNALS;
        if (t->job_stopped) {
            was_stopped = 1;
            t->job_stopped = 0;
            if (t->state == PROC_BLOCKED && t->nt_parked &&
                t->nt_suspend_count == 0) {
                sched_wake(t);
            }
        }
    }
    if (was_stopped) {
        struct process *leader = leader_of(p);

        leader->stop_report = 0;
        leader->cont_report = 1;
        notify_parent_job(leader);
    }
}

void signal_send(struct process *p, int signo) {
    uint64 bit = sigmask_of(signo);

    if (p == NULL || bit == 0 || p->state == PROC_UNUSED ||
        p->state == PROC_ZOMBIE) {
        return;
    }
    if (signo == SIGCONT) {
        job_continue(p);
    } else if (is_stop_signal(signo)) {
        /* A stop cancels a SIGCONT not yet delivered, and vice versa. */
        p->sig_pending &= ~sigmask_of(SIGCONT);
    } else if (signo == SIGKILL && p->job_stopped) {
        /* A stopped process can still be killed: take the whole group out
         * of its stop so every thread reaches the kill. */
        int i;

        for (i = 0; i < proc_slots_used(); i++) {
            struct process *t = proc_at(i);

            if (t != NULL && t->state != PROC_UNUSED && t->tgid == p->tgid &&
                t->job_stopped) {
                t->job_stopped = 0;
                if (t->state == PROC_BLOCKED && t->nt_parked &&
                    t->nt_suspend_count == 0) {
                    sched_wake(t);
                }
            }
        }
    }
    /* An IGNORED signal is discarded here, never made pending - Linux's
     * sig_ignored(). Unless it is blocked: then the disposition may change
     * before it is unblocked, and it has to be there when that happens.
     *
     * Pending-and-ignored used to be the rule, and it was invisible until
     * wait4 learned to wait for ONE child: the SIGCHLD of a different child
     * exiting (ignored by default) sat pending, a pending signal makes a
     * blocking wait return -EINTR, and `wait $pid` failed whenever another
     * job finished first. Every blocking call had the same exposure. */
    if (!(p->sig_blocked & bit) && signo != SIGKILL && signo != SIGSTOP &&
        (p->sig_handlers[signo].handler == SIG_IGN_HANDLER ||
         (p->sig_handlers[signo].handler == SIG_DFL_HANDLER &&
          default_action(signo) == DFL_IGNORE))) {
        return;
    }
    p->sig_pending |= bit;

    /* A BLOCKED signal may be what a signalfd is waiting for, and the
     * thread polling that signalfd need not be the target: tell every
     * readiness waiter to look again (waitq.h). */
    if (p->sig_blocked & bit) {
        waitq_wake_all(waitq_readiness());
    }

    /* Waking a blocked process is what makes a signal interrupt a read.
     * Delivery does not happen here - this may be an interrupt handler, and
     * there is no user context to rewrite from one. */
    if (p->state == PROC_BLOCKED && !p->nt_parked) {
        /* Not a SUSPENDED thread: return_to_user would only park it again,
         * and one created suspended has not run yet - waking it would start
         * it, which only NtResumeThread may do. */
        sched_wake(p);
    } else if (p->state == PROC_RUNNING && p->oncpu) {
        /* Running in ring 3 on another CPU: it would not look at its pending
         * set until its next system call. Make that CPU enter the kernel
         * now, so a fatal signal lands (interrupt_dispatch acts on those)
         * without waiting for the target to cooperate. */
        sched_poke(p);
    }
}

void signal_send_group(int pgid, int signo) {
    int i;

    if (pgid <= 0) {
        return;
    }
    for (i = 0; i < proc_slots_used(); i++) {
        struct process *p = proc_at(i);
        if (p != NULL && p->pgid == pgid) {
            signal_send(p, signo);
        }
    }
}

int signal_pending(struct process *p) {
    if (p == NULL) {
        return 0;
    }
    /* SIGKILL and SIGSTOP cannot be blocked. Masking that off here rather
     * than at sigprocmask time means a process cannot become unkillable by
     * blocking them before the check is added elsewhere. */
    return (p->sig_pending & ~(p->sig_blocked &
            ~(sigmask_of(SIGKILL) | sigmask_of(SIGSTOP)))) != 0;
}

/* The frame pushed onto the user stack. Its layout is private to this file:
 * nothing but signal_return reads it, so it holds exactly what resuming
 * needs and nothing for compatibility's sake. */
struct sigframe {
    uint64 restorer;            /* [rsp] on entry: where the handler returns */
    struct syscall_frame saved;
    uint64 saved_user_rsp;
    uint64 saved_blocked;
    uint64 signo;
    /* Whether THIS delivery is the one that stepped onto the alternate stack.
     * Recorded per frame rather than inferred from RSP at return time,
     * because a handler may legitimately have switched stacks itself, and
     * because the nesting depth has to be decremented exactly once by the
     * delivery that incremented it. */
    uint64 on_altstack;
    /* A delivery from the INTERRUPT path (signal_deliver_irq): the process
     * was stopped at an arbitrary instruction, not at a syscall, so rcx and
     * r11 are live and must come back - which sysret cannot do (it loads
     * them with RIP and RFLAGS). rt_sigreturn leaves by iretq for these. */
    uint64 full;
    uint64 saved_rcx;
    uint64 saved_r11;
    /* The interrupted FPU/SSE state, an FXSAVE image at this user address
     * (16-aligned, just above the frame). A handler is ordinary C and uses
     * xmm registers freely; the interrupted code may have had live values
     * in them - always so when interrupted mid-instruction-stream. */
    uint64 fpu_at;
    uint64 magic;
};

#define SIGFRAME_MAGIC 0x5349474652414D45ULL   /* "SIGFRAME" */

static int lowest_pending(struct process *p) {
    uint64 deliverable = p->sig_pending &
        ~(p->sig_blocked & ~(sigmask_of(SIGKILL) | sigmask_of(SIGSTOP)));
    int i;

    for (i = 1; i < SIG_COUNT; i++) {
        if (deliverable & sigmask_of(i)) {
            return i;
        }
    }
    return 0;
}

static void terminate(struct process *p, int signo);

/* Act on a pending signal whose effect is to terminate - SIGKILL, or one
 * left at its default action where that action is to terminate - without a
 * syscall frame. The interrupt return path calls this for a thread it is
 * about to resume in ring 3, which is how a signal reaches a thread that
 * never makes a system call. Anything with a handler is left pending for
 * signal_deliver at the next syscall boundary. Returns 1 if it killed. */
int signal_kill_if_fatal(struct process *p) {
    int signo = lowest_pending(p);

    if (signo == 0) {
        return 0;
    }
    if (signo == SIGKILL ||
        (p->sig_handlers[signo].handler == SIG_DFL_HANDLER &&
         default_action(signo) == DFL_TERM)) {
        p->sig_pending &= ~sigmask_of(signo);
        terminate(p, signo);
        return 1;
    }
    return 0;
}

static void terminate(struct process *p, int signo) {
    /* What wait4 reports as WIFSIGNALED - on the process, whichever of its
     * threads took the signal. */
    p->term_signal = signo;
    leader_of(p)->term_signal = signo;
    print_string("\n[process ", 0x0E);
    print_hex((uint32)p->pid, 0x0E);
    print_string(" killed by signal ", 0x0E);
    print_hex((uint32)signo, 0x0E);
    print_string("]\n", 0x0E);

    /* Marking the process a zombie is not enough on its own, and used not to
     * be followed by anything: the syscall this was reached from still
     * returned to ring 3, so a "killed" process carried on executing with a
     * frame nobody had restored. What that looks like from outside is a
     * process dying twice - once from the signal, once from the page fault it
     * takes a few instructions later at whatever address happened to be in a
     * register.
     *
     * proc_retire releases what it holds, tells the parent, and takes it off
     * the run queue; return_to_user refuses to resume a zombie. */
    proc_retire(p, 128 + signo);    /* 128 + signo: what a shell reports */
}

/* Pick the signal to act on and do everything short of entering a handler:
 * the default actions, SIG_IGN, SIGKILL. Returns the signal whose handler
 * must run, or 0 when there is nothing more to do (the signal was consumed,
 * or the process is now a zombie). */
static int take_signal(struct process *p) {
    int signo;
    struct k_sigaction *sa;

    if (p == NULL) {
        return 0;
    }
    signo = lowest_pending(p);
    if (signo == 0) {
        return 0;
    }
    p->sig_pending &= ~sigmask_of(signo);
    sa = &p->sig_handlers[signo];

    /* SIGKILL is not negotiable regardless of what was registered. */
    if (signo == SIGKILL) {
        terminate(p, signo);
        return 0;
    }
    if (sa->handler == SIG_IGN_HANDLER) {
        return 0;
    }
    if (sa->handler == SIG_DFL_HANDLER) {
        switch (default_action(signo)) {
            case DFL_IGNORE:
                return 0;
            case DFL_STOP:
                job_stop(p, signo);
                return 0;
            case DFL_TERM:
            default:
                terminate(p, signo);
                return 0;
        }
    }

    /* A handler with no restorer cannot return, and a handler that cannot
     * return corrupts the process the moment it tries. Refusing to deliver is
     * better than delivering something that will fault somewhere unrelated. */
    if (!(sa->flags & SA_RESTORER) || sa->restorer == 0) {
        terminate(p, signo);
        return 0;
    }
    return signo;
}

/* Build the handler's frame on the user stack below `user_rsp` and return
 * the RSP the handler starts with. `regs` is the interrupted context in
 * syscall_frame form; `full` says rcx/r11 are live too (the interrupt path).
 * The live FPU state - this CPU's registers, which are the process's own
 * while it is in the kernel - is saved alongside. */
static uint64 build_frame(struct process *p, int signo,
                          const struct syscall_frame *regs, uint64 user_rsp,
                          int full, uint64 rcx, uint64 r11) {
    struct k_sigaction *sa = &p->sig_handlers[signo];
    struct sigframe *sf;
    uint64 sp = user_rsp, fpu_at;
    int    sf_on_alt = 0;

    /* --- SA_ONSTACK: run this handler on the alternate stack -------------
     *
     * Only when the handler asked for it AND a stack is installed AND we are
     * not already running on it. The third test is what makes nesting safe:
     * a second SA_ONSTACK signal delivered inside the first handler must
     * continue DOWN from the current RSP, not restart at the top, or it
     * overwrites the frame the outer handler is standing on.
     *
     * The red zone is skipped in this branch and that is deliberate rather
     * than an oversight. The 128 bytes below RSP belong to the interrupted
     * function; on a fresh alternate stack there is no interrupted function
     * and nothing below the top to protect. Subtracting it anyway would be
     * harmless but would say something untrue about why. */
    if ((sa->flags & SA_ONSTACK) && p->sigalt_sp != 0 && p->sigalt_on == 0) {
        sp = p->sigalt_sp + p->sigalt_size;
        p->sigalt_on++;
        sf_on_alt = 1;
    } else {
        /* The red zone: 128 bytes below RSP that a leaf function may be using
         * without having adjusted RSP. Writing there corrupts live data in the
         * interrupted function. */
        sp -= 128;
    }

    /* The FXSAVE image first (it must be 16-aligned), the frame below it. */
    sp = (sp - FXSAVE_SIZE) & ~15ULL;
    fpu_at = sp;
    fpu_save((void *)fpu_at);

    sp -= sizeof(struct sigframe);
    sp &= ~15ULL;
    /* The ABI wants RSP+8 aligned to 16 at a function's first instruction,
     * because a call pushes 8 bytes. The handler is entered as though called,
     * so the return address must sit at an address that leaves it so. */
    sp -= 8;

    sf = (struct sigframe *)sp;
    sf->restorer       = sa->restorer;
    sf->saved          = *regs;
    sf->saved_user_rsp = user_rsp;
    /* The mask the handler's return goes back to. Under a temporary mask
     * (sigsuspend, pselect6) that is the caller's own, not the temporary one
     * - see sig_saved_mask in process.h. The handler itself runs with the
     * temporary mask plus its own, exactly as on Linux. */
    if (p->sig_restore_mask) {
        sf->saved_blocked  = p->sig_saved_mask;
        p->sig_restore_mask = 0;
    } else {
        sf->saved_blocked  = p->sig_blocked;
    }
    sf->signo          = (uint64)signo;
    sf->on_altstack    = (uint64)sf_on_alt;
    sf->full           = (uint64)full;
    sf->saved_rcx      = rcx;
    sf->saved_r11      = r11;
    sf->fpu_at         = fpu_at;
    sf->magic          = SIGFRAME_MAGIC;

    /* Block this signal, plus whatever the handler asked for, for the
     * duration - otherwise a signal arriving during its own handler
     * re-enters it and the stack grows without bound. */
    p->sig_blocked |= sa->mask | sigmask_of(signo);
    return sp;
}

int signal_deliver(struct process *p, struct syscall_frame *frame) {
    int signo = take_signal(p);
    uint64 sp;

    if (signo == 0) {
        return 0;
    }
    sp = build_frame(p, signo, frame, syscall_get_user_rsp(), 0, 0, 0);
    frame->rip = p->sig_handlers[signo].handler;
    frame->rdi = (uint64)signo;
    frame->rsi = 0;                 /* siginfo: SA_SIGINFO is not supported */
    frame->rdx = 0;                 /* ucontext: likewise */
    frame->rflags = 0x202;
    syscall_set_user_rsp(sp);
    return 1;
}

/* The interrupt path's delivery: a thread about to return to ring 3 from an
 * interrupt - the timer tick that ends a compute-bound quantum, the IPI that
 * signal_send uses to kick a thread running on another CPU - with a handled
 * signal pending.
 *
 * Without this a handler ran only at a system call. A program that computes
 * without making one - bash running `while :; do :; done` - never saw its
 * SIGINT handler: ^C was echoed and the loop went on for ever. The interrupt
 * path could only KILL (signal_kill_if_fatal), which is right for the
 * default action and wrong for everything a program catches. */
int signal_deliver_irq(struct process *p, struct interrupt_frame *f) {
    struct syscall_frame regs;
    int signo;
    uint64 sp;

    if (p == NULL || p->personality != PERSONALITY_LINUX ||
        !signal_pending(p)) {
        return 0;
    }
    signo = take_signal(p);
    if (signo == 0) {
        return 0;
    }
    regs.rax = f->rax;
    regs.rdi = f->rdi;
    regs.rsi = f->rsi;
    regs.rdx = f->rdx;
    regs.r10 = f->r10;
    regs.r8  = f->r8;
    regs.r9  = f->r9;
    regs.rip = f->rip;
    regs.rflags = f->rflags;
    regs.rbx = f->rbx;
    regs.rbp = f->rbp;
    regs.r12 = f->r12;
    regs.r13 = f->r13;
    regs.r14 = f->r14;
    regs.r15 = f->r15;
    sp = build_frame(p, signo, &regs, f->rsp, 1, f->rcx, f->r11);
    f->rip = p->sig_handlers[signo].handler;
    f->rdi = (uint64)signo;
    f->rsi = 0;
    f->rdx = 0;
    f->rflags = 0x202;
    f->rsp = sp;
    return 1;
}

void signal_set_temp_mask(struct process *p, uint64 mask) {
    if (!p->sig_restore_mask) {
        p->sig_saved_mask   = p->sig_blocked;
        p->sig_restore_mask = 1;
    }
    p->sig_blocked = mask & ~(sigmask_of(SIGKILL) | sigmask_of(SIGSTOP));
}

void signal_restore_temp_mask(struct process *p) {
    if (p != NULL && p->sig_restore_mask) {
        p->sig_blocked      = p->sig_saved_mask;
        p->sig_restore_mask = 0;
    }
}

uint64 signal_return(struct process *p, struct syscall_frame *frame) {
    /* MINUS EIGHT, and it is not an adjustment - it is the return address the
     * handler just popped.
     *
     * Delivery puts the restorer at [rsp] as the frame's FIRST member, which
     * is what makes the handler look like an ordinary call. The handler ends
     * with `ret`, which pops that member; by the time the restorer issues
     * rt_sigreturn, rsp points one slot ABOVE the frame. Reading the frame at
     * rsp therefore reads eight bytes into it, the magic lands in the wrong
     * field, and every delivery ends in "bad signal frame" - which looks like
     * a corrupt handler rather than an off-by-one here.
     *
     * Linux computes the same thing the same way: rsp - sizeof(long). */
    struct sigframe *sf =
        (struct sigframe *)(syscall_get_user_rsp() - sizeof(uint64));

    /* rt_sigreturn is reachable by any process that cares to call it, with
     * RSP pointing anywhere. The magic is not security - a hostile process
     * can write it - but it catches the overwhelmingly more likely case of a
     * handler that corrupted its own stack, and turns a wild resume into a
     * clean kill. */
    if (sf->magic != SIGFRAME_MAGIC) {
        print_string("\n[bad signal frame in rt_sigreturn]\n", 0x4F);
        terminate(p, SIGSEGV);
        return (uint64)-14;
    }

    /* Step back off the alternate stack, if THIS delivery is the one that
     * stepped onto it. Decremented from the frame's own record rather than by
     * comparing RSP against the alt-stack range, because a handler is allowed
     * to move RSP wherever it likes and the depth must come back down exactly
     * as many times as it went up. Without this, sigaltstack reports
     * SS_ONSTACK forever after the first delivery and every later SA_ONSTACK
     * signal is delivered on the interrupted stack instead. */
    if (sf->on_altstack && p->sigalt_on > 0) {
        p->sigalt_on--;
    }

    p->sig_blocked = sf->saved_blocked & ~(sigmask_of(SIGKILL) |
                                           sigmask_of(SIGSTOP));

    /* The FPU state the delivery saved. Copied into the kernel and made safe
     * before it is loaded: a reserved MXCSR bit - which the handler, or
     * anything else with the stack, may have written - makes fxrstor raise
     * #GP in ring 0, and a torn copy read twice could differ between the
     * check and the load. */
    if (sf->fpu_at != 0 && (sf->fpu_at & 15) == 0 &&
        sf->fpu_at < 0x0000800000000000ULL - FXSAVE_SIZE) {
        uint8 area[FXSAVE_SIZE] __attribute__((aligned(16)));
        uint8 mine[FXSAVE_SIZE] __attribute__((aligned(16)));
        uint32 mask;
        int i;

        for (i = 0; i < FXSAVE_SIZE; i++) {
            area[i] = ((const uint8 *)sf->fpu_at)[i];
        }
        fpu_save(mine);
        mask = *(const uint32 *)(mine + 28);   /* MXCSR_MASK */
        if (mask == 0) {
            mask = 0xFFBFu;
        }
        *(uint32 *)(area + 24) &= mask;
        fpu_restore(area);
    }

    if (sf->full) {
        /* Interrupted mid-instruction-stream (signal_deliver_irq): every
         * register comes back, rcx and r11 included, so the way out is
         * iretq rather than sysret. Does not return. */
        struct interrupt_frame f;

        f.rax = sf->saved.rax;  f.rbx = sf->saved.rbx;
        f.rcx = sf->saved_rcx;  f.rdx = sf->saved.rdx;
        f.rsi = sf->saved.rsi;  f.rdi = sf->saved.rdi;
        f.rbp = sf->saved.rbp;
        f.r8  = sf->saved.r8;   f.r9  = sf->saved.r9;
        f.r10 = sf->saved.r10;  f.r11 = sf->saved_r11;
        f.r12 = sf->saved.r12;  f.r13 = sf->saved.r13;
        f.r14 = sf->saved.r14;  f.r15 = sf->saved.r15;
        f.vector = 0;
        f.error_code = 0;
        f.rip = sf->saved.rip;
        f.cs = USER_CS;
        /* IOPL, VM, NT and friends are not the process's to set; IF must
         * be on. The same mask ntctx_sanitize applies. */
        f.rflags = (sf->saved.rflags & 0x240DD5ULL) | 0x202ULL;
        f.rsp = sf->saved_user_rsp;
        f.ss = USER_SS;
        if (f.rip >= 0x0000800000000000ULL || f.rsp >= 0x0000800000000000ULL) {
            terminate(p, SIGSEGV);
            return (uint64)-14;
        }
        signal_iret_exit(&f);
    }

    *frame = sf->saved;
    syscall_set_user_rsp(sf->saved_user_rsp);

    /* The interrupted syscall's return value, restored from the frame rather
     * than recomputed - a handler must not be able to change what the call
     * the process was making returns. */
    return frame->rax;
}

/* Leave for ring 3 through iretq with every register from `f`. What
 * NtContinue does (nt_context.c), and for the same reason: sysret cannot
 * restore rcx and r11. Everything return_to_user owes a returning thread -
 * a reschedule, a suspension, a zombie never coming back - happens first. */
void nt_iret_exit(struct interrupt_frame *f) __attribute__((noreturn));

void signal_iret_exit(struct interrupt_frame *f) {
    return_to_user(1);
    __asm__ volatile ("cli");
    nt_iret_exit(f);
}
