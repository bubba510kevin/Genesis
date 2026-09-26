#include "paging.h"
#include "cpu.h"
#include "gdt.h"
#include "io.h"
#include "kprintf.h"
#include "kstack.h"
#include "object.h"
#include "sched.h"
#include "screen.h"
#include "signal.h"
#include "syscall.h"
#include "waitq.h"
#include "process.h"
#include "tty.h"
#include "typesk.h"

/* A fixed table rather than a linked list off the heap.
 *
 * Sixteen is a limit you will hit and notice, which is the point: a process
 * leak becomes "cannot fork" at process seventeen rather than a slow heap
 * exhaustion three hours in. It also means proc_find is a loop over a small
 * array with no allocation anywhere in it, so it is safe to call from
 * contexts where the heap is not. */

/* FXSAVE and FXRSTOR raise #GP on a misaligned area, and a #GP inside the
 * context switcher tells you nothing about which of sixteen slots was wrong.
 * These fail the build instead. The second one is the part that is easy to
 * lose: aligning the FIELD only helps if the enclosing struct is aligned too,
 * which it is exactly as long as nobody adds __attribute__((packed)) to
 * process_t. */
typedef char fpu_field_is_aligned[
    (__builtin_offsetof(process_t, thread.fpu_state) % 16 == 0) ? 1 : -1];
typedef char process_is_aligned[
    (__alignof__(process_t) % 16 == 0) ? 1 : -1];

static process_t table[MAX_PROCESSES];
static process_t *current;
static int next_pid = 1;

/* Which kstack slot each table entry owns. Tied to the table index rather
 * than allocated separately, so a slot cannot outlive or be orphaned by its
 * process - the two lifetimes are the same lifetime. */
static int slot_of(const process_t *p) {
    return (int)(p - table);
}

/* Point both kernel entry paths at a thread's stack.
 *
 * Two writes, not one, and forgetting either is a distinct bug:
 *   gdt_set_kernel_stack     -> TSS.rsp0, used on ring 3 -> ring 0 through an
 *                               IDT gate (an interrupt or an exception)
 *   syscall_set_kernel_stack -> the per-CPU block the SYSCALL stub reads
 *
 * A context switch that updates only the TSS works until the new thread makes
 * a syscall, at which point it runs on the outgoing thread's stack and
 * corrupts a frame that is still live. */
void proc_activate_stack(process_t *p) {
    if (p == NULL || p->thread.kstack_top == 0) {
        return;
    }
    gdt_set_kernel_stack(p->thread.kstack_top);
    syscall_set_kernel_stack(p->thread.kstack_top);

    /* The thread pointer, restored with the stacks because it is the same
     * kind of thing: state the CPU holds on behalf of exactly one process.
     * Leaving it out meant a process resuming after another one had called
     * arch_prctl found %fs pointing into the other's address space - reading
     * whatever now lived at that address as its TLS block, and faulting on
     * the first pointer it found there. */
    wrmsr(MSR_FS_BASE, p->thread.fs_base);

    /* And the GS base, which Windows puts the TEB at. Same reasoning, same
     * bug class - but NOT the same instruction: swapgs means the user's GS
     * base is in one of two MSRs depending on which side of it the CPU is
     * on, and writing the wrong one destroys the per-CPU block. That
     * decision lives in syscall.c, next to the block itself. */
    syscall_set_user_gs_base(p->thread.gs_base);
}

static uint64 boot_stack_top;

void proc_init(uint64 boot_kernel_stack_top) {
    int i;

    boot_stack_top = boot_kernel_stack_top;

    for (i = 0; i < MAX_PROCESSES; i++) {
        table[i].pid   = 0;
        table[i].state = PROC_UNUSED;
        table[i].space = NULL;
    }

    /* Process 1 starts in the kernel's address space because that is where it
     * genuinely is: this runs before any binary is loaded, and claiming a
     * space it is not using would be a lie the first fault would expose.
     *
     * The boot path replaces it with a real one in start_init_process(),
     * before ring 3 is ever entered. It has to: a process left in the kernel
     * space cannot fork, since there is nothing meaningful to copy-on-write.
     * That used to be masked by the init binary always being one that execve'd
     * immediately. */
    current = &table[0];
    current->pid         = next_pid++;
    current->ppid        = 0;
    current->state       = PROC_RUNNING;
    current->space       = vmm_kernel_space();
    current->brk_base    = 0;
    current->brk_current = 0;
    current->mmap_next   = 0;
    current->exit_status = 0;
    current->entry       = 0;
    current->user_rsp    = 0;
    current->cwd[0] = '/';
    current->cwd[1] = '\0';
    current->personality   = PERSONALITY_LINUX;
    current->shares_space  = 0;
    current->vfork_waiter  = 0;
    current->waiting_for_child = 0;
    current->blocked_on        = NULL;
    current->wake_tick         = 0;
    current->sig_pending   = 0;
    current->sig_blocked   = 0;
    /* The alternate stack is an address in an address space that is being
     * replaced. Carrying it across would hand the next program's handler a
     * pointer into the previous program's image. */
    current->sigalt_sp     = 0;
    current->sigalt_size   = 0;
    current->sigalt_on     = 0;
    current->pgid          = current->pid;   /* process 1 leads its own group */

    /* Point the shared-state pointers at this entry's own storage, BEFORE
     * anything below writes through them. Process 1 is a thread group of one
     * and owns everything in it. */
    current->handles      = current->handle_store;
    current->sig_handlers = current->sighand_store;
    current->owns_files   = 1;
    current->owns_sighand = 1;
    current->tgid         = current->pid;
    current->clear_child_tid = 0;
    current->run_ticks       = 0;
    current->sleep_ticks     = 0;
    current->cpu_ticks       = 0;
    {
        int k;
        for (k = 0; k < SIG_COUNT; k++) {
            current->sig_handlers[k].handler  = SIG_DFL_HANDLER;
            current->sig_handlers[k].flags    = 0;
            current->sig_handlers[k].restorer = 0;
            current->sig_handlers[k].mask     = 0;
        }
    }
    current->saved_user_rsp = 0;
    current->thread.tid  = current->pid;
    current->is_kthread  = 0;
    current->kentry      = NULL;
    current->karg        = NULL;
    current->kname       = NULL;

    /* Process 1 keeps the boot stack rather than being given a fresh one.
     * It is already executing on that stack - switching underneath itself
     * would discard the frames that got it here. It picks up an allocated
     * stack the first time it execs. `boot_stack_top` is recorded so the
     * switch path has something to restore. */
    current->thread.kstack_top  = boot_stack_top;
    current->thread.kstack_base = 0;   /* static; no guard page below it */
    current->thread.fs_base     = 0;   /* no TLS until libc asks for one */
    current->thread.gs_base     = 0;   /* no TEB until a PE image asks       */
    fpu_thread_init(current->thread.fpu_state);

    /* Descriptors 0, 1 and 2 on the console.
     *
     * Three separate open instances, not one shared three ways: a shell that
     * redirects stdout must not move stdin's position with it. They happen to
     * reference the same object, which is correct - one console, three views
     * of it.
     *
     * The preassignment is a POSIX convention and belongs to that
     * personality. A Windows process gets no preassigned handles and reaches
     * the console through the namespace instead. */
    handle_table_init(current->handles);
    {
        object_t *con = tty_console();
        handle_alloc(current->handles, of_open(con, ACCESS_READ), 0);
        handle_alloc(current->handles, of_open(con, ACCESS_WRITE), 0);
        handle_alloc(current->handles, of_open(con, ACCESS_WRITE), 0);
    }
}

process_t *proc_current(void) {
    return current;
}

void proc_set_current(process_t *p) {
    if (p != NULL) {
        current = p;
        p->state = PROC_RUNNING;
        proc_activate_stack(p);
    }
}

/* First zombie child of `p`, or NULL. */
process_t *proc_at(int index) {
    if (index < 0 || index >= MAX_PROCESSES) {
        return NULL;
    }
    return table[index].state == PROC_UNUSED ? NULL : &table[index];
}

int proc_index(const process_t *p) {
    if (p == NULL) {
        return 0;
    }
    return (int)(p - table);
}

void proc_set_current_raw(process_t *p) {
    if (p != NULL) {
        current = p;
    }
}

/* A thread of the same group is not a child, however much the table makes it
 * look like one.
 *
 * clone() records the creating thread as ppid, so a thread and a child are
 * indistinguishable by parentage alone. wait4 must not reap a thread: POSIX
 * says wait(2) reports CHILD PROCESSES, and a pthread that exits is not one.
 * A shell whose wait() returned a thread id would believe the job it was
 * waiting for had finished.
 *
 * The tgid is what separates them, and it is the only thing that does. */
static int is_thread_of(const process_t *t, const process_t *p) {
    return t->tgid == p->tgid;
}

/* A kernel thread is not a child and not a thread-group peer of anything.
 *
 * It is allocated with ppid 0 and nothing has pid 0, so today the two
 * predicates below could not match one anyway. That is a property of
 * next_pid starting at 1 rather than a guarantee, and the failure it would
 * produce is not one anybody would trace back here: a shell's wait() would
 * return, reporting that the job it was waiting for had finished, when what
 * had actually exited was the ZFS txg sync thread. Asserted rather than
 * inferred. */
static int is_kernel_thread(const process_t *t) {
    return t->is_kthread != 0;
}

process_t *proc_reap_child(process_t *p) {
    int i;

    for (i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].state == PROC_ZOMBIE && table[i].ppid == p->pid &&
            !is_kernel_thread(&table[i]) && !is_thread_of(&table[i], p)) {
            return &table[i];
        }
    }
    return NULL;
}

int proc_has_children(const process_t *p) {
    int i;

    for (i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].state != PROC_UNUSED && table[i].ppid == p->pid &&
            !is_kernel_thread(&table[i]) && !is_thread_of(&table[i], p)) {
            return 1;
        }
    }
    return 0;
}

process_t *proc_alloc(int ppid) {
    int i;

    for (i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].state == PROC_UNUSED) {
            process_t *p = &table[i];

            p->pid         = next_pid++;
            p->ppid        = ppid;
            p->state       = PROC_READY;
            p->space       = NULL;
            p->brk_base    = 0;
            p->brk_current = 0;
            p->mmap_next   = 0;
            p->exit_status = 0;
            p->entry       = 0;
            p->user_rsp    = 0;
            p->cwd[0] = '/';
            p->cwd[1] = '\0';
            p->personality   = PERSONALITY_LINUX;
            p->shares_space  = 0;
            p->vfork_waiter  = 0;
            p->waiting_for_child = 0;
            p->blocked_on    = NULL;
            p->wake_tick     = 0;
            p->sig_pending   = 0;
            p->sig_blocked   = 0;
            p->sigalt_sp     = 0;
            p->sigalt_size   = 0;
            p->sigalt_on     = 0;
            p->pgid          = 0;   /* the caller sets it; vfork inherits */

            /* --- credentials, which were never initialised here -----------
             *
             * proc_alloc set nineteen fields and not these four. It looked
             * harmless because `table` is static, so a slot handed out for
             * the first time is zero - but a REUSED slot is not, and it kept
             * whatever the dead process had. A process that called setuid,
             * exited, and had its slot recycled handed its uid to an
             * unrelated new process.
             *
             * That was unreachable as a security bug only because nothing in
             * the kernel consulted uid or gid for anything; every access
             * check said yes. It stops being unreachable the moment one says
             * no, which is the point of the ACL work these fields now feed.
             *
             * Zero means root, and that is deliberate rather than a leftover
             * of the bug above: a process created by the kernel IS the
             * system, which is the same reason init is uid 0 on Unix. A
             * process gets a lesser identity by being given one. */
            p->uid           = 0;
            p->gid           = 0;
            p->sid           = 0;
            p->ngroups       = 0;
            /* Zero, not 022. 022 is the conventional default and is what
             * this should become, but nothing creates a file with a mode
             * yet, so changing it now would alter no behaviour except to
             * make the first thing that does behave differently from every
             * test written against today. */
            p->umask         = 0;
            {
                int g;
                for (g = 0; g < CRED_NGROUPS; g++) {
                    p->groups[g] = 0;
                }
            }

            /* Owning its own storage is the DEFAULT, and clone() redirects
             * these afterwards for a thread. That direction round matters:
             * a slot that came back from proc_alloc still pointing at a dead
             * process's tables is a use-after-free that looks like a working
             * process. */
            p->handles       = p->handle_store;
            p->sig_handlers  = p->sighand_store;
            p->owns_files    = 1;
            p->owns_sighand  = 1;
            p->tgid          = p->pid;
            p->clear_child_tid = 0;
            p->run_ticks       = 0;
            p->sleep_ticks     = 0;
            p->cpu_ticks       = 0;
            {
                int k;
                for (k = 0; k < SIG_COUNT; k++) {
                    p->sig_handlers[k].handler  = SIG_DFL_HANDLER;
                    p->sig_handlers[k].flags    = 0;
                    p->sig_handlers[k].restorer = 0;
                    p->sig_handlers[k].mask     = 0;
                }
            }
            p->saved_user_rsp = 0;
            p->is_kthread         = 0;
            p->kentry             = NULL;
            p->karg               = NULL;
            p->kname              = NULL;
            p->thread.tid         = p->pid;
            /* Allocate the stack here rather than at first use: a thread
             * that cannot get one is a thread that cannot take a syscall, and
             * failing at creation is far easier to handle than failing on the
             * first entry into the kernel. */
            p->thread.kstack_top = kstack_alloc(slot_of(p));
            if (p->thread.kstack_top == 0) {
                p->state = PROC_UNUSED;
                return NULL;
            }
            p->thread.kstack_base = kstack_base_of(slot_of(p));
            p->thread.saved_rsp   = 0;
            p->thread.fs_base     = 0;
            p->thread.gs_base     = 0;
            fpu_thread_init(p->thread.fpu_state);
            handle_table_init(p->handles);
            return p;
        }
    }
    return NULL;
}

/* Retire a process that is dying somewhere it cannot unwind from - a signal's
 * default action, a bad signal frame, an unrecoverable fault.
 *
 * Everything here used to be spread across three call sites in two files and
 * done differently by each, which is how a process could end up a zombie that
 * still held every descriptor it had opened, or a zombie that was still on
 * the run queue. sys_exit does not use this: it has a frame to rewrite and a
 * vfork parent it can genuinely resume, which is a different job.
 */
void proc_retire(process_t *p, int exit_status) {
    process_t *parent;

    if (p == NULL || p->state == PROC_ZOMBIE || p->state == PROC_UNUSED) {
        return;
    }
    p->exit_status = exit_status;
    p->state       = PROC_ZOMBIE;

    /* Off any wait queue, before anything else.
     *
     * A process can reach here while it is still listed as waiting on a pipe
     * or on the keyboard - killed by a signal whose default action is to
     * terminate, or by a fault it could not survive - and those paths do not
     * unwind through waitq_wait, which is what would otherwise have removed
     * it. What is left behind is a pointer to a retired slot in a waiters[]
     * array that waitq_wake_all will walk later.
     *
     * Done first because everything below can schedule, and the window where
     * a zombie is still on a queue is exactly the window something can wake
     * it. */
    waitq_leave(p);

    /* The status is still wanted, so the slot stays - but nothing holding a
     * resource should wait for a parent that may never call wait4.
     *
     * Only if this entry OWNS the table. A thread points at its leader's, and
     * closing it here would shut every descriptor of every other thread in
     * the group - the file another thread is halfway through reading, and
     * the pipe the group's output is going down. Nothing about that failure
     * points back to the thread that exited. */
    if (p->owns_files) {
        handle_close_all(p->handles);
    }

    parent = proc_find(p->ppid);

    /* A vfork parent is suspended in a way only its child's clean syscall
     * exit can undo, and a child dying here has no clean syscall exit coming.
     * It also shares the child's stack and memory, so nothing about it is
     * trustworthy afterwards. It dies too, and says so - the alternative is a
     * parent blocked forever with no indication why. */
    if (parent != NULL && p->vfork_waiter == parent->pid) {
        print_string("[process ", 0x0E);
        print_hex((uint32)parent->pid, 0x0E);
        print_string(" died with its vfork child]\n", 0x0E);
        parent->exit_status = exit_status;
        parent->state       = PROC_ZOMBIE;
        /* The vfork parent becomes a zombie by a different route than the
         * child did, so it needs the same removal. It is suspended rather
         * than blocked on a queue today - but "today" is what this whole
         * class of bug is made of, and the call costs a null check. */
        waitq_leave(parent);
        if (parent->owns_files) {
            handle_close_all(parent->handles);
        }
        sched_dequeue(parent);
        p->vfork_waiter = 0;
        parent = NULL;
    }
    if (parent != NULL) {
        signal_send(parent, SIGCHLD);
        if (parent->waiting_for_child) {
            sched_wake(parent);
        }
    }

    sched_dequeue(p);
}

void proc_free(process_t *p) {
    if (p == NULL || p == current) {
        return;
    }
    /* proc_retire already did this for anything that died normally. Repeated
     * here because this is the function that hands the slot back for reuse,
     * and it is reachable for a process that never went through retire at
     * all - proc_alloc's own failure path unwinds through it. A stale waiter
     * matters precisely when the slot is recycled, so the last function to
     * touch the slot should be the one that guarantees it. */
    waitq_leave(p);
    if (p->owns_files) {
        handle_close_all(p->handles);
    }
    /* The pointers go back to this slot's own storage whatever they were
     * pointing at, because the next proc_alloc to hand this slot out must not
     * find it aimed at another process's tables. proc_alloc sets them too;
     * both, because a slot can be reused without passing through either one
     * of them being obviously the last writer. */
    p->handles      = p->handle_store;
    p->sig_handlers = p->sighand_store;
    p->owns_files   = 1;
    p->owns_sighand = 1;
    /* Never destroy a borrowed address space - a vfork child's `space` is
     * its parent's, and freeing it here would unmap the parent's memory. */
    if (p->shares_space) {
        p->space = NULL;
    }
    if (p->thread.kstack_base != 0) {
        kstack_free(slot_of(p));
    }
    p->thread.kstack_top  = 0;
    p->thread.kstack_base = 0;
    if (p->space != NULL && p->space != vmm_kernel_space()) {
        vmm_space_destroy(p->space);
    }
    p->space = NULL;
    /* Cleared with the rest of the slot, not only by kthread_reap.
     *
     * proc_free is the function that hands a slot back for reuse, and it is
     * reachable for a kernel thread that never went through kthread_exit -
     * kthread_create's own failure path unwinds through it. A flag left set
     * on a recycled slot would make an ordinary user process look like a
     * kernel thread to kern_synch.c, which would then let it deschedule from
     * a context that has nothing to deschedule. */
    p->is_kthread = 0;
    p->kentry     = NULL;
    p->karg       = NULL;
    p->kname      = NULL;

    p->state = PROC_UNUSED;
    p->pid   = 0;
}

/* -1: nobody holds the supreme privilege. The default, and what a boot that
 * never calls PR_GENESIS_GRANT_SUPREME leaves in place - this kernel does not
 * start with a second root. */
static int g_supreme_uid = -1;

int genesis_supreme_uid_get(void) {
    return g_supreme_uid;
}

void genesis_supreme_uid_set(int uid) {
    if (uid < 0) {
        kprintf("supreme: revoked (was uid %d)\n", g_supreme_uid);
        g_supreme_uid = -1;
        return;
    }
    kprintf("supreme: granted to uid %d (was %d)\n", uid, g_supreme_uid);
    g_supreme_uid = uid;
}

void proc_cred(const process_t *p, cred_t *out) {
    uint32 i;

    if (out == NULL) {
        return;
    }
    if (p == NULL) {
        /* No process is not root. cred_init_nobody, not a zeroed struct -
         * a zeroed cred_t has euid 0 and would pass every check. */
        cred_init_nobody(out);
        return;
    }
    out->uid  = p->uid;
    out->euid = p->uid;
    out->gid  = p->gid;
    out->egid = p->gid;
    out->ngroups = p->ngroups > CRED_NGROUPS ? CRED_NGROUPS : p->ngroups;
    for (i = 0; i < CRED_NGROUPS; i++) {
        out->groups[i] = (i < out->ngroups) ? p->groups[i] : 0;
    }
    /* Computed, not stored on process_t - the same reasoning this function's
     * own header comment gives for assembling the rest of cred_t on demand
     * rather than caching it: a per-process copy of "am I supreme" would be
     * a second place that fact could go stale the moment the grant changes
     * out from under a process that outlives the change. */
    out->supreme = (g_supreme_uid >= 0 && (uint32)g_supreme_uid == p->uid) ? 1u : 0u;
}

process_t *proc_find(int pid) {
    int i;

    for (i = 0; i < MAX_PROCESSES; i++) {
        if (table[i].state != PROC_UNUSED && table[i].pid == pid) {
            return &table[i];
        }
    }
    return NULL;
}
