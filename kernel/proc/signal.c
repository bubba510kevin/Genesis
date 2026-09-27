#include "process.h"
#include "sched.h"
#include "screen.h"
#include "signal.h"
#include "syscall.h"
#include "typesk.h"

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
            return DFL_IGNORE;
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            /* No job control yet, so nothing can resume a stopped process -
             * stopping one would wedge it permanently. Ignoring is the less
             * wrong of two wrong answers until SIGCONT can actually arrive. */
            return DFL_STOP;
        default:
            return DFL_TERM;
    }
}

void signal_send(struct process *p, int signo) {
    uint64 bit = sigmask_of(signo);

    if (p == NULL || bit == 0 || p->state == PROC_UNUSED ||
        p->state == PROC_ZOMBIE) {
        return;
    }
    p->sig_pending |= bit;

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
    for (i = 0; i < MAX_PROCESSES; i++) {
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

int signal_deliver(struct process *p, struct syscall_frame *frame) {
    int signo;
    struct k_sigaction *sa;
    uint64 sp;
    struct sigframe *sf;
    int    sf_on_alt;

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
            case DFL_STOP:
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

    sf_on_alt = 0;
    sp = syscall_get_user_rsp();

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
    sp -= sizeof(struct sigframe);
    sp &= ~15ULL;
    /* The ABI wants RSP+8 aligned to 16 at a function's first instruction,
     * because a call pushes 8 bytes. The handler is entered as though called,
     * so the return address must sit at an address that leaves it so. */
    sp -= 8;

    sf = (struct sigframe *)sp;
    sf->restorer       = sa->restorer;
    sf->saved          = *frame;
    sf->saved_user_rsp = syscall_get_user_rsp();
    sf->saved_blocked  = p->sig_blocked;
    sf->signo          = (uint64)signo;
    sf->on_altstack    = (uint64)sf_on_alt;
    sf->magic          = SIGFRAME_MAGIC;

    /* Block this signal, plus whatever the handler asked for, for the
     * duration - otherwise a signal arriving during its own handler
     * re-enters it and the stack grows without bound. */
    p->sig_blocked |= sa->mask | sigmask_of(signo);

    frame->rip = sa->handler;
    frame->rdi = (uint64)signo;
    frame->rsi = 0;                 /* siginfo: SA_SIGINFO is not supported */
    frame->rdx = 0;                 /* ucontext: likewise */
    frame->rflags = 0x202;
    syscall_set_user_rsp(sp);
    return 1;
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

    p->sig_blocked = sf->saved_blocked;
    *frame = sf->saved;
    syscall_set_user_rsp(sf->saved_user_rsp);

    /* The interrupted syscall's return value, restored from the frame rather
     * than recomputed - a handler must not be able to change what the call
     * the process was making returns. */
    return frame->rax;
}
