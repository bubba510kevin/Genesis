#ifndef PROCESS_H
#define PROCESS_H

#include "acl.h"
#include "object.h"
#include "cpu.h"
#include "paging.h"
#include "path.h"
#include "signal.h"
#include "syscall.h"
#include "typesk.h"

/* What a process is, in one struct.
 *
 * Everything here was a file-scope global in syscall.c: brk_current,
 * mmap_next, current_dir. That worked precisely as long as there was one
 * process, and every syscall that touched one of them was silently assuming
 * it. Collecting them is most of the work of supporting a second - the rest
 * is a scheduler deciding which `current` points at.
 *
 * No thread concept: one thread per process. Splitting them is what you do
 * when address space and execution context need separate lifetimes, and
 * nothing here needs that yet. */

/* Processes, threads, kernel threads and idle threads, all of them: each is
 * a process_t (about 3KB) and a kernel stack slot (KSTACK_SLOTS, which must
 * match). 64 until 2026-10-03; a Win32 program with a thread pool runs more
 * threads than that by itself. The scans over this table stop at
 * proc_slots_used(), so its size costs memory and nothing per switch. */
#define MAX_PROCESSES  256

/* Resource limits: Linux's numbering (asm-generic/resource.h). */
#define RLIMIT_CPU         0
#define RLIMIT_FSIZE       1
#define RLIMIT_DATA        2
#define RLIMIT_STACK       3
#define RLIMIT_CORE        4
#define RLIMIT_RSS         5
#define RLIMIT_NPROC       6
#define RLIMIT_NOFILE      7
#define RLIMIT_MEMLOCK     8
#define RLIMIT_AS          9
#define RLIMIT_LOCKS      10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE   12
#define RLIMIT_NICE       13
#define RLIMIT_RTPRIO     14
#define RLIMIT_RTTIME     15
#define RLIMIT_COUNT      16
#define RLIM_INFINITY     (~0ULL)

/* Set a fresh process's limits to the boot defaults. */
struct process;
void rlimit_defaults(struct process *p);
/* Inherit across fork: the child gets the parent's limits, soft and hard. */
void rlimit_copy(struct process *child, const struct process *parent);

/* What wait4/waitid are looking for, and which of them was found. */
#define PROC_WAIT_EXITED    1
#define PROC_WAIT_STOPPED   2
#define PROC_WAIT_CONTINUED 4

/* The first child of p that answers to `sel` (wait4's pid: >0 a pid, 0 p's
 * own group, -1 any, <-1 the group -sel) and has something `want` asks for:
 * a reapable zombie, an unreported stop, an unreported continue. *kind says
 * which. *any_match is set if ANY child answers to sel at all - the
 * difference between "wait" and -ECHILD. Consumes nothing: the caller
 * clears the report, or reaps. */
struct process *proc_wait_child(struct process *p, int sel, int want,
                                int *kind, int *any_match);

/* Fold a child that is about to be reaped into its parent's child CPU time.
 * Called by wait4 and waitid before proc_free. */
void proc_account_reaped(struct process *parent, const struct process *child);

/* CPU ticks of a whole thread group: every live thread sharing p's tgid.
 * RUSAGE_SELF and times() report the process, not the calling thread. */
uint64 proc_group_cpu_ticks(const struct process *p);
#define PROC_ARG_MAX   32

/* Which ABI a process speaks.
 *
 * On the process rather than decided per-binary at each syscall, because the
 * syscall entry has no idea what loaded the image - it has a number in rax
 * and nothing else. This is the WSL1 arrangement: the personality is a
 * property of the process, chosen by whichever loader built it, and syscall
 * dispatch selects a table from it.
 *
 * One value today. The field exists now because retrofitting it after forty
 * syscalls have been written against the assumption of a single ABI is a
 * rewrite, and adding it now is a line. */
typedef enum {
    PERSONALITY_LINUX = 0,
    PERSONALITY_WINDOWS
} personality_t;

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,        /* runnable, waiting for a CPU                      */
    PROC_RUNNING,      /* on the CPU now                                   */
    PROC_BLOCKED,      /* waiting on something - keyboard, a child         */
    PROC_ZOMBIE        /* exited; the status is still wanted by a parent   */
} proc_state_t;

/* State that belongs to a thread of execution rather than to the process.
 *
 * Nested rather than separate because there is one thread per process today.
 * It is nested rather than flattened because the kernel stack is inherently
 * per-thread - it holds a blocked syscall, and two threads can block
 * independently no matter what else they share - so this becomes its own
 * struct with its own table the day clone() exists. Keeping the fields
 * together now makes that a move rather than an audit of every call site. */
typedef struct thread {
    int    tid;
    uint64 kstack_top;   /* top of this thread's kernel stack               */
    uint64 kstack_base;  /* lowest mapped page; the guard page is below it  */
    uint64 saved_rsp;    /* where the context switcher parked it            */
    /* The FS base MSR: where a libc keeps its thread pointer.
     *
     * Per-CPU state belonging to exactly one thread, which makes it as much
     * a part of a context switch as RIP is. It was declared here and never
     * restored - arch_prctl wrote the MSR and nothing else - so the last
     * thread to call SET_FS owned the thread pointer for the whole machine.
     * Invisible while only one process used TLS; a general protection fault
     * in an innocent process the moment two did. */
    uint64 fs_base;

    /* The GS base MSR: where Windows keeps the TEB, at GS:0.
     *
     * Exactly the same kind of state as fs_base and exactly the same bug
     * class if it is not restored - the last thread to set it would own the
     * thread pointer for the whole machine. It is listed separately because
     * restoring it is NOT the same instruction: GS has two MSRs and which
     * one holds the user's value depends on which side of a swapgs the CPU
     * is on. syscall_set_user_gs_base sorts that out; nothing else should
     * write either MSR directly. */
    uint64 gs_base;

    /* x87 + SSE registers. See cpu.h: 16-byte alignment is not optional,
     * FXSAVE and FXRSTOR fault without it, and declaring it here is what
     * makes every process_t in the table correctly aligned too. */
    uint8  fpu_state[FXSAVE_SIZE] __attribute__((aligned(16)));
} thread_t;

/* Declared rather than included: waitq.h includes this header, so including
 * it back would be a cycle. Only the pointer is needed here. */
struct wait_queue;

typedef struct process {
    int             pid;
    int             ppid;
    proc_state_t    state;
    personality_t   personality;

    address_space_t *space;

    /* Memory layout, per process. */
    uint64          brk_base;
    uint64          brk_current;
    uint64          mmap_next;

    /* Where relative paths resolve from. Always absolute and normalized. */
    char            cwd[PATH_MAX_LEN];

    /* --- what a thread shares with its group -----------------------------
     *
     * A thread in this kernel is a process_t entry that SHARES three things
     * with the thread that created it: an address space, a descriptor table,
     * and a table of signal dispositions. That is the Linux arrangement - one
     * task per thread, with the sharing expressed as pointers rather than as
     * a separate thread object hanging off a process - and it is the one that
     * fits a scheduler which scans a table.
     *
     * The sharing is expressed by making these POINTERS. A group leader
     * points at its own storage below; a thread points at its leader's. Every
     * existing use site reads `p->handles` and `p->sig_handlers[n]`, both of
     * which mean the same thing through a pointer as through an array - which
     * is why this restructuring did not touch forty call sites.
     *
     * What it does change is ownership, and that is the part with teeth:
     * proc_retire and proc_free used to assume they owned all three, because
     * with one thread per process they always did. Closing the leader's
     * descriptor table when a thread exits would take the file the other
     * threads are reading out from under them. See owns_* below. */
    handle_t        *handles;
    handle_t         handle_store[MAX_HANDLES];

    /* Non-zero if this entry owns the storage the pointers above and below
     * refer to. A thread owns none of the three; a leader owns all three.
     *
     * Three flags rather than one `is_thread`, because clone() genuinely
     * offers them separately - CLONE_FILES without CLONE_VM is legal, and
     * vfork already produces a process that shares an address space and owns
     * its own descriptors. Collapsing them into one bit would make vfork and
     * a thread indistinguishable at teardown, which is where the distinction
     * matters most. */
    int              owns_files;
    int              owns_sighand;

    /* Thread group id: the pid of the group leader, and what getpid() must
     * report for EVERY thread in the group.
     *
     * A thread's own pid is its tid. POSIX says all threads of a process
     * share a process id, and a libc that sees two different answers from
     * getpid() in two threads will do something inventive with it - musl
     * caches it, so the damage is deferred and lands somewhere unrelated. */
    int              tgid;

    /* Where to write a zero and futex-wake when this thread exits, or 0.
     *
     * This is CLONE_CHILD_CLEARTID, and it is the entire mechanism behind
     * pthread_join: the joining thread futex-waits on this word, and the
     * kernel clearing it on exit is what releases them. Nothing in userspace
     * can do this itself - the thread cannot write the word after it has
     * stopped running, and a write before it stops is a lie. */
    uint64           clear_child_tid;

    /* --- an NT thread ------------------------------------------------------
     *
     * What NtCreateThreadEx gave this thread beyond what clone() gives a
     * POSIX one, and what its exit has to undo.
     *
     * nt_thread_obj is the waitable half (OBJ_THREAD): the thread holds one
     * reference so it can signal the object when it dies, and each handle
     * holds its own. NULL for the main thread of an NT process - nothing
     * created it through NtCreateThreadEx, so nothing holds a handle to wait
     * on - and for every POSIX task.
     *
     * nt_teb_va / nt_stack_lo / nt_stack_pages name this thread's private TEB
     * and user stack inside the SHARED address space, so they can be
     * unmapped when it exits rather than leaking a slot per thread for the
     * life of the process. 0 for a thread that has none of its own.
     *
     * nt_thread_start is ntdll's RtlUserThreadStart in this process's image,
     * found by the PE loader at execve; every NtCreateThreadEx thread begins
     * there. Inherited by the threads of the group. 0 for a process with no
     * ntdll, which therefore cannot create NT threads. */
    struct object   *nt_thread_obj;
    uint64           nt_teb_va;
    uint64           nt_stack_lo;
    uint64           nt_stack_pages;
    uint64           nt_thread_start;
    /* ntdll's KiUserApcDispatcher / KiUserExceptionDispatcher in this
     * image (pe.c finds them with RtlUserThreadStart), inherited by the
     * group's threads; 0 when the image has none. */
    uint64           nt_apc_dispatcher;
    uint64           nt_exc_dispatcher;
    /* This thread's implicit-TLS area (teb.h), unmapped when it exits the
     * same way its TEB and stack are. 0 pages: the process has no .tls. */
    uint64           nt_tls_va;
    uint64           nt_tls_pages;
    /* A process started from an ELF image that has since been given an NT
     * environment (PEB, parameters, a TEB per thread) so it can run Windows
     * DLLs - ROADMAP item 19, kernel/exec/ntmix.c. Set on every thread of
     * the group; inherited by clone and fork; cleared by exec. */
    int              nt_attached;
    /* thread.gs_base was changed while this thread was not the one running
     * the change (nt_attach gave it a TEB); load it on the way back to ring
     * 3 rather than waiting for the next context switch. */
    int              gs_reload;
    /* An NtAlertThreadByThreadId that has not been consumed by a wait yet. */
    volatile int     nt_alerted;
    /* NtSuspendThread's count; the thread runs only while it is 0. A thread
     * with a count parks on its way back to ring 3 (return_to_user), or -
     * created suspended - is never put on a run queue at all. nt_parked is
     * "blocked for this reason and no other": NtResumeThread wakes only a
     * parked thread, and a signal does not (see signal_send). */
    volatile int     nt_suspend_count;
    volatile int     nt_parked;
    /* Queued user-mode APCs, oldest first (nt_context.c). Freed when the
     * thread exits. */
    struct nt_apc   *nt_apc_head;
    struct nt_apc   *nt_apc_tail;

    thread_t        thread;

    /* --- kernel thread ---------------------------------------------------
     *
     * Non-zero if this entry is a KERNEL THREAD: something schedulable that
     * has no user address space and never returns to ring 3. See
     * kernel/include/kthread.h for what one is and the rule that comes with
     * it (they are not preemptible; they must yield or block).
     *
     * A flag on process_t rather than a separate schedulable type, because
     * the scheduler's table scan, the readiness states, sched_wake, the wait
     * queues and the tick accounting all already operate on a process_t -
     * and a second type would have meant a second copy of every one of them.
     *
     * What the flag actually gates is small and worth listing, because
     * anything NOT on this list must keep working identically for a kernel
     * thread or the choice above was the wrong one:
     *   sched.c        - skips the user-RSP parking and reaps exited threads
     *   process.c      - a kernel thread is nobody's child and nobody's
     *                    thread-group peer, so wait4 must never see one
     *   kern_synch.c   - a sleep may DESCHEDULE here, and only here
     *
     * `kentry`/`karg` are what the bootstrap trampoline calls. They live on
     * the process rather than being smuggled through the fabricated stack's
     * callee-saved slots because a C trampoline can read a struct field and
     * cannot read %rbx.
     *
     * `kname` is borrowed, not copied - a string literal from the creator. */
    int             is_kthread;
    void          (*kentry)(void *);
    void           *karg;
    const char     *kname;

    /* --- suspension ------------------------------------------------------
     * A process is only ever suspended inside a syscall, so its whole
     * resumable state is the frame it entered with plus the user stack
     * pointer the entry stub parked. Restoring both into the frame of
     * whichever process is running redirects the return path back into this
     * one - the same trick execve uses to leave the kernel somewhere new. */
    struct syscall_frame saved_frame;
    uint64          saved_user_rsp;

    /* Non-zero if `space` belongs to another process. A vfork child runs in
     * its parent's address space, so exit and execve must not destroy it -
     * that would take the parent's memory with it. */
    int             shares_space;

    /* pid of a parent suspended in vfork waiting for this process, or 0. */
    int             vfork_waiter;

    /* Non-zero while this process is blocked inside wait4. An exiting child
     * looks at its parent's copy to decide whether to wake it, which is what
     * keeps the wakeup from landing on a parent blocked on something else
     * entirely - the keyboard, most often. */
    int             waiting_for_child;

    /* The wait queue this process is currently listed on, or NULL.
     *
     * A back pointer rather than a search, because teardown needs to answer
     * "which queue is this process on" and there is no registry of queues to
     * walk - they are embedded in whatever object owns them, a pipe or the
     * keyboard, and nothing enumerates those either. Maintained by waitq_add
     * and waitq_remove, which are the only two functions that touch a
     * waiters[] array, so the pointer cannot disagree with the list unless
     * one of those two is wrong.
     *
     * A process is only ever on one queue at a time: waitq_wait is the sole
     * way onto one and it blocks, so there is no path that queues a process
     * and then queues it again somewhere else. If that ever stops being true
     * this becomes a list, and the assignment in waitq_add is where it will
     * be noticed. */
    struct wait_queue *blocked_on;

    /* Tick at which a sleeping process becomes runnable again, or 0 for a
     * process that is not sleeping. Zero is safe as the "not sleeping" value
     * because tick 0 is boot, and nothing can be waiting for a deadline that
     * has already passed by the time any process exists.
     *
     * Separate from blocked_on rather than folded into it: a wait queue is
     * woken by whoever owns the thing being waited FOR, and there is nobody
     * on the other side of a timeout. The timer is the only waker. */
    uint64          wake_tick;

    /* --- CPU accounting --------------------------------------------------
     * Ticks this process has spent RUNNING, and ticks it has spent blocked.
     * Charged by sched_tick, which is the only place that knows a tick has
     * elapsed and which process it elapsed for.
     *
     * Two counters rather than one, because the ratio between them is the
     * thing worth having. It is what an interactivity score reads: a process
     * that sleeps far more than it runs is waiting on a person or on I/O and
     * should be promoted; one that never blocks is compute-bound and should
     * not. Recording only runtime gives you accounting; recording both gives
     * you the input ULE actually wants, and adding the second one later would
     * mean a scoring function written against a number that did not exist
     * when the first was chosen.
     *
     * Ticks, not nanoseconds, and the reported value says so: the clock
     * resolution here is one tick whatever unit the ABI demands, and
     * converting on the way out is honest about that in a way storing
     * nanoseconds would not be. */
    uint64          run_ticks;
    uint64          sleep_ticks;

    /* Total ticks this process has ever run. NEVER DECAYED.
     *
     * A third counter, and it exists because the two above are the wrong
     * shape for accounting and were being used for it anyway.
     *
     * run_ticks is a SCORING input: kernel/proc/sched_ule.c halves it and
     * sleep_ticks every two seconds, deliberately, so the ratio reflects
     * recent behaviour rather than a lifetime average. That is right for the
     * scheduler and ruinous for a clock. clock_gettime(CLOCK_PROCESS_CPUTIME_
     * ID) and times(2) both read run_ticks, so the CPU time they reported
     * HALVED every two seconds - a clock that runs backwards, which times(2)
     * is defined never to do.
     *
     * It showed up as a verification check failing intermittently: a process
     * that spun for 1.4 seconds with the CPU to itself was charged 46% of it,
     * and the number moved run to run because it depended on where the
     * two-second decay boundary fell inside the measurement. The check was
     * right and the counter was wrong.
     *
     * Two fields rather than teaching ULE to decay a copy: the scheduler
     * wants a value it can scale freely, and accounting wants one nothing may
     * touch. Those are different lifetimes, so they are different fields. */
    uint64          cpu_ticks;

    /* CPU time of every child this process has reaped, and of everything
     * THEY reaped - the tms_cutime/RUSAGE_CHILDREN total. Added at reap time
     * (proc_account_reaped), which is exactly POSIX's rule: a child that has
     * not been waited for is not counted. */
    uint64          child_cpu_ticks;

    /* --- signals ---------------------------------------------------------
     * pending and blocked are bitmasks over signals 1..31. handlers is
     * per-process rather than per-thread because that is what POSIX says:
     * threads share dispositions and have their own masks. With one thread
     * per process the distinction is invisible, and putting it in the wrong
     * place now would be invisible too - right up until clone(). */
    uint64             sig_pending;
    uint64             sig_blocked;

    /* A mask to put back on the way out of the current system call - Linux's
     * TIF_RESTORE_SIGMASK. rt_sigsuspend, pselect6 and ppoll wait under a
     * TEMPORARY mask, and the signal that ends the wait is usually one the
     * caller's own mask blocks (bash blocks SIGCHLD and sigsuspends for it).
     * Restoring the old mask inside the syscall re-blocks that signal before
     * delivery, so the handler never runs and the wait was for nothing. So
     * the syscall leaves the temporary mask in place and records the old one
     * here; signal_deliver saves THIS in the signal frame (the handler runs,
     * and rt_sigreturn restores the caller's mask), and syscall_dispatch
     * restores it itself if nothing was delivered. */
    uint64             sig_saved_mask;
    int                sig_restore_mask;

    /* --- job control (ROADMAP item 15) -----------------------------------
     *
     * job_stopped is per THREAD: set on every thread of a group when a stop
     * signal's default action is taken, cleared by SIGCONT (or SIGKILL).
     * A stopped thread parks in return_to_user - the same parking NT
     * suspension uses, so nothing but a resume wakes it - and never runs
     * ring-3 code while it is set.
     *
     * The reports are on the process (the group leader) and are what wait4
     * with WUNTRACED/WCONTINUED hands the parent: stop_report holds the
     * signal that stopped it until a wait consumes it, cont_report is set by
     * a SIGCONT that resumed it. A stop clears a pending continue report and
     * a continue clears a pending stop report, as Linux's do.
     *
     * term_signal is the signal that killed the process, 0 if it exited:
     * what makes WIFSIGNALED true. It used to be folded into the exit status
     * as 128+signo, so a shell saw every killed child as one that had called
     * exit(130) - $? was right and `kill -l $?`-style reporting was not. */
    int                job_stopped;
    int                stop_report;
    int                cont_report;
    int                term_signal;

    /* --- resource limits (getrlimit/setrlimit/prlimit64) -----------------
     *
     * Linux's sixteen, soft then hard, RLIM_INFINITY as all ones. Per
     * process, inherited across fork and kept across exec, as POSIX says.
     * ENFORCED rather than reported where this kernel has the thing being
     * limited: RLIMIT_NOFILE bounds descriptor allocation (handle_alloc_from
     * and handle_install_at), and RLIMIT_STACK's hard limit is the stack exec
     * actually builds, so it cannot be raised past what exists. The rest are
     * recorded and returned faithfully; see rlimit_defaults in process.c for
     * which are which. */
    struct { uint64 cur, max; } rlim[RLIMIT_COUNT];

    /* --- the alternate signal stack (sigaltstack(2)) ---------------------
     *
     * Where a handler declared SA_ONSTACK runs. The reason it exists is the
     * one case the ordinary path cannot serve: a SIGSEGV raised BY the stack
     * itself - a guard-page hit, or a recursion that ran off the end - leaves
     * no room below RSP to build a signal frame, so delivering on the user
     * stack faults again, and the second fault is unrecoverable. A handler on
     * separate memory is the only way that signal can be caught at all.
     *
     * sigalt_on is a depth rather than a flag. A second SA_ONSTACK signal
     * delivered while the first handler is running must NOT restart at the
     * top of the alternate stack - that would write over the frame the outer
     * handler is standing on. Counting means the inner delivery sees "already
     * on it" and stacks below the outer frame, which is what Linux does. A
     * plain flag gets this right only until signals nest.
     *
     * Cleared on execve and inherited across fork, matching POSIX: the stack
     * is an address in an address space, so it survives a fork (same memory)
     * and cannot survive an exec (different memory). Getting that backwards
     * gives a handler a pointer into the previous program's image. */
    uint64             sigalt_sp;      /* base; 0 means none installed */
    uint64             sigalt_size;
    int                sigalt_on;      /* nesting depth, not a flag */

    /* Dispositions are SHARED by a thread group and the mask is not - that is
     * exactly what POSIX says, and it is why sig_blocked stays a plain field
     * while this became a pointer. With one thread per process the
     * distinction was invisible, and putting it in the wrong place would have
     * been invisible too, right up until clone(). */
    struct k_sigaction *sig_handlers;
    struct k_sigaction  sighand_store[SIG_COUNT];

    /* Process group, for signal delivery from the terminal. A new process
     * inherits its parent's; setpgid changes it. */
    int             pgid;

    /* --- added by Part 17's capability audit ----------------------------
     *
     * The session id, and the credentials setresuid/getresuid report.
     *
     * ONE uid rather than the real/effective/saved triple POSIX describes,
     * and setresuid REFUSES any request that would make the three differ
     * rather than accepting it and silently collapsing them - see
     * sys_setresuid. A caller that gets 0 back can rely on the value it
     * asked for being the value in effect.
     *
     * sid is 0 until setsid runs, and getsid falls back to pgid for a
     * process that never called it - which is the right answer for a
     * process that has only ever been in its parent's session. */
    int             sid;
    uint32          uid;
    uint32          gid;
    uint64          umask;

    /* --- supplementary groups --------------------------------------------
     *
     * Added because an ACL can name a group, and an identity with only one
     * gid can only ever match one of them. group@ and ACE_IDENTIFIER_GROUP
     * entries are the half of the NFSv4 model that a single gid cannot
     * express, so without this the ACL evaluator would silently answer "no"
     * to every group grant a user really does hold.
     *
     * CRED_NGROUPS of them, flat, by value. Linux allows 65536 and needs an
     * allocation to do it; nothing here is close, and a fixed array is one
     * fewer lifetime to get wrong on a fork. */
    uint32          ngroups;
    uint32          groups[CRED_NGROUPS];

    /* Set at exit, read by a parent that is not yet able to ask. */
    int             exit_status;

    /* Entry point and stack for the first return to user mode. Saved rather
     * than jumped to immediately, because execve has to finish tearing down
     * the old address space before it can leave the kernel. */
    uint64          entry;
    uint64          user_rsp;

    /* --- which CPU ---------------------------------------------------------
     *
     * `cpu` is where this thread last ran (the CPU it is running on, while
     * RUNNING). `oncpu` is non-zero from the moment a CPU switches to it
     * until that CPU switches AWAY - which is later than the thread turning
     * into a zombie, because a thread killed while running on another CPU
     * goes on executing there until that CPU next enters the kernel. Nothing
     * may free a slot, a kernel stack or an address space while its thread is
     * oncpu; proc_free and the reapers check.
     *
     * `affinity` is the set of CPUs it may run on, one bit each; all ones by
     * default and inherited across fork and thread creation, as Linux and NT
     * both do. `ideal_cpu` is NT's hint (SetThreadIdealProcessor): preferred
     * when free, never required. -1 for none.
     *
     * `is_idle` marks a CPU's idle thread: bound to that CPU, never counted
     * as work, picked only when nothing else is runnable there. */
    int             cpu;
    volatile int    oncpu;
    uint64          affinity;
    int             ideal_cpu;
    int             is_idle;

    /* Syscall tracing (PR_GENESIS_TRACE, /bin/gtrace): non-zero prints every
     * Linux-personality call this process makes, with its result. Inherited
     * across fork and kept across exec, so tracing a shell traces what it
     * runs - which is the point when the question is "what did bash do". */
    int             trace;
} process_t;

/* Set up the table and build the first process, which inherits the boot
 * kernel stack rather than being handed a fresh one - it is already running
 * on it. Called once, before the boot path loads anything. */
void proc_init(uint64 boot_kernel_stack_top);

/* Make this process's per-CPU state current: TSS.rsp0, the SYSCALL entry
 * stub's stack, and the FS base. Every context switch must call this; see the
 * comment on the definition for what happens when only some of it is
 * updated. */
void proc_activate_stack(process_t *p);

/* The process the current syscall is running on behalf of. Never NULL after
 * proc_init. */
process_t *proc_current(void);

/* Allocate an empty slot, or NULL if the table is full. The caller fills it
 * in; nothing is started here. */
process_t *proc_alloc(int ppid);

/* Mark a process dead where it cannot unwind: releases its descriptors,
 * notifies or kills its parent, and takes it off the run queue. The caller
 * must not return to ring 3 afterwards - return_to_user enforces that. */
void proc_retire(process_t *p, int exit_status);

/* Release a slot and its address space. Refuses the current process for the
 * same reason vmm_space_destroy does. */
void proc_free(process_t *p);

/* The NT half of a thread's death: signal its Thread object with
 * `exit_code` (a no-op if something already did - the first code wins) and
 * drop the thread's reference to it, and unmap its private TEB and user
 * stack from the shared address space. Safe to call more than once and on
 * a task that is not an NT thread. Called by both exit paths - the thread's
 * own and proc_retire - so no way of dying skips it. */
void proc_nt_thread_exit(process_t *p, uint32 exit_code);

/* Look up by pid, or NULL. */
process_t *proc_find(int pid);

/* This process's identity, in the form the access checks take.
 *
 * Assembled on demand rather than stored, because process_t already holds
 * uid and gid and a second copy inside a cred_t would be the two-sources-of-
 * truth mistake kernel/include/acl.h exists to argue against. The cost is a
 * ~90-byte copy per check; the benefit is that setuid cannot leave a stale
 * credential behind it.
 *
 * uid and euid are filled from the same field. Genesis carries one uid per
 * process, not the real/effective/saved triple - see the comment on
 * process_t::uid and sys_setresuid, which REFUSES a request that would make
 * them differ rather than collapsing them silently. */
void proc_cred(const process_t *p, cred_t *out);

/* The "supreme" privilege: root + NT AUTHORITY\SYSTEM + NT
 * SERVICE\TrustedInstaller fused into one exemption from every ACL check
 * (see cred_is_supreme in acl.h). Exactly one uid holds it at a time -
 * "give one user the permission" is enforced by there being only one
 * kernel-global to hold, not by a check on top of a list.
 *
 * genesis_supreme_uid_get returns the current holder's uid, or -1 if no one
 * holds it (the default at boot). genesis_supreme_uid_set changes who does;
 * it does not check the caller's privilege itself - PR_GENESIS_GRANT_SUPREME
 * in syscall.c is what refuses a non-root caller, so that the one gate lives
 * at the one place a user-reachable request enters, rather than being
 * duplicated here for every future kernel-internal caller to also satisfy.
 * It kprintf-logs every change, because a privilege grant that leaves no
 * trace is the one kind of bug this file's own culture does not tolerate
 * elsewhere (see bus_devclass_report). */
int genesis_supreme_uid_get(void);
void genesis_supreme_uid_set(int uid);

/* Make `p` the running process and point both kernel entry paths at its
 * stack. Does not touch CR3 - a vfork child shares its parent's address
 * space, and execve switches explicitly. */
void proc_set_current(process_t *p);

/* A new, NOT YET ENQUEUED thread of `parent`'s group: shares its address
 * space, descriptors, signal dispositions and credentials; starts by
 * returning to ring 3 through `start` on user stack `user_rsp`, with the
 * given FS and GS bases. The one constructor behind both clone()'s thread
 * shape and NtCreateThreadEx, so the two cannot disagree about what a
 * thread shares. NULL if the table is full. */
process_t *proc_spawn_thread(process_t *parent,
                             const struct syscall_frame *start,
                             uint64 user_rsp, uint64 fs_base,
                             uint64 gs_base);

/* First exited-but-unreaped child of `p`, or NULL. A child that is a
 * thread-group leader is not offered while any of its threads still runs. */
process_t *proc_reap_child(process_t *p);

/* Free every exited NON-LEADER thread (tgid != pid). Threads are not
 * children - wait4 never reports them - so without this each one that ever
 * exited held a process slot until reboot, and a program that created and
 * joined threads in a loop ran the table out after a dozen. Called from
 * schedule(), beside kthread_reap, where the dead thread is guaranteed not
 * to be the one running. */
void proc_reap_threads(void);

/* Table access by index, for a scheduler that scans rather than links. NULL
 * for an unused slot. */
process_t *proc_at(int index);
int proc_index(const process_t *p);

/* One past the highest table slot EVER allocated: every live entry has an
 * index below it, so a scan may stop there. It only grows. proc_alloc takes
 * the lowest free slot, so it settles at the most threads ever alive at once
 * rather than at MAX_PROCESSES - which is what keeps the scans on the
 * scheduling path (pick, reap, the per-tick passes) priced by the machine's
 * actual load and not by the size of the table (ROADMAP 16(k)). */
int proc_slots_used(void);

/* Make `p` current without touching stacks or CR3 - the scheduler does those
 * itself, in an order it controls. Current is PER CPU: this sets it for the
 * CPU executing the call. */
void proc_set_current_raw(process_t *p);

/* A CPU's idle thread: a kernel thread bound to `cpu` that runs
 * sched_idle_loop. NULL if the table is full. */
process_t *proc_create_idle(int cpu);

/* Print every task and every CPU's current thread - bound to Ctrl-T on the
 * console (see kbd_inject). */
void proc_dump(void);

/* Non-zero if `p` has any child at all, reaped or not. wait4 needs the
 * distinction: no children is -ECHILD, children that have not exited is a
 * different answer entirely. */
int proc_has_children(const process_t *p);

#endif
