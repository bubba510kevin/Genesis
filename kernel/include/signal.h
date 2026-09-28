#ifndef SIGNAL_H
#define SIGNAL_H

#include "syscall.h"
#include "typesk.h"

/* Signals.
 *
 * --- Where delivery happens ---
 * On the way out of a syscall, never in the middle of one and never from an
 * interrupt handler. A signal handler runs in USER mode on the USER stack, so
 * delivering it means rewriting the user context the kernel is about to
 * return to - and that context only exists, in a form we can rewrite, at the
 * syscall boundary.
 *
 * That is also why interrupting a blocked read works the way it does. Pressing
 * Ctrl-C while a process sits in read() does not deliver anything from the
 * keyboard IRQ. It marks the signal pending and wakes the process; the read
 * notices, returns -EINTR, and delivery happens on the way out. Every blocking
 * path in the kernel has to check for this, which is the real cost of signals
 * and the reason they land after the scheduler rather than before it.
 *
 * --- The frame ---
 * Delivery pushes a frame onto the user stack holding the interrupted
 * context, sets RIP to the handler, and puts a return address at the top of
 * the stack pointing at the restorer. The handler returns into the restorer,
 * which calls rt_sigreturn, which pops the frame and resumes.
 *
 * The layout is the kernel's own - nothing but rt_sigreturn ever reads it -
 * but SA_RESTORER is required, because a kernel-provided trampoline would
 * have to live in user memory and there is nowhere natural to put one. musl
 * always sets it on x86-64. */

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGABRT    6
#define SIGFPE     8
#define SIGKILL    9
#define SIGSEGV   11
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGWINCH  28

#define SIG_COUNT 32

#define SIG_DFL_HANDLER  0UL
#define SIG_IGN_HANDLER  1UL

#define SA_SIGINFO   0x00000004UL
/* Run this handler on the alternate stack installed by sigaltstack(2).
 * See process.h's sigalt_sp for why that facility exists at all. */
#define SA_NOCLDSTOP 0x00000001UL
#define SA_ONSTACK   0x08000000UL
#define SA_RESTORER  0x04000000UL
#define SA_RESTART   0x10000000UL

/* Exactly the layout rt_sigaction receives from a libc. */
struct k_sigaction {
    uint64 handler;
    uint64 flags;
    uint64 restorer;
    uint64 mask;
};

/* Bit for a signal number, or 0 if the number is out of range. Signals are
 * 1-based and the mask is 0-based, which is an off-by-one waiting to happen
 * in every function that touches both. */
static inline uint64 sigmask_of(int signo) {
    if (signo < 1 || signo >= SIG_COUNT) {
        return 0;
    }
    return 1ULL << (signo - 1);
}

struct process;

/* Mark a signal pending on a process and wake it if it is blocked. Safe from
 * an interrupt handler - it only sets bits and marks the process ready. */
void signal_send(struct process *p, int signo);

/* Terminate `p` now if its lowest pending signal is fatal (SIGKILL, or a
 * default-action-terminate signal with no handler). For the interrupt
 * return path, which has no syscall frame to deliver a handler into.
 * Returns 1 if it did. */
int signal_kill_if_fatal(struct process *p);

/* The same for every process in a process group. This is what Ctrl-C does. */
void signal_send_group(int pgid, int signo);

/* Non-zero if the current process has a signal pending that is not blocked -
 * what a blocking syscall checks to decide whether to give up with -EINTR. */
int signal_pending(struct process *p);

/* Deliver one pending signal by rewriting `frame` and the user stack, or take
 * the default action. Returns non-zero if the frame was modified. Called on
 * the syscall exit path. */
int signal_deliver(struct process *p, struct syscall_frame *frame);

/* Restore the context a delivery saved. Returns the value to leave in rax. */
uint64 signal_return(struct process *p, struct syscall_frame *frame);

/* Wait under a temporary mask (rt_sigsuspend, pselect6, ppoll). The caller's
 * mask is remembered and put back on the way out of the system call - by the
 * signal frame if a handler runs, by signal_restore_temp_mask otherwise - so
 * the signal that ended the wait is delivered under the temporary mask. See
 * sig_saved_mask in process.h. SIGKILL and SIGSTOP are never masked. */
void signal_set_temp_mask(struct process *p, uint64 mask);
void signal_restore_temp_mask(struct process *p);

/* Deliver one pending HANDLED signal to a thread returning to ring 3 from an
 * interrupt, by rewriting the interrupt frame (all sixteen registers are in
 * it). Default actions and SIG_IGN are taken as signal_deliver takes them.
 * Returns non-zero if the frame now enters a handler. Linux personality
 * only. Called after return_to_user, so the thread is the one that returns. */
struct interrupt_frame;
int signal_deliver_irq(struct process *p, struct interrupt_frame *f);

/* rt_sigreturn's exit for a frame delivered by signal_deliver_irq. */
void signal_iret_exit(struct interrupt_frame *f) __attribute__((noreturn));

#endif
