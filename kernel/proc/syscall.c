#include "syscall.h"
#include "io.h"
#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "disk.h"
#include "pe.h"
#include "personality.h"
#include "nt.h"
#include "teb.h"
#include "pipe.h"
#include "elf.h"
#include "fileobj.h"
#include "fs.h"
#include "kheap.h"
#include "keyboard.h"
#include "path.h"
#include "ns.h"
#include "object.h"
#include "eventfd.h"
#include "socketfd.h"
#include "socketpair.h"
#include "process.h"
#include "timer.h"
#include "sched.h"
#include "signal.h"
#include "tty.h"
#include "waitq.h"
#include "futex.h"
#include "paging.h"
#include "pmm.h"
#include "ksmp.h"
#include "bkl.h"
#include "kprintf.h"
#include "nt_context.h"
#include "screen.h"
#include "typesk.h"

/* The per-CPU block lives in ksmp.h - one structure with one set of offsets.
 * The offsets are load-bearing: the assembly below hardcodes gs:0 and gs:8.
 *
 * Every accessor here is for the EXECUTING CPU. Each CPU runs its own
 * thread, so "the kernel stack the SYSCALL stub switches to" and "where the
 * user's RSP is parked" are that CPU's, and a context switch on one CPU must
 * not touch another's. */
#define thiscpu (*smp_this_cpu())

extern void syscall_entry(void);

/* SYSCALL does not switch stacks the way an interrupt does - there is no
 * TSS.rsp0 equivalent for it, only whatever the entry stub loads out of GS.
 * So a context switch has to update BOTH this and gdt_set_kernel_stack, or
 * half the entry paths land on the outgoing thread's stack. */
void syscall_set_kernel_stack(uint64 rsp) {
    thiscpu.kernel_rsp = rsp;
}

/* Where the entry stub will restore RSP from on the way out.
 *
 * This is how execve leaves the kernel somewhere other than where it came in.
 * The stub already ends with `movq %gs:8, %rsp; sysretq`, and sysret takes
 * its RIP from rcx - which is the frame's `rip` field. Rewrite both and the
 * ordinary return path lands in the new image, with no second exit route to
 * write or keep correct. */
void syscall_set_user_rsp(uint64 rsp) {
    thiscpu.user_rsp = rsp;
}

uint64 syscall_get_user_rsp(void) {
    return thiscpu.user_rsp;
}

/* The user's GS base, which is not simply "the GS base MSR".
 *
 * GS has two: IA32_GS_BASE and IA32_KERNEL_GS_BASE, and swapgs exchanges
 * them. The convention here puts the per-CPU block in KERNEL_GS_BASE while in
 * ring 3 and in GS_BASE while in the kernel - so the USER's value lives in
 * whichever one the block is not in, and that depends on which side of a
 * swapgs the CPU is standing.
 *
 * Both states are real. A context switch runs in the kernel, after swapgs, so
 * the user's value is in KERNEL_GS_BASE. But proc_set_current also runs at
 * boot, before anything has ever entered ring 3 and therefore before any
 * swapgs, when the two are still the other way round. Writing the wrong one
 * overwrites the per-CPU pointer, and the machine then dies inside
 * syscall_entry on the next system call with nothing to suggest why - which
 * is the same failure the interrupt path's missing swapgs produced, and it
 * took a disassembly to find once already.
 *
 * That ambiguity is gone now: smp_early_init puts the per-CPU block in
 * GS_BASE before anything else runs, and the first entry to ring 3
 * (enter_user_mode) swaps like every later one - so while the kernel runs,
 * on any CPU, GS_BASE is the block and the user's value is in
 * KERNEL_GS_BASE. It had to be made unconditional: an AP has no "before the
 * first swapgs" phase, and smp_this_cpu() reads the block through GS. The
 * test is kept as a check, because a GS_BASE that is NOT the block here is
 * a machine about to fault in the next syscall_entry, and saying so is
 * worth one MSR read. */
void syscall_set_user_gs_base(uint64 base) {
    if (rdmsr(MSR_GS_BASE) != (uint64)smp_this_cpu()->self) {
        kprintf_c(0x0C, "syscall: GS_BASE is not this CPU's block\n");
    }
    wrmsr(MSR_KERNEL_GS_BASE, base);
}

uint64 syscall_get_user_gs_base(void) {
    return rdmsr(MSR_KERNEL_GS_BASE);
}

/* Lay out a kernel stack so that switch_context's tail lands in
 * syscall_return with a user context ready to pop. From the top down:
 *
 *   [ syscall_frame        ]  <- RSP when syscall_return begins
 *   [ &syscall_return      ]  <- the `ret` in switch_context jumps here
 *   [ RFLAGS               ]  <- popped by switch_context's popfq
 *   [ six callee-saved     ]  <- popped by switch_context, values irrelevant
 *   ^ the returned RSP
 *
 * The zeroed callee-saved slots are why a new thread starts with rbx, rbp and
 * r12-r15 clear rather than holding whatever the previous occupant left.
 *
 * The RFLAGS slot is 0x002 - bit 1, which is architecturally always set, and
 * nothing else. IF CLEAR, deliberately: this stack resumes in
 * syscall_return, which is the tail of a syscall, and a syscall runs with
 * interrupts disabled because MSR_SFMASK clears IF on the way in. Starting a
 * forked child with IF set would give it a different interrupt state from
 * the parent it is a copy of, on the same code path. The user's own RFLAGS
 * are restored separately, from the frame's r11, by the SYSRET that ends
 * syscall_return - so this value never reaches ring 3. */
uint64 thread_bootstrap_stack(uint64 kstack_top, const struct syscall_frame *frame) {
    uint64 sp = kstack_top;
    struct syscall_frame *slot;
    int i;

    sp -= sizeof(struct syscall_frame);
    slot = (struct syscall_frame *)sp;
    *slot = *frame;

    sp -= 8;
    *(uint64 *)sp = (uint64)&syscall_return;

    sp -= 8;
    *(uint64 *)sp = 0x002;

    for (i = 0; i < 6; i++) {
        sp -= 8;
        *(uint64 *)sp = 0;
    }
    return sp;
}

/* The SYSCALL machinery's MSRs, which are PER CPU: an AP that never had
 * these written takes #UD on a user thread's first syscall instruction. */
static void syscall_msrs(void) {
    uint64 star;

    /* See the header for why these two constants are what they are. */
    star = ((uint64)0x0008u << 32)    /* SYSCALL: CS 0x08, SS 0x10 */
         | ((uint64)0x0013u << 48);   /* SYSRET : CS 0x23, SS 0x1B */
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_LSTAR, (uint64)syscall_entry);

    /* RFLAGS bits cleared on entry. IF matters most: without it, interrupts
     * are still enabled during the window where RSP is a user-controlled
     * value, and an interrupt there would push onto the user's stack. */
    wrmsr(MSR_SFMASK, 0x200u | 0x100u | 0x40000u);  /* IF | TF | AC */

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void syscall_init(uint64 kernel_stack_top) {
    thiscpu.kernel_rsp = kernel_stack_top;
    thiscpu.user_rsp   = 0;

    /* swapgs exchanges GS_BASE and KERNEL_GS_BASE. The convention: while the
     * KERNEL runs, GS_BASE holds this CPU's block (smp_early_init set it);
     * while ring 3 runs, the user's value is there and the block is in
     * KERNEL_GS_BASE. Every door between the two swaps - syscall_entry and
     * syscall_return, isr_common in both directions, and enter_user_mode on
     * the very first descent. */
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    syscall_msrs();
}

void syscall_init_ap(void) {
    syscall_msrs();
}

/* --- entry stub ---------------------------------------------------------
 * Written in assembly because none of the first four instructions can be
 * expressed in C: they run before there is a usable stack. */
__asm__(
".text\n"
".global syscall_entry\n"
".align 16\n"
"syscall_entry:\n"
"    swapgs\n"                    /* GS_BASE now points at cpu_local       */
"    movq %rsp, %gs:8\n"          /* park the user's RSP                   */
"    movq %gs:0, %rsp\n"          /* adopt the kernel stack                */

/* Callee-saved registers first, so they land at the high end of the frame and
 * the argument registers keep the offsets everything already uses. The kernel
 * would preserve these anyway; they are saved because resuming a process that
 * was suspended while another ran needs them, and the syscall path is the
 * only place a process is ever suspended. */
"    pushq %r15\n"
"    pushq %r14\n"
"    pushq %r13\n"
"    pushq %r12\n"
"    pushq %rbp\n"
"    pushq %rbx\n"

/* rcx and r11 must be saved before anything else touches them: SYSCALL puts
 * the return address in rcx and the caller's RFLAGS in r11, and the argument
 * shuffle below overwrites rcx. */
"    pushq %r11\n"
"    pushq %rcx\n"
"    pushq %r9\n"
"    pushq %r8\n"
"    pushq %r10\n"
"    pushq %rdx\n"
"    pushq %rsi\n"
"    pushq %rdi\n"
"    pushq %rax\n"

"    movq %rsp, %rdi\n"           /* the frame is the argument             */
/* Fifteen pushes is 120 bytes, so RSP is 8 mod 16 here and the ABI wants 0.
 * Correcting it matters even with -mno-sse: the compiler is entitled to
 * assume alignment for anything it decides to vectorise. */
"    subq $8, %rsp\n"
"    call syscall_dispatch\n"
"    addq $8, %rsp\n"
"    movq %rax, (%rsp)\n"         /* return value overwrites the rax slot  */

/* A thread that has never run cannot arrive here by returning from
 * syscall_dispatch - it was never called. It arrives by having its kernel
 * stack fabricated so that the context switch's `ret` lands exactly here,
 * with RSP pointing at a syscall_frame someone filled in. That makes this
 * label the single exit to user mode, used by both paths, rather than a
 * second copy of the pops that would have to stay in step with these. */
"    .globl syscall_return\n"
"syscall_return:\n"

/* The exit to ring 3 is where the big kernel lock is released - here rather
 * than at the end of syscall_dispatch because a NEW thread never ran
 * syscall_dispatch: it arrives at this label straight out of the context
 * switch, holding the lock the switching CPU held. Interrupts off first, so
 * nothing can enter the kernel on this CPU between the release and the
 * sysret with the user's GS half-restored. The eight bytes keep the call
 * 16-byte aligned (RSP is 8 mod 16 at the frame). Every register the call
 * may clobber is popped from the frame below. */
"    cli\n"
"    subq $8, %rsp\n"
"    call bkl_exit_to_user\n"
"    addq $8, %rsp\n"

"    popq %rax\n"
"    popq %rdi\n"
"    popq %rsi\n"
"    popq %rdx\n"
"    popq %r10\n"
"    popq %r8\n"
"    popq %r9\n"
"    popq %rcx\n"                 /* user RIP  -> sysretq uses rcx         */
"    popq %r11\n"                 /* user RFLAGS -> sysretq uses r11       */

/* Popped last because they were pushed first. A process resuming after
 * another one ran gets these back from its saved frame rather than from the
 * CPU, which is the entire reason they are here. */
"    popq %rbx\n"
"    popq %rbp\n"
"    popq %r12\n"
"    popq %r13\n"
"    popq %r14\n"
"    popq %r15\n"
"    movq %gs:8, %rsp\n"          /* back to the user stack                */
"    swapgs\n"
"    sysretq\n"
);

/* --- user stack --------------------------------------------------------- */

static uint64 str_len(const char *s) {
    uint64 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* Push a NUL-terminated string onto the descending stack, returning where it
 * landed. Strings live above the vectors so the vectors can point up at them. */
static uint64 push_string(uint64 *sp, const char *s) {
    uint64 len = str_len(s);
    uint64 i;

    *sp -= (len + 1);
    for (i = 0; i <= len; i++) {
        ((char *)*sp)[i] = s[i];
    }
    return *sp;
}

uint64 user_stack_create(uint64 top, uint64 size,
                         const char *const *argv,
                         const char *const *envp,
                         uint64 phdr, uint64 phnum, uint64 phentsize,
                         uint64 entry, uint64 interp_base) {
    uint64 base = (top - size) & ~0xFFFULL;
    uint64 page, sp, random_ptr;
    uint64 argc = 0, envc = 0, words, i;
    uint64 argp[USER_MAX_ARGV];
    uint64 envq[USER_MAX_ENVP];
    uint64 *w;

    for (page = base; page < top; page += PMM_PAGE_SIZE) {
        if (vmm_get_phys(page) != 0) {
            continue;
        }
        /* PAGE_NX: a stack is never executable. There is nothing to weigh
         * here - no personality in this kernel puts code on the stack. The
         * signal path is the usual exception and is not one: signal.h
         * REQUIRES SA_RESTORER, so the trampoline a handler returns through
         * lives in the caller's own text, not in a frame the kernel wrote.
         * A kernel-provided trampoline would have had to go on the stack,
         * and this line would then be the thing that broke every signal. */
        if (vmm_alloc_page(page, PAGE_PRESENT | PAGE_RW | PAGE_USER |
                                 PAGE_NX) == 0) {
            return 0;
        }
    }

    while (argv != NULL && argv[argc] != NULL) {
        argc++;
    }
    while (envp != NULL && envp[envc] != NULL) {
        envc++;
    }
    if (argc > USER_MAX_ARGV || envc > USER_MAX_ENVP) {
        return 0;
    }

    /* The frame is built BELOW `top`, never at it: `top` is the first
     * unmapped address, so an RSP of exactly `top` faults on the first read.
     *
     * The extra headroom is not padding. A binary with a PT_TLS segment - which
     * ash has and the earlier hello.c did not - makes musl allocate its thread
     * control block on the stack ABOVE the start frame, before arch_prctl is
     * ever called. Start the frame at `top` and that allocation runs straight
     * off the end of the mapping. There is no syscall to catch it on, which is
     * why the fault arrives with no trace output at all. */
    sp = top - 0x2000;

    sp -= 16;
    random_ptr = sp;
    for (i = 0; i < 16; i++) {
        ((uint8 *)random_ptr)[i] = (uint8)(0xA5 ^ (i * 17));
    }

    /* Strings in reverse, so that after the descending pushes argp[0] holds
     * the lowest address and the array reads in order. */
    for (i = argc; i > 0; i--) {
        argp[i - 1] = push_string(&sp, argv[i - 1]);
    }
    for (i = envc; i > 0; i--) {
        envq[i - 1] = push_string(&sp, envp[i - 1]);
    }

    /* argc, the argv pointers plus terminator, the envp pointers plus
     * terminator, then 13 auxv pairs. Aligning DOWN after subtracting leaves
     * a small unused gap above the frame and guarantees RSP is 16-aligned,
     * which the ABI requires and which changes with every extra argument. */
    words = 1 + argc + 1 + envc + 1 + 2 * 14;
    sp = (sp - words * 8) & ~15ULL;

    w = (uint64 *)sp;
    *w++ = argc;
    for (i = 0; i < argc; i++) {
        *w++ = argp[i];
    }
    *w++ = 0;                       /* argv terminator */
    for (i = 0; i < envc; i++) {
        *w++ = envq[i];
    }
    *w++ = 0;                       /* envp terminator */

    *w++ = AT_PHDR;    *w++ = phdr;
    *w++ = AT_PHENT;   *w++ = phentsize;
    *w++ = AT_PHNUM;   *w++ = phnum;
    *w++ = AT_PAGESZ;  *w++ = PMM_PAGE_SIZE;
    /* Always emitted, zero when there is no interpreter. An rtld reads this
     * to relocate itself before it can call anything through its own GOT, and
     * emitting the tag only sometimes would change the auxv LAYOUT between a
     * static and a dynamic exec - which is exactly the kind of difference
     * that makes one of the two work and the other fail in a way that looks
     * like a libc bug. */
    *w++ = AT_BASE;    *w++ = interp_base;
    /* The PROGRAM's entry, never the interpreter's. This is what the rtld
     * jumps to once it has resolved everything; hand it the rtld's own entry
     * and the linker re-enters itself and the program never runs. */
    *w++ = AT_ENTRY;   *w++ = entry;
    *w++ = AT_UID;     *w++ = 0;
    *w++ = AT_EUID;    *w++ = 0;
    *w++ = AT_GID;     *w++ = 0;
    *w++ = AT_EGID;    *w++ = 0;
    *w++ = AT_SECURE;  *w++ = 0;
    *w++ = AT_CLKTCK;  *w++ = 100;
    *w++ = AT_RANDOM;  *w++ = random_ptr;
    *w++ = AT_NULL;    *w++ = 0;

    return sp;
}

/* --- ring 3 ------------------------------------------------------------- */

void enter_user_mode(uint64 entry, uint64 stack) {
    /* The first descent into ring 3 is an exit from the kernel like any
     * other: the big kernel lock goes (the boot path held it since
     * smp_early_init), and GS swaps so the user's base is live and this
     * CPU's block is parked in KERNEL_GS_BASE for the next entry. Interrupts
     * off across both - an interrupt taken between the swap and the iretq
     * would enter from "kernel mode" with the user's GS. */
    __asm__ volatile ("cli");
    bkl_exit_to_user();
    /* There is no instruction that simply "returns to ring 3" - the only way
     * down is to make the CPU believe it is returning from an interrupt that
     * came FROM ring 3. So we fabricate the exact frame iretq expects and
     * execute it. The selectors carry RPL 3, and RFLAGS 0x202 is bit 1 (which
     * is always set) plus IF. */
    __asm__ volatile (
        "pushq %0\n\t"       /* SS     */
        "pushq %1\n\t"       /* RSP    */
        "pushq $0x202\n\t"   /* RFLAGS */
        "pushq %2\n\t"       /* CS     */
        "pushq %3\n\t"       /* RIP    */
        "swapgs\n\t"
        "iretq\n\t"
        :
        : "i"((uint64)USER_SS), "r"(stack), "i"((uint64)USER_CS), "r"(entry)
        : "memory"
    );

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* --- dispatch ----------------------------------------------------------- */


/* --- descriptor I/O ------------------------------------------------------
 * These three no longer know what a console is. They look up a handle, check
 * the access mode, and call through the object's type. Pointing descriptor 1
 * at a file later changes nothing here - which is the entire reason the
 * object layer exists rather than a switch on fd. */

static int64 do_read(int fd, void *buf, uint64 count) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, fd);

    if (f == NULL) {
        return -9;                    /* -EBADF */
    }
    if (!(f->access & ACCESS_READ)) {
        return -9;
    }
    if (f->obj == NULL || f->obj->type == NULL || f->obj->type->read == NULL) {
        /* A directory is a specific case with its own errno, and a libc uses
         * it to tell "you opened a directory" apart from "this object cannot
         * be read at all". A FAT directory already answers -EISDIR from its
         * own read; a namespace directory has no read to answer with. */
        if (ob_is_directory(f->obj)) {
            return -21;               /* -EISDIR */
        }
        return -22;                   /* -EINVAL: this type cannot be read */
    }
    /* O_NONBLOCK, and it is the readiness predicate poll added that makes
     * this possible rather than a promise. Before that, every object could
     * only answer "block until a read would succeed", so a non-blocking read
     * had nothing to test and the flag was refused by F_SETFL.
     *
     * Asked through ob_poll rather than by calling read and hoping it
     * returns: the whole point is not to start an operation that would then
     * have to be undone. A pipe whose writer has gone reports POLLIN along
     * with POLLHUP, so end of file still arrives as a zero-length read and
     * not as -EAGAIN - which would be a program spinning on a pipe that will
     * never have data again. */
    if ((f->status & O_NONBLOCK) && !(ob_poll(f->obj, OB_POLLIN) & OB_POLLIN)) {
        return -11;                   /* -EAGAIN */
    }
    return f->obj->type->read(f->obj, buf, count, &f->offset);
}

static int64 do_write(int fd, const void *buf, uint64 count) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, fd);

    if (f == NULL) {
        return -9;
    }
    if (!(f->access & ACCESS_WRITE)) {
        return -9;
    }
    if (f->obj == NULL || f->obj->type == NULL || f->obj->type->write == NULL) {
        return -22;
    }
    /* The same test on the other side. A pipe with no readers left reports
     * POLLOUT alongside POLLERR, so a non-blocking write to it still reaches
     * pipe_write and still gets SIGPIPE and -EPIPE - the error a program
     * needs, rather than -EAGAIN, which would tell it to try again forever. */
    if ((f->status & O_NONBLOCK) && !(ob_poll(f->obj, OB_POLLOUT) & OB_POLLOUT)) {
        return -11;                   /* -EAGAIN */
    }
    /* O_APPEND: seek to the end IMMEDIATELY before the write, every time,
     * rather than once at open. That is the whole content of the flag - two
     * processes appending to one file must not overwrite each other, which
     * they would if each remembered where the end was when it opened.
     *
     * Atomic here because the kernel is non-preemptive: nothing else runs
     * between this line and the write below. On a preemptible kernel the seek
     * and the write would have to be one operation, and this is the line that
     * would have to move down into the filesystem.
     *
     * Guarded by fileobj_is_file, not applied blindly: fileobj_size reads
     * obj->body as an fs_node_t, and a console's body is not one. A pipe has
     * no end to seek to, and POSIX asks for nothing there. */
    if ((f->status & O_APPEND) && fileobj_is_file(f->obj)) {
        f->offset = fileobj_size(f->obj);
    }
    return f->obj->type->write(f->obj, buf, count, &f->offset);
}

static uint64 sys_write(uint64 fd, const char *buf, uint64 count) {
    if (count == 0) {
        return 0;
    }
    if (!user_ptr_ok((uint64)buf)) {
        return (uint64)-14;           /* -EFAULT */
    }
    return (uint64)do_write((int)fd, buf, count);
}

/* --- reading from the console -------------------------------------------
 * fd 0 only. The line discipline is in keyboard.c; this is the syscall shape
 * around it.
 *
 * This call BLOCKS, and with one process and no scheduler that means the whole
 * kernel sits in a halt loop until a key arrives. That is acceptable exactly
 * as long as there is nothing else to run, and it is the first place a
 * scheduler will be needed rather than merely nice to have. */
static void fill(uint8 *dst, uint8 v, uint64 n);

static uint64 sys_read(uint64 fd, uint64 buf, uint64 count) {
    if (count == 0) {
        return 0;
    }
    if (!user_ptr_ok(buf)) {
        return (uint64)-14;
    }
    if (count > (1u << 20)) {
        count = 1u << 20;
    }
    {
        int64 rc = do_read((int)fd, (void *)buf, count);

        /* A read woken by a signal rather than by data returns -EINTR, and
         * the signal is delivered on the way out of this syscall. Reporting
         * 0 instead would look like end of input and make a shell exit on
         * Ctrl-C. */
        if (rc == 0 && signal_pending(proc_current())) {
            return (uint64)-4;               /* -EINTR */
        }
        return (uint64)rc;
    }
}

/* --- opening files -------------------------------------------------------
 * The payoff for the object layer. Nothing in read(), write() or close()
 * changes to support this - a descriptor from here works because those three
 * never knew what they were talking to. */

/* "/dev" exactly, or anything under it. Matched here rather than by a prefix
 * compare at the call site so that "/development" is not mistaken for a
 * device directory - the character after the prefix has to be a separator or
 * the end of the string. */
static int is_dev_path(const char *path) {
    if (!(path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
          path[3] == 'v')) {
        return 0;
    }
    return path[4] == '\0' || path[4] == '/';
}

/* The rewrite from a POSIX /dev path to a namespace path used to live here,
 * and a second copy of it lived in sys_stat_path. Both are gone: dev_lookup()
 * in devices.c owns it now, because "which directories is /dev a view onto"
 * is one policy and two implementations of it drift - which is precisely how
 * a device could be openable and not listable at the same time. */

static uint64 sys_openat(uint64 dirfd, uint64 path_ptr, uint64 flags, uint64 mode) {
    process_t *p = proc_current();
    char resolved[PATH_MAX_LEN];
    object_t *obj;
    open_file_t *of;
    uint32 access;
    uint32 hflags = 0;
    int err = 0;
    int fd;

    /* Only AT_FDCWD is supported: resolving against an arbitrary directory
     * descriptor needs the path of the directory that fd names, and an open
     * file object stores an entry rather than a path. Worth doing when
     * something asks; nothing does yet. */
    if ((int64)dirfd != AT_FDCWD) {
        return (uint64)-22;                  /* -EINVAL */
    }
    if (!user_ptr_ok(path_ptr)) {
        return (uint64)-14;
    }
    if (path_normalize(p->cwd, (const char *)path_ptr,
                       resolved, sizeof(resolved)) != PATH_OK) {
        return (uint64)-36;
    }

    switch (flags & 3) {
        case O_WRONLY: access = ACCESS_WRITE; break;
        case O_RDWR:   access = ACCESS_READ | ACCESS_WRITE; break;
        default:       access = ACCESS_READ; break;
    }
    /* O_CREAT, O_TRUNC and O_APPEND used to be refused here with -EROFS,
     * because nothing could modify the volume. All three are handled now -
     * O_CREAT below, before the object is opened; O_TRUNC and O_APPEND after
     * it - and the -EROFS they used to get comes from the filesystem's own
     * vtable instead, on a volume that genuinely has no write path.
     *
     * O_TRUNC without write access is refused rather than ignored. POSIX
     * leaves it undefined; emptying a file that was opened read-only is the
     * one interpretation nobody wants, and silently ignoring the flag means a
     * program that asked for an empty file gets an old one. */
    if ((flags & O_TRUNC) && !(access & ACCESS_WRITE)) {
        return (uint64)-22;                  /* -EINVAL */
    }

    /* /dev is not on the volume and never will be. It is the POSIX spelling
     * of the object namespace's DOS-device directory, so the lookup goes
     * there instead of to the filesystem - one set of device names, reachable
     * under whichever name the caller's personality uses.
     *
     * The remainder is required to be empty: /dev/console names a device and
     * nothing follows it. \??\C:\etc\motd, which DOES carry a remainder,
     * is exactly what the I/O manager will consume in the next phase; until
     * something can parse it, accepting it would mean silently opening the
     * whole volume when a file was asked for. */
    /* O_CREAT, before anything is opened.
     *
     * Only for filesystem paths. /dev is the object namespace, where names
     * are made by drivers registering devices and not by a process asking -
     * so O_CREAT there is not "unsupported", it is meaningless, and creating
     * a FILE called /dev/whatever on the volume would be actively wrong.
     *
     * -EEXIST from fs_create is not an error unless O_EXCL was asked for:
     * plain O_CREAT means "make it if it is missing", and the file being
     * there already is the ordinary case. */
    if ((flags & O_CREAT) && !is_dev_path(resolved)) {
        cred_t cc;
        int crc;

        proc_cred(proc_current(), &cc);
        crc = fs_create(resolved, (const struct cred *)&cc,
                        (uint32)(mode & ~p->umask));

        if (crc == -17) {                    /* -EEXIST */
            if (flags & O_EXCL) {
                return (uint64)-17;
            }
        } else if (crc != 0) {
            return (uint64)(int64)crc;
        }
    }

    if (is_dev_path(resolved)) {
        char remainder[NS_PATH_MAX];
        object_t *found = NULL;
        int  rc = dev_lookup(resolved, &found, remainder, sizeof(remainder));

        if (rc != 0) {
            return (uint64)(int64)rc;
        }
        /* A non-empty remainder used to be an unconditional -ENOTDIR, with a
         * comment saying the I/O manager would consume it in the next phase.
         * It does now: dev_open_object hands it to the device, so
         * /dev/HarddiskVolume2/bin/sh opens a file on that volume whether or
         * not it is mounted anywhere on the POSIX side.
         *
         * -ENOTDIR is still the answer for a device with no parse op, which
         * is every character device - but it comes from that NULL slot rather
         * than from this call site deciding it for every device at once. */
        rc = dev_open_object(found, remainder, access, &obj);
        if (rc != 0) {
            return (uint64)(int64)rc;
        }
    } else {
        /* --- permission, before the object exists -------------------------
         *
         * Here rather than inside fileobj_open because this is where the
         * caller's intent is still known: `access` says whether the open was
         * for reading, writing or both, and one level down that has already
         * become a flag on an object.
         *
         * The check is skipped when the lookup fails, and the open is allowed
         * to proceed and produce the real error. Answering -EACCES for a file
         * that does not exist would leak the difference between "no such
         * file" and "not yours", which is the one thing an access check on a
         * path is supposed not to do - and it would do it backwards, since
         * -ENOENT is the honest answer.
         *
         * O_CREAT has already run at this point, so a file this open just
         * created is checked against the ACL it was created with, and passes:
         * it belongs to the caller. */
        fs_node_t node;

        if (fs_lookup(resolved, &node) == 0) {
            cred_t c;
            uint32 want;

            proc_cred(p, &c);
            want = acl_mask_for_posix((access & ACCESS_READ) != 0,
                                      (access & ACCESS_WRITE) != 0,
                                      0, node.is_dir);
            if (want != 0) {
                int arc = fs_access(&node, (const struct cred *)&c, want);

                if (arc != 0) {
                    return (uint64)(int64)arc;
                }
            }
        }

        obj = fileobj_open(resolved, access, &err);
        if (obj == NULL) {
            return (uint64)(int64)err;
        }
    }
    /* ob_is_directory, not fileobj_is_dir. The latter answers by casting the
     * object's body to a fat_entry_t and reading its attribute byte, which is
     * meaningless for anything that is not a FAT file object - handed the
     * /dev directory object, whose body is an ns_entry_t, it read byte 24 of
     * the entry's name, found zero, and reported -ENOTDIR. opendir("/dev")
     * passes O_DIRECTORY, so `ls /dev` failed on exactly that. */
    if ((flags & O_DIRECTORY) && !ob_is_directory(obj)) {
        ob_deref(obj);
        return (uint64)-20;                  /* -ENOTDIR */
    }

    /* O_TRUNC, on the object rather than on the path: the file has just been
     * resolved, and re-resolving it by name would open a window in which the
     * name means something else. Before of_open, so a truncate that fails
     * leaves no descriptor behind pointing at a file the caller believes is
     * empty. */
    if (flags & O_TRUNC) {
        int trc = fileobj_truncate(obj, 0);

        /* -EINVAL means "not a file object" - a device or a directory - and
         * O_TRUNC on those is ignored the way POSIX ignores it for a
         * terminal, rather than failing an open that is otherwise fine. */
        if (trc != 0 && trc != -22) {
            ob_deref(obj);
            return (uint64)(int64)trc;
        }
    }

    of = of_open(obj, access);
    ob_deref(obj);            /* the open instance holds it now */
    if (of == NULL) {
        return (uint64)-23;                  /* -ENFILE */
    }
    /* O_NONBLOCK is a STATUS flag, so it lands on the open instance and is
     * shared by every descriptor dup'd from this one - which is what F_GETFL
     * through a dup has to report. O_CLOEXEC below is a per-descriptor flag
     * and lands one level up. Two flags from one word going to two different
     * levels, three lines apart, is the clearest place in the kernel to see
     * why open_file_t and handle_t are separate structs. */
    if (flags & O_NONBLOCK) {
        of->status |= O_NONBLOCK;
    }
    /* O_APPEND is a status flag like O_NONBLOCK, so it lands on the open
     * instance and is shared by every descriptor dup'd from this one. That is
     * what POSIX says and it is what makes two descriptors appending to one
     * log file interleave lines instead of overwriting each other. */
    if (flags & O_APPEND) {
        of->status |= O_APPEND;
    }
    if (flags & O_CLOEXEC) {
        hflags |= HANDLE_CLOEXEC;
    }

    fd = handle_alloc(p->handles, of, hflags);
    return (uint64)(int64)fd;
}

static uint64 sys_lseek(uint64 fd, uint64 offset, uint64 whence) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);
    int64 base;
    int64 target;

    if (f == NULL) {
        return (uint64)-9;
    }
    /* A console has no position, and neither does a pipe. -ESPIPE is what a
     * libc expects here, and it is how a program detects it is talking to a
     * stream rather than a file - the errno is named after the pipe case. */
    if (f->obj == NULL || f->obj->type == NULL ||
        f->obj->type->klass == OBJ_CONSOLE ||
        f->obj->type->klass == OBJ_PIPE) {
        return (uint64)-29;                  /* -ESPIPE */
    }

    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (int64)f->offset; break;
        case SEEK_END:
            /* Asked of the right accessor for the object's type. A block
             * device is seekable and its size comes from the device, not from
             * a directory entry - fileobj_size would read a disk object's
             * body as an fs_node_t and answer with whatever a pointer looks
             * like as a size. `dd seek=` on a raw disk is the caller that
             * finds out.
             *
             * OBJ_BLOCK is deliberately NOT in the -ESPIPE list above: a disk
             * has a position, and that is most of what makes it different
             * from every other device here. */
            base = disk_is_block(f->obj) ? (int64)disk_size(f->obj)
                                         : (int64)fileobj_size(f->obj);
            break;
        default:       return (uint64)-22;
    }
    target = base + (int64)offset;
    /* Seeking past the end is legal and read() returns nothing there.
     * Seeking before it is not. */
    if (target < 0) {
        return (uint64)-22;
    }
    f->offset = (uint64)target;
    return (uint64)target;
}

static uint64 sys_getdents64(uint64 fd, uint64 buf, uint64 count) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);

    if (f == NULL) {
        return (uint64)-9;
    }
    if (!user_ptr_ok(buf)) {
        return (uint64)-14;
    }
    /* Dispatched through the object rather than straight to the FAT reader.
     * /dev is a directory whose entries are namespace names and not on any
     * disk, and getdents64 should no more know that than read(2) knows
     * whether it is talking to a file or a console. A type with no getdents
     * is not a directory at all. */
    if (f->obj == NULL || f->obj->type == NULL ||
        f->obj->type->getdents == NULL) {
        return (uint64)-20;                      /* -ENOTDIR */
    }

    /* The position doubles as "how many entries have been returned". A
     * directory has no byte offset to speak of, and reusing the field means
     * lseek(fd, 0, SEEK_SET) rewinds a directory, which is what rewinddir
     * does. */
    return (uint64)f->obj->type->getdents(f->obj, (void *)buf, count,
                                          &f->offset);
}

/* --- descriptor management ----------------------------------------------- */

static uint64 sys_close(uint64 fd) {
    return (uint64)handle_close(proc_current()->handles, (int)fd);
}

static uint64 sys_dup(uint64 fd) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);
    int rc;

    if (f == NULL) {
        return (uint64)-9;
    }
    /* The new descriptor references the SAME open instance, so the two share
     * a file offset. That is dup's actual contract, and it is why the offset
     * lives in open_file_t rather than in the handle.
     *
     * of_ref first because handle_alloc consumes a reference and the original
     * descriptor still needs its own. */
    of_ref(f);
    rc = handle_alloc(p->handles, f, 0);
    return (uint64)(int64)rc;
}

/* pipe(2) and pipe2(2).
 *
 * Two descriptors onto one buffer, and the whole of the interesting behaviour
 * is in pipe.c. What is here is the descriptor bookkeeping, and the one thing
 * worth stating about it is the ORDER: nothing is written back to the user's
 * array until both handles are installed. A caller that got fds[0] filled in
 * and then an error would have no way to know it now owns a descriptor it was
 * never told about, and would leak it on every failed pipe().
 */
static uint64 sys_pipe2(uint64 fds_ptr, uint64 flags) {
    process_t   *p = proc_current();
    object_t    *rd = NULL;
    object_t    *wr = NULL;
    open_file_t *rf;
    open_file_t *wf;
    int         *fds = (int *)fds_ptr;
    uint32       hflags = 0;
    int          rfd, wfd, rc;

    if (!user_ptr_ok(fds_ptr)) {
        return (uint64)-14;                  /* -EFAULT */
    }
    /* Both flags have an implementation behind them now. O_NONBLOCK used to
     * be refused here, and refusing it was right at the time: accepting it
     * and then blocking anyway hangs a program in a place it has no reason to
     * look at. Now that a pipe end can say whether a read or a write would
     * block, the flag means what it says - and it is applied to BOTH ends,
     * which is what pipe2 promises.
     *
     * Anything else is still -EINVAL rather than ignored, for the original
     * reason. */
    if ((flags & ~(uint64)(O_CLOEXEC | O_NONBLOCK)) != 0) {
        return (uint64)-22;                  /* -EINVAL */
    }
    if (flags & O_CLOEXEC) {
        hflags |= HANDLE_CLOEXEC;
    }

    rc = pipe_create(&rd, &wr);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }

    rf = of_open(rd, ACCESS_READ);
    wf = of_open(wr, ACCESS_WRITE);
    ob_deref(rd);                            /* the open instances hold them */
    ob_deref(wr);
    if (rf != NULL && (flags & O_NONBLOCK)) {
        rf->status |= O_NONBLOCK;
    }
    if (wf != NULL && (flags & O_NONBLOCK)) {
        wf->status |= O_NONBLOCK;
    }
    if (rf == NULL || wf == NULL) {
        of_deref(rf);
        of_deref(wf);
        return (uint64)-23;                  /* -ENFILE */
    }

    rfd = handle_alloc(p->handles, rf, hflags);
    if (rfd < 0) {
        of_deref(wf);                        /* handle_alloc consumed rf */
        return (uint64)(int64)rfd;
    }
    wfd = handle_alloc(p->handles, wf, hflags);
    if (wfd < 0) {
        handle_close(p->handles, rfd);
        return (uint64)(int64)wfd;
    }

    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

/* eventfd2(2) - one descriptor holding a counter a poll loop can wait on.
 *
 * The object is in kernel/fs/eventfd.c; this is the descriptor half, and it
 * is deliberately the same shape as sys_pipe2 above - create the object, wrap
 * it in an open instance, hand that to the handle table - because they are
 * the same operation and the second one written differently is the one that
 * forgets to drop a reference on a failure path.
 *
 * ACCESS_READ | ACCESS_WRITE, unlike a pipe end: an eventfd is ONE descriptor
 * that is both readable and writable, which is the entire point of it over
 * the self-pipe trick it replaces. */
#define EFD_SEMAPHORE  0x00000001UL
#define EFD_NONBLOCK   0x00000800UL     /* == O_NONBLOCK */
#define EFD_CLOEXEC    0x00080000UL     /* == O_CLOEXEC  */

static uint64 sys_eventfd2(uint64 initval, uint64 flags) {
    process_t   *p = proc_current();
    object_t    *obj = NULL;
    open_file_t *of;
    uint32       hflags = 0;
    int          fd, rc;

    /* Unknown flags are refused rather than ignored, matching pipe2 above.
     * An ignored EFD_SEMAPHORE would give the caller the wrong read
     * semantics silently, which is the worst of the three ways to be wrong
     * about it. */
    if ((flags & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC)) != 0) {
        return (uint64)-22;
    }

    rc = eventfd_create(&obj, initval, (flags & EFD_SEMAPHORE) ? 1 : 0);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }

    of = of_open(obj, ACCESS_READ | ACCESS_WRITE);
    ob_deref(obj);                       /* the open instance holds it now */
    if (of == NULL) {
        return (uint64)-23;              /* -ENFILE */
    }
    if (flags & EFD_NONBLOCK) {
        of->status |= O_NONBLOCK;
    }
    if (flags & EFD_CLOEXEC) {
        hflags |= HANDLE_CLOEXEC;
    }

    fd = handle_alloc(p->handles, of, hflags);
    if (fd < 0) {
        return (uint64)(int64)fd;        /* handle_alloc consumed `of` */
    }
    return (uint64)fd;
}

/* socketpair(2). AF_UNIX/SOCK_STREAM only - see kernel/fs/socketpair.c.
 *
 * The domain and type are CHECKED rather than ignored, and that is the whole
 * of the honesty here: this returns something that behaves like a stream
 * socketpair and is not a socket, so a caller asking for AF_INET or
 * SOCK_DGRAM must be told no. Accepting AF_INET would hand back a channel
 * that works perfectly until the program tries to give the descriptor an
 * address, and accepting SOCK_DGRAM would hand back one that silently
 * coalesces messages - a bug that shows up as corrupted application framing
 * a long way from here.
 *
 * SOCK_CLOEXEC and SOCK_NONBLOCK ride in the type argument, which is Linux's
 * arrangement rather than a hack; they are masked off before the type is
 * compared. */
#define AF_UNIX_        1
#define SOCK_STREAM_    1
#define SOCK_NONBLOCK_  0x800
#define SOCK_CLOEXEC_   0x80000

static uint64 sys_socketpair(uint64 domain, uint64 type, uint64 protocol,
                             uint64 sv_ptr) {
    process_t   *p = proc_current();
    object_t    *e0 = NULL, *e1 = NULL;
    open_file_t *f0, *f1;
    int         *sv = (int *)sv_ptr;
    uint32       hflags = 0;
    uint64       base_type = type & ~(uint64)(SOCK_NONBLOCK_ | SOCK_CLOEXEC_);
    int          fd0, fd1, rc;

    if (!user_range_ok(sv_ptr, 2 * sizeof(int))) {
        return (uint64)-14;                  /* -EFAULT */
    }
    if (domain != AF_UNIX_) {
        return (uint64)-97;                  /* -EAFNOSUPPORT */
    }
    if (base_type != SOCK_STREAM_) {
        return (uint64)-94;                  /* -ESOCKTNOSUPPORT */
    }
    if (protocol != 0) {
        return (uint64)-93;                  /* -EPROTONOSUPPORT */
    }

    rc = socketpair_create(&e0, &e1);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }

    f0 = of_open(e0, ACCESS_READ | ACCESS_WRITE);
    f1 = of_open(e1, ACCESS_READ | ACCESS_WRITE);
    ob_deref(e0);
    ob_deref(e1);
    if (f0 == NULL || f1 == NULL) {
        of_deref(f0);
        of_deref(f1);
        return (uint64)-23;                  /* -ENFILE */
    }
    if (type & SOCK_NONBLOCK_) {
        f0->status |= O_NONBLOCK;
        f1->status |= O_NONBLOCK;
    }
    if (type & SOCK_CLOEXEC_) {
        hflags |= HANDLE_CLOEXEC;
    }

    fd0 = handle_alloc(p->handles, f0, hflags);
    if (fd0 < 0) {
        of_deref(f1);                        /* handle_alloc consumed f0 */
        return (uint64)(int64)fd0;
    }
    fd1 = handle_alloc(p->handles, f1, hflags);
    if (fd1 < 0) {
        handle_close(p->handles, fd0);
        return (uint64)(int64)fd1;
    }

    sv[0] = fd0;
    sv[1] = fd1;
    return 0;
}

/* --- socket(2) and the four calls that make it useful --------------------
 *
 * ROADMAP item 6's "there is no socket(2)". The socket layer itself has been
 * there since the vendored netinet landed - net_selftest.c calls socreate,
 * soconnect and sosend directly - and what was missing was only the descriptor
 * in front of it. kernel/bsd/kern_socketfd.c is that, as an object type; this
 * is the descriptor and errno half.
 *
 * These are thin ON PURPOSE. Every one is: validate the user pointers, look
 * up the descriptor, call one socketfd_* function, return what it says. The
 * socket layer's errnos come back already negated, so there is no translation
 * table here to drift out of step with the one in the socket layer.
 *
 * WHY read(2) AND write(2) ALSO WORK on these without appearing here: they go
 * through the object vtable like every other descriptor, so a socket is
 * readable and writable by the ordinary paths the moment it is an object.
 * That is the property that made this small. */

static open_file_t *sock_file(process_t *p, uint64 fd) {
    return handle_get(p->handles, (int)fd);
}

static void sock_sigpipe(void) {
    signal_send(proc_current(), SIGPIPE);
}

/* Install a new socket object as a descriptor, with SOCK_NONBLOCK /
 * SOCK_CLOEXEC applied - shared by socket(2) and accept4(2). */
static uint64 sock_install(object_t *obj, uint64 flags) {
    process_t   *p = proc_current();
    open_file_t *of;
    uint32       hflags = 0;
    int          fd;

    socketfd_set_sigpipe_hook(sock_sigpipe);
    of = of_open(obj, ACCESS_READ | ACCESS_WRITE);
    ob_deref(obj);
    if (of == NULL) {
        return (uint64)-23;
    }
    if (flags & SOCK_NONBLOCK_) {
        of->status |= O_NONBLOCK;
        socketfd_set_nonblock(of->obj, 1);
    }
    if (flags & SOCK_CLOEXEC_) {
        hflags |= HANDLE_CLOEXEC;
    }
    fd = handle_alloc(p->handles, of, hflags);
    if (fd < 0) {
        return (uint64)(int64)fd;
    }
    return (uint64)fd;
}

static uint64 sys_socket(uint64 domain, uint64 type, uint64 protocol) {
    object_t    *obj = NULL;
    uint64       base_type = type & ~(uint64)(SOCK_NONBLOCK_ | SOCK_CLOEXEC_);
    int          rc;

    rc = socketfd_create((int)domain, (int)base_type, (int)protocol, &obj);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    return sock_install(obj, type);
}

/* Every call below starts the same way: the descriptor must exist (EBADF)
 * and be a socket (ENOTSOCK, which the socketfd_* side answers). */
static uint64 sys_bind(uint64 fd, uint64 addr, uint64 len) {
    open_file_t *f = sock_file(proc_current(), fd);

    if (f == NULL) {
        return (uint64)-9;                       /* -EBADF */
    }
    if (!user_range_ok(addr, len)) {
        return (uint64)-14;
    }
    return (uint64)(int64)socketfd_bind(f->obj, (const void *)addr,
                                        (unsigned int)len);
}

static uint64 sys_connect(uint64 fd, uint64 addr, uint64 len) {
    open_file_t *f = sock_file(proc_current(), fd);

    if (f == NULL) {
        return (uint64)-9;
    }
    if (!user_range_ok(addr, len)) {
        return (uint64)-14;
    }
    return (uint64)(int64)socketfd_connect(f->obj, (const void *)addr,
                                           (unsigned int)len);
}

static uint64 sys_listen(uint64 fd, uint64 backlog) {
    open_file_t *f = sock_file(proc_current(), fd);

    if (f == NULL) {
        return (uint64)-9;
    }
    return (uint64)(int64)socketfd_listen(f->obj, (int)backlog);
}

/* An in/out socklen_t: validate the length word, then the buffer it sizes. */
static int user_socklen(uint64 addr, uint64 alen_ptr, unsigned int *alen) {
    if ((addr != 0) != (alen_ptr != 0)) {
        return -14;
    }
    if (alen_ptr == 0) {
        return 0;
    }
    if (!user_range_ok(alen_ptr, sizeof(unsigned int))) {
        return -14;
    }
    *alen = *(unsigned int *)alen_ptr;
    if ((int)*alen < 0) {
        return -22;
    }
    if (!user_range_ok(addr, *alen)) {
        return -14;
    }
    return 0;
}

static uint64 sys_accept4(uint64 fd, uint64 addr, uint64 alen_ptr,
                          uint64 flags) {
    open_file_t *f = sock_file(proc_current(), fd);
    object_t    *nobj = NULL;
    unsigned int alen = 0;
    int          rc;

    if (f == NULL) {
        return (uint64)-9;
    }
    if (flags & ~(uint64)(SOCK_NONBLOCK_ | SOCK_CLOEXEC_)) {
        return (uint64)-22;
    }
    rc = user_socklen(addr, alen_ptr, &alen);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    /* The LISTENER's O_NONBLOCK decides whether this waits; the flag
     * argument decides what the NEW descriptor gets. Linux keeps the two
     * separate (accept4's SOCK_NONBLOCK is not inherited from the listener)
     * and so does this. */
    rc = socketfd_accept(f->obj, (f->status & O_NONBLOCK) != 0, &nobj,
                         addr != 0 ? (void *)addr : NULL,
                         alen_ptr != 0 ? &alen : NULL);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    if (alen_ptr != 0) {
        *(unsigned int *)alen_ptr = alen;
    }
    return sock_install(nobj, flags);
}

static uint64 sys_shutdown(uint64 fd, uint64 how) {
    open_file_t *f = sock_file(proc_current(), fd);

    if (f == NULL) {
        return (uint64)-9;
    }
    return (uint64)(int64)socketfd_shutdown(f->obj, (int)how);
}

/* A descriptor with O_NONBLOCK behaves as if every call carried
 * MSG_DONTWAIT - the socket's SS_NBIO says so to the stack already, so the
 * flags pass through untouched. */
static uint64 sys_sendto(uint64 fd, uint64 buf, uint64 len, uint64 flags,
                         uint64 addr, uint64 alen) {
    open_file_t *f = sock_file(proc_current(), fd);

    if (f == NULL) {
        return (uint64)-9;
    }
    if (!user_range_ok(buf, len)) {
        return (uint64)-14;
    }
    if (addr != 0 && !user_range_ok(addr, alen)) {
        return (uint64)-14;
    }
    return (uint64)socketfd_sendto(f->obj, (const void *)buf, len, (int)flags,
                                   addr != 0 ? (const void *)addr : NULL,
                                   (unsigned int)alen);
}

static uint64 sys_recvfrom(uint64 fd, uint64 buf, uint64 len, uint64 flags,
                           uint64 addr, uint64 alen_ptr) {
    open_file_t *f = sock_file(proc_current(), fd);
    unsigned int alen = 0;
    int64        got;
    int          rc;

    if (f == NULL) {
        return (uint64)-9;
    }
    if (!user_range_ok(buf, len)) {
        return (uint64)-14;
    }
    /* addr and alen_ptr travel together: an address buffer with no length is
     * a buffer of unknown size, which is the shape of an overflow. */
    if ((addr != 0) != (alen_ptr != 0)) {
        return (uint64)-22;
    }
    rc = user_socklen(addr, alen_ptr, &alen);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    got = socketfd_recvfrom(f->obj, (void *)buf, len, (int)flags,
                            addr != 0 ? (void *)addr : NULL,
                            alen_ptr != 0 ? &alen : NULL);
    if (alen_ptr != 0 && got >= 0) {
        *(unsigned int *)alen_ptr = alen;
    }
    return (uint64)got;
}

/* struct msghdr, as Linux lays it out on amd64. */
typedef struct {
    uint64 msg_name;
    uint32 msg_namelen;
    uint32 pad0;
    uint64 msg_iov;
    uint64 msg_iovlen;
    uint64 msg_control;
    uint64 msg_controllen;
    int32  msg_flags;
    uint32 pad1;
} lx_msghdr_t;

#define SOCK_IOV_MAX 1024                /* UIO_MAXIOV */

static int user_iov(uint64 iov, uint64 iovcnt) {
    uint64 i;

    if (iovcnt > SOCK_IOV_MAX) {
        return -22;
    }
    if (!user_range_ok(iov, iovcnt * 16)) {
        return -14;
    }
    for (i = 0; i < iovcnt; i++) {
        uint64 base = ((uint64 *)iov)[2 * i];
        uint64 len  = ((uint64 *)iov)[2 * i + 1];
        if (len != 0 && !user_range_ok(base, len)) {
            return -14;
        }
    }
    return 0;
}

static uint64 sys_sendmsg(uint64 fd, uint64 msg, uint64 flags) {
    open_file_t *f = sock_file(proc_current(), fd);
    lx_msghdr_t *m = (lx_msghdr_t *)msg;
    int          rc;

    if (f == NULL) {
        return (uint64)-9;
    }
    if (!user_range_ok(msg, sizeof(*m))) {
        return (uint64)-14;
    }
    /* Ancillary data is refused: there is nothing it could carry here
     * (no SCM_RIGHTS over AF_INET, no IP_PKTINFO) and dropping it silently
     * would be the same kind of lie as ignoring a flag. */
    if (m->msg_controllen != 0) {
        return (uint64)-95;
    }
    rc = user_iov(m->msg_iov, m->msg_iovlen);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    if (m->msg_name != 0 && !user_range_ok(m->msg_name, m->msg_namelen)) {
        return (uint64)-14;
    }
    return (uint64)socketfd_sendmsg(f->obj, (const void *)m->msg_iov,
                                    (int)m->msg_iovlen, (int)flags,
                                    m->msg_name != 0 ? (const void *)m->msg_name
                                                     : NULL,
                                    m->msg_namelen);
}

static uint64 sys_recvmsg(uint64 fd, uint64 msg, uint64 flags) {
    open_file_t *f = sock_file(proc_current(), fd);
    lx_msghdr_t *m = (lx_msghdr_t *)msg;
    unsigned int alen;
    int          rc, oflags = 0;
    int64        got;

    if (f == NULL) {
        return (uint64)-9;
    }
    if (!user_range_ok(msg, sizeof(*m))) {
        return (uint64)-14;
    }
    rc = user_iov(m->msg_iov, m->msg_iovlen);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    alen = m->msg_namelen;
    if (m->msg_name != 0 && !user_range_ok(m->msg_name, alen)) {
        return (uint64)-14;
    }
    got = socketfd_recvmsg(f->obj, (void *)m->msg_iov, (int)m->msg_iovlen,
                           (int)flags,
                           m->msg_name != 0 ? (void *)m->msg_name : NULL,
                           m->msg_name != 0 ? &alen : NULL, &oflags);
    if (got >= 0) {
        if (m->msg_name != 0) {
            m->msg_namelen = alen;
        }
        m->msg_controllen = 0;          /* no ancillary data, ever */
        m->msg_flags = oflags;
    }
    return (uint64)got;
}

static uint64 sys_getsockname(uint64 fd, uint64 addr, uint64 alen_ptr) {
    open_file_t *f = sock_file(proc_current(), fd);
    unsigned int alen = 0;
    int64        rc;

    if (f == NULL) {
        return (uint64)-9;
    }
    if (addr == 0 || alen_ptr == 0) {
        return (uint64)-14;
    }
    rc = user_socklen(addr, alen_ptr, &alen);
    if (rc != 0) {
        return (uint64)rc;
    }
    rc = socketfd_getsockname(f->obj, (void *)addr, &alen);
    if (rc == 0) {
        *(unsigned int *)alen_ptr = alen;
    }
    return (uint64)rc;
}

static uint64 sys_getpeername(uint64 fd, uint64 addr, uint64 alen_ptr) {
    open_file_t *f = sock_file(proc_current(), fd);
    unsigned int alen = 0;
    int64        rc;

    if (f == NULL) {
        return (uint64)-9;
    }
    if (addr == 0 || alen_ptr == 0) {
        return (uint64)-14;
    }
    rc = user_socklen(addr, alen_ptr, &alen);
    if (rc != 0) {
        return (uint64)rc;
    }
    rc = socketfd_getpeername(f->obj, (void *)addr, &alen);
    if (rc == 0) {
        *(unsigned int *)alen_ptr = alen;
    }
    return (uint64)rc;
}

static uint64 sys_setsockopt(uint64 fd, uint64 level, uint64 name,
                             uint64 val, uint64 len) {
    open_file_t *f = sock_file(proc_current(), fd);

    if (f == NULL) {
        return (uint64)-9;
    }
    if ((int)len < 0 || (len != 0 && !user_range_ok(val, len))) {
        return (uint64)-14;
    }
    return (uint64)(int64)socketfd_setsockopt(f->obj, (int)level, (int)name,
                                              (const void *)val,
                                              (unsigned int)len);
}

static uint64 sys_getsockopt(uint64 fd, uint64 level, uint64 name,
                             uint64 val, uint64 len_ptr) {
    open_file_t *f = sock_file(proc_current(), fd);
    unsigned int len;
    int          rc;

    if (f == NULL) {
        return (uint64)-9;
    }
    if (val == 0 || len_ptr == 0) {
        return (uint64)-14;
    }
    rc = user_socklen(val, len_ptr, &len);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    rc = socketfd_getsockopt(f->obj, (int)level, (int)name, (void *)val, &len);
    if (rc == 0) {
        *(unsigned int *)len_ptr = len;
    }
    return (uint64)(int64)rc;
}

static uint64 sys_pipe(uint64 fds_ptr) {
    return sys_pipe2(fds_ptr, 0);
}

/* --- fcntl ---------------------------------------------------------------
 *
 * Three sets of flags live at three levels, and this call is the only place
 * a program can see the difference. See the comment on open_file_t: access
 * bits are fixed at open, status bits are shared by every descriptor for one
 * open instance, and FD_CLOEXEC is private to one descriptor. Storing any of
 * them at the wrong level is invisible until a program dups a descriptor and
 * expects one of the three to follow and the other two not to. */
static uint64 sys_fcntl(uint64 fd, uint64 cmd, uint64 arg) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);
    int          flags;

    if (f == NULL) {
        return (uint64)-9;                    /* -EBADF */
    }

    switch (cmd) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            int newfd;

            /* of_ref first: handle_alloc_from takes ownership of the
             * reference it is handed, and the caller keeps its own. Getting
             * this backwards frees the open instance out from under the
             * descriptor that is still pointing at it. */
            of_ref(f);
            newfd = handle_alloc_from(p->handles, (int)arg, f,
                                      cmd == F_DUPFD_CLOEXEC ? HANDLE_CLOEXEC : 0);
            return (uint64)(int64)newfd;
        }

        case F_GETFD:
            flags = handle_flags(p->handles, (int)fd);
            if (flags < 0) {
                return (uint64)(int64)flags;
            }
            /* Translated, not returned raw. HANDLE_CLOEXEC and FD_CLOEXEC
             * have the same value today and are not the same constant. */
            return (flags & HANDLE_CLOEXEC) ? FD_CLOEXEC : 0;

        case F_SETFD: {
            uint32 hflags = (arg & FD_CLOEXEC) ? HANDLE_CLOEXEC : 0;

            /* Everything else in `arg` is discarded rather than rejected:
             * FD_CLOEXEC is the only per-descriptor flag that exists, and a
             * libc that sets a bit this kernel has never heard of is not
             * making an error it can act on. */
            return (uint64)(int64)handle_set_flags(p->handles, (int)fd, hflags);
        }

        case F_GETFL:
            /* The access mode is reconstructed rather than stored, because
             * open_file_t records what the object manager needs (two
             * independent bits) and not what open(2) was called with (an
             * enumeration where O_RDONLY is zero).
             *
             * The both-bits case is tested FIRST and that is not style: with
             * O_RDONLY == 0, testing for read before testing for read+write
             * reports every O_RDWR descriptor as read-only, and a libc that
             * checks before writing then refuses to write to a file it opened
             * for writing. */
            if ((f->access & (ACCESS_READ | ACCESS_WRITE)) ==
                (ACCESS_READ | ACCESS_WRITE)) {
                return O_RDWR | (f->status & O_SETFL_MASK);
            }
            if (f->access & ACCESS_WRITE) {
                return O_WRONLY | (f->status & O_SETFL_MASK);
            }
            return O_RDONLY | (f->status & O_SETFL_MASK);

        case F_SETFL:
            /* Both settable flags are honoured now.
             *
             * O_APPEND used to be refused with -EROFS, because no write path
             * implemented it and the volume was read-only anyway - accepting
             * it would have been a lie a program cannot detect. There is a
             * write path now and do_write seeks to the end before every write
             * when this bit is set, so accepting it is the truth.
             *
             * O_NONBLOCK was already accepted because the readiness predicate
             * poll needed is exactly what a non-blocking read tests. Both are
             * STATUS flags, so both live on the open instance and are shared
             * by every descriptor dup'd from it - which is why the mask below
             * covers the pair rather than each being handled separately. */
            f->status = (f->status & ~(uint32)O_SETFL_MASK) |
                        ((uint32)arg & (uint32)O_SETFL_MASK);
            /* A socket reads its own copy of the flag (SS_NBIO), which a
             * blocking connect or accept consults inside the stack. */
            if (socketfd_is_socket(f->obj)) {
                socketfd_set_nonblock(f->obj, (f->status & O_NONBLOCK) != 0);
            }
            return 0;

        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
            /* Declined, and that is the right answer while the filesystem is
             * read-only. A program told it holds an exclusive lock that this
             * kernel is not enforcing will happily interleave writes with
             * another that was told the same thing, and corrupt a file
             * quietly. -EINVAL makes it fall back to running unlocked, which
             * is what it would be doing anyway - honestly this time.
             *
             * This becomes the wrong answer the moment two processes can
             * write to one file. It is in the Owed list for that reason. */
            return (uint64)-22;               /* -EINVAL */

        default:
            return (uint64)-22;
    }
}

/* --- access --------------------------------------------------------------
 *
 * R_OK, X_OK and F_OK all collapse to "does this name resolve": there is one
 * uid, and FAT stores no permission bits, so a file that exists is a file
 * this process may read and execute. Inventing a distinction the filesystem
 * cannot express would mean answering from the same made-up mode bits stat
 * reports, which is a check that consults its own answer.
 *
 * W_OK does not collapse, and that is the point of implementing this at all.
 * The volume really is read-only, so -EACCES is the truth rather than a
 * placeholder, and a program that checks before writing gets a useful answer
 * instead of a successful open followed by -EROFS. Devices answer 0, because
 * the console genuinely is writable.
 *
 * When the volume becomes writable this function changes with it. That is in
 * the roadmap next to the ext4 work, and it is the one line of this that is
 * a statement about today rather than about POSIX. */
static uint64 sys_faccessat(uint64 dirfd, uint64 path_ptr, uint64 mode,
                            uint64 flags) {
    process_t *p = proc_current();
    char resolved[PATH_MAX_LEN];
    fs_node_t node;
    int rc;

    (void)flags;   /* AT_EACCESS and AT_SYMLINK_NOFOLLOW: no distinction to
                    * draw. One uid, and no symbolic links on the volume. */

    if ((int64)dirfd != AT_FDCWD) {
        return (uint64)-22;
    }
    if (!user_ptr_ok(path_ptr)) {
        return (uint64)-14;
    }
    /* Bits outside the four defined modes are an error, not something to
     * ignore. faccessat2 passes a flags word here in some libcs' fallback
     * paths, and silently accepting a garbage mode would report every file
     * accessible. */
    if (mode & ~(uint64)(R_OK | W_OK | X_OK)) {
        return (uint64)-22;
    }
    if (path_normalize(p->cwd, (const char *)path_ptr,
                       resolved, sizeof(resolved)) != PATH_OK) {
        return (uint64)-36;
    }

    if (is_dev_path(resolved)) {
        char      remainder[NS_PATH_MAX];
        object_t *obj = NULL;

        rc = dev_lookup(resolved, &obj, remainder, sizeof(remainder));
        if (rc != 0) {
            return (uint64)(int64)rc;
        }
        if (remainder[0] != '\0') {
            ob_deref(obj);
            return (uint64)-20;
        }
        ob_deref(obj);
        return 0;                              /* devices are read/write */
    }

    rc = fs_lookup(resolved, &node);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    /* Against the file's real ACL, through the same fs_access that open(2)
     * uses. That identity of implementation is the whole point: this function
     * used to answer from "does the volume have a write slot", and its own
     * comment above admitted the rest was made up - "a file that exists is a
     * file this process may read and execute", because there was no uid to
     * check against and no permission bits to check.
     *
     * Now there are both, and there is exactly one function that decides. An
     * access(2) that agrees with open(2) is not a nicety; a program that asks
     * before opening and then gets a different answer is a program that has
     * been told a lie, and the read-only-volume case fs_access still handles
     * separately is the one part of this that an ACL genuinely cannot say. */
    {
        cred_t c;
        uint32 want;

        proc_cred(p, &c);
        want = acl_mask_for_posix((mode & R_OK) != 0, (mode & W_OK) != 0,
                                  (mode & X_OK) != 0, node.is_dir);
        /* F_OK is mode 0: "does it exist". The lookup above answered that
         * already, and asking the ACL for no bits at all would answer yes
         * for a reason unrelated to the question. */
        if (want == 0) {
            return 0;
        }
        rc = fs_access(&node, (const struct cred *)&c, want);
        if (rc != 0) {
            return (uint64)(int64)rc;
        }
    }
    return 0;
}

/* --- poll ----------------------------------------------------------------
 *
 * The last syscall between here and building real programs against musl, and
 * the only one left that was design rather than plumbing. What made it design
 * is in object.h: every object in this kernel could answer "block until a
 * read would not block" and none of them could answer "would a read block",
 * which is a different question that cannot be built from the first one -
 * attempting it is a read that then has to be un-read.
 *
 * The blocking half is in waitq.c. poll waits on N objects and a process can
 * be listed on one wait queue, so it parks on the shared readiness queue that
 * waitq_wake_all pokes; see waitq.h for why that is one shared queue rather
 * than N registrations.
 *
 * select(2) is deliberately absent. musl implements it over poll wherever
 * poll exists, so adding it would be a second copy of this decision table
 * with a different bitmap on the front - two implementations of one question,
 * which is how the answers come apart. */

/* One entry of the user's array. Not `packed`: this is an ABI structure, and
 * its bytes are whatever a compiler would have produced. The assertion below
 * is what checks that. */
struct pollfd {
    int   fd;
    short events;
    short revents;
};

/* Both ends of the ABI must agree, and only one of them is in this tree.
 * A negative array size is the build-time assertion idiom this kernel already
 * uses for every NT structure - the check costs nothing and fires at compile
 * time rather than as a poll that reads the next entry's fd. */
typedef char poll_layout_assert[(sizeof(struct pollfd) == 8) ? 1 : -1];

/* And that the object layer's readiness vocabulary still lines up with the
 * Linux ABI's. They are separate constants on purpose - see object.h - which
 * is exactly why something has to check they have not drifted. */
typedef char poll_bits_assert[
    (OB_POLLIN  == POLLIN  && OB_POLLOUT == POLLOUT &&
     OB_POLLERR == POLLERR && OB_POLLHUP == POLLHUP &&
     OB_POLLNVAL == POLLNVAL) ? 1 : -1];

/* Context for the readiness callback. `hit` carries the count out, so the
 * scan that decides whether to keep waiting is the same scan that produces
 * the answer - one walk over the array, and no way for the two to disagree
 * about which descriptors were ready. */
struct poll_ctx {
    struct pollfd *fds;
    uint64         nfds;
    int            hit;
};

static int poll_scan(void *ctx) {
    struct poll_ctx *pc = (struct poll_ctx *)ctx;
    process_t *p = proc_current();
    uint64 i;

    pc->hit = 0;
    for (i = 0; i < pc->nfds; i++) {
        int fd = pc->fds[i].fd;
        int events = pc->fds[i].events;
        int revents;

        /* A negative fd is not an error: POSIX says it is IGNORED, and
         * revents is set to zero. Programs use it to punch a hole in a
         * persistent array rather than rebuilding it, and reporting POLLNVAL
         * for it would make every such program see a permanent error. */
        if (fd < 0) {
            pc->fds[i].revents = 0;
            continue;
        }
        {
            open_file_t *f = handle_get(p->handles, fd);

            revents = (f == NULL) ? POLLNVAL : ob_poll(f->obj, events);
        }
        pc->fds[i].revents = (short)revents;
        if (revents != 0) {
            pc->hit++;
        }
    }
    return pc->hit;
}

static uint64 do_poll(uint64 fds_ptr, uint64 nfds, int64 timeout_ms) {
    struct poll_ctx pc;
    uint64 deadline = 0;
    int rc;

    if (nfds > MAX_HANDLES) {
        /* -EINVAL rather than a silent truncation. A program polling more
         * descriptors than this kernel can hold open has a bug of its own,
         * and quietly ignoring the tail would hang it on the descriptor that
         * was going to be the ready one. */
        return (uint64)-22;
    }
    if (nfds > 0 && !user_ptr_ok(fds_ptr)) {
        return (uint64)-14;
    }

    pc.fds  = (struct pollfd *)fds_ptr;
    pc.nfds = nfds;
    pc.hit  = 0;

    /* The zero-timeout probe answers from one scan and never blocks. It is
     * separated out rather than handled by an already-expired deadline
     * because it is the common case - a program draining what is ready
     * without giving up the CPU - and because it must work identically when
     * nfds is zero, where the loop below would otherwise be a plain sleep. */
    if (timeout_ms == 0) {
        poll_scan(&pc);
        return (uint64)(int64)pc.hit;
    }

    if (timeout_ms > 0) {
        uint64 hz = timer_hz();

        /* Rounded UP, for the reason nanosleep rounds up: a timeout that
         * expires early is indistinguishable from one that worked, and a
         * program that polls with a 10ms timeout in a loop would spin. The
         * +1 guarantees at least one whole tick even for a sub-tick timeout,
         * so poll(fds, n, 1) is a sleep rather than a spin. */
        deadline = timer_ticks_now() +
                   (((uint64)timeout_ms * hz + 999) / 1000) + 1;
    }

    rc = waitq_wait_until(waitq_readiness(), poll_scan, &pc, deadline);
    if (rc == WAITQ_SIGNAL) {
        /* -EINTR, and revents is left as the last scan wrote it. A caller
         * that gets -EINTR is required to ignore the array, so there is
         * nothing to preserve - but there is also no reason to spend a scan
         * clearing it. */
        return (uint64)-4;
    }
    if (rc == WAITQ_TIMEOUT) {
        /* Zero, not an errno. A poll that times out with nothing ready has
         * not failed; it has answered. Reporting -ETIMEDOUT here is the bug
         * that makes a select-style event loop treat an idle second as a
         * fatal error. */
        return 0;
    }
    return (uint64)(int64)pc.hit;
}

static uint64 sys_poll(uint64 fds_ptr, uint64 nfds, uint64 timeout_ms) {
    return do_poll(fds_ptr, nfds, (int64)(int)timeout_ms);
}

/* ppoll is poll with a timespec instead of a millisecond count and a signal
 * mask applied for the duration.
 *
 * The mask is NOT implemented, and it is declined rather than ignored. The
 * whole reason ppoll exists is the race it closes: a program that unblocks a
 * signal and then calls poll can take the signal in the gap between the two
 * and wait forever for an event that already happened. Accepting a mask and
 * not applying it would leave that race open while telling the caller it had
 * been closed - which is worse than not having ppoll, because the program
 * would have written the safe version if it knew.
 *
 * A NULL mask has nothing to apply, so that case is exactly poll and is
 * served. musl's ppoll passes NULL unless the caller asked for a mask. */
static uint64 sys_ppoll(uint64 fds_ptr, uint64 nfds, uint64 ts_ptr,
                        uint64 sigmask_ptr) {
    int64 timeout_ms = -1;

    if (sigmask_ptr != 0) {
        return (uint64)-38;                    /* -ENOSYS */
    }
    if (ts_ptr != 0) {
        const uint64 *ts = (const uint64 *)ts_ptr;

        if (!user_ptr_ok(ts_ptr)) {
            return (uint64)-14;
        }
        if (ts[1] >= 1000000000ULL) {
            return (uint64)-22;
        }
        /* Rounded up to whole milliseconds before do_poll rounds up again to
         * whole ticks. Two roundings in the same direction overshoot; the
         * alternative is a nanosecond timeout that truncates to zero and
         * turns a blocking ppoll into a spin. */
        timeout_ms = (int64)(ts[0] * 1000ULL + (ts[1] + 999999ULL) / 1000000ULL);
    }
    return do_poll(fds_ptr, nfds, timeout_ms);
}

/* --- futex ---------------------------------------------------------------
 *
 * The syscall shape; the substance is in futex.c. What lives here is the
 * argument decoding and the decision about which operations to refuse.
 *
 * Refusing rather than approximating is the whole policy. FUTEX_REQUEUE and
 * the priority-inheritance operations are distinct protocols, not variations
 * on WAIT and WAKE, and something built out of the wrong two primitives works
 * under low contention and deadlocks under high - which is the worst possible
 * testing profile, since it passes everything you would think to try. */
static uint64 sys_futex(uint64 uaddr, uint64 op, uint64 val, uint64 timeout_ptr) {
    int cmd = (int)(op & FUTEX_CMD_MASK);

    switch (cmd) {
        case FUTEX_WAIT: {
            uint64 deadline = 0;

            /* The timeout is RELATIVE for FUTEX_WAIT - unlike almost every
             * other timeout in this kernel, and unlike FUTEX_WAIT_BITSET,
             * which takes an absolute one. Getting this backwards produces a
             * wait that returns instantly, because an absolute deadline read
             * as a relative one is a deadline in the past. */
            if (timeout_ptr != 0) {
                const uint64 *ts = (const uint64 *)timeout_ptr;
                uint64 hz, ns;

                if (!user_ptr_ok(timeout_ptr)) {
                    return (uint64)-14;
                }
                if (ts[1] >= 1000000000ULL) {
                    return (uint64)-22;
                }
                hz = timer_hz();
                ns = ts[0] * 1000000000ULL + ts[1];
                /* Rounded up, and +1, for nanosleep's reason: a wait that
                 * expires early is indistinguishable from one that worked,
                 * and a sub-tick timeout that truncates to zero turns a
                 * blocking wait into a spin. */
                deadline = timer_ticks_now() +
                           ((ns * hz + 999999999ULL) / 1000000000ULL) + 1;
            }
            return (uint64)futex_wait(uaddr, (uint32)val, deadline);
        }

        case FUTEX_WAKE:
            return (uint64)futex_wake(uaddr, (int)val);

        default:
            /* -ENOSYS rather than -EINVAL, and the difference is load
             * bearing: a libc reads -EINVAL as "you passed me bad arguments"
             * and retries or aborts, and -ENOSYS as "this kernel does not
             * have that", which is the one it can fall back from. */
            return (uint64)-38;
    }
}

static uint64 sys_dup2(uint64 oldfd, uint64 newfd) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)oldfd);

    if (f == NULL) {
        return (uint64)-9;
    }
    /* dup2(fd, fd) returns fd without closing anything - a shell doing
     * `exec 2>&2` must not lose the descriptor it is duplicating. */
    if (oldfd == newfd) {
        return newfd;
    }
    of_ref(f);
    return (uint64)(int64)handle_install_at(p->handles, (int)newfd, f, 0);
}

/* --- vfork ---------------------------------------------------------------
 * The child runs in the PARENT's address space, on the parent's user stack,
 * and the parent is suspended. That is what makes it cheap: no page tables
 * are copied, no frames are duplicated, and no copy-on-write machinery is
 * needed. It is also what makes it sharp - the child must not return from the
 * function that called vfork, because the stack frame it would return through
 * belongs to the parent. In practice a child execs or exits immediately, and
 * that is the only thing busybox does with it.
 *
 * --- A deliberate deviation from POSIX ---
 * POSIX resumes the parent when the child execs OR exits. This resumes it
 * only when the child EXITS. The difference matters exactly once: at the
 * moment of exec, POSIX has two runnable processes and this has one - which
 * is why this works with no scheduler, no timer and no context switcher.
 *
 * The cost is background jobs, and that is not a real cost yet: `cmd &`
 * additionally needs process groups, SIGCHLD and tcsetpgrp, which do not
 * exist either. When the scheduler lands, what changes is the line that
 * decides when the parent is woken - not this function.
 *
 * Resumption works the same way execve's exit does. A suspended process's
 * whole state is its syscall frame plus the parked user RSP; writing both
 * back over the running frame redirects the return path into it. */

static uint64 sys_vfork(struct syscall_frame *frame) {
    process_t *parent = proc_current();
    process_t *child;

    child = proc_alloc(parent->pid);
    if (child == NULL) {
        return (uint64)-11;      /* -EAGAIN: the process table is full */
    }

    /* Share, do not copy. The whole point. */
    child->space        = parent->space;
    child->shares_space = 1;
    child->personality  = parent->personality;
    /* Same memory, so the same thread pointer resolves to the same TLS, and
     * the same live FPU state for the same reason. */
    child->thread.fs_base = parent->thread.fs_base;
    child->thread.gs_base = parent->thread.gs_base;
    fpu_save(child->thread.fpu_state);
    /* The process group is inherited, and proc_alloc leaves it at 0 waiting
     * for exactly this. Without it every command the shell runs sits in group
     * zero, which signal_send_group refuses to match - so Ctrl-C during a
     * running command reached nothing, and neither did kill(0, sig). */
    child->pgid         = parent->pgid;
    child->sig_blocked  = parent->sig_blocked;
    /* Credentials, which neither fork path copied. A child inherited
     * whatever proc_alloc left in the slot instead of its parent's identity,
     * so a shell that had dropped privilege forked a child that had not. Same
     * root cause as the missing initialisation in proc_alloc, and the same
     * reason it was invisible: nothing read these. */
    child->uid          = parent->uid;
    child->gid          = parent->gid;
    child->sid          = parent->sid;
    child->umask        = parent->umask;
    child->ngroups      = parent->ngroups;
    {
        uint32 g;
        for (g = 0; g < CRED_NGROUPS; g++) {
            child->groups[g] = parent->groups[g];
        }
    }

    child->brk_base     = parent->brk_base;
    child->brk_current  = parent->brk_current;
    child->mmap_next    = parent->mmap_next;
    {
        uint64 i;
        for (i = 0; i < PATH_MAX_LEN; i++) {
            child->cwd[i] = parent->cwd[i];
        }
    }
    /* POSIX inheritance: everything except close-on-exec, sharing the open
     * instances so parent and child share file offsets. */
    handle_table_clone(child->handles, parent->handles, 0);

    /* Suspend the parent exactly where it is. */
    parent->saved_frame    = *frame;
    parent->saved_user_rsp = thiscpu.user_rsp;
    parent->state          = PROC_BLOCKED;
    child->vfork_waiter    = parent->pid;

    /* Give the child a kernel stack it can be scheduled onto later. It is not
     * used to reach user mode now - we do that by returning 0 below, on this
     * stack - but a process the scheduler can never switch TO is a process
     * that can only ever run once, and the scheduler exists now.
     *
     * The frame it is bootstrapped with is the parent's with rax zeroed:
     * same RIP, same user stack, returning 0. That is vfork. */
    {
        struct syscall_frame child_frame = *frame;
        child_frame.rax = 0;
        child->thread.saved_rsp =
            thread_bootstrap_stack(child->thread.kstack_top, &child_frame);
        child->saved_user_rsp = thiscpu.user_rsp;
    }

    /* Become the child and return 0 into it. The user RSP is untouched - the
     * child really does continue on the parent's stack, which is vfork's
     * defining and most dangerous property. */
    proc_set_current(child);
    return 0;
}

/* --- fork ----------------------------------------------------------------
 * The real one. Both processes are runnable when this returns, they have
 * separate address spaces, and not one page of memory was copied to make that
 * true - vmm_space_clone maps the same frames into both sets of tables
 * read-only, and the page fault handler splits a page the first time either
 * side writes to it.
 *
 * What this costs that vfork does not: one PML4, and one page table per 2MB
 * of mapped address space. What it buys is everything vfork cannot do -
 * pipelines, subshells, and a child that can return from the function that
 * called fork, because it is standing on its own stack rather than borrowing
 * its parent's.
 *
 * The child is left READY rather than made current. That is the whole
 * difference in shape from sys_vfork: there is no suspension to arrange,
 * because there is nothing being shared that the parent must not touch. Which
 * of the two runs first is the scheduler's business, and a program that
 * depends on the answer is broken on every other kernel too. */

static uint64 sys_fork(struct syscall_frame *frame) {
    process_t *parent = proc_current();
    process_t *child;
    int k;

    /* Nothing to clone. Process 1 before execve runs in the kernel space, and
     * copy-on-writing the kernel's own mappings is not a thing that could be
     * made to mean anything. */
    if (parent->space == NULL || parent->space == vmm_kernel_space()) {
        return (uint64)-22;              /* -EINVAL */
    }

    child = proc_alloc(parent->pid);
    if (child == NULL) {
        return (uint64)-11;              /* -EAGAIN: the process table is full */
    }

    child->space = vmm_space_clone(parent->space);
    if (child->space == NULL) {
        proc_free(child);
        return (uint64)-12;              /* -ENOMEM */
    }
    child->shares_space = 0;             /* it owns what it just got */

    child->personality  = parent->personality;
    child->brk_base     = parent->brk_base;
    child->brk_current  = parent->brk_current;
    child->mmap_next    = parent->mmap_next;
    child->pgid         = parent->pgid;
    /* Credentials, which neither fork path copied. A child inherited
     * whatever proc_alloc left in the slot instead of its parent's identity,
     * so a shell that had dropped privilege forked a child that had not. Same
     * root cause as the missing initialisation in proc_alloc, and the same
     * reason it was invisible: nothing read these. */
    child->uid          = parent->uid;
    child->gid          = parent->gid;
    child->sid          = parent->sid;
    child->umask        = parent->umask;
    child->ngroups      = parent->ngroups;
    {
        uint32 g;
        for (g = 0; g < CRED_NGROUPS; g++) {
            child->groups[g] = parent->groups[g];
        }
    }

    child->sig_blocked  = parent->sig_blocked;
    /* The child resumes at the same instruction in a copy of the same memory,
     * so it needs the same thread pointer to find its TLS at the same
     * address. */
    child->thread.fs_base = parent->thread.fs_base;
    child->thread.gs_base = parent->thread.gs_base;
    /* The parent's LIVE state, not its saved copy: thread.fpu_state is only
     * current for a thread that is switched out, and the parent is running
     * right now. Straight into the child's area, which is what a child
     * resuming at the same instruction needs. */
    fpu_save(child->thread.fpu_state);

    /* Dispositions are inherited by fork and reset by execve - not the other
     * way round, which is the mistake that makes a shell's SIGINT handler
     * vanish in every child it spawns. */
    for (k = 0; k < SIG_COUNT; k++) {
        child->sig_handlers[k] = parent->sig_handlers[k];
    }
    {
        uint64 i;
        for (i = 0; i < PATH_MAX_LEN; i++) {
            child->cwd[i] = parent->cwd[i];
        }
    }
    handle_table_clone(child->handles, parent->handles, 0);

    /* The child resumes at the same instruction with rax = 0, on a kernel
     * stack laid out so the scheduler's switch_context can land on it. Its
     * user RSP is the parent's - the same address in a different space, whose
     * page will be copied the first time either of them pushes. */
    {
        struct syscall_frame child_frame = *frame;

        child_frame.rax = 0;
        child->thread.saved_rsp =
            thread_bootstrap_stack(child->thread.kstack_top, &child_frame);
        child->saved_user_rsp = syscall_get_user_rsp();
    }

    child->state = PROC_READY;
    sched_enqueue(child);

    return (uint64)child->pid;
}

/* --- a new thread of `parent`'s group -------------------------------------
 *
 * What every thread shares with its creator and what it gets of its own,
 * in one place, because there are two ways to make one - clone() with the
 * thread flags, and NtCreateThreadEx - and they must not disagree about
 * what a thread IS. The caller supplies how it starts: the frame it will
 * return to ring 3 through, its user stack, and its two thread-pointer
 * bases. Not enqueued; the caller does that once anything else it writes
 * (a tid into user memory, an NT handle) is in place.
 *
 * NULL if the table is full. */
static process_t *spawn_thread(process_t *parent,
                               const struct syscall_frame *start,
                               uint64 user_rsp, uint64 fs_base,
                               uint64 gs_base) {
    process_t *child = proc_alloc(parent->pid);

    if (child == NULL) {
        return NULL;
    }

    /* The three shares, and the ownership flags that stop teardown undoing
     * them. Set together, in one place, because the pointer and the flag
     * describing it are two halves of one fact - and the version of this
     * where they disagree is a descriptor table freed while three threads
     * are still reading through it. */
    child->space        = parent->space;
    child->shares_space = 1;
    child->handles      = parent->handles;
    child->owns_files   = 0;
    child->sig_handlers = parent->sig_handlers;
    child->owns_sighand = 0;

    /* Every thread of a group reports the group leader's pid from getpid().
     * The slot's own pid is the tid. musl caches getpid()'s answer, so two
     * threads seeing two different values does not fail here - it fails
     * later, somewhere that has no visible connection to threading. */
    child->tgid        = parent->tgid;
    child->personality = parent->personality;
    child->pgid        = parent->pgid;
    child->brk_base    = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->mmap_next   = parent->mmap_next;
    child->nt_thread_start = parent->nt_thread_start;
    child->nt_apc_dispatcher = parent->nt_apc_dispatcher;
    child->nt_exc_dispatcher = parent->nt_exc_dispatcher;

    /* Credentials. A thread IS its process as far as identity goes - POSIX
     * and NT agree on that - and this copy was missing: fork learned it
     * (see the same block in sys_fork, and the bug it records), the thread
     * path did not, and proc_alloc hands every new slot uid 0. So a thread
     * created by a process that had dropped to uid 1000 ran as ROOT. Nothing
     * noticed while no thread asked an access question; the first one that
     * opened a file would have been answered for the wrong user. */
    child->uid          = parent->uid;
    child->gid          = parent->gid;
    child->sid          = parent->sid;
    child->umask        = parent->umask;
    child->ngroups      = parent->ngroups;
    {
        uint32 g;
        for (g = 0; g < CRED_NGROUPS; g++) {
            child->groups[g] = parent->groups[g];
        }
    }

    /* The MASK is inherited and is per-thread from here on; the DISPOSITIONS
     * are shared through the pointer above. That split is POSIX's, and it is
     * the reason sig_blocked stayed a plain field when sig_handlers became a
     * pointer. */
    child->sig_blocked = parent->sig_blocked;
    {
        uint64 i;
        for (i = 0; i < PATH_MAX_LEN; i++) {
            child->cwd[i] = parent->cwd[i];
        }
    }
    fpu_save(child->thread.fpu_state);

    /* Per-thread CPU state, restored by proc_activate_stack on every switch
     * like fs_base always was. */
    child->thread.fs_base = fs_base;
    child->thread.gs_base = gs_base;

    child->thread.saved_rsp =
        thread_bootstrap_stack(child->thread.kstack_top, start);
    child->saved_user_rsp = user_rsp;
    return child;
}

process_t *proc_spawn_thread(process_t *parent,
                             const struct syscall_frame *start,
                             uint64 user_rsp, uint64 fs_base,
                             uint64 gs_base) {
    return spawn_thread(parent, start, user_rsp, fs_base, gs_base);
}

/* --- clone ---------------------------------------------------------------
 *
 * Two jobs behind one number, and telling them apart is the whole of this
 * function. Without any sharing flag, clone IS fork - glibc has implemented
 * fork() as clone(SIGCHLD) for years, musl calls SYS_fork directly, and
 * accepting both removes a class of "the same binary works under one libc and
 * not the other". With CLONE_VM|CLONE_THREAD it is a thread, which shares an
 * address space, a descriptor table and a table of signal dispositions with
 * its creator.
 *
 * What is NOT accepted is a partial answer. CLONE_THREAD without CLONE_VM,
 * or CLONE_VM without CLONE_THREAD, describes something this kernel does not
 * build, and silently giving a "thread" its own copy of memory produces a
 * program that runs and is wrong - two threads incrementing what they both
 * believe is one counter and getting two. So anything outside the two shapes
 * below is -EINVAL, loudly, at creation. */
static uint64 sys_clone(struct syscall_frame *frame) {
    uint64 flags     = frame->rdi;
    uint64 child_sp  = frame->rsi;
    uint64 ptid_ptr  = frame->rdx;
    uint64 tls       = frame->r10;
    uint64 ctid_ptr  = frame->r8;
    process_t *parent = proc_current();
    process_t *child;

    /* --- the fork shape ------------------------------------------------- */
    if (!(flags & (CLONE_VM | CLONE_THREAD))) {
        if (flags & (CLONE_FILES | CLONE_SIGHAND | CLONE_VFORK)) {
            return (uint64)-22;
        }
        if ((flags & CLONE_CSIGNAL) != SIGCHLD && (flags & CLONE_CSIGNAL) != 0) {
            return (uint64)-22;
        }
        return sys_fork(frame);
    }

    /* --- the thread shape ----------------------------------------------- */

    /* All five together or none. A caller asking for a subset is asking for
     * something with no implementation, and the honest answer is to say so
     * rather than to approximate the nearest thing that exists. */
    if ((flags & CLONE_THREAD_SET) != CLONE_THREAD_SET) {
        return (uint64)-22;
    }
    /* A thread must be given its own stack. It shares the address space, so
     * without this both threads would push onto the same one and each would
     * corrupt the other's frames - and the corruption appears as a return to
     * a garbage address in whichever thread happened to lose the race. */
    if (child_sp == 0 || !user_ptr_ok(child_sp)) {
        return (uint64)-22;
    }
    if (parent->space == NULL || parent->space == vmm_kernel_space()) {
        return (uint64)-22;
    }
    /* A thread group signal is a contradiction: CLONE_THREAD means the new
     * task is not a child and has no parent to notify. */
    if ((flags & CLONE_CSIGNAL) != 0) {
        return (uint64)-22;
    }

    /* The new thread starts at the same instruction with rax = 0 - the same
     * arrangement fork uses - but on the stack the CALLER supplied rather
     * than on a copy of its own. That single difference is what makes this a
     * thread rather than a process.
     *
     * CLONE_SETTLS: musl passes the new thread's pthread struct here and
     * every __thread access in that thread resolves through it, so a thread
     * created without it reads another thread's TLS. */
    {
        struct syscall_frame child_frame = *frame;

        child_frame.rax = 0;
        child = spawn_thread(parent, &child_frame, child_sp,
                             (flags & CLONE_SETTLS) ? tls
                                                    : parent->thread.fs_base,
                             parent->thread.gs_base);
    }
    if (child == NULL) {
        return (uint64)-11;              /* -EAGAIN: the table is full */
    }

    /* CLONE_PARENT_SETTID writes the new tid where the CREATOR can see it,
     * and it must be written before the child can run: musl reads it in the
     * parent immediately after clone returns. Writing it here rather than
     * from the child closes that race by construction - the child is not
     * enqueued yet.
     *
     * CLONE_CHILD_SETTID targets the child's address space, which is the same
     * address space, so it is the same store. */
    if ((flags & CLONE_PARENT_SETTID) && ptid_ptr != 0 && user_ptr_ok(ptid_ptr)) {
        *(int *)ptid_ptr = child->pid;
    }
    if ((flags & CLONE_CHILD_SETTID) && ctid_ptr != 0 && user_ptr_ok(ctid_ptr)) {
        *(int *)ctid_ptr = child->pid;
    }
    /* CLONE_CHILD_CLEARTID is what a joining thread waits on: the kernel
     * zeroes the word and futex-wakes it when the thread dies. Recorded here
     * and acted on in syscall_exit_process. Without it pthread_join waits
     * forever on a thread that has already exited. */
    child->clear_child_tid =
        (flags & CLONE_CHILD_CLEARTID) ? ctid_ptr : 0;

    child->state = PROC_READY;
    sched_enqueue(child);

    return (uint64)child->pid;           /* the TID, which is what musl wants */
}

/* Wake whoever is suspended in vfork on `child`, if anyone, and arrange for
 * the return path to land in them. Returns non-zero if the frame was
 * rewritten and the caller must return frame->rax. */
static int resume_vfork_parent(process_t *child, struct syscall_frame *frame) {
    process_t *parent;

    if (child->vfork_waiter == 0) {
        return 0;
    }
    parent = proc_find(child->vfork_waiter);
    if (parent == NULL || parent->state != PROC_BLOCKED) {
        return 0;
    }

    child->vfork_waiter = 0;

    /* If the child execed it acquired its own address space, and the parent
     * is suspended in a different one. CR3 has to move back before the parent
     * runs, and the child's space can only be destroyed once it is not the
     * one loaded - which is exactly the ordering vmm_space_destroy refuses to
     * do for you. */
    if (child->space != parent->space) {
        address_space_t *dead = child->space;

        vmm_switch_to(parent->space);
        child->space = NULL;
        if (dead != vmm_kernel_space()) {
            vmm_space_destroy(dead);
        }
    } else {
        /* A child that never execed was borrowing the parent's space. Drop
         * the borrow so proc_free does not reason about it later. */
        child->space        = NULL;
        child->shares_space = 0;
    }

    /* Everything the parent had that the child may have changed. The FS base
     * is the one that is easy to forget, because nothing in this function
     * looks like it touches the CPU: the child execs, calls arch_prctl, and
     * the MSR now points at the child's TLS - so the resumed parent reads its
     * thread pointer out of an address that belongs to an image which no
     * longer exists, and faults on the first thing it finds there.
     *
     * proc_set_current below does the actual restore; this is why it must
     * come after the state is right rather than before. */
    *frame = parent->saved_frame;
    frame->rax = (uint64)child->pid;   /* vfork returns the pid in the parent */
    syscall_set_user_rsp(parent->saved_user_rsp);
    proc_set_current(parent);
    return 1;
}

/* --- exit ---------------------------------------------------------------- */

/* Take down every OTHER thread of `p`'s group.
 *
 * This is the difference between exit and exit_group, and with one thread per
 * process it had no content - which is why both numbers landed in the same
 * place for so long. Now exit_group means what it says: return(3) from main,
 * or a call to exit(3), ends the whole program, and a surviving thread would
 * keep running code whose main function has returned.
 *
 * The threads are retired rather than exited: proc_retire is the path for a
 * process dying somewhere it cannot unwind from, which is exactly what this
 * is - the thread is not running, it is parked in a syscall or on the run
 * queue, and there is no frame of its own to return through.
 *
 * `p` itself is skipped. The caller is in the middle of its own exit and
 * retiring it here would close the descriptor table twice and dequeue a
 * process that is still executing. */
static void kill_thread_group(process_t *p, int status) {
    int i;

    for (i = 0; i < MAX_PROCESSES; i++) {
        process_t *t = proc_at(i);

        if (t == NULL || t == p) {
            continue;
        }
        if (t->tgid != p->tgid) {
            continue;
        }
        if (t->state == PROC_ZOMBIE || t->state == PROC_UNUSED) {
            continue;
        }
        proc_retire(t, status);
    }
}

uint64 syscall_exit_group(uint64 status, struct syscall_frame *frame) {
    /* The others first, while this thread still owns the tables the retire
     * path reads - see SYS_exit_group's case for why the order matters. */
    kill_thread_group(proc_current(), (int)(status & 0xFF));
    return syscall_exit_process(status, frame);
}

uint64 syscall_exit_process(uint64 status, struct syscall_frame *frame) {
    process_t *p = proc_current();


    p->exit_status = (int)(status & 0xFF);
    p->state       = PROC_ZOMBIE;

    /* The join handshake, and it has to happen before anything else can run.
     * A thread created with CLONE_CHILD_CLEARTID has a joiner futex-waiting
     * on that word; clearing it and waking is the only thing that releases
     * them. Nothing in userspace can do this - the thread cannot write the
     * word after it has stopped running, and writing it before it stops is a
     * lie that lets the joiner free a stack still in use. */
    if (p->clear_child_tid != 0 && user_ptr_ok(p->clear_child_tid)) {
        *(int *)p->clear_child_tid = 0;
        futex_wake_addr(p->clear_child_tid);
        p->clear_child_tid = 0;
    }

    /* The NT equivalent of the handshake above: signal the Thread object
     * that WaitForSingleObject(hThread) is blocked on, and give back this
     * thread's private TEB and stack. A no-op for anything NtCreateThreadEx
     * did not make. NtTerminateThread has already recorded the full 32-bit
     * code by the time it gets here; this code only lands if nothing did. */
    proc_nt_thread_exit(p, (uint32)status);

    /* Zombie, not freed: the status is still wanted. What CAN go now is
     * everything that holds a resource - descriptors, and the address space
     * if this process owned one. handle_close_all runs here rather than in
     * proc_free so a parent that never calls wait4 does not pin every file
     * its children ever opened.
     *
     * Only if this thread OWNS the table. A thread shares its leader's, and
     * closing it when one thread returns would shut the descriptors the rest
     * of the group is still using. */
    if (p->owns_files) {
        handle_close_all(p->handles);
    }

    if (resume_vfork_parent(p, frame)) {
        return frame->rax;
    }

    /* Not a vfork child, so nobody is suspended on this process specifically.
     * Tell the parent the ordinary way: SIGCHLD, plus a direct wakeup if it
     * is sitting in wait4. The flag is what keeps this from disturbing a
     * parent blocked on something else - waking a process that was waiting
     * for the keyboard would send it round its own loop for nothing. */
    /* A THREAD's exit tells nobody: it is not a child, its creator did
     * not fork it, and Linux sends no signal for it. It used to send its
     * creator SIGCHLD, and a program with a SIGCHLD handler saw a child
     * exit that no wait could ever find. */
    if (p->tgid == p->pid) {
        process_t *parent = proc_find(p->ppid);

        if (parent != NULL) {
            signal_send(parent, SIGCHLD);
            if (parent->waiting_for_child) {
                sched_wake(parent);
            }
        }
    } else {
        /* But a thread may be the LAST of a group whose leader already
         * exited alone - and that leader only becomes reapable now (see
         * proc_reap_child). Its parent's wait4 re-checks on waking, so a
         * wake that turns out early costs one pass round its loop. */
        process_t *leader = proc_find(p->tgid);

        if (leader != NULL && leader->state == PROC_ZOMBIE) {
            process_t *parent = proc_find(leader->ppid);

            if (parent != NULL && parent->waiting_for_child) {
                sched_wake(parent);
            }
        }
    }

    /* Off the run queue and away. A zombie is not runnable, so this never
     * comes back - the process's kernel stack stays parked until wait4 reaps
     * it, which is the only reason it is safe to switch off it here.
     *
     * The address space is NOT freed here: it is the one in CR3, and
     * vmm_space_destroy refuses the loaded space for the good reason that
     * tearing down the tables you are executing on faults on the next
     * instruction. proc_free does it at reap time, when it is not current. */
    sched_dequeue(p);
    schedule();

    /* schedule() returned, which means nothing else was runnable. Now the
     * halt is the truth rather than a missing scheduler. */
    print_string("\n[process ", 0x0E);
    print_hex((uint32)p->pid, 0x0E);
    print_string(" exited with status ", 0x0E);
    print_hex((uint32)p->exit_status, 0x0E);
    print_string(" - nothing left to run]\n", 0x0E);
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* --- wait4 ---------------------------------------------------------------
 * Under cooperative vfork a child was always already a zombie by the time the
 * parent ran again, so this never had to block. fork breaks that: the child
 * is scheduled independently and the parent reaches wait4 while it is still
 * running. So this blocks now, which is the half that was deferred.
 *
 * The loop shape matters. Reaping is attempted BEFORE the signal check, so a
 * parent woken by its own child's SIGCHLD reaps the child rather than
 * returning -EINTR and making the caller work out that it should try again. */
#define WNOHANG 1

/* Block until one of this process's children is reapable.
 *
 * EXTRACTED from sys_wait4 when waitid(2) arrived, rather than copied into
 * it. The loop below is four lines of policy wrapped around one line of
 * hard-won detail - the `sti; hlt; cli` and the exact ordering of the
 * waiting_for_child flag around the block - and a second copy of that in
 * waitid would be a second place to get it wrong, in a function whose bugs
 * present as a shell that hangs.
 *
 * Returns the child, or NULL with *err set to the negative errno to return.
 * *err is 0 for the WNOHANG "no child ready, and that is not an error" case,
 * which is the one result the caller cannot infer from a NULL. */
static process_t *wait_reap_blocking(process_t *p, uint64 options, int64 *err) {
    process_t *child;

    *err = 0;
    for (;;) {
        child = proc_reap_child(p);
        if (child != NULL) {
            return child;
        }
        if (!proc_has_children(p)) {
            *err = -10;              /* -ECHILD */
            return NULL;
        }
        if (options & WNOHANG) {
            return NULL;             /* children, none exited: not an error */
        }

        /* The flag goes up before the block and comes down after it, so an
         * exiting child either sees it and wakes us, or has already exited
         * and will be found by the reap at the top of the loop. */
        p->waiting_for_child = 1;
        sched_block(p);
        p->waiting_for_child = 0;

        if (proc_reap_child(p) != NULL) {
            continue;                /* reaped at the top of the loop */
        }
        if (signal_pending(p)) {
            *err = -4;               /* -EINTR */
            return NULL;
        }

        /* sched_block returns immediately when nothing else is runnable,
         * which here means every child is itself blocked - on the keyboard,
         * usually. Spinning would be a hard hang: SFMASK clears IF on every
         * SYSCALL, so the interrupt that would unblock a child can never
         * arrive while this loop holds the CPU. Same fix as kbd_wait, and
         * the same ordering: sti leaves interrupts off for exactly one more
         * instruction, so the wakeup cannot slip in before the hlt. */
        bkl_wait_for_interrupt();
    }
}

static uint64 sys_wait4(uint64 pid, uint64 status_ptr, uint64 options,
                        uint64 rusage) {
    process_t *p = proc_current();
    process_t *child;
    int64      err;

    (void)rusage;

    child = wait_reap_blocking(p, options, &err);
    if (child == NULL) {
        return (uint64)err;
    }

    if (pid != (uint64)-1 && (int)pid > 0 && child->pid != (int)pid) {
        return (uint64)-10;
    }

    if (status_ptr != 0) {
        if (!user_ptr_ok(status_ptr)) {
            return (uint64)-14;
        }
        /* The wait status encoding: exit code in bits 15:8, signal bits
         * clear. WEXITSTATUS shifts it back down, and a shell that reports
         * $? reads it from here. */
        *(int *)status_ptr = (child->exit_status & 0xFF) << 8;
    }

    {
        int reaped = child->pid;
        proc_free(child);
        return (uint64)reaped;
    }
}

/* waitid(2) - wait, and say what happened in a siginfo_t.
 *
 * --- why it is not just wait4 with a different output shape ---------------
 *
 * Two differences that callers depend on, and both are about being able to
 * distinguish cases wait4 cannot.
 *
 * IT RETURNS 0, NOT THE PID. The pid comes back inside the siginfo, which
 * means "no child was ready" under WNOHANG is reported as 0 with si_pid == 0
 * rather than as a return value that has to be told apart from a real pid.
 * wait4 overloads its return for both and a caller has to know that 0 is
 * special.
 *
 * IT SAYS WHY. si_code distinguishes CLD_EXITED from CLD_KILLED, so a caller
 * learns whether the status is an exit code or a signal number without the
 * WIFEXITED/WTERMSIG macro dance over a packed int.
 *
 * WEXITED IS MANDATORY IN THE OPTIONS, and this is checked rather than
 * assumed: waitid with no state flags set is a call that can never report
 * anything, and Linux answers -EINVAL. Accepting it would give a caller that
 * forgot the flag a wait that blocks forever.
 *
 * WSTOPPED and WCONTINUED parse and are then not satisfiable, because this
 * kernel does not keep stopped children - SIGSTOP's default action returns
 * without changing state (see signal.c's DFL_STOP). Accepted rather than
 * refused so that a caller passing WEXITED|WSTOPPED, which is ordinary, is
 * not rejected over the half that will simply never fire.
 *
 * WNOWAIT is refused. It means "report this child but leave it reapable",
 * which needs a zombie to survive being reported, and proc_reap_child hands
 * out a child that the caller is then expected to free. Refusing is honest;
 * silently reaping anyway would lose a child the caller expected to wait for
 * a second time. */
#define P_ALL   0
#define P_PID   1
#define P_PGID  2

#define WEXITED_    0x00000004UL
#define WSTOPPED_   0x00000002UL
#define WCONTINUED_ 0x00000008UL
#define WNOWAIT_    0x01000000UL

#define CLD_EXITED  1
#define CLD_KILLED  2

/* The x86-64 siginfo_t fields waitid fills, at their real offsets. Written as
 * a struct rather than as stores through a byte pointer so the offsets are
 * checkable by reading it, and padded to the full 128 bytes because a caller
 * declares siginfo_t and this writes all of it - leaving the tail untouched
 * would hand back whatever was on the caller's stack in fields it will read. */
struct k_siginfo {
    int32  si_signo;      /* 0  */
    int32  si_errno;      /* 4  */
    int32  si_code;       /* 8  */
    int32  pad0;          /* 12 */
    int32  si_pid;        /* 16 */
    int32  si_uid;        /* 20 */
    int32  si_status;     /* 24 */
    int32  pad1;          /* 28 */
    uint64 si_utime;      /* 32 */
    uint64 si_stime;      /* 40 */
    uint8  rest[128 - 48];
};

static uint64 sys_waitid(uint64 idtype, uint64 id, uint64 info_ptr,
                         uint64 options, uint64 rusage) {
    process_t      *p = proc_current();
    process_t      *child;
    struct k_siginfo si;
    int64           err;
    uint64          i;

    (void)rusage;

    if (p == NULL) {
        return (uint64)-22;
    }
    if (idtype != P_ALL && idtype != P_PID && idtype != P_PGID) {
        return (uint64)-22;
    }
    if (!(options & (WEXITED_ | WSTOPPED_ | WCONTINUED_))) {
        return (uint64)-22;          /* see the comment above */
    }
    if (options & WNOWAIT_) {
        return (uint64)-22;
    }
    if (info_ptr != 0 && !user_range_ok(info_ptr, sizeof(si))) {
        return (uint64)-14;
    }

    child = wait_reap_blocking(p, options, &err);
    if (child == NULL) {
        if (err != 0) {
            return (uint64)err;
        }
        /* WNOHANG, nothing ready. Zeroed siginfo and success - and the zeroed
         * si_pid IS the answer, so it has to be written rather than left
         * alone. A caller distinguishes this from a real report by testing
         * si_pid, which only works if this call clears it. */
        if (info_ptr != 0) {
            uint8 *dst = (uint8 *)info_ptr;
            for (i = 0; i < sizeof(si); i++) {
                dst[i] = 0;
            }
        }
        return 0;
    }

    if (idtype == P_PID && child->pid != (int)id) {
        return (uint64)-10;          /* -ECHILD, same limitation as wait4 */
    }

    for (i = 0; i < sizeof(si); i++) {
        ((uint8 *)&si)[i] = 0;
    }
    si.si_signo  = SIGCHLD;
    si.si_code   = CLD_EXITED;
    si.si_pid    = child->pid;
    si.si_status = child->exit_status & 0xFF;

    if (info_ptr != 0) {
        *(struct k_siginfo *)info_ptr = si;
    }
    proc_free(child);
    return 0;
}

/* --- execve --------------------------------------------------------------
 * Replace this process's image with another.
 *
 * The ordering below is the whole design. Everything that can fail happens
 * while the OLD address space is still live and still the one in CR3, so a
 * failure returns an errno to a process that never noticed. Once CR3 moves
 * there is no way back - the caller's code, stack and arguments are gone -
 * so nothing after that point is allowed to fail.
 *
 * Concretely: arguments and the file are copied into KERNEL memory first,
 * because both live in the address space about to be destroyed. Reading argv
 * after the switch reads the new image's memory at the old addresses, which
 * is not an error the CPU can catch. */

#define EXEC_ARG_MAX   32
#define EXEC_STR_BYTES 2048

struct exec_copy {
    const char *argv[EXEC_ARG_MAX + 1];
    const char *envp[EXEC_ARG_MAX + 1];
    char        buf[EXEC_STR_BYTES];
    uint64      used;
};

/* Copy one user string into the kernel buffer, returning a kernel pointer to
 * it, or NULL if it does not fit or the pointer is bad. */
static const char *exec_copy_str(struct exec_copy *ec, uint64 user_ptr) {
    const char *src = (const char *)user_ptr;
    char *dst;
    uint64 i = 0;

    if (!user_ptr_ok(user_ptr)) {
        return NULL;
    }
    dst = ec->buf + ec->used;
    while (ec->used + i < EXEC_STR_BYTES - 1) {
        char c = src[i];
        dst[i] = c;
        if (c == '\0') {
            ec->used += i + 1;
            return dst;
        }
        i++;
    }
    return NULL;   /* -E2BIG: the argument block is larger than we accept */
}

/* Copy a NULL-terminated vector of user string pointers. */
static int exec_copy_vector(struct exec_copy *ec, uint64 vec_ptr,
                            const char **out) {
    const uint64 *vec = (const uint64 *)vec_ptr;
    uint64 n = 0;

    if (vec_ptr == 0) {
        out[0] = NULL;
        return 0;
    }
    if (!user_ptr_ok(vec_ptr)) {
        return -14;
    }
    while (n < EXEC_ARG_MAX && vec[n] != 0) {
        out[n] = exec_copy_str(ec, vec[n]);
        if (out[n] == NULL) {
            return -7;    /* -E2BIG */
        }
        n++;
    }
    if (n >= EXEC_ARG_MAX && vec[n] != 0) {
        return -7;
    }
    out[n] = NULL;
    return 0;
}

/* How the PE loader reads a DLL.
 *
 * Passed in rather than called directly from pe.c, so the loader does not
 * have to know what a filesystem is - the same reason it writes through the
 * direct map rather than through the addresses it is mapping. The day there
 * are two volumes, or an ext4 one, this is the only line that knows. */
static int exec_read_file(const char *path, uint8 **out, uint32 *size) {
    /* The whole body moved into fs_read_whole. What is left is the signature
     * the PE loader is handed, which is the only reason this function still
     * exists - pe.c takes a read-a-file callback so it does not have to know
     * what a filesystem is, and that indirection is now one level thinner. */
    return fs_read_whole(path, out, size) == 0 ? 0 : -1;
}

static void exec_release_file(uint8 *buf) {
    fs_free_file(buf);
}

static uint64 sys_execve(uint64 path_ptr, uint64 argv_ptr, uint64 envp_ptr,
                         struct syscall_frame *frame) {
    process_t *p = proc_current();
    char resolved[PATH_MAX_LEN];
    struct exec_copy *ec = NULL;
    uint8 *image = NULL;
    uint32 size = 0;
    address_space_t *old_space, *new_space = NULL;
    elf_info_t info;
    personality_t new_personality = PERSONALITY_LINUX;
    uint64        new_gs_base   = 0;
    uint64        new_thread_start = 0;
    uint64        new_apc_dispatcher = 0;
    uint64        new_exc_dispatcher = 0;
    uint64        new_tls_va = 0, new_tls_pages = 0;
    uint64        tls_entry_via_ntdll = 0;
    /* Both zero for a static binary, and both are read unconditionally below.
     * interp_entry is a SEPARATE variable from info.entry rather than an
     * overwrite of it, because the two addresses are different and both are
     * needed at once: the CPU starts at the interpreter's, and AT_ENTRY has
     * to carry the program's for the interpreter to jump to when it is
     * done. Overwriting info.entry would make the auxv describe the rtld as
     * the program, and the rtld would re-enter itself. */
    uint64        interp_base   = 0;
    uint64        interp_entry  = 0;
    uint64 stack;
    int rc;

    if (!user_ptr_ok(path_ptr)) {
        return (uint64)-14;
    }
    if (path_normalize(p->cwd, (const char *)path_ptr,
                       resolved, sizeof(resolved)) != PATH_OK) {
        return (uint64)-36;
    }

    if (fs_root() == NULL) {
        return (uint64)-19;                     /* -ENODEV: nothing mounted */
    }

    /* --- everything below is still recoverable ------------------------- */

    ec = (struct exec_copy *)kmalloc(sizeof(struct exec_copy));
    if (ec == NULL) {
        return (uint64)-12;
    }
    ec->used = 0;
    rc = exec_copy_vector(ec, argv_ptr, ec->argv);
    if (rc == 0) {
        rc = exec_copy_vector(ec, envp_ptr, ec->envp);
    }
    if (rc != 0) {
        kfree(ec);
        return (uint64)(int64)rc;
    }

    /* One call where there were three: a size probe, an allocation, and a
     * read, each with its own FAT_ERR_ translation. fs_read_whole does all of
     * it and returns an errno, and the -ENOEXEC for an empty file moved in
     * there with it - execve is not the only caller that wants an image
     * rather than a zero-length buffer. */
    rc = fs_read_whole(resolved, &image, &size);
    if (rc != 0) {
        kfree(ec);
        return (uint64)(int64)rc;
    }

    new_space = vmm_space_create();
    if (new_space == NULL) {
        kfree(image);
        kfree(ec);
        return (uint64)-12;
    }

    /* Which loader, decided by the file and not by the name or an extension.
     * Two magic numbers is the whole test, and it is the same question the
     * personality tag answers for the rest of the process's life - so the tag
     * is set from the same branch that chose the loader rather than being
     * derived again later and getting a different answer.
     *
     * Either way the image is loaded through the direct map into a space that
     * is not in CR3, so the caller's mappings are untouched and a malformed
     * file is still just an errno. */
    if (pe_is_pe(image, size)) {
        pe_info_t pe;

        rc = pe_load_executable(new_space, image, size, &pe,
                                exec_read_file, exec_release_file);
        if (rc != PE_OK) {
            print_string("execve: ", 0x0C);
            print_string(pe_strerror(rc), 0x0C);
            print_string("\n", 0x0C);
            vmm_space_destroy(new_space);
            kfree(image);
            kfree(ec);
            return (uint64)-8;    /* -ENOEXEC */
        }
        /* elf_info_t is what the rest of execve reads. Only three of its
         * fields mean anything for a PE - there are no program headers to
         * publish through auxv, so phnum stays zero and the stack builder
         * emits an empty AT_PHDR rather than a fabricated one. */
        info.entry         = pe.entry;
        info.lowest_vaddr  = pe.image_base;
        info.highest_vaddr = pe.highest_vaddr;
        info.load_count    = pe.section_count;
        info.phdr_vaddr    = 0;
        info.phnum         = 0;
        info.phentsize     = 0;
        new_personality    = PERSONALITY_WINDOWS;
        new_thread_start   = pe.thread_start;
        new_apc_dispatcher = pe.apc_dispatcher;
        new_exc_dispatcher = pe.exception_dispatcher;

        /* The TEB and PEB, built by the kernel before the image runs -
         * exactly as NT does it, and it has to be that way round:
         * LdrInitializeThunk reads its arguments out of the block, so it
         * cannot be the thing that creates it. Built into the NEW address
         * space, so a failure here is still just -ENOEXEC. */
        rc = nt_process_init(new_space, pe.image_base,
                             USER_STACK_TOP, USER_STACK_SIZE,
                             p->pid, p->thread.tid);
        if (rc != 0) {
            vmm_space_destroy(new_space);
            kfree(image);
            kfree(ec);
            return (uint64)-12;   /* -ENOMEM */
        }

        /* The module table, for ntdll's exception unwinder. */
        {
            nt_module_table_t mt;
            int k;

            mt.magic = NT_MODULES_MAGIC;
            mt.count = 0;
            for (k = 0; k < NT_MAX_MODULES; k++) {
                mt.mod[k].base = mt.mod[k].size = 0;
            }
            for (k = 0; k < pe.mod_count && k < NT_MAX_MODULES; k++) {
                mt.mod[k].base = pe.mods[k].base;
                mt.mod[k].size = pe.mods[k].size;
                mt.count++;
            }
            (void)nt_modules_publish(new_space, &mt);
        }

        /* Implicit TLS: lay out one thread's area (the pointer array, then
         * each module's block, 16-aligned), publish the table in the PEB for
         * every later thread and for ntdll's callbacks, and build the main
         * thread's area now. */
        new_tls_va = 0;
        new_tls_pages = 0;
        if (pe.tls_count > 0) {
            nt_tls_table_t tt;
            uint64 off;
            int k;

            tt.magic = NT_TLS_MAGIC;
            tt.count = (uint32)pe.tls_count;
            off = ((uint64)pe.tls_count * 8 + 15) & ~15ULL;
            for (k = 0; k < NT_TLS_MAX_MODULES; k++) {
                nt_tls_module_t *m = &tt.mod[k];

                if (k >= pe.tls_count) {
                    m->module_base = m->start = m->end = m->zero_fill = 0;
                    m->index_addr = m->callbacks = m->block_offset = 0;
                    continue;
                }
                m->module_base  = pe.tls[k].module_base;
                m->start        = pe.tls[k].start;
                m->end          = pe.tls[k].end;
                m->zero_fill    = pe.tls[k].zero_fill;
                m->index_addr   = pe.tls[k].index_addr;
                m->callbacks    = pe.tls[k].callbacks;
                m->block_offset = off;
                off += ((m->end - m->start) + m->zero_fill + 15) & ~15ULL;
            }
            tt.area_bytes = off;
            if (off > NT_TLS_AREA_STRIDE || nt_tls_publish(new_space, &tt) != 0 ||
                nt_thread_tls_init(new_space, NT_TLS_MAIN_SLOT, NT_TEB_BASE,
                                   &new_tls_va, &new_tls_pages) != 0) {
                print_string("execve: implicit TLS does not fit\n", 0x0C);
                vmm_space_destroy(new_space);
                kfree(image);
                kfree(ec);
                return (uint64)-8;
            }
            /* TLS callbacks run in ring 3, so the main thread enters through
             * ntdll's RtlUserThreadStart - StartRoutine in RDX, as for every
             * other thread - which runs them (DLL_PROCESS_ATTACH) first. */
            if (pe.has_tls_callbacks && pe.thread_start != 0) {
                tls_entry_via_ntdll = pe.thread_start;
            }
        }

        /* RTL_USER_PROCESS_PARAMETERS: what the process was started with.
         *
         * The standard handles are the interesting part, and the answer was
         * already here. A Windows process gets no preassigned handles, so
         * GetStdHandle has to read them out of this block - but a PE exec'd
         * from ash inherited descriptors 0, 1 and 2 like any other program,
         * and an NT HANDLE is an encoding of an index into that same table.
         * So the three fields are the encodings of 0, 1 and 2, and the
         * console a Windows program writes to is the one the shell handed
         * it, redirection included. Nothing has to be opened here, which is
         * the whole argument for this over kernel32 opening \??\CON lazily:
         * that version cannot be redirected, because it invents a console
         * instead of inheriting one.
         *
         * Checked rather than assumed open. A descriptor closed before
         * execve leaves a zero, and zero is the one value NT guarantees is
         * never a valid handle - so a program that checks gets INVALID and a
         * program that does not gets a failed call, rather than either
         * getting whatever object happens to sit at index 0 later. */
        {
            nt_params_desc_t pd;

            pd.image_path = resolved;
            pd.cwd        = p->cwd;
            pd.argv       = ec->argv;
            pd.envp       = ec->envp;
            pd.std_input  = handle_get(p->handles, 0) != NULL
                                ? NT_HANDLE_FROM_INDEX(0) : 0;
            pd.std_output = handle_get(p->handles, 1) != NULL
                                ? NT_HANDLE_FROM_INDEX(1) : 0;
            pd.std_error  = handle_get(p->handles, 2) != NULL
                                ? NT_HANDLE_FROM_INDEX(2) : 0;

            rc = nt_process_params_init(new_space, &pd);
            if (rc != 0) {
                vmm_space_destroy(new_space);
                kfree(image);
                kfree(ec);
                return (uint64)(int64)rc;
            }
        }
        new_gs_base = NT_TEB_BASE;
    } else {
        rc = elf_load_into(new_space, image, size, &info);
        if (rc != ELF_OK) {
            vmm_space_destroy(new_space);
            kfree(image);
            kfree(ec);
            return (uint64)-8;    /* -ENOEXEC */
        }
        new_personality = PERSONALITY_LINUX;

        /* --- the dynamic linker ------------------------------------------
         *
         * PT_INTERP used to be ELF_ERR_DYNAMIC and this branch did not exist.
         * What it does now is what every Unix kernel does and no more: load a
         * SECOND image at a fixed base, and start the process at ITS entry
         * point instead of the program's.
         *
         * Everything after that - DT_NEEDED, symbol lookup, relocation - is
         * the interpreter's job in ring 3, and deliberately so. The kernel
         * resolving symbols would mean the kernel parsing untrusted dynamic
         * sections, and it would mean the PE side's export walk and this one
         * being two resolvers in kernel context rather than one in each
         * process. The whole reason this bullet moves LdrGetProcedureAddress
         * out of the kernel is the same reason this stops here.
         *
         * The interpreter is read through fs_read_whole, so it comes off
         * whatever volume is mounted at "/" - the same route the program came
         * from, which matters because a dynamic binary and its rtld arriving
         * from different volumes is a version skew with no error message. */
        if (info.has_interp) {
            uint8      *interp_image = NULL;
            uint32      interp_size  = 0;
            elf_info_t  interp_info;

            rc = fs_read_whole(info.interp, &interp_image, &interp_size);
            if (rc != 0) {
                vmm_space_destroy(new_space);
                kfree(image);
                kfree(ec);
                /* -ENOENT, not -ENOEXEC. "the interpreter is missing" and
                 * "this file is not executable" send you to different places,
                 * and a missing /lib/ld-gen.so is by far the more
                 * likely of the two on a volume that was just staged. */
                return (uint64)(int64)rc;
            }

            rc = elf_load_biased(new_space, interp_image, interp_size,
                                 ELF_INTERP_BASE, &interp_info);
            fs_free_file(interp_image);
            if (rc != ELF_OK) {
                vmm_space_destroy(new_space);
                kfree(image);
                kfree(ec);
                return (uint64)-8;    /* -ENOEXEC */
            }
            /* An interpreter that itself wants an interpreter is refused
             * rather than followed. One level is what the format means; a
             * chain is a malformed rtld or a loop, and following it costs a
             * recursion the loader has no depth cap for. The PE side capped
             * dependency depth at 4 for the same reason. */
            if (interp_info.has_interp) {
                vmm_space_destroy(new_space);
                kfree(image);
                kfree(ec);
                return (uint64)-8;
            }

            /* Start at the RTLD's entry; AT_ENTRY still carries the
             * program's. info.entry is left alone precisely so that the auxv
             * built below is right - the two addresses are different and both
             * are needed, which is why interp_entry is a separate variable
             * rather than an overwrite. */
            interp_base  = ELF_INTERP_BASE;
            interp_entry = interp_info.entry;
        }
    }

    /* --- point of no return -------------------------------------------- */

    /* Every OTHER thread of this group dies here, and it has to be here: past
     * this line the address space they are all running in is replaced, so a
     * surviving thread would resume at an instruction that no longer exists.
     *
     * POSIX says exec makes the calling thread the only one. This is the
     * rule, and it is also the only safe thing - the alternative is not "the
     * threads keep running", it is "the threads run whatever bytes the new
     * image happens to have at their old instruction pointers".
     *
     * Before vmm_switch_to rather than after, so proc_retire still reads the
     * tables it expects. Nothing below can fail, so no thread is killed for
     * an exec that then does not happen. */
    kill_thread_group(p, 0);

    old_space = p->space;
    vmm_switch_to(new_space);
    p->space = new_space;

    /* A vfork child was running in its parent's address space. It owns this
     * one now, and the old one must survive - the parent is suspended in it
     * and will resume there. */
    if (p->shares_space) {
        p->shares_space = 0;
    } else if (old_space != vmm_kernel_space()) {
        vmm_space_destroy(old_space);
    }

    /* The only reason HANDLE_CLOEXEC exists. Descriptors the new image must
     * not inherit go now, after the switch, because nothing before this is
     * allowed to have side effects the failure paths would need to undo. */
    handle_close_on_exec(p->handles);

    /* Caught signals do not survive exec. The handler address belongs to the
     * image being replaced, so keeping it means the first SIGINT in the new
     * program jumps to whatever now sits at that address. Ignored signals DO
     * survive, and that asymmetry is load-bearing: it is how a shell starts a
     * background job that does not die on Ctrl-C.
     *
     * This was invisible until fork existed. Every child used to arrive from
     * proc_alloc with a table full of SIG_DFL, so there was nothing to reset;
     * a forked child inherits its parent's, and a shell's SIGINT handler
     * inherited into every command it runs is a program jumping into the
     * shell's address space. */
    {
        int k;

        for (k = 0; k < SIG_COUNT; k++) {
            if (p->sig_handlers[k].handler != SIG_IGN_HANDLER) {
                p->sig_handlers[k].handler  = SIG_DFL_HANDLER;
                p->sig_handlers[k].flags    = 0;
                p->sig_handlers[k].restorer = 0;
                p->sig_handlers[k].mask     = 0;
            }
        }
    }

    /* The new image has its own TLS and sets it up itself; the old thread
     * pointer names memory that no longer exists. */
    p->thread.fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);

    /* And the same is true of clear_child_tid, for the same reason - it is a
     * USER ADDRESS in the image being replaced. Linux clears it on execve
     * too. Without this, syscall_exit_process writes a zero through it when
     * the new program eventually exits, into whatever now lives at that
     * address in the NEW address space.
     *
     * How this was found is worth recording, because the failure it usually
     * produces is silent. It faulted only when the new image was a PE:
     * busybox forks at 0x400000, execs /bin/k32.exe which lives at
     * 0x140000000, and the inherited address is then unmapped, so the exit
     * path took a #PF in supervisor mode. Every ELF exec had been getting
     * away with it - a second static musl binary maps the same region at the
     * same address, so the write landed on a real page belonging to the new
     * program and quietly corrupted four bytes of it instead of faulting.
     * The PE case is not a different bug, it is the same one finally
     * hitting an unmapped page.
     *
     * user_ptr_ok in syscall_exit_process cannot catch this: the address IS
     * a legal user address, it just is not this process's any more. */
    p->clear_child_tid = 0;
    /* An ELF process gets no GS base and a PE process gets its TEB. Set from
     * the same branch that chose the loader, for the same reason the
     * personality tag is: two places deciding what kind of image this is can
     * disagree, and one of them will. */
    p->thread.gs_base = new_gs_base;
    syscall_set_user_gs_base(new_gs_base);

    /* And a clean FPU. The old image's rounding mode and exception mask are
     * no more the new image's business than its registers are, and restoring
     * immediately matters because execve does not go through a context
     * switch on its way back to ring 3. */
    fpu_thread_init(p->thread.fpu_state);
    fpu_restore(p->thread.fpu_state);

    p->mmap_next = USER_MMAP_BASE;
    user_set_brk(info.highest_vaddr);
    /* Set from the branch that chose the loader, above, so the tag and the
     * image can never disagree about what was actually mapped. */
    p->personality = new_personality;
    /* Where NtCreateThreadEx will start this image's threads. Replaced on
     * every exec, ELF included (0), because it is an address in the image
     * being thrown away. */
    p->nt_thread_start = new_thread_start;
    p->nt_apc_dispatcher = new_apc_dispatcher;
    p->nt_exc_dispatcher = new_exc_dispatcher;
    nt_apc_flush(p);                     /* addresses in the old image */
    /* The main thread's implicit-TLS area, in the space just installed (the
     * old one's is gone with it). */
    p->nt_tls_va    = new_tls_va;
    p->nt_tls_pages = new_tls_pages;

    stack = user_stack_create(USER_STACK_TOP, USER_STACK_SIZE,
                              ec->argv, ec->envp,
                              info.phdr_vaddr, info.phnum,
                              info.phentsize, info.entry, interp_base);

    kfree(image);
    kfree(ec);

    if (stack == 0) {
        /* Out of memory after the old image is gone. There is nothing to
         * return to and no process left to return it to - this is exactly
         * the case a real kernel answers with SIGKILL, and will once signals
         * exist. Until then, say so rather than jumping to a garbage RSP. */
        print_string("execve: no memory for the new stack, and the old image "
                     "is already gone\n", 0x4F);
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    /* Leave the kernel somewhere new. sysretq takes RIP from the frame's rcx
     * slot and RSP from the per-CPU block, so rewriting both redirects the
     * ordinary return path instead of needing a second one. */
    /* The interpreter's entry when there is one, the program's otherwise.
     * Decided HERE rather than by the loader, because "which address does the
     * CPU start at" is execve's question - the loader's job ended at "both
     * images are mapped and here is where each of them went". */
    frame->rip    = (interp_entry != 0) ? interp_entry : info.entry;
    frame->rflags = 0x202;    /* IF set, bit 1 reserved-one; a clean start */
    syscall_set_user_rsp(stack);
    if (new_personality == PERSONALITY_WINDOWS) {
        /* A PE's entry point is entered as a Win64 FUNCTION: RSP 8 mod 16,
         * as if a call had just pushed a return address - a zero one here,
         * which is also where an exception unwinder's walk ends - with the
         * 32-byte home area above it. The argv block the ELF path builds
         * is not what a PE reads (it has the PEB), and its 16-aligned RSP
         * is exactly wrong: compiled code that keeps SSE values on the
         * stack with movaps takes #GP on its first one. */
        *(uint64 *)((stack & ~0xFULL) - 40) = 0;
        syscall_set_user_rsp((stack & ~0xFULL) - 40);
    }
    if (tls_entry_via_ntdll != 0) {
        /* A PE with TLS callbacks: into RtlUserThreadStart(RDX = the image's
         * entry), on a Win64-shaped stack - 40 bytes below the aligned top
         * for the home area and a (never used) return slot. */
        frame->rip = tls_entry_via_ntdll;
        frame->rdx = info.entry;
        frame->r8  = 0;
        syscall_set_user_rsp((stack & ~0xFULL) - 40);
    }

    return 0;
}

/* --- signals -------------------------------------------------------------- */

static uint64 sys_rt_sigaction(uint64 signo, uint64 act_ptr, uint64 old_ptr,
                               uint64 sigsetsize) {
    process_t *p = proc_current();
    const struct k_sigaction *act = (const struct k_sigaction *)act_ptr;
    struct k_sigaction *old = (struct k_sigaction *)old_ptr;

    /* A libc built for a different signal-set width would silently agree with
     * a kernel that ignored this, and then disagree about every mask. */
    if (sigsetsize != 8) {
        return (uint64)-22;
    }
    if (signo < 1 || signo >= SIG_COUNT) {
        return (uint64)-22;
    }
    /* Refusing to install a handler for these is what makes them reliable.
     * A process that could catch SIGKILL could not be killed. */
    if (signo == SIGKILL || signo == SIGSTOP) {
        return (uint64)-22;
    }

    if (old_ptr != 0) {
        if (!user_ptr_ok(old_ptr)) {
            return (uint64)-14;
        }
        *old = p->sig_handlers[signo];
    }
    if (act_ptr != 0) {
        if (!user_ptr_ok(act_ptr)) {
            return (uint64)-14;
        }
        p->sig_handlers[signo] = *act;
    }
    return 0;
}

static uint64 sys_rt_sigprocmask(uint64 how, uint64 set_ptr, uint64 old_ptr,
                                 uint64 sigsetsize) {
    process_t *p = proc_current();

    if (sigsetsize != 8) {
        return (uint64)-22;
    }
    if (old_ptr != 0) {
        if (!user_ptr_ok(old_ptr)) {
            return (uint64)-14;
        }
        *(uint64 *)old_ptr = p->sig_blocked;
    }
    if (set_ptr != 0) {
        uint64 set;

        if (!user_ptr_ok(set_ptr)) {
            return (uint64)-14;
        }
        set = *(uint64 *)set_ptr;
        /* Silently dropped rather than rejected, which is what Linux does:
         * a program blocking every signal generically should not have to
         * special-case the two it cannot. */
        set &= ~(sigmask_of(SIGKILL) | sigmask_of(SIGSTOP));

        switch (how) {
            case 0: p->sig_blocked |= set;  break;   /* SIG_BLOCK   */
            case 1: p->sig_blocked &= ~set; break;   /* SIG_UNBLOCK */
            case 2: p->sig_blocked  = set;  break;   /* SIG_SETMASK */
            default: return (uint64)-22;
        }
    }
    return 0;
}

static uint64 sys_kill(uint64 pid, uint64 signo) {
    process_t *target;

    if (signo == 0) {
        /* Signal 0 tests whether the process exists without sending
         * anything - a shell uses it to check whether a job is still alive. */
        return proc_find((int)(int64)pid) != NULL ? 0 : (uint64)-3;
    }
    if (signo >= SIG_COUNT) {
        return (uint64)-22;
    }

    /* Negative pid means the process group of its absolute value; 0 means the
     * caller's own group. Both matter to a shell, which kills jobs by group
     * rather than by pid. */
    if ((int64)pid == 0) {
        signal_send_group(proc_current()->pgid, (int)signo);
        return 0;
    }
    if ((int64)pid < 0) {
        signal_send_group((int)-(int64)pid, (int)signo);
        return 0;
    }

    target = proc_find((int)pid);
    if (target == NULL) {
        return (uint64)-3;                   /* -ESRCH */
    }
    signal_send(target, (int)signo);
    return 0;
}

/* --- process groups ------------------------------------------------------ */

static uint64 sys_setpgid(uint64 pid, uint64 pgid) {
    process_t *p = (pid == 0) ? proc_current() : proc_find((int)pid);

    if (p == NULL) {
        return (uint64)-3;
    }
    /* pgid 0 means "make this process a group leader", i.e. the group id
     * equals the pid. That is how a shell creates a job. */
    p->pgid = (pgid == 0) ? p->pid : (int)pgid;
    return 0;
}

static uint64 sys_getpgid(uint64 pid) {
    process_t *p = (pid == 0) ? proc_current() : proc_find((int)pid);

    if (p == NULL) {
        return (uint64)-3;
    }
    return (uint64)p->pgid;
}

/* --- reboot -------------------------------------------------------------
 * Fifteen lines that save a window-close per test cycle.
 *
 * There is no ACPI here, so this uses the shutdown port QEMU and Bochs
 * implement: writing 0x2000 to 0x604 powers the machine off. On real hardware
 * that port is not shutdown and the write is ignored, which is why the reset
 * path below follows it - the 8042 pulse works on anything with a keyboard
 * controller, and a triple fault works on everything. */
static uint64 sys_reboot(uint64 magic1, uint64 magic2, uint64 cmd, uint64 arg) {
    (void)magic1; (void)magic2; (void)arg;

    /* LINUX_REBOOT_CMD_POWER_OFF. Everything else - restart, halt - ends the
     * same way here, because there is nothing to unmount and nothing to
     * flush: the filesystem is read-only. */
    print_string("\n[system going down]\n", 0x0E);

    if (cmd == 0x4321FEDCu) {          /* POWER_OFF */
        outw(0x604, 0x2000);           /* QEMU, Bochs                       */
        outw(0xB004, 0x2000);          /* older QEMU                        */
    }

    /* Reset via the 8042: pulse the CPU reset line. */
    {
        int timeout = 100000;
        while ((inb(0x64) & 0x02) && timeout-- > 0) {
        }
        outb(0x64, 0xFE);
    }

    /* Nothing worked, so stop. Halting with interrupts off is the honest end
     * state - the alternative is returning to a process that believes the
     * machine is off. */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
    return 0;   /* unreachable; the compiler cannot see that hlt never ends */
}

/* --- terminal ioctls ----------------------------------------------------
 * This used to return 0 for every request, which is a worse answer than it
 * looks. A success return on TCGETS tells the caller the termios struct it
 * passed has been filled in, when nothing was written to it - so the shell
 * reads uninitialised stack as terminal settings and decides what to do from
 * whatever was there. Unknown requests get -ENOTTY now.
 *
 * TCSETS is accepted and IGNORED. The kernel line discipline is always
 * canonical with echo, so a program that asks for raw mode is told yes and
 * gets cooked input anyway. That is a real lie and it has one specific
 * consequence: busybox must be built with FEATURE_EDITING off, or ash will
 * switch to raw mode, echo every character itself, and you will see each
 * keystroke twice. */
/* How many bytes the argument points at, decoded from the request number.
 *
 * Linux packs direction, size, type and number into the ioctl code -
 * dir(2) size(14) type(8) nr(8) - so a driver's control op can be told how
 * big its argument is without a table mapping every code to a length. That is
 * exactly what device_ops_t::control's arg_size parameter wants and what
 * sys_ioctl had no way to supply.
 *
 * Zero for the legacy terminal codes (TCGETS is 0x5401, with nothing in the
 * size field), which is correct: those never reach a driver, and the termios
 * block below knows its own sizes because they are fixed by the ABI. */
static uint64 ioctl_arg_size(uint64 request) {
    return (request >> 16) & 0x3FFFULL;
}

static uint64 sys_ioctl(uint64 fd, uint64 request, uint64 arg) {
    process_t   *proc = proc_current();
    open_file_t *f = handle_get(proc->handles, (int)fd);
    uint8 *p = (uint8 *)arg;
    device_t *dev;

    /* --- what this used to be ---------------------------------------------
     *
     * `if (fd > 2) return -EBADF;` and then a termios table. Two things were
     * wrong with it, and the second is the one with teeth:
     *
     *   An ioctl on a perfectly good descriptor above 2 answered -EBADF,
     *   which tells a caller its descriptor is closed. -ENOTTY is the answer
     *   for a valid descriptor that is not a terminal.
     *
     *   Worse: fd 0, 1 and 2 got the terminal answers WHATEVER THEY POINTED
     *   AT. With stdin redirected from a file, TCGETS still returned a
     *   fully-populated termios describing a console that was not there - so
     *   isatty() said yes for a redirected stdin, which is precisely the
     *   question isatty exists to answer. Every program that decides whether
     *   to colourise or to prompt asks it.
     *
     * And device_ops_t::control - documented as "everything that is not a
     * byte transfer: geometry, eject, media-change polling, and later an
     * ioctl surface" - had no caller at all. ROADMAP item 14 lists that as
     * the one bug-shaped inconsistency in the object manager. This is the
     * caller. */
    if (f == NULL || f->obj == NULL) {
        return (uint64)-9;    /* -EBADF */
    }

    /* KDSETMODE takes its argument BY VALUE (KD_TEXT is 0, KD_GRAPHICS 1),
     * so it is answered before the pointer check below, which would call
     * KD_GRAPHICS a bad address. Console only, like every terminal ioctl
     * here. The owner is recorded so that a program which exits holding the
     * display loses it - see screen_set_graphics. */
    if (request == KDSETMODE && f->obj == tty_console()) {
        if (arg != KD_TEXT && arg != KD_GRAPHICS) {
            return (uint64)-22;
        }
        screen_set_graphics(arg == KD_GRAPHICS, proc->tgid);
        return 0;
    }

    if (arg != 0 && (arg < 0x1000ULL || arg >= 0x0000800000000000ULL)) {
        return (uint64)-14;   /* -EFAULT */
    }

    /* Not the console: a device gets its control op, anything else gets
     * -ENOTTY. Checked by IDENTITY rather than by descriptor number, which is
     * the whole of the redirection fix. */
    if (f->obj != tty_console()) {
        dev = dev_from_object(f->obj);
        if (dev != NULL) {
            return (uint64)(int64)dev_control(dev, (uint32)request,
                                              (void *)arg,
                                              ioctl_arg_size(request));
        }
        return (uint64)-25;   /* -ENOTTY: a valid fd that is not a terminal */
    }

    switch (request) {
        case TCGETS: {
            /* Kernel struct termios: four 32-bit flag words, c_line, then
             * c_cc[19]. Describing the console honestly - canonical, echoing,
             * mapping CR to NL on input and NL to CRLF on output - is what
             * makes a libc treat it as a line-oriented terminal. */
            if (arg == 0) {
                return (uint64)-14;
            }
            fill(p, 0, 36);
            *(uint32 *)(p +  0) = 0x0100;              /* c_iflag: ICRNL     */
            *(uint32 *)(p +  4) = 0x0005;              /* c_oflag: OPOST|ONLCR */
            *(uint32 *)(p +  8) = 0x00BF;              /* c_cflag: 38400 8N1 */
            *(uint32 *)(p + 12) = 0x8A3B;              /* c_lflag: ISIG|ICANON|ECHO|ECHOE|ECHOK|IEXTEN */
            p[17 +  0] = 3;                            /* VINTR  = Ctrl-C    */
            p[17 +  1] = 28;                           /* VQUIT  = Ctrl-\    */
            p[17 +  2] = 0x7F;                         /* VERASE = DEL       */
            p[17 +  3] = 21;                           /* VKILL  = Ctrl-U    */
            p[17 +  4] = 4;                            /* VEOF   = Ctrl-D    */
            p[17 +  6] = 1;                            /* VMIN               */
            return 0;
        }

        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            return 0;         /* accepted, ignored - see the note above */

        case TIOCGPGRP:
            if (arg == 0) {
                return (uint64)-14;
            }
            *(int *)p = tty_foreground_pgid();
            return 0;

        case TIOCSPGRP:
            if (arg == 0) {
                return (uint64)-14;
            }
            /* The shell claiming the terminal for a job. Everything typed at
             * the keyboard - Ctrl-C in particular - goes to this group from
             * here on. */
            tty_set_foreground_pgid(*(const int *)p);
            return 0;

        case KDGETMODE:
            if (arg == 0) {
                return (uint64)-14;
            }
            *(int *)p = screen_graphics_owner() != 0 ? KD_GRAPHICS : KD_TEXT;
            return 0;

        case TIOCGWINSZ:
            if (arg == 0) {
                return (uint64)-14;
            }
            /* The console's real grid: 80x25 in text mode, larger once it
             * draws into a framebuffer. */
            *(uint16 *)(p + 0) = (uint16)screen_height_chars();  /* ws_row */
            *(uint16 *)(p + 2) = (uint16)screen_width_chars();   /* ws_col */
            *(uint16 *)(p + 4) = 0;    /* ws_xpixel */
            *(uint16 *)(p + 6) = 0;    /* ws_ypixel */
            return 0;

        default:
            return (uint64)-25;   /* -ENOTTY */
    }
}

/* --- vectored write -----------------------------------------------------
 * Everything busybox's ash prints goes through writev rather than write: it
 * assembles a message from pieces ("sh: ", the command, ": not found") and
 * hands the kernel the list instead of concatenating it into a buffer first.
 * A kernel with write but not writev therefore looks like it works right up
 * until the program has something to say, and then goes silent - which is how
 * a shell reporting a real error turns into a bare exit code with nothing to
 * explain it. */

/* The x86-64 iovec: two 64-bit words, base then length. */
struct iovec {
    uint64 base;
    uint64 len;
};

/* --- Part 17's gap-fill --------------------------------------------------
 *
 * The audit's method was mechanical: build a POSIX-ish checklist by
 * category, cross-reference it against what this table actually dispatches,
 * and close what is missing. It found 65 of 107. Everything below is a gap
 * it named that could be closed WITHOUT a new filesystem operation.
 *
 * The fourteen it named that could not - mkdir, rmdir, unlink, rename,
 * symlink, link, chmod, chown and the rest of the file-NAMESPACE cluster -
 * are all blocked on the same thing: fs_ops_t (kernel/include/fs.h) has
 * lookup, read, write and iterate, and no mutation slots at all. That is a
 * filesystem change, not a syscall one, and it is recorded in ROADMAP with
 * a concrete description rather than half-answered here. Returning -ENOSYS
 * from a mkdir that could have worked would be worse than not having it. */

/* readv, the mirror of writev. Same iovec walk, same partial-success rule -
 * and the same bound, for the same reason: an unbounded count walks the
 * kernel off the end of the array. */
static uint64 sys_readv(uint64 fd, uint64 iov_ptr, uint64 iovcnt) {
    const struct iovec *iov = (const struct iovec *)iov_ptr;
    uint64 total = 0;
    uint64 i;

    if (iovcnt > 1024) {
        return (uint64)-22;
    }
    if (iovcnt != 0 &&
        (iov_ptr < 0x1000ULL || iov_ptr >= 0x0000800000000000ULL)) {
        return (uint64)-14;
    }
    for (i = 0; i < iovcnt; i++) {
        uint64 rc;

        if (iov[i].len == 0) {
            continue;
        }
        rc = (uint64)do_read((int)fd, (void *)iov[i].base, iov[i].len);
        if ((int64)rc < 0) {
            return total != 0 ? total : rc;
        }
        total += rc;
        if (rc < iov[i].len) {
            break;            /* short read - end of file */
        }
    }
    return total;
}

/* pread/pwrite: read or write at an explicit offset WITHOUT moving the
 * descriptor's own. That last part is the whole point of them, and it is
 * why they are not lseek-then-read: two threads sharing a descriptor would
 * race on the seek. */
static uint64 sys_pread64(uint64 fd, uint64 buf, uint64 count, uint64 off) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);
    uint64 saved;
    uint64 rc;

    if (f == NULL) {
        return (uint64)-9;
    }
    saved = f->offset;
    f->offset = off;
    rc = (uint64)do_read((int)fd, (void *)buf, count);
    f->offset = saved;
    return rc;
}

static uint64 sys_pwrite64(uint64 fd, uint64 buf, uint64 count, uint64 off) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);
    uint64 saved;
    uint64 rc;

    if (f == NULL) {
        return (uint64)-9;
    }
    saved = f->offset;
    f->offset = off;
    rc = (uint64)do_write((int)fd, (const void *)buf, count);
    f->offset = saved;
    return rc;
}

/* fsync/fdatasync/msync all succeed, and that is TRUE rather than
 * convenient: kernel/bcache.c is WRITE-THROUGH, so every write has already
 * reached the device by the time the write syscall returned. There is
 * nothing buffered for these to flush. If the cache ever becomes
 * write-back, these three become real work and this comment is where to
 * start. */
static uint64 sys_fsync(uint64 fd) {
    process_t *p = proc_current();

    return handle_get(p->handles, (int)fd) == NULL ? (uint64)-9 : 0;
}

/* madvise is advisory by definition - a kernel is always permitted to
 * ignore it. Succeeding is correct, not a stub. */
static uint64 sys_madvise(void) {
    return 0;
}

/* umask: the bits open(O_CREAT) and mkdir(2) clear from the mode they are
 * asked for - applied in sys_openat and sys_mkdir, enforced by whichever
 * filesystem has permission bits to store (gnfs; FAT has none). Per-process,
 * inherited across fork, 022 for a process the kernel creates. */
static uint64 sys_umask(uint64 mask) {
    process_t *p = proc_current();
    uint64 old = p->umask;

    p->umask = mask & 0777;
    return old;
}

/* sched_yield: give up the rest of the quantum. Real - it goes through the
 * same schedule() every preemption does. */
static uint64 sys_sched_yield(void) {
    schedule();
    return 0;
}

/* --- CPUs: affinity and "where am I" ---------------------------------------
 *
 * sched_setaffinity / sched_getaffinity name a THREAD (pid 0 is the caller;
 * any other number is a tid, as on Linux), and the mask is a bit per CPU.
 * The mask a thread may be given is intersected with the CPUs that exist;
 * one naming none of them is EINVAL, which is Linux's answer too. Moving a
 * thread off the CPU it is running on happens at once - sched_set_affinity
 * makes that CPU reschedule.
 *
 * The length rules are Linux's, because musl's sysconf(_SC_NPROCESSORS_ONLN)
 * reads the CPU count out of sched_getaffinity's mask and passes 128 bytes:
 * the buffer must hold at least the kernel's mask (8 bytes, 64 CPUs) and be
 * a whole number of longs, and the return value is the byte count written. */
static process_t *affinity_target(uint64 pid) {
    process_t *p;

    if (pid == 0) {
        return proc_current();
    }
    p = proc_find((int)pid);
    if (p == NULL || p->is_kthread || p->state == PROC_ZOMBIE) {
        return NULL;
    }
    return p;
}

static uint64 sys_sched_setaffinity(uint64 pid, uint64 len, uint64 mask_ptr) {
    process_t *p = affinity_target(pid);
    uint64 mask = 0;
    uint64 i;

    if (p == NULL) {
        return (uint64)-3;                          /* -ESRCH */
    }
    if (len == 0 || !user_range_ok(mask_ptr, len)) {
        return (uint64)-14;
    }
    for (i = 0; i < len && i < sizeof(uint64); i++) {
        mask |= (uint64)((const uint8 *)mask_ptr)[i] << (8 * i);
    }
    return (uint64)(int64)sched_set_affinity(p, mask);
}

static uint64 sys_sched_getaffinity(uint64 pid, uint64 len, uint64 mask_ptr) {
    process_t *p = affinity_target(pid);
    uint64 mask, i;

    if (p == NULL) {
        return (uint64)-3;
    }
    if (len < sizeof(uint64) || (len & (sizeof(uint64) - 1)) != 0) {
        return (uint64)-22;
    }
    if (!user_range_ok(mask_ptr, sizeof(uint64))) {
        return (uint64)-14;
    }
    mask = p->affinity & smp_online_mask();
    for (i = 0; i < sizeof(uint64); i++) {
        ((uint8 *)mask_ptr)[i] = (uint8)(mask >> (8 * i));
    }
    return sizeof(uint64);
}

/* getcpu(cpu, node, cache): the CPU this thread is on at the moment of the
 * call - true when it is answered and possibly stale the instant after,
 * which is the documented contract. One NUMA node. */
static uint64 sys_getcpu(uint64 cpu_ptr, uint64 node_ptr) {
    if (cpu_ptr != 0) {
        if (!user_range_ok(cpu_ptr, sizeof(uint32))) {
            return (uint64)-14;
        }
        *(uint32 *)cpu_ptr = (uint32)smp_cpu_index();
    }
    if (node_ptr != 0) {
        if (!user_range_ok(node_ptr, sizeof(uint32))) {
            return (uint64)-14;
        }
        *(uint32 *)node_ptr = 0;
    }
    return 0;
}

/* Sessions. A session is a process-group container, and Genesis tracks the
 * group already - so a session id is the pid of whoever called setsid, and
 * setsid starts a new group at the same time, which is POSIX's rule. */
static uint64 sys_setsid(void) {
    process_t *p = proc_current();

    /* POSIX: fails if the caller is already a group leader, because a
     * session leader must be a NEW group and its pid is already taken as a
     * group id. Refusing rather than quietly re-using it. */
    if (p->pgid == p->pid) {
        return (uint64)-1;            /* -EPERM */
    }
    p->sid  = p->pid;
    p->pgid = p->pid;
    return p->pid;
}

static uint64 sys_getsid(uint64 pid) {
    process_t *p = pid == 0 ? proc_current() : proc_find(pid);

    if (p == NULL) {
        return (uint64)-3;            /* -ESRCH */
    }
    return p->sid != 0 ? p->sid : p->pgid;
}

/* setuid/setgid.
 *
 * Root may become anyone. Anyone else may only "become" who they already
 * are, which is a no-op that succeeds - POSIX requires that much, because a
 * program calling setuid(getuid()) to shed privilege it does not have must
 * not fail.
 *
 * There is no saved-set-user-id here, so there is also no coming back. On a
 * real Unix a non-root process can move between its real and saved ids;
 * Genesis carries one uid per process (see process_t::uid), so dropping
 * privilege is one-way. That is the safe direction to be wrong in, and it is
 * refused explicitly rather than by silently doing nothing.
 */
static uint64 sys_setuid(uint64 uid) {
    process_t *p = proc_current();

    if (p->uid != 0 && (uint32)uid != p->uid) {
        return (uint64)-1;            /* -EPERM */
    }
    p->uid = (uint32)uid;
    return 0;
}

static uint64 sys_setgid(uint64 gid) {
    process_t *p = proc_current();

    if (p->uid != 0 && (uint32)gid != p->gid) {
        return (uint64)-1;
    }
    p->gid = (uint32)gid;
    return 0;
}

/* setresuid/getresuid. Genesis has one uid per process rather than the
 * real/effective/saved triple, so all three answer the same value - and
 * setresuid refuses any request that would make them differ, instead of
 * accepting it and silently collapsing three ids into one. */
static uint64 sys_setresuid(uint64 r, uint64 e, uint64 sv) {
    process_t *p = proc_current();

    if ((r != (uint64)-1 && e != (uint64)-1 && r != e) ||
        (e != (uint64)-1 && sv != (uint64)-1 && e != sv)) {
        return (uint64)-22;           /* -EINVAL: cannot represent it */
    }
    if (r != (uint64)-1) {
        p->uid = (uint32)r;
    } else if (e != (uint64)-1) {
        p->uid = (uint32)e;
    }
    return 0;
}

/* prctl. Real Linux uses this syscall as its own extension point - an op
 * code plus arguments, no new syscall number needed - which is exactly the
 * shape "grant one user root+SYSTEM+TrustedInstaller power" needs and a
 * bare new syscall number would not have been: this kernel's numbers are
 * real Linux amd64 ones throughout (see syscall.h), and inventing a new one
 * risks colliding with a number Linux assigns later. The three ops below
 * are given magic values ("GEN" + a byte) rather than small integers for the
 * same reason applied to Genesis's own additions.
 *
 * GRANT and REVOKE are root-only - a non-root process granting itself
 * supreme would just be a syscall-shaped backdoor, which is not what "give
 * one user the permission" asked for. QUERY is open to anyone, the same
 * posture getresuid already has: knowing who currently holds it is not
 * itself a privilege. Every other op falls through unrecognised and is
 * accepted-and-ignored, which is what this whole syscall used to be
 * unconditionally. */
#define PR_GENESIS_GRANT_SUPREME  0x47454e01u
#define PR_GENESIS_REVOKE_SUPREME 0x47454e02u
#define PR_GENESIS_QUERY_SUPREME  0x47454e03u

static uint64 sys_prctl(uint64 op, uint64 arg1) {
    process_t *p = proc_current();

    switch ((uint32)op) {
    case PR_GENESIS_GRANT_SUPREME:
        if (p->uid != 0) {
            return (uint64)-1;            /* -EPERM */
        }
        genesis_supreme_uid_set((int)arg1);
        return 0;
    case PR_GENESIS_REVOKE_SUPREME:
        if (p->uid != 0) {
            return (uint64)-1;            /* -EPERM */
        }
        genesis_supreme_uid_set(-1);
        return 0;
    case PR_GENESIS_QUERY_SUPREME:
        return (uint64)(int64)genesis_supreme_uid_get();
    default:
        return 0;
    }
}

/* getgroups/setgroups. The supplementary group list is what makes a group@
 * or ACE_IDENTIFIER_GROUP entry in an ACL mean anything for a user whose
 * primary gid is something else - without it, every group grant a process
 * legitimately holds evaluates to "no".
 *
 * setgroups is root-only, which is not decoration: a process that could add
 * itself to a group could grant itself any group-conferred access on the
 * system, and the ACL evaluator would be right to honour it. */
static uint64 sys_getgroups(uint64 size, uint64 listp) {
    process_t *p = proc_current();
    uint32 *list = (uint32 *)listp;
    uint32 i;

    if ((int64)size < 0) {
        return (uint64)-22;
    }
    /* size 0 is the "how many are there" probe, and must not touch the
     * pointer - callers pass NULL for it. */
    if (size == 0) {
        return (uint64)p->ngroups;
    }
    if (size < (uint64)p->ngroups) {
        return (uint64)-22;           /* -EINVAL, as POSIX specifies */
    }
    if (!user_range_ok(listp, (uint64)p->ngroups * sizeof(uint32))) {
        return (uint64)-14;
    }
    for (i = 0; i < p->ngroups; i++) {
        list[i] = p->groups[i];
    }
    return (uint64)p->ngroups;
}

static uint64 sys_setgroups(uint64 size, uint64 listp) {
    process_t *p = proc_current();
    const uint32 *list = (const uint32 *)listp;
    uint32 i;

    if (p->uid != 0) {
        return (uint64)-1;            /* -EPERM */
    }
    if ((int64)size < 0 || size > CRED_NGROUPS) {
        return (uint64)-22;
    }
    if (size != 0) {
        if (!user_range_ok(listp, size * sizeof(uint32))) {
            return (uint64)-14;
        }
    }
    /* Written into the process only after every argument has been checked.
     * A partial setgroups - some groups applied, then -EFAULT - would leave
     * an identity nobody asked for. */
    for (i = 0; i < (uint32)size; i++) {
        p->groups[i] = list[i];
    }
    for (i = (uint32)size; i < CRED_NGROUPS; i++) {
        p->groups[i] = 0;
    }
    p->ngroups = (uint32)size;
    return 0;
}

static uint64 sys_getresuid(uint64 rp, uint64 ep, uint64 sp) {
    process_t *p = proc_current();

    if (rp) *(uint32 *)rp = p->uid;
    if (ep) *(uint32 *)ep = p->uid;
    if (sp) *(uint32 *)sp = p->uid;
    return 0;
}

static uint64 sys_setresgid(uint64 r, uint64 e, uint64 sv) {
    process_t *p = proc_current();

    if ((r != (uint64)-1 && e != (uint64)-1 && r != e) ||
        (e != (uint64)-1 && sv != (uint64)-1 && e != sv)) {
        return (uint64)-22;
    }
    if (r != (uint64)-1) {
        p->gid = (uint32)r;
    } else if (e != (uint64)-1) {
        p->gid = (uint32)e;
    }
    return 0;
}

static uint64 sys_getresgid(uint64 rp, uint64 ep, uint64 sp) {
    process_t *p = proc_current();

    if (rp) *(uint32 *)rp = p->gid;
    if (ep) *(uint32 *)ep = p->gid;
    if (sp) *(uint32 *)sp = p->gid;
    return 0;
}

static uint64 sys_writev(uint64 fd, uint64 iov_ptr, uint64 iovcnt) {
    const struct iovec *iov = (const struct iovec *)iov_ptr;
    uint64 total = 0;
    uint64 i;

    /* No fd check here any more - do_write does it once, against the handle
     * table, and gets -EBADF right for a descriptor that was closed rather
     * than for one that is not 1 or 2. */
    /* Linux caps this at UIO_MAXIOV and returns -EINVAL past it. Worth
     * matching: without a bound, a garbage count walks the kernel off the end
     * of the array dereferencing user pointers as it goes. */
    if (iovcnt > 1024) {
        return (uint64)-22;   /* -EINVAL */
    }
    if (iovcnt != 0 &&
        (iov_ptr < 0x1000ULL || iov_ptr >= 0x0000800000000000ULL)) {
        return (uint64)-14;   /* -EFAULT */
    }

    for (i = 0; i < iovcnt; i++) {
        uint64 rc;

        if (iov[i].len == 0) {
            continue;         /* legal, and means nothing */
        }
        rc = (uint64)do_write((int)fd, (const void *)iov[i].base, iov[i].len);

        /* Partial success is success. Reporting an error after some bytes have
         * already reached the console would tell the caller nothing was
         * written when something was, and a libc that responds by retrying the
         * whole buffer duplicates output. Only a failure on the FIRST segment
         * is reported as an error. */
        if ((int64)rc < 0) {
            return total != 0 ? total : rc;
        }
        total += rc;
        if (rc < iov[i].len) {
            break;            /* short write: stop and report what went out */
        }
    }
    return total;
}

/* --- process heap -------------------------------------------------------
 * brk() moves a single boundary. Everything below it is mapped; everything
 * above is not. malloc() in libc sits on top of this. */
/* brk_base/brk_current used to live here as file-scope globals. They are
 * fields of the running process now - the values are the same, the difference
 * is that a second process gets its own. */
void user_set_brk(uint64 base) {
    process_t *p = proc_current();

    p->brk_base    = (base + 0xFFFULL) & ~0xFFFULL;
    p->brk_current = p->brk_base;
}

static uint64 sys_brk(uint64 addr) {
    process_t *p = proc_current();
    uint64 brk_base    = p->brk_base;
    uint64 brk_current = p->brk_current;
    uint64 old_end, new_end, page;

    /* brk(0) is how libc asks where the heap starts - the single most common
     * call, and returning something sane is what lets malloc initialise. */
    if (addr == 0 || addr < brk_base) {
        return brk_current;
    }

    old_end = (brk_current + 0xFFFULL) & ~0xFFFULL;
    new_end = (addr + 0xFFFULL) & ~0xFFFULL;

    for (page = old_end; page < new_end; page += PMM_PAGE_SIZE) {
        if (vmm_get_phys(page) != 0) {
            continue;
        }
        /* PAGE_NX, for the stack's reason: nothing executes out of the
         * break. A libc that wanted executable memory would ask mmap for it
         * with PROT_EXEC, which is honoured below. */
        if (vmm_alloc_page(page, PAGE_PRESENT | PAGE_RW | PAGE_USER |
                                 PAGE_NX) == 0) {
            /* Linux returns the UNCHANGED break on failure, not an errno.
             * Return -ENOMEM here and libc concludes it has an enormous heap. */
            return p->brk_current;
        }
        {
            uint8 *zero = (uint8 *)page;   /* not `p` - that is the process now */
            uint64 i;
            for (i = 0; i < PMM_PAGE_SIZE; i++) {
                zero[i] = 0;
            }
        }
    }

    p->brk_current = addr;
    return p->brk_current;
}

/* --- anonymous memory ---------------------------------------------------
 * malloc uses brk for small requests and mmap for larger ones, so a libc that
 * cannot mmap reports "out of memory" the moment an allocation crosses its
 * threshold - which is what ash hit.
 *
 * Anonymous private mappings only. File-backed mmap needs a page cache and an
 * fd table; there is neither yet, and returning something plausible for a file
 * mapping would be far worse than refusing it.
 *
 * The allocator is a bump pointer with no reuse: munmap unmaps the pages but
 * never lowers the mark. Fine for a process that starts, runs a command and
 * exits; replace it before anything long-running. */
/* mmap_next moved into the process object; see process.h. */
/* Anonymous mapping, as mechanism rather than as a Linux call.
 *
 * mmap(2) and NtAllocateVirtualMemory are two spellings of one operation -
 * give me some zeroed pages - and the pages are identical either way. Only
 * the argument shape and the error convention differ, and those belong in
 * each personality's own file. */
uint64 syscall_map_anonymous(uint64 addr, uint64 length, int fixed);

/* --- mapping a device --------------------------------------------------
 *
 * mmap of a descriptor that names a DEVICE whose driver has an mmap slot -
 * today /dev/fb0, the linear framebuffer (ROADMAP item 14(h)). Each page is
 * the driver's own physical page, mapped straight into the caller, so the
 * program writes video memory with no copy and no syscall per pixel. Regular
 * files are still -ENODEV: there is no page cache to map them from.
 *
 * MAP_SHARED only. A private mapping of a device is copy-on-write of video
 * memory - Linux allows it and it is never what a program drawing to a
 * screen means - so it is refused rather than half-honoured. PROT_WRITE
 * needs a descriptor opened for writing, as it does on Linux.
 *
 * The pages carry PAGE_DEVICE (paging.h): munmap and exit leave the frames
 * alone, and fork shares them writable instead of copy-on-write. The whole
 * range is validated with the driver BEFORE anything is mapped, so a request
 * running past the end of the device fails cleanly instead of leaving half a
 * mapping behind; dev_mmap is also what refuses an offset that is not page
 * aligned. */
static uint64 mmap_device(uint64 addr, uint64 length, uint64 prot,
                          uint64 flags, uint64 fd, uint64 offset) {
    process_t   *proc = proc_current();
    open_file_t *f = handle_get(proc->handles, (int)fd);
    device_t    *dev;
    uint64 start, off, phys, cache, page_flags;
    int rc;

    if (f == NULL || f->obj == NULL) {
        return (uint64)-9;                         /* -EBADF */
    }
    dev = dev_from_object(f->obj);
    if (dev == NULL) {
        return (uint64)-19;                        /* -ENODEV */
    }
    if ((flags & MAP_SHARED) == 0 || (flags & MAP_PRIVATE) != 0) {
        return (uint64)-22;
    }
    if ((prot & PROT_WRITE) && !(f->access & ACCESS_WRITE)) {
        return (uint64)-13;                        /* -EACCES */
    }
    if (prot & PROT_EXEC) {
        return (uint64)-13;       /* executing video memory: no */
    }

    length = (length + 0xFFFULL) & ~0xFFFULL;
    for (off = 0; off < length; off += PMM_PAGE_SIZE) {
        rc = dev_mmap(dev, offset + off, &phys, &cache);
        if (rc != 0) {
            return (uint64)(int64)rc;
        }
    }

    if ((flags & MAP_FIXED) && addr != 0) {
        start = addr & ~0xFFFULL;
    } else {
        if (proc->mmap_next == 0) {
            proc->mmap_next = USER_MMAP_BASE;
        }
        start = proc->mmap_next;
        proc->mmap_next += length;
    }
    if (start < USER_MMAP_BASE || start + length > USER_MMAP_LIMIT) {
        return (uint64)-12;
    }

    for (off = 0; off < length; off += PMM_PAGE_SIZE) {
        (void)dev_mmap(dev, offset + off, &phys, &cache);
        /* MAP_FIXED over something already there replaces it, which is
         * what MAP_FIXED means; for an ordinary page the frame goes back. */
        if (vmm_get_phys(start + off) != 0) {
            vmm_unmap_page_free(start + off);
        }
        page_flags = PAGE_PRESENT | PAGE_USER | PAGE_NX | PAGE_DEVICE |
                     (cache & (PAGE_PCD | PAGE_PWT));
        if (prot & PROT_WRITE) {
            page_flags |= PAGE_RW;
        }
        if (!vmm_map_page(start + off, phys, page_flags)) {
            return (uint64)-12;
        }
    }
    return start;
}

static uint64 sys_mmap(uint64 addr, uint64 length, uint64 prot,
                       uint64 flags, uint64 fd, uint64 offset) {
    uint64 start, page, end, page_flags;

    if (length == 0) {
        return (uint64)-22;   /* -EINVAL */
    }
    if (!(flags & MAP_ANONYMOUS)) {
        return mmap_device(addr, length, prot, flags, fd, offset);
    }
    if ((int64)fd >= 0) {
        return (uint64)-19;   /* -ENODEV: an anonymous mapping names no file */
    }

    length = (length + 0xFFFULL) & ~0xFFFULL;

    if ((flags & MAP_FIXED) && addr != 0) {
        start = addr & ~0xFFFULL;
    } else {
        {
            process_t *proc = proc_current();
            if (proc->mmap_next == 0) {
                proc->mmap_next = USER_MMAP_BASE;
            }
            start = proc->mmap_next;
            proc->mmap_next += length;
        }
    }

    if (start + length > USER_MMAP_LIMIT) {
        return (uint64)-12;   /* -ENOMEM */
    }

    end = start + length;

    /* PROT_EXEC is the one protection bit this call honours, and it is
     * honoured in the only direction that is safe to get wrong: asking for it
     * gets an executable mapping, not asking for it gets PAGE_NX. Read and
     * write are still ignored - refusing writes would need per-page
     * permission changes on an existing mapping, which mprotect will want
     * anyway and does not exist yet.
     *
     * Ignoring PROT_EXEC entirely, as this did, is an executable heap: every
     * anonymous mapping in the system, including everything malloc hands out,
     * was a page you could jump into. Honouring it also means the ELF rtld
     * in roadmap item 4 can map a text segment through mmap without this
     * having to change again. */
    page_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
    if (!(prot & PROT_EXEC)) {
        page_flags |= PAGE_NX;
    }

    for (page = start; page < end; page += PMM_PAGE_SIZE) {
        if (vmm_get_phys(page) != 0) {
            continue;
        }
        if (vmm_alloc_page(page, page_flags) == 0) {
            return (uint64)-12;
        }
        /* No per-page tracing here. It was a line per page, which is fine
         * while the only caller is a program asking for one page and
         * unreadable the moment RtlAllocateHeap asks for sixteen - it buried
         * the output of the thing it was meant to help debug. The mapping is
         * observable from the fault handler and from /proc-shaped things
         * later; a printf in the hot path is not the way. */
        /* MAP_ANONYMOUS is specified to return zeroed memory, and a fresh
         * frame holds whatever the last user left in it. Skipping this leaks
         * one process's data into the next and breaks any caller that trusts
         * calloc. */
        {
            uint8 *p = (uint8 *)page;
            uint64 i;
            for (i = 0; i < PMM_PAGE_SIZE; i++) {
                p[i] = 0;
            }
        }
    }

    /* PROT_EXEC is honoured above. PROT_READ, PROT_WRITE and PROT_NONE are
     * still accepted and ignored - everything is mapped readable and
     * writable, because refusing writes needs per-page permission changes on
     * an existing mapping that mprotect will want too and that nothing yet
     * asks for correctly. */
    return start;
}

/* sigaltstack(2) - install, or read back, the alternate signal stack.
 *
 * The facility exists for one case the ordinary delivery path cannot serve:
 * a SIGSEGV raised BY the stack (a guard-page hit, or runaway recursion)
 * leaves no room below RSP to build a signal frame, so delivering on the user
 * stack faults again and the second fault is unrecoverable. See process.h's
 * sigalt_sp. Half of this is kernel/proc/signal.c honouring SA_ONSTACK; this
 * half is the installation.
 *
 * THE ORDER OF THE TWO HALVES MATTERS. `old` is filled in BEFORE `new` is
 * installed, because the standard call is sigaltstack(&new, &old) and a
 * caller doing that is asking what the stack was, not what it just became.
 * Writing old afterwards returns the new stack and makes save/restore
 * impossible - the bug is silent, because the two structs usually differ only
 * in a field nobody prints.
 *
 * REFUSALS, and both are real rather than defensive:
 *   - changing the stack WHILE EXECUTING ON IT is -EPERM. The handler
 *     standing on it would have the ground moved; POSIX says so and the
 *     failure without it is a corrupted frame at the next return.
 *   - a stack smaller than MINSIGSTKSZ is -ENOMEM. A signal frame here is
 *     sizeof(struct sigframe) plus alignment, and a stack too small to hold
 *     one turns the facility into the fault it exists to prevent.
 *
 * SS_DISABLE is honoured and is not the same as passing a null pointer: null
 * means "do not change", disable means "there is no alternate stack now", and
 * conflating them leaves a stale pointer armed after the caller freed it. */
#define SS_ONSTACK   1
#define SS_DISABLE   2
#define MINSIGSTKSZ  2048

struct k_stack_t {
    uint64 ss_sp;
    uint32 ss_flags;
    uint32 pad;
    uint64 ss_size;
};

static uint64 sys_sigaltstack(uint64 new_ptr, uint64 old_ptr) {
    process_t *p = proc_current();
    struct k_stack_t out;
    struct k_stack_t in;

    if (p == NULL) {
        return (uint64)-22;
    }
    if (new_ptr != 0 && !user_range_ok(new_ptr, sizeof(in))) {
        return (uint64)-14;                    /* -EFAULT */
    }
    if (old_ptr != 0 && !user_range_ok(old_ptr, sizeof(out))) {
        return (uint64)-14;
    }

    /* Read `new` before writing `old`, so that sigaltstack(&s, &s) - the same
     * buffer for both, which is legal and which a save/restore idiom does -
     * does not read back what this call is about to write. */
    if (new_ptr != 0) {
        in = *(const struct k_stack_t *)new_ptr;
    }

    if (old_ptr != 0) {
        out.ss_sp    = p->sigalt_sp;
        out.ss_size  = p->sigalt_size;
        out.pad      = 0;
        out.ss_flags = (p->sigalt_sp == 0) ? (uint32)SS_DISABLE
                     : (p->sigalt_on > 0)  ? (uint32)SS_ONSTACK
                                           : 0u;
        *(struct k_stack_t *)old_ptr = out;
    }

    if (new_ptr == 0) {
        return 0;
    }

    if (p->sigalt_on > 0) {
        return (uint64)-1;                     /* -EPERM */
    }

    if (in.ss_flags & SS_DISABLE) {
        p->sigalt_sp   = 0;
        p->sigalt_size = 0;
        return 0;
    }
    /* Any flag other than SS_DISABLE is meaningless here; SS_ONSTACK is
     * output-only. Refused rather than ignored so a caller that sets it by
     * mistake finds out. */
    if (in.ss_flags != 0) {
        return (uint64)-22;
    }
    if (in.ss_size < MINSIGSTKSZ) {
        return (uint64)-12;                    /* -ENOMEM */
    }
    if (in.ss_sp == 0 || !user_range_ok(in.ss_sp, in.ss_size)) {
        return (uint64)-14;
    }

    p->sigalt_sp   = in.ss_sp;
    p->sigalt_size = in.ss_size;
    return 0;
}

/* mremap(2) - resize an existing anonymous mapping.
 *
 * --- what makes this implementable at all --------------------------------
 *
 * sys_mmap keeps NO record of what it handed out: the address space is a bump
 * pointer (proc->mmap_next) and the page tables are the only account of what
 * is mapped. That would sink a resize primitive except that mremap's caller
 * passes old_size, so the extent it wants changed is an argument rather than
 * something this has to look up. The page tables answer the only other
 * question - "is the space after it free?" - through vmm_get_phys.
 *
 * --- the three cases, and why growing IN PLACE is tried first -------------
 *
 * SHRINK: unmap the tail, return the same address. Always possible, never
 * moves, and the pages really are freed (sys_munmap frees the frame, not just
 * the mapping) rather than merely forgotten.
 *
 * GROW IN PLACE: only if every page in the new tail is currently unmapped.
 * Tried first because it is the case that does not copy - a realloc of a 4MB
 * buffer that can extend costs nothing, and one that moves costs 4MB of
 * memcpy plus 4MB of new frames held at the same time as the old. With a bump
 * allocator this succeeds nearly always, since the region above the last
 * allocation is untouched.
 *
 * MOVE: MREMAP_MAYMOVE only. A new region from the bump pointer, the old
 * contents copied, the old region unmapped. Refusing without MAYMOVE is not
 * pedantry - the flag exists because a caller holding pointers into the
 * mapping cannot survive a move, and answering -ENOMEM is what lets it find
 * out rather than have its pointers silently invalidated.
 *
 * MREMAP_FIXED is refused. It means "move it to exactly here", which requires
 * unmapping whatever is already at the destination, and nothing in this tree
 * asks for it. Refused explicitly rather than ignored, because ignoring it
 * would move the mapping somewhere else and report success. */
#define MREMAP_MAYMOVE  1UL
#define MREMAP_FIXED    2UL

/* Declared here because sys_munmap is defined below this point and mremap's
 * shrink and unwind paths both call it. */
static uint64 sys_munmap(uint64 addr, uint64 length);

static uint64 sys_mremap(uint64 old_addr, uint64 old_size, uint64 new_size,
                         uint64 flags, uint64 new_addr) {
    uint64 old_end, new_end, page;
    uint64 dest;

    (void)new_addr;

    /* The address must be page aligned; the sizes get rounded up, which is
     * what Linux does. A misaligned address is a caller bug rather than a
     * request to round, because the pages it would round to belong to
     * whatever mapping actually starts there. */
    if (old_addr == 0 || (old_addr & 0xFFFULL) != 0 || new_size == 0) {
        return (uint64)-22;                     /* -EINVAL */
    }
    if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED)) {
        return (uint64)-22;
    }
    if (flags & MREMAP_FIXED) {
        return (uint64)-22;                     /* see the comment above */
    }

    old_size = (old_size + 0xFFFULL) & ~0xFFFULL;
    new_size = (new_size + 0xFFFULL) & ~0xFFFULL;
    old_end  = old_addr + old_size;
    new_end  = old_addr + new_size;

    /* The mapping has to BE there. Without this, mremap on an address that
     * was never mapped grows "it" by mapping fresh pages and reports success,
     * which turns a caller's bug into a mapping that exists for no reason. */
    if (old_size != 0 && vmm_get_phys(old_addr) == 0) {
        return (uint64)-22;
    }

    if (new_size == old_size) {
        return old_addr;
    }

    if (new_size < old_size) {
        (void)sys_munmap(new_end, old_size - new_size);
        return old_addr;
    }

    /* --- grow in place, if the space above is genuinely free -------------- */
    if (new_end <= USER_MMAP_LIMIT) {
        int clear = 1;

        for (page = old_end; page < new_end; page += PMM_PAGE_SIZE) {
            if (vmm_get_phys(page) != 0) {
                clear = 0;
                break;
            }
        }
        if (clear) {
            uint64 got = sys_mmap(old_end, new_size - old_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_FIXED, (uint64)-1, 0);

            if (got == old_end) {
                /* Keep the bump pointer above the mapping if the extension
                 * ran past it, or the next plain mmap would hand out pages
                 * this call just took. */
                {
                    process_t *proc = proc_current();
                    if (proc != NULL && proc->mmap_next < new_end) {
                        proc->mmap_next = new_end;
                    }
                }
                return old_addr;
            }
            /* The fixed mapping failed part way. Anything it did map is
             * inside the region being grown and is released here rather than
             * left as a hole the move path would then copy over. */
            (void)sys_munmap(old_end, new_size - old_size);
        }
    }

    if (!(flags & MREMAP_MAYMOVE)) {
        return (uint64)-12;                     /* -ENOMEM */
    }

    /* --- move ------------------------------------------------------------- */
    dest = sys_mmap(0, new_size, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS, (uint64)-1, 0);
    if ((int64)dest < 0) {
        return dest;
    }
    {
        const uint8 *src = (const uint8 *)old_addr;
        uint8       *dst = (uint8 *)dest;
        uint64       i;

        for (i = 0; i < old_size; i++) {
            dst[i] = src[i];
        }
    }
    (void)sys_munmap(old_addr, old_size);
    return dest;
}

uint64 syscall_map_anonymous(uint64 addr, uint64 length, int fixed) {
    return sys_mmap(addr, length, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | (fixed ? MAP_FIXED : 0), (uint64)-1, 0);
}


static uint64 sys_munmap(uint64 addr, uint64 length) {
    uint64 page, end;

    if (length == 0) {
        return (uint64)-22;
    }
    end = (addr + length + 0xFFFULL) & ~0xFFFULL;

    /* Frees the frame, not just the mapping. Anonymous pages have exactly one
     * mapping, so this is the last one - and without it every munmap leaked a
     * frame permanently, which a bump-pointer mmap that never reuses address
     * space turns into a steady drain. The VMM also reclaims page tables the
     * unmapping empties.
     *
     * This is the assumption that has to be revisited before shared mappings
     * or fork exist: at that point "the last mapping" stops being knowable
     * here and becomes a refcount in the PMM. */
    for (page = addr & ~0xFFFULL; page < end; page += PMM_PAGE_SIZE) {
        vmm_unmap_page_free(page);
    }
    return 0;
}

/* --- thread pointer -----------------------------------------------------
 * The one call libc cannot start without. Thread-local storage is addressed
 * through FS, and in long mode FS.base comes from an MSR rather than a
 * descriptor. Fail this and musl dies before main() with no message. */
static uint64 sys_arch_prctl(uint64 code, uint64 addr) {
    switch (code) {
        case ARCH_SET_FS:
            /* A zero thread pointer is how libc reports that its own TLS setup
             * failed. Accepting it silently turns that into a NULL dereference
             * later, at a %fs:0 access far from the cause. */
            if (addr == 0) {
                print_string("  !! arch_prctl(SET_FS, 0) - TLS setup failed\n", 0x0C);
            }
            /* Recorded as well as written. The MSR is one register shared by
             * every process on the CPU; the copy on the thread is what makes
             * it survive a context switch, and proc_activate_stack puts it
             * back on the way in. */
            proc_current()->thread.fs_base = addr;
            wrmsr(MSR_FS_BASE, addr);
            return 0;
        case ARCH_GET_FS:
            *(uint64 *)addr = rdmsr(MSR_FS_BASE);
            return 0;
        case ARCH_SET_GS:
            /* GS used to be refused outright, on the grounds that it holds
             * the per-CPU block. That was never quite true and is not true at
             * all now: the block lives in ONE of the two GS MSRs and the
             * user's value in the other, so a process can have a GS base
             * without touching the kernel's. Windows needs this - the TEB is
             * at GS:0 - and Linux's own ARCH_SET_GS means exactly the same
             * thing, so both personalities get it from one implementation. */
            proc_current()->thread.gs_base = addr;
            syscall_set_user_gs_base(addr);
            return 0;
        case ARCH_GET_GS:
            if (!user_ptr_ok(addr)) {
                return (uint64)-14;   /* -EFAULT */
            }
            *(uint64 *)addr = syscall_get_user_gs_base();
            return 0;
        default:
            return (uint64)-22;
    }
}

/* --- small fillers ------------------------------------------------------ */

static void fill(uint8 *dst, uint8 v, uint64 n) {
    while (n--) { *dst++ = v; }
}

static uint64 copy_str(char *dst, const char *src, uint64 max) {
    uint64 n = 0;
    while (src[n] != '\0' && n + 1 < max) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    return n;
}

/* struct utsname: six fixed 65-byte fields, 390 bytes total. */
static uint64 sys_uname(char *buf) {
    fill((uint8 *)buf, 0, 390);
    copy_str(buf + 0,   "Linux",   65);   /* musl checks for "Linux" */
    copy_str(buf + 65,  "genesis", 65);
    copy_str(buf + 130, "6.0.0",   65);
    copy_str(buf + 195, "Genesis", 65);
    copy_str(buf + 260, "x86_64",  65);
    copy_str(buf + 325, "",        65);
    return 0;
}

/* struct stat on x86-64 is 144 bytes. Only st_mode really matters here:
 * reporting stdout as a character device is what makes libc pick unbuffered
 * output, so a write() actually reaches the screen instead of sitting in a
 * buffer until an exit that never flushes. */
static uint64 sys_newfstatat(uint64 fd, uint64 statbuf) {
    uint8 *st = (uint8 *)statbuf;

    fill(st, 0, 144);
    *(uint32 *)(st + 24) = 0x2000 | 0666;   /* S_IFCHR | rw-rw-rw- */
    *(uint64 *)(st + 16) = 1;               /* st_nlink            */
    *(uint64 *)(st + 56) = 1024;            /* st_blksize          */
    (void)fd;
    return 0;
}

/* fstat on a descriptor. The only three that exist are the console, so they
 * get the same character-device answer newfstatat gives. Reporting a character
 * device rather than a regular file is what makes a libc treat stdout as
 * unbuffered and line-oriented, which is what you want on a console. */
static uint64 sys_fstat(uint64 fd, uint64 statbuf) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);
    uint8 *st = (uint8 *)statbuf;

    if (f == NULL) {
        return (uint64)-9;    /* -EBADF */
    }
    if (!user_ptr_ok(statbuf)) {
        return (uint64)-14;
    }
    /* A console still reports as a character device - that is what makes a
     * libc treat it as unbuffered. Anything else reports from its entry. */
    if (f->obj == NULL || f->obj->type == NULL ||
        f->obj->type->klass == OBJ_CONSOLE) {
        return sys_newfstatat(fd, statbuf);
    }

    /* S_IFIFO, before anything reaches for a FAT body a pipe does not have.
     * A libc reads this to pick its buffering: a pipe gets block buffering, a
     * terminal gets line buffering, and getting it backwards is what makes
     * `cmd | cat` produce nothing until the process exits. */
    if (f->obj->type->klass == OBJ_PIPE) {
        fill(st, 0, 144);
        *(uint32 *)(st + 24) = 0010600u;     /* S_IFIFO | rw------- */
        *(uint64 *)(st + 16) = 1;
        *(uint64 *)(st + 56) = 4096;
        return 0;
    }

    fill(st, 0, 144);
    /* The class first, and only then the FAT body. fileobj_size and
     * fileobj_is_dir both read obj->body as a fat_entry_t, which is true for
     * a file object and garbage for a device-directory object whose body is
     * an ns_entry_t. Asking the type what it is costs nothing and is right
     * for every object rather than for one kind of them. */
    if (ob_is_directory(f->obj)) {
        *(uint32 *)(st + 24) = 0040755u;
        *(uint64 *)(st + 16) = 1;
        *(uint64 *)(st + 56) = 4096;
        return 0;
    }
    *(uint32 *)(st + 24) = 0100755u;
    *(uint64 *)(st + 48) = fileobj_size(f->obj);
    *(uint64 *)(st + 16) = 1;
    *(uint64 *)(st + 56) = 4096;
    *(uint64 *)(st + 64) = (fileobj_size(f->obj) + 511) / 512;
    return 0;
}

/* stat and lstat on a path.
 *
 * This returned -ENOENT unconditionally until there was a filesystem behind
 * it and an execve to make the answer matter. Both exist now, so it answers
 * honestly - which is what makes a PATH search terminate: ash stats
 * /bin/busybox, gets a regular file with the execute bits set, and execs it.
 *
 * lstat is the same call because FAT has no symbolic links. That is a
 * property of the filesystem, not a shortcut, and it stops being true the day
 * anything else is mounted. */
static uint64 sys_stat_path(uint64 path_ptr, uint64 statbuf) {
    process_t *p = proc_current();
    char resolved[PATH_MAX_LEN];
    uint8 *st = (uint8 *)statbuf;
    fs_node_t node;
    int rc;

    if (!user_ptr_ok(path_ptr) || !user_ptr_ok(statbuf)) {
        return (uint64)-14;
    }
    if (path_normalize(p->cwd, (const char *)path_ptr,
                       resolved, sizeof(resolved)) != PATH_OK) {
        return (uint64)-36;
    }

    /* Same reasoning as open: /dev is the namespace, not the volume. A stat
     * that fell through to the FAT lookup would report -ENOENT for a device
     * that opens perfectly well, and a shell checking before it opens would
     * believe it. */
    if (is_dev_path(resolved)) {
        char      remainder[NS_PATH_MAX];
        object_t *obj = NULL;
        int       rc  = dev_lookup(resolved, &obj, remainder, sizeof(remainder));

        /* Through the same dev_lookup open() uses, so that a name which opens
         * also stats and a name which does not opens does not stat. Two
         * copies of the rewrite is how those two answers came apart. */
        if (rc != 0) {
            return (uint64)(int64)rc;
        }
        if (remainder[0] != '\0') {
            ob_deref(obj);
            return (uint64)-20;               /* -ENOTDIR */
        }
        fill(st, 0, 144);
        *(uint64 *)(st + 16) = 1;
        if (dev_is_directory(obj)) {
            /* 040555. `ls /dev` checks the type bits before it opens. */
            *(uint32 *)(st + 24) = 0040555u;
            *(uint64 *)(st + 56) = 1024;
        } else if (disk_is_block(obj)) {
            /* 060660: S_IFBLK. Everything under /dev used to report S_IFCHR,
             * which was wrong for a disk in a way programs act on - dd picks
             * its transfer size from the type, and a partition tool refuses
             * to touch what it believes is a character device.
             *
             * The block size is the sector size rather than 1024, because for
             * a block device it is a real number rather than a placeholder,
             * and it is the one a caller will align its reads to. */
            *(uint32 *)(st + 24) = 0060660u;
            *(uint64 *)(st + 56) = 512;
            /* st_size on a block device is the size of the device. A zero
             * here is what makes `dd if=/dev/sda` of a whole disk stop
             * immediately, because a caller sizing the transfer from stat
             * concludes there is nothing there. */
            *(uint64 *)(st + 48) = disk_size(obj);
        } else {
            *(uint32 *)(st + 24) = 0020666u;  /* S_IFCHR | rw-rw-rw- */
            *(uint64 *)(st + 56) = 1024;
        }
        ob_deref(obj);
        return 0;
    }

    rc = fs_lookup(resolved, &node);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }

    fill(st, 0, 144);
    /* From the node, which is to say from the filesystem, which is to say
     * from the disk on a filesystem that records it.
     *
     * This used to be two literals, 0040755 and 0100755, with a comment
     * ending "the day a filesystem HAS them, this is the line that reads
     * them instead of inventing them". fs_node_t carries mode, uid and gid
     * now: ZFS fills them from a walk of the object's System Attribute
     * layout, and fatfs fills them with the fixed convention it states in
     * one place rather than having each caller invent its own.
     *
     * st_uid and st_gid were not written at all before - the buffer was
     * zeroed, so every file on the system belonged to root. They are at
     * offsets 28 and 32; getting those wrong writes the uid into the pad
     * word and reports zero, which looks exactly like the old behaviour and
     * is why they are spelled out here beside st_mode rather than somewhere
     * else. */
    *(uint32 *)(st + 24) = node.mode;
    *(uint32 *)(st + 28) = node.uid;
    *(uint32 *)(st + 32) = node.gid;
    *(uint64 *)(st + 48) = node.is_dir ? 0 : (uint64)node.size;
    *(uint64 *)(st + 16) = 1;               /* st_nlink                     */
    /* The identity the FILESYSTEM supplies, whatever it chooses to derive it
     * from. FAT uses the first cluster; a filesystem with real inodes gives a
     * real one, and this line does not change when it does. */
    *(uint64 *)(st +  8) = node.ino;
    /* From the NODE's volume, not from fs_root(). They are the same volume
     * today because there is one; asking the node is what stays correct when
     * there are two, and it is also the version that cannot dereference NULL
     * - a node only exists because a lookup succeeded. */
    *(uint64 *)(st + 56) = node.vol->block_size;
    *(uint64 *)(st + 64) = (node.size + 511) / 512;  /* st_blocks, 512B units */
    return 0;
}

/* --- time ---------------------------------------------------------------
 *
 * These arrived because musl asks for them and nothing else could. A libc
 * with no clock cannot implement time(), and every program that stamps a
 * file, measures an interval or sleeps goes through here.
 *
 * The resolution is one timer tick - 10ms - and every one of these reports
 * nanoseconds. That is not a pretence: the UNIT is nanoseconds because the
 * ABI says so, and the resolution is whatever the clock underneath has. A
 * program that needs better needs the TSC, which needs calibrating, which is
 * its own change and not this one.
 */

/* struct timespec / struct timeval, written out rather than included: these
 * are the userspace ABI's shapes and both are two 64-bit fields on x86-64.
 * The difference between them is the SCALE of the second field - nanoseconds
 * against microseconds - which is exactly the kind of distinction that
 * survives a wrong guess for a thousand-fold error. */
#define CLOCK_REALTIME            0
#define CLOCK_MONOTONIC           1
#define CLOCK_PROCESS_CPUTIME_ID  2
#define CLOCK_THREAD_CPUTIME_ID   3
#define CLOCK_MONOTONIC_RAW       4
#define CLOCK_REALTIME_COARSE     5
#define CLOCK_MONOTONIC_COARSE    6
#define CLOCK_BOOTTIME            7

static uint64 sys_clock_gettime(uint64 clock_id, uint64 ts_ptr) {
    uint64 *ts = (uint64 *)ts_ptr;
    uint64 ns;

    if (!user_ptr_ok(ts_ptr)) {
        return (uint64)-14;                             /* -EFAULT        */
    }

    switch (clock_id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        ns = timer_realtime_ns();
        break;

    /* MONOTONIC and BOOTTIME differ only in whether they advance while the
     * machine is suspended. Nothing here suspends, so they are the same
     * clock - and they are the same clock HONESTLY rather than by oversight,
     * which is why BOOTTIME is named instead of falling through to default. */
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME:
        ns = timer_ns();
        break;

    /* Per-process and per-thread CPU time. These used to be refused, because
     * answering with wall time would give a program timing itself a plausible
     * number that is wrong under load - and it would never think to check.
     * The runtime accounting they needed now exists in sched_tick.
     *
     * The two are the same answer here and that is a real approximation, not
     * an oversight: run_ticks is charged per PROCESS TABLE ENTRY, and a
     * thread is its own entry, so THREAD_CPUTIME is exact and PROCESS_CPUTIME
     * is short by whatever the group's other threads have used. Summing the
     * group would fix it and is four lines; it is not done here because the
     * sum has to skip zombies whose slots have been reused, and that check
     * belongs with the thread-group accounting rather than in a clock.
     *
     * The RESOLUTION is one tick - 10ms - whatever the ABI's nanoseconds
     * suggest. That is true of every clock in this kernel and is in the Owed
     * list; it matters more here, because a program measuring a short
     * function against this clock gets zero and concludes the function is
     * free. */
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID: {
        process_t *me = proc_current();

        if (me == NULL) {
            return (uint64)-22;
        }
        /* cpu_ticks, not run_ticks. run_ticks is ULE's scoring input and is
         * HALVED every two seconds by design, so reading it here made this
         * clock run backwards - and made a process that spun for a second
         * and a half report less than half of it, by an amount that depended
         * on where the decay boundary fell. See process.h. */
        ns = (me->cpu_ticks * 1000000000ULL) / timer_hz();
        break;
    }

    default:
        return (uint64)-22;
    }

    ts[0] = ns / 1000000000ULL;
    ts[1] = ns % 1000000000ULL;
    return 0;
}

static uint64 sys_gettimeofday(uint64 tv_ptr, uint64 tz_ptr) {
    uint64 *tv = (uint64 *)tv_ptr;
    uint64 ns;

    /* Both pointers are optional. A caller wanting only the timezone is
     * asking for something this does not have, and gets zeroes. */
    if (tv_ptr != 0) {
        if (!user_ptr_ok(tv_ptr)) {
            return (uint64)-14;
        }
        ns = timer_realtime_ns();
        tv[0] = ns / 1000000000ULL;
        tv[1] = (ns % 1000000000ULL) / 1000;            /* MICROseconds   */
    }
    if (tz_ptr != 0) {
        uint64 *tz = (uint64 *)tz_ptr;

        if (!user_ptr_ok(tz_ptr)) {
            return (uint64)-14;
        }
        /* struct timezone is two ints and has been obsolete for decades;
         * Linux fills it with zeroes and so does this. */
        tz[0] = 0;
    }
    return 0;
}

/* clock_getres. The answer is ONE TICK, and that is the honest one - it is
 * what ROADMAP's Owed section already records as "clock resolution is one
 * tick". A caller told 1ns and then handed tick-granular timestamps has been
 * lied to in a way it cannot detect. */
static uint64 sys_clock_getres(uint64 clock_id, uint64 ts_ptr) {
    uint64 *ts = (uint64 *)ts_ptr;
    uint64 hz;

    (void)clock_id;
    if (ts_ptr == 0) {
        return 0;                     /* legal: "is this clock supported" */
    }
    if (!user_ptr_ok(ts_ptr)) {
        return (uint64)-14;
    }
    hz = timer_hz();
    ts[0] = 0;
    ts[1] = hz != 0 ? 1000000000ULL / hz : 0;
    return 0;
}

/* clock_nanosleep. The flags word is what distinguishes it from nanosleep:
 * TIMER_ABSTIME (1) means the timespec is a DEADLINE rather than a
 * duration. Relative is delegated to nanosleep rather than duplicated. */
static uint64 sys_nanosleep(uint64 req_ptr, uint64 rem_ptr);

static uint64 sys_clock_nanosleep(uint64 clock_id, uint64 flags,
                                  uint64 req_ptr, uint64 rem_ptr) {
    (void)clock_id;

    if ((flags & 1) == 0) {
        return sys_nanosleep(req_ptr, rem_ptr);
    }
    /* Absolute. Converted to a relative sleep against the current time
     * rather than implemented separately - the deadline arithmetic is
     * nanosleep's and having two copies of it is how they drift apart. */
    {
        const uint64 *req = (const uint64 *)req_ptr;
        uint64 target, now;

        if (!user_ptr_ok(req_ptr)) {
            return (uint64)-14;
        }
        if (req[1] >= 1000000000ULL) {
            return (uint64)-22;
        }
        target = req[0] * 1000000000ULL + req[1];
        now    = timer_ns();
        if (target <= now) {
            return 0;                 /* the deadline has passed */
        }
        /* The deadline arithmetic is repeated rather than delegated:
         * sys_nanosleep bounds-checks its timespec as a USER pointer, so a
         * kernel-built relative one cannot be handed to it. Kept to the few
         * lines below rather than factored out, because a shared helper
         * would have to take a flag saying which of the two it was serving.
         */
        {
            uint64 hz = timer_hz();
            uint64 deadline = ((target) * hz + 999999999ULL) / 1000000000ULL;
            process_t *me = proc_current();

            while (timer_ticks_now() < deadline) {
                sched_sleep_until(me, deadline);
                if (signal_pending(me)) {
                    if (rem_ptr != 0 && user_ptr_ok(rem_ptr)) {
                        uint64 *rem = (uint64 *)rem_ptr;
                        uint64 left = timer_ns() < target ? target - timer_ns() : 0;
                        rem[0] = left / 1000000000ULL;
                        rem[1] = left % 1000000000ULL;
                    }
                    return (uint64)-4;   /* -EINTR */
                }
            }
        }
        return 0;
    }
}

/* times(2). Genesis accounts run_ticks per process (sched.c), which is
 * exactly tms_utime + tms_stime with no way to separate the two - there is
 * no user/kernel time split in the accounting. Reported as user time with
 * system time zero rather than split arbitrarily. */
static uint64 sys_times(uint64 buf_ptr) {
    process_t *p = proc_current();
    uint64 *tms = (uint64 *)buf_ptr;

    if (buf_ptr != 0) {
        if (!user_ptr_ok(buf_ptr)) {
            return (uint64)-14;
        }
        /* cpu_ticks for the same reason clock_gettime uses it: times(2) is
         * defined to be monotonic, and run_ticks is decayed. */
        tms[0] = p->cpu_ticks;    /* tms_utime  */
        tms[1] = 0;               /* tms_stime  */
        tms[2] = 0;               /* tms_cutime */
        tms[3] = 0;               /* tms_cstime */
    }
    return timer_ticks_now();
}

/* Signals delivered but blocked. A real answer, from the same word
 * signal_send sets. */
static uint64 sys_rt_sigpending(uint64 set_ptr, uint64 setsize) {
    process_t *p = proc_current();

    if (setsize != 8) {
        return (uint64)-22;
    }
    if (!user_ptr_ok(set_ptr)) {
        return (uint64)-14;
    }
    *(uint64 *)set_ptr = p->sig_pending & p->sig_blocked;
    return 0;
}

/* Wait with a temporary mask, restoring the old one. ALWAYS returns -EINTR:
 * that is not a shortcut, it is the specification - rt_sigsuspend has no
 * successful return, it returns only when a handler has run. */
static uint64 sys_rt_sigsuspend(uint64 mask_ptr, uint64 setsize) {
    process_t *p = proc_current();
    uint64 saved;

    if (setsize != 8) {
        return (uint64)-22;
    }
    if (!user_ptr_ok(mask_ptr)) {
        return (uint64)-14;
    }
    saved = p->sig_blocked;
    p->sig_blocked = *(const uint64 *)mask_ptr;

    while (!signal_pending(p)) {
        sched_block(p);
    }
    p->sig_blocked = saved;
    return (uint64)-4;                /* -EINTR, always */
}

/* pause: block until a signal arrives. Same "-EINTR always" rule. */
static uint64 sys_pause(void) {
    process_t *p = proc_current();

    while (!signal_pending(p)) {
        sched_block(p);
    }
    return (uint64)-4;
}

static uint64 sys_nanosleep(uint64 req_ptr, uint64 rem_ptr) {
    const uint64 *req = (const uint64 *)req_ptr;
    process_t *me = proc_current();
    uint64 hz, ns, deadline, now;

    if (!user_ptr_ok(req_ptr)) {
        return (uint64)-14;
    }
    if (req[1] >= 1000000000ULL) {
        return (uint64)-22;             /* tv_nsec out of range           */
    }

    hz = timer_hz();
    ns = timer_ns() + req[0] * 1000000000ULL + req[1];

    /* Round the deadline UP to the next whole tick. A sleep that ends early
     * is a bug the caller cannot detect and cannot correct: sleep(1) that
     * returns after 990ms looks like it worked. Rounding up means the sleep
     * is never shorter than asked, which is what the interface promises -
     * "at least this long" - and the overshoot is bounded by one tick. */
    deadline = (ns * hz + 999999999ULL) / 1000000000ULL;

    while ((now = timer_ticks_now()) < deadline) {
        if (me == NULL) {
            /* No process context: the idle path before anything runs. Halt
             * until the next interrupt rather than spinning. */
            bkl_wait_for_interrupt();
            continue;
        }
        if (signal_pending(me)) {
            /* Interrupted. The REMAINING time goes back to the caller, which
             * is what lets a program restart the sleep after handling the
             * signal rather than starting it over from the top - and what
             * makes sleep(60) interrupted at 59s not sleep for another 60. */
            if (rem_ptr != 0 && user_ptr_ok(rem_ptr)) {
                uint64 *rem = (uint64 *)rem_ptr;
                uint64 left = (deadline > now)
                            ? ((deadline - now) * 1000000000ULL) / hz : 0;

                rem[0] = left / 1000000000ULL;
                rem[1] = left % 1000000000ULL;
            }
            return (uint64)-4;                          /* -EINTR         */
        }

        sched_sleep_until(me, deadline);

        /* Still blocked means schedule() found nothing else to run and came
         * straight back, so there is no other process whose activity could
         * end this wait - only the tick. Same `sti; hlt; cli` ordering as
         * waitq_wait, and load-bearing for the same reason: sti leaves
         * interrupts off for one more instruction, so the tick cannot arrive
         * in the gap and leave the CPU halted with the wakeup already spent.
         */
        if (me->state == PROC_BLOCKED) {
            me->state = PROC_RUNNING;
            bkl_wait_for_interrupt();
        }
    }
    return 0;
}

static uint64 sys_getrandom(uint8 *buf, uint64 len) {
    uint64 i;
    /* NOT random. Enough to let libc finish initialising its stack guard;
     * replace before anything depends on it being unpredictable. */
    for (i = 0; i < len; i++) {
        buf[i] = (uint8)(0x5A ^ (i * 31));
    }
    return len;
}

/* --- current working directory ------------------------------------------
 * The first piece of per-process state that was not memory, and now the
 * reason all three - cwd, brk, mmap_next - live in process_t rather than
 * here.
 *
 * Always absolute, always normalized, never with a trailing slash except for
 * the root. Every path that enters the kernel goes through path_normalize
 * against this, so a relative path never reaches the filesystem layer. */
/* The cwd is a field of the running process now. This accessor keeps the call
 * sites below unchanged and makes the indirection obvious in one place. */
static char *current_dir_of(void) {
    return proc_current()->cwd;
}

int user_ptr_ok(uint64 p) {
    return p >= 0x1000ULL && p < 0x0000800000000000ULL;
}

/* The same question for a RANGE, which is not the same question.
 *
 * user_ptr_ok answers it for the first byte only, and every caller that
 * passes a pointer plus a length has been asking the wrong one: a pointer
 * just below the user limit with a large length passes, and the kernel then
 * reads or writes across the boundary. The overflow case is the sharp edge -
 * `p + len` for a large len wraps and lands back in the user range, so a
 * naive `p + len < LIMIT` test says yes to exactly the argument that is
 * trying to escape. The length is checked against the limit BEFORE the
 * addition here, which is the form that cannot wrap.
 *
 * A zero length is accepted for a valid base: read(fd, buf, 0) is legal and
 * touches nothing. */
int user_range_ok(uint64 p, uint64 len) {
    if (!user_ptr_ok(p)) {
        return 0;
    }
    if (len == 0) {
        return 1;
    }
    if (len > 0x0000800000000000ULL) {
        return 0;
    }
    return p <= 0x0000800000000000ULL - len;
}

/* fat_errno() used to be here. It is in fatfs.c now, next to the filesystem
 * whose error space it translates - which is the whole point of the vtable:
 * a second filesystem brings its own error codes, and two translation tables
 * at the syscall layer would be two tables that drift. */

/* --- the file NAMESPACE syscalls -----------------------------------------
 *
 * The fourteen Part 17's audit found missing and could not close, because
 * fs_ops_t had no way to express directory mutation. It has one now.
 *
 * Every one of these normalizes against the current directory first, so a
 * relative path works and ".." is resolved before the filesystem sees it -
 * a FAT driver asked to mkdir "/a/../b" would otherwise try to create a
 * component called "..".
 */

/* Resolve a user path argument into `out`. The shared preamble of all of
 * them, written once because getting the -EFAULT and empty-path cases
 * subtly different between six syscalls is exactly how they drift. */
static int resolve_user_path(uint64 path_ptr, char *out, uint64 out_size) {
    const char *path = (const char *)path_ptr;

    if (!user_ptr_ok(path_ptr)) {
        return -14;                   /* -EFAULT */
    }
    if (path[0] == '\0') {
        return -2;                    /* -ENOENT: "" is not "." */
    }
    if (path_normalize(current_dir_of(), path, out, out_size) != PATH_OK) {
        return -36;                   /* -ENAMETOOLONG */
    }
    return 0;
}

/* truncate(2) and ftruncate(2).
 *
 * Two entry points because they start from different things and fail
 * differently: truncate takes a path and can report -ENOENT, ftruncate takes
 * a descriptor and cannot - it can report -EBADF instead, and it must check
 * that the descriptor was opened for WRITING, which a path-based call has no
 * equivalent of.
 *
 * A negative length is -EINVAL rather than a very large positive one. The
 * argument is off_t, so a caller that computes a length by subtraction and
 * gets it backwards passes -1 here; accepting it as 0xFFFFFFFFFFFFFFFF would
 * turn a bug into an attempt to fill the disk. */
/* --- statfs(2) and fstatfs(2) --------------------------------------------
 *
 * The Linux x86-64 struct statfs, 120 bytes:
 *
 *   0   f_type      the filesystem's magic number
 *   8   f_bsize     the optimal transfer block size
 *   16  f_blocks    total blocks
 *   24  f_bfree     free blocks
 *   32  f_bavail    free blocks available to an unprivileged caller
 *   40  f_files     total inodes
 *   48  f_ffree     free inodes
 *   56  f_fsid      two 32-bit words
 *   64  f_namelen   longest filename
 *   72  f_frsize    fragment size
 *   80  f_flags     mount flags
 *   88  f_spare[4]
 *
 * Assembled HERE rather than by the filesystem, because most of it is ABI
 * shape rather than filesystem fact - the magic number, the padded fsid and
 * the four spare words say nothing about how full a disk is. fs_statfs_t is
 * the four numbers a filesystem actually knows.
 *
 * f_files and f_ffree are ZERO, and that is FAT being answered honestly
 * rather than a gap: FAT has no inode table, so there is no count of them to
 * report and no limit to run out of. Linux's own vfat driver reports zero for
 * exactly the same reason. A caller that divides by f_files has a bug on real
 * Linux too.
 */
#define MSDOS_SUPER_MAGIC 0x4D44u

static uint64 fill_statfs(uint64 buf_ptr, const fs_statfs_t *st) {
    uint8 *p = (uint8 *)buf_ptr;
    uint64 i;

    if (!user_ptr_ok(buf_ptr)) {
        return (uint64)-14;
    }
    for (i = 0; i < 120; i++) {
        p[i] = 0;
    }
    *(uint64 *)(p +  0) = MSDOS_SUPER_MAGIC;
    *(uint64 *)(p +  8) = st->block_size;
    *(uint64 *)(p + 16) = st->blocks;
    *(uint64 *)(p + 24) = st->blocks_free;
    /* f_bavail equals f_bfree: there is no reserved-for-root pool here,
     * because there is no root to reserve it from. Reporting a smaller
     * f_bavail would be inventing a policy nothing implements. */
    *(uint64 *)(p + 32) = st->blocks_free;
    *(uint64 *)(p + 64) = st->name_max;
    *(uint64 *)(p + 72) = st->block_size;    /* f_frsize */
    return 0;
}

static uint64 sys_statfs(uint64 path_ptr, uint64 buf_ptr) {
    char resolved[PATH_MAX_LEN];
    fs_statfs_t st;
    int rc = resolve_user_path(path_ptr, resolved, sizeof(resolved));

    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    rc = fs_statfs(resolved, &st);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    return fill_statfs(buf_ptr, &st);
}

/* fstatfs(2). Answered from the CWD rather than from the descriptor, and that
 * is a real limitation rather than an oversight worth hiding: an open file
 * object holds a node, and a node does not carry a path - so there is no way
 * from here to name the volume the descriptor is on. It is right whenever the
 * descriptor and the working directory are on the same volume, which with one
 * mounted volume is always, and wrong the moment a program fstatfs's a file
 * on a second volume.
 *
 * The honest alternative is fs_node_t carrying its fs_volume_t, which it
 * already does - so the real fix is an fs_statfs_vol() taking the volume the
 * node names. That is small and it is not done here because sys_fstatfs has
 * no caller yet; the descriptor is validated so that a bad one is -EBADF
 * rather than a plausible answer about the wrong disk. */
static uint64 sys_fstatfs(uint64 fd, uint64 buf_ptr) {
    process_t *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);
    fs_statfs_t st;
    int rc;

    if (f == NULL) {
        return (uint64)-9;
    }
    rc = fs_statfs(p->cwd, &st);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    return fill_statfs(buf_ptr, &st);
}

static uint64 sys_truncate(uint64 path_ptr, uint64 length) {
    char resolved[PATH_MAX_LEN];
    int rc = resolve_user_path(path_ptr, resolved, sizeof(resolved));

    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    if ((int64)length < 0) {
        return (uint64)-22;                   /* -EINVAL */
    }
    return (uint64)(int64)fs_truncate_at(resolved, length);
}

static uint64 sys_ftruncate(uint64 fd, uint64 length) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);

    if (f == NULL) {
        return (uint64)-9;                    /* -EBADF */
    }
    if ((int64)length < 0) {
        return (uint64)-22;
    }
    /* POSIX: ftruncate on a descriptor not open for writing is -EINVAL, not
     * -EBADF. The descriptor is fine; the request is not one it can carry. */
    if (!(f->access & ACCESS_WRITE)) {
        return (uint64)-22;
    }
    return (uint64)(int64)fileobj_truncate(f->obj, length);
}

/* chmod(2) and fchmod(2), both fs_chmod (kernel/fs/vfs.c): rewrite exactly
 * the owner@/group@/everyone@ entries of the object's ACL to match `mode` -
 * acl_apply_chmod - leaving everything else (a named grant, a deny, an
 * inherited entry) untouched, and set the setuid/setgid/sticky bits from
 * it, subject to acl_chmod_mode's silent S_ISGID rule.
 *
 * fs_chmod's gate is fs_setacl's, and it is what actually decides whether
 * this caller may: it checks
 * ACE_WRITE_ACL, which acl_from_mode already grants the owner
 * unconditionally (POSIX gives the owner chmod regardless of the mode bits)
 * and which cred_is_supreme's bypass covers the same way it covers every
 * other check - so root, or the one uid holding the supreme privilege
 * (kernel/fs/acl.c), can always chmod, exactly like every other ACL check
 * in this kernel.
 *
 * chown, below, does NOT go through here: ACE_WRITE_ACL is the owner's
 * unconditionally, and an owner must not thereby be able to give a file
 * away. */
static uint64 sys_chmod_node(fs_node_t *n, uint64 mode) {
    cred_t c;

    proc_cred(proc_current(), &c);
    return (uint64)(int64)fs_chmod(n, (const struct cred *)&c, (uint32)mode);
}

static uint64 sys_chmod(uint64 path_ptr, uint64 mode) {
    char resolved[PATH_MAX_LEN];
    fs_node_t node;
    int rc = resolve_user_path(path_ptr, resolved, sizeof(resolved));

    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    rc = fs_lookup(resolved, &node);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    return sys_chmod_node(&node, mode);
}

static uint64 sys_fchmod(uint64 fd, uint64 mode) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);
    fs_node_t    node;

    if (f == NULL) {
        return (uint64)-9;                    /* -EBADF */
    }
    if (!fileobj_is_file(f->obj)) {
        return (uint64)-9;
    }
    /* A COPY of the object's node, not the live one fileobj_node holds -
     * that accessor is const on purpose (see fileobj.h). The change still
     * reaches disk correctly (gnfs_op_setacl updates the real onode this
     * copy's object number names); what does not happen is this
     * DESCRIPTOR'S cached mode refreshing until the next lookup re-reads
     * it - the same small, named gap sys_fstatfs's own comment already
     * accepts for a different field. */
    node = *fileobj_node(f->obj);
    return sys_chmod_node(&node, mode);
}

/* chown(2) and fchown(2). Everything that decides anything is fs_setowner's
 * (kernel/fs/vfs.c) and acl_chown_permitted's (kernel/fs/acl.c): an owner
 * may chgrp its own file into a group it is in, a holder of ACE_WRITE_OWNER
 * may take ownership for ITSELF, and only root or the supreme uid may give a
 * file to an arbitrary other uid.
 *
 * The ids are truncated to 32 bits rather than compared as 64: uid_t is
 * 32-bit, and a libc that sign-extends its (uid_t)-1 into the register
 * must still mean "keep", not an id that matches nothing. */
static uint64 sys_chown_node(fs_node_t *n, uint64 uid, uint64 gid) {
    cred_t c;

    proc_cred(proc_current(), &c);
    return (uint64)(int64)fs_setowner(n, (const struct cred *)&c,
                                     (uint32)uid, (uint32)gid);
}

static uint64 sys_chown(uint64 path_ptr, uint64 uid, uint64 gid) {
    char resolved[PATH_MAX_LEN];
    fs_node_t node;
    int rc = resolve_user_path(path_ptr, resolved, sizeof(resolved));

    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    rc = fs_lookup(resolved, &node);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    return sys_chown_node(&node, uid, gid);
}

static uint64 sys_fchown(uint64 fd, uint64 uid, uint64 gid) {
    process_t   *p = proc_current();
    open_file_t *f = handle_get(p->handles, (int)fd);
    fs_node_t    node;

    if (f == NULL) {
        return (uint64)-9;                    /* -EBADF */
    }
    if (!fileobj_is_file(f->obj)) {
        return (uint64)-9;
    }
    /* A copy, for the reason sys_fchmod's own comment gives: the owner
     * reaches disk, this descriptor's cached uid/gid does not refresh. */
    node = *fileobj_node(f->obj);
    return sys_chown_node(&node, uid, gid);
}

static uint64 sys_mkdir(uint64 path_ptr, uint64 mode) {
    char resolved[PATH_MAX_LEN];
    cred_t c;
    int rc = resolve_user_path(path_ptr, resolved, sizeof(resolved));

    /* `mode`, masked by this process's umask - fs_mkdir decides which bits
     * of it can survive at all. FAT still has nowhere to store any of it;
     * gnfs stores it. */
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    proc_cred(proc_current(), &c);
    return (uint64)(int64)fs_mkdir(resolved, (const struct cred *)&c,
                                   (uint32)(mode & ~proc_current()->umask));
}

static uint64 sys_rmdir(uint64 path_ptr) {
    char resolved[PATH_MAX_LEN];
    cred_t c;
    int rc = resolve_user_path(path_ptr, resolved, sizeof(resolved));

    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    proc_cred(proc_current(), &c);
    return (uint64)(int64)fs_rmdir(resolved, (const struct cred *)&c);
}

static uint64 sys_unlink(uint64 path_ptr) {
    char resolved[PATH_MAX_LEN];
    cred_t c;
    int rc = resolve_user_path(path_ptr, resolved, sizeof(resolved));

    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    proc_cred(proc_current(), &c);
    return (uint64)(int64)fs_unlink(resolved, (const struct cred *)&c);
}

static uint64 sys_rename(uint64 old_ptr, uint64 new_ptr) {
    char old_resolved[PATH_MAX_LEN];
    char new_resolved[PATH_MAX_LEN];
    cred_t c;
    int rc;

    rc = resolve_user_path(old_ptr, old_resolved, sizeof(old_resolved));
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    rc = resolve_user_path(new_ptr, new_resolved, sizeof(new_resolved));
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    proc_cred(proc_current(), &c);
    return (uint64)(int64)fs_rename(old_resolved, new_resolved,
                                    (const struct cred *)&c);
}

/* unlinkat's AT_REMOVEDIR flag makes it rmdir instead. That single bit is
 * why musl implements both rmdir(3) and unlink(3) through this one call, so
 * getting it wrong breaks rmdir with no rmdir syscall in sight. */
#define AT_REMOVEDIR 0x200

static uint64 sys_unlinkat(uint64 dirfd, uint64 path_ptr, uint64 flags) {
    char resolved[PATH_MAX_LEN];
    cred_t c;
    int rc;

    /* AT_FDCWD only, matching every other *at call in this file: a real
     * dirfd needs a per-descriptor directory to resolve against, which
     * openat does not keep either. */
    if ((int64)dirfd != AT_FDCWD) {
        return (uint64)-9;            /* -EBADF */
    }
    rc = resolve_user_path(path_ptr, resolved, sizeof(resolved));
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    proc_cred(proc_current(), &c);
    if (flags & AT_REMOVEDIR) {
        return (uint64)(int64)fs_rmdir(resolved, (const struct cred *)&c);
    }
    return (uint64)(int64)fs_unlink(resolved, (const struct cred *)&c);
}

static uint64 sys_mkdirat(uint64 dirfd, uint64 path_ptr, uint64 mode) {
    if ((int64)dirfd != AT_FDCWD) {
        return (uint64)-9;
    }
    return sys_mkdir(path_ptr, mode);
}

static uint64 sys_renameat(uint64 olddirfd, uint64 old_ptr, uint64 newdirfd,
                           uint64 new_ptr) {
    if ((int64)olddirfd != AT_FDCWD || (int64)newdirfd != AT_FDCWD) {
        return (uint64)-9;
    }
    return sys_rename(old_ptr, new_ptr);
}

static uint64 sys_chdir(uint64 path_ptr) {
    const char *path = (const char *)path_ptr;
    char resolved[PATH_MAX_LEN];
    fs_node_t node;
    int rc;
    uint64 i;

    if (!user_ptr_ok(path_ptr)) {
        return (uint64)-14;       /* -EFAULT */
    }
    if (path[0] == '\0') {
        return (uint64)-2;        /* -ENOENT: chdir("") is not chdir(".") */
    }

    if (path_normalize(current_dir_of(), path, resolved, sizeof(resolved)) != PATH_OK) {
        return (uint64)-36;       /* -ENAMETOOLONG */
    }

    rc = fs_lookup(resolved, &node);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    /* The check that makes chdir mean something. Without it "cd /etc/motd"
     * succeeds and every later relative path resolves against a file. */
    if (!node.is_dir) {
        return (uint64)-20;       /* -ENOTDIR */
    }

    /* Commit only after every check has passed - a failed cd must leave the
     * process exactly where it was. */
    {
        char *cwd = current_dir_of();
        for (i = 0; resolved[i] != '\0'; i++) {
            cwd[i] = resolved[i];
        }
        cwd[i] = '\0';
    }
    return 0;
}

static uint64 sys_getcwd(char *buf, uint64 size) {
    uint64 len = 0;
    uint64 i;

    if (!user_ptr_ok((uint64)buf)) {
        return (uint64)-14;
    }
    {
        const char *cwd = current_dir_of();
        while (cwd[len] != '\0') {
            len++;
        }
        if (size < len + 1) {
            return (uint64)-34;       /* -ERANGE */
        }
        for (i = 0; i <= len; i++) {
            buf[i] = cwd[i];
        }
    }
    return len + 1;               /* length INCLUDING the terminator */
}

/* --- tracing ------------------------------------------------------------
 * strace, from the inside. Every call prints before it runs, so when the
 * process dies the LAST line is what it was doing - which is the one fact a
 * register dump cannot give you.
 *
 * Off now that the startup path reaches a prompt: an interactive shell issues
 * a burst of calls per keystroke, and a trace line between each one makes the
 * echo unreadable and scrolls the prompt off the screen. Set it back to 1 the
 * moment something unexplained happens - it is still the fastest way to find
 * out which call a process died in. */
#define SYSCALL_TRACE 0

#if SYSCALL_TRACE
static const char *syscall_name(uint64 nr) {
    switch (nr) {
        case SYS_read:            return "read";
        case SYS_close:           return "close";
        case SYS_open:            return "open";
        case SYS_openat:          return "openat";
        case SYS_lseek:           return "lseek";
        case SYS_getdents64:      return "getdents64";
        case SYS_dup:             return "dup";
        case SYS_dup2:            return "dup2";
        case SYS_pipe:            return "pipe";
        case SYS_pipe2:           return "pipe2";
        case SYS_write:           return "write";
        case SYS_writev:          return "writev";
        case SYS_stat:            return "stat";
        case SYS_fstat:           return "fstat";
        case SYS_lstat:           return "lstat";
        case SYS_mmap:            return "mmap";
        case SYS_mremap:          return "mremap";
        case SYS_sigaltstack:     return "sigaltstack";
        case SYS_waitid:          return "waitid";
        case SYS_execveat:        return "execveat";
        case SYS_socketpair:      return "socketpair";
        case SYS_socket:          return "socket";
        case SYS_sched_setaffinity: return "sched_setaffinity";
        case SYS_sched_getaffinity: return "sched_getaffinity";
        case SYS_getcpu:          return "getcpu";
        case SYS_listen:          return "listen";
        case SYS_accept:          return "accept";
        case SYS_accept4:         return "accept4";
        case SYS_shutdown:        return "shutdown";
        case SYS_getpeername:     return "getpeername";
        case SYS_setsockopt:      return "setsockopt";
        case SYS_getsockopt:      return "getsockopt";
        case SYS_sendmsg:         return "sendmsg";
        case SYS_recvmsg:         return "recvmsg";
        case SYS_bind:            return "bind";
        case SYS_connect:         return "connect";
        case SYS_sendto:          return "sendto";
        case SYS_recvfrom:        return "recvfrom";
        case SYS_getsockname:     return "getsockname";
        case SYS_eventfd2:        return "eventfd2";
        case SYS_munmap:          return "munmap";
        case SYS_brk:             return "brk";
        case SYS_arch_prctl:      return "arch_prctl";
        case SYS_uname:           return "uname";
        case SYS_newfstatat:      return "newfstatat";
        case SYS_clock_gettime:   return "clock_gettime";
        case SYS_gettimeofday:    return "gettimeofday";
        case SYS_nanosleep:       return "nanosleep";
        case SYS_getrandom:       return "getrandom";
        case SYS_fcntl:           return "fcntl";
        case SYS_access:          return "access";
        case SYS_faccessat:       return "faccessat";
        case SYS_faccessat2:      return "faccessat2";
        case SYS_poll:            return "poll";
        case SYS_ppoll:           return "ppoll";
        case SYS_futex:           return "futex";
        case SYS_gettid:          return "gettid";
        case SYS_getcwd:          return "getcwd";
        case SYS_chdir:           return "chdir";
        /* A block of the DISPATCH switch used to sit here, spliced into the
         * middle of this name table: forty-six lines of
         * `return sys_rt_sigaction(frame->...)` and friends inside a function
         * whose return type is const char *. It was invisible because
         * SYSCALL_TRACE is 0, so the whole function is behind an #if and
         * nothing ever compiled it.
         *
         * The effect was that turning the trace on - which the comment above
         * recommends as the fastest way to find out which call a process died
         * in - did not build. The one tool you reach for when something is
         * unexplained was itself broken, and you would have found that out
         * while already debugging something else.
         *
         * Dead code behind an #if is still code, and nothing had ever
         * compiled this. That is the actual lesson: a debug facility that is
         * never built is a debug facility that does not work. */
        case SYS_rt_sigreturn:  return "rt_sigreturn";
        case SYS_kill:          return "kill";
        case SYS_setpgid:       return "setpgid";
        case SYS_getpgrp:       return "getpgrp";
        case SYS_getpgid:       return "getpgid";
        case SYS_tgkill:        return "tgkill";
        case SYS_reboot:          return "reboot";
        case SYS_execve:          return "execve";
        case SYS_vfork:           return "vfork";
        case SYS_fork:            return "fork";
        case SYS_clone:           return "clone";
        case SYS_wait4:           return "wait4";
        case SYS_fchdir:          return "fchdir";
        case SYS_getpid:          return "getpid";
        case SYS_getppid:         return "getppid";
        case SYS_getuid:          return "getuid";
        case SYS_geteuid:         return "geteuid";
        case SYS_getgid:          return "getgid";
        case SYS_getegid:         return "getegid";
        case SYS_setuid:          return "setuid";
        case SYS_getgroups:       return "getgroups";
        case SYS_setgroups:       return "setgroups";
        case SYS_setgid:          return "setgid";
        case SYS_mprotect:        return "mprotect";
        case SYS_prctl:           return "prctl";
        case SYS_rt_sigaction:    return "rt_sigaction";
        case SYS_rt_sigprocmask:  return "rt_sigprocmask";
        case SYS_ioctl:           return "ioctl";
        case SYS_set_tid_address: return "set_tid_address";
        case SYS_set_robust_list: return "set_robust_list";
        case SYS_rseq:            return "rseq";
        case SYS_prlimit64:       return "prlimit64";
        case SYS_readlinkat:      return "readlinkat";
        case SYS_exit:            return "exit";
        case SYS_exit_group:      return "exit_group";
        default:                  return NULL;
    }
}

static void trace_call(struct syscall_frame *f) {
    const char *name = syscall_name(f->rax);

    print_string("  >", 0x08);
    if (name != NULL) {
        print_string(name, 0x08);
    } else {
        print_hex((uint32)f->rax, 0x08);
    }
    print_string("(", 0x08);
    print_hex64(f->rdi, 0x08);
    print_string(",", 0x08);
    print_hex64(f->rsi, 0x08);
    print_string(",", 0x08);
    print_hex64(f->rdx, 0x08);
    print_string(")\n", 0x08);
}
#endif   /* SYSCALL_TRACE - the name table and the printer are only referenced
          * from here, so both go with it rather than sitting unused. */


/* The preemption point.
 *
 * A timer interrupt only sets a flag; the switch happens here, on the way out
 * of a syscall, because this is a kernel stack with nothing live below the
 * caller. Switching from inside the interrupt handler would unwind a frame
 * the handler is still standing on.
 *
 * The interrupt path does the same on its way out, so a process that never
 * makes a syscall is preempted anyway: isr_common saves all fifteen general
 * registers, interrupt_dispatch charges the tick and calls return_to_user,
 * and a compute-bound user loop loses the CPU at the end of its quantum like
 * anything else. Both entry paths converge on return_to_user for that reason
 * - two places deciding when to switch would drift. */
/* Answers for the Linux number space. Its own function rather than a constant
 * comparison inside syscall_dispatch because 15 means rt_sigreturn only under
 * this ABI - see personality.h. */
int linux_is_sigreturn(uint64 nr) {
    return nr == SYS_rt_sigreturn;
}

uint64 syscall_dispatch(struct syscall_frame *frame) {
    const syscall_personality_t *pers;
    uint64 rc;

    /* Into the kernel: take the big kernel lock. SYSCALL cleared IF, so the
     * spin (if another CPU is in the kernel) runs with interrupts off and
     * answers IPIs through the mailboxes. Released at syscall_return. */
    bkl_acquire();
    smp_this_cpu()->user_entries++;

    /* Killed while it waited for the lock - by exit_group, a fatal signal or
     * NtTerminateThread on another CPU. The call it was making must not run:
     * a terminated thread's last WriteFile going out anyway is exactly what
     * "terminated" rules out. return_to_user switches away from a zombie
     * for good. */
    if (proc_current() != NULL && proc_current()->state == PROC_ZOMBIE) {
        return_to_user(1);
        return 0;                       /* not reached */
    }

    /* The fork in the road, taken once per syscall - by the CALL, not by the
     * process (personality_route; ROADMAP item 19). Everything below this
     * line is personality-independent: signals, preemption and the return to
     * user mode work the same whichever ABI made the call. */
    pers = personality_route(frame, &rc);
    if (pers != NULL) {
        rc = pers->dispatch(frame);
    }

    /* Signal delivery goes here rather than in return_to_user, because it
     * needs a syscall_frame to rewrite and the interrupt path has a different
     * struct. The practical effect is that a signal is delivered at the next
     * syscall boundary, which for anything that blocks is immediately - the
     * blocking call returns -EINTR and lands here.
     *
     * rt_sigreturn is excluded: it has just finished restoring a context, and
     * delivering into it would nest a second handler on top of the frame it
     * was in the middle of unwinding. */
    if (pers == NULL || !pers->is_sigreturn(frame->rax)) {
        struct process *me = proc_current();

        if (signal_pending(me)) {
            /* The return value is already in rc and must survive delivery -
             * the handler runs first, and the interrupted call's result is
             * restored by rt_sigreturn. */
            frame->rax = rc;
            if (signal_deliver(me, frame)) {
                rc = frame->rax;
            }
        }
    }

    /* Always a return to user mode from here, so the flag is unconditional. */
    return_to_user(1);
    return rc;
}

uint64 linux_syscall_dispatch(struct syscall_frame *frame) {
#if SYSCALL_TRACE
    trace_call(frame);
#endif

    switch (frame->rax) {
        case SYS_read:
            return sys_read(frame->rdi, frame->rsi, frame->rdx);

        case SYS_close:
            return sys_close(frame->rdi);

        case SYS_open:
            /* open() is openat(AT_FDCWD, ...) with the arguments shifted.
             * musl only emits openat, but busybox applets built against
             * other libcs still reach for this. */
            return sys_openat((uint64)(int64)AT_FDCWD, frame->rdi,
                              frame->rsi, frame->rdx);

        case SYS_openat:
            return sys_openat(frame->rdi, frame->rsi, frame->rdx, frame->r10);

        case SYS_lseek:
            return sys_lseek(frame->rdi, frame->rsi, frame->rdx);

        case SYS_getdents64:
            return sys_getdents64(frame->rdi, frame->rsi, frame->rdx);

        case SYS_pipe:
            return sys_pipe(frame->rdi);

        case SYS_pipe2:
            return sys_pipe2(frame->rdi, frame->rsi);

        case SYS_dup:
            return sys_dup(frame->rdi);

        case SYS_dup2:
            return sys_dup2(frame->rdi, frame->rsi);

        case SYS_write:
            return sys_write(frame->rdi, (const char *)frame->rsi, frame->rdx);

        case SYS_writev:
            return sys_writev(frame->rdi, frame->rsi, frame->rdx);

        case SYS_fstat:
            return sys_fstat(frame->rdi, frame->rsi);

        case SYS_stat:
        case SYS_lstat:
            return sys_stat_path(frame->rdi, frame->rsi);

        case SYS_brk:
            return sys_brk(frame->rdi);

        /* execveat(2). AT_FDCWD only, matching every other *at call in this
         * file - see sys_openat for the argument, which is that resolving
         * against an arbitrary directory descriptor needs a path back from an
         * open file object and a node does not carry one.
         *
         * AT_EMPTY_PATH is refused rather than ignored, and that is the flag
         * that matters here: it means "execute the descriptor itself", which
         * is how a caller runs a file it opened but cannot name - and
         * ignoring it would execute whatever the empty path resolved to
         * instead, which is the current directory. */
        case SYS_execveat:
            if ((int64)frame->rdi != AT_FDCWD) {
                return (uint64)-9;                  /* -EBADF */
            }
            if (frame->r8 != 0) {
                return (uint64)-22;                 /* -EINVAL: see above */
            }
            return sys_execve(frame->rsi, frame->rdx, frame->r10, frame);

        case SYS_socket:
            return sys_socket(frame->rdi, frame->rsi, frame->rdx);
        case SYS_bind:
            return sys_bind(frame->rdi, frame->rsi, frame->rdx);
        case SYS_connect:
            return sys_connect(frame->rdi, frame->rsi, frame->rdx);
        case SYS_sendto:
            return sys_sendto(frame->rdi, frame->rsi, frame->rdx,
                              frame->r10, frame->r8, frame->r9);
        case SYS_recvfrom:
            return sys_recvfrom(frame->rdi, frame->rsi, frame->rdx,
                                frame->r10, frame->r8, frame->r9);

        case SYS_getsockname:
            return sys_getsockname(frame->rdi, frame->rsi, frame->rdx);
        case SYS_getpeername:
            return sys_getpeername(frame->rdi, frame->rsi, frame->rdx);
        case SYS_listen:
            return sys_listen(frame->rdi, frame->rsi);
        case SYS_accept:
            return sys_accept4(frame->rdi, frame->rsi, frame->rdx, 0);
        case SYS_accept4:
            return sys_accept4(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        case SYS_shutdown:
            return sys_shutdown(frame->rdi, frame->rsi);
        case SYS_sendmsg:
            return sys_sendmsg(frame->rdi, frame->rsi, frame->rdx);
        case SYS_recvmsg:
            return sys_recvmsg(frame->rdi, frame->rsi, frame->rdx);
        case SYS_setsockopt:
            return sys_setsockopt(frame->rdi, frame->rsi, frame->rdx,
                                  frame->r10, frame->r8);
        case SYS_getsockopt:
            return sys_getsockopt(frame->rdi, frame->rsi, frame->rdx,
                                  frame->r10, frame->r8);

        case SYS_socketpair:
            return sys_socketpair(frame->rdi, frame->rsi, frame->rdx,
                                  frame->r10);

        case SYS_eventfd2:
            return sys_eventfd2(frame->rdi, frame->rsi);

        case SYS_waitid:
            return sys_waitid(frame->rdi, frame->rsi, frame->rdx,
                              frame->r10, frame->r8);

        case SYS_sigaltstack:
            return sys_sigaltstack(frame->rdi, frame->rsi);

        case SYS_mremap:
            return sys_mremap(frame->rdi, frame->rsi, frame->rdx,
                              frame->r10, frame->r8);

        case SYS_mmap:
            /* mmap takes six arguments; the sixth (offset) is in r9 and is
             * meaningless for an anonymous mapping, but names the first page
             * of a device mapping. */
            return sys_mmap(frame->rdi, frame->rsi, frame->rdx,
                            frame->r10, frame->r8, frame->r9);

        case SYS_munmap:
            return sys_munmap(frame->rdi, frame->rsi);

        case SYS_arch_prctl:
            return sys_arch_prctl(frame->rdi, frame->rsi);

        case SYS_uname:
            return sys_uname((char *)frame->rdi);

        case SYS_newfstatat:
            return sys_newfstatat(frame->rdi, frame->rdx);

        case SYS_clock_gettime:
            return sys_clock_gettime(frame->rdi, frame->rsi);

        case SYS_gettimeofday:
            return sys_gettimeofday(frame->rdi, frame->rsi);

        case SYS_nanosleep:
            return sys_nanosleep(frame->rdi, frame->rsi);

        case SYS_getrandom:
            return sys_getrandom((uint8 *)frame->rdi, frame->rsi);

        case SYS_getcwd:
            return sys_getcwd((char *)frame->rdi, frame->rsi);

        case SYS_chdir:
            return sys_chdir(frame->rdi);

        case SYS_rt_sigaction:
            return sys_rt_sigaction(frame->rdi, frame->rsi, frame->rdx, frame->r10);

        case SYS_rt_sigprocmask:
            return sys_rt_sigprocmask(frame->rdi, frame->rsi, frame->rdx, frame->r10);

        case SYS_rt_sigreturn:
            return signal_return(proc_current(), frame);

        case SYS_kill:
            return sys_kill(frame->rdi, frame->rsi);

        case SYS_tgkill:
            /* tgkill(tgid, tid, sig). The tid names the SLOT, which is what
             * pid means in the process table, so the second argument is the
             * one to signal - and it now names a real thread rather than
             * being the same number as the first by construction.
             *
             * The tgid is checked rather than ignored. Its purpose is to stop
             * a signal landing on a recycled tid that now belongs to an
             * unrelated program: a thread exits, its slot is reused, and a
             * kill aimed at the dead thread would otherwise hit whatever
             * moved in. That is the entire reason tgkill exists in preference
             * to tkill, so ignoring the first argument would be implementing
             * the call without the property it was added for. */
            {
                process_t *t = proc_find((int)frame->rsi);

                if (t == NULL) {
                    return (uint64)-3;         /* -ESRCH */
                }
                if (t->tgid != (int)frame->rdi) {
                    return (uint64)-3;
                }
                return sys_kill(frame->rsi, frame->rdx);
            }

        case SYS_setpgid:
            return sys_setpgid(frame->rdi, frame->rsi);

        case SYS_getpgrp:
            return (uint64)proc_current()->pgid;

        case SYS_getpgid:
            return sys_getpgid(frame->rdi);

        case SYS_reboot:
            return sys_reboot(frame->rdi, frame->rsi, frame->rdx, frame->r10);

        case SYS_execve:
            return sys_execve(frame->rdi, frame->rsi, frame->rdx, frame);

        case SYS_vfork:
            return sys_vfork(frame);

        case SYS_fork:
            return sys_fork(frame);

        case SYS_clone:
            return sys_clone(frame);

        case SYS_wait4:
            return sys_wait4(frame->rdi, frame->rsi, frame->rdx, frame->r10);

        case SYS_fchdir:
            /* Needs a descriptor table to name a directory by fd. ash uses
             * this for `cd -` and falls back to the path form when it fails,
             * so -EBADF here costs nothing today. */
            return (uint64)-9;

        /* Identity. The pid is real now - it comes from the process table
         * rather than being the constant 1 that a one-process kernel could
         * get away with. set_tid_address still answers with the pid because
         * there is one thread per process. */
        case SYS_getpid:
            /* The thread GROUP id, not the slot's own pid. Every thread of a
             * process must see the same answer - POSIX says so, and musl
             * caches the first one it gets, so two threads disagreeing does
             * not fail here. It fails later, somewhere with no visible
             * connection to threading. For a process with one thread the two
             * are equal, which is why this read correctly for so long. */
            return (uint64)proc_current()->tgid;

        case SYS_gettid:
            /* The slot's own pid, which IS the tid. This is the one call that
             * must NOT answer with the tgid, and it is what a thread uses to
             * name itself to tgkill. */
            return (uint64)proc_current()->pid;

        case SYS_set_tid_address:
            /* Records where to write a zero and futex-wake on exit, and
             * returns the caller's tid. musl calls this from its thread
             * bootstrap; answering without recording the address is a
             * pthread_join that waits forever on a thread that has already
             * exited. */
            {
                process_t *me = proc_current();

                if (frame->rdi == 0 || user_ptr_ok(frame->rdi)) {
                    me->clear_child_tid = frame->rdi;
                }
                return (uint64)me->pid;
            }
        case SYS_getppid:
            return (uint64)proc_current()->ppid;
        /* Real per-process credentials now, not a constant 0. They still
         * START at 0 - Genesis has no login - so every existing caller sees
         * what it always did, but setresuid/setuid are observable through
         * them, which they were not when this returned a literal. */
        case SYS_getuid:
        case SYS_geteuid:
            return proc_current()->uid;
        case SYS_getgid:
        case SYS_getegid:
            return proc_current()->gid;

        case SYS_readv:
            return sys_readv(frame->rdi, frame->rsi, frame->rdx);
        case SYS_pread64:
            return sys_pread64(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        case SYS_pwrite64:
            return sys_pwrite64(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        case SYS_dup3:
            /* dup3 is dup2 plus a flags word whose only defined bit is
             * O_CLOEXEC, and which must REFUSE oldfd == newfd rather than
             * silently succeeding the way dup2 does. That difference is the
             * entire reason dup3 exists. */
            if (frame->rdi == frame->rsi) {
                return (uint64)-22;
            }
            return sys_dup2(frame->rdi, frame->rsi);
        case SYS_fsync:
        case SYS_fdatasync:
            return sys_fsync(frame->rdi);
        case SYS_msync:
            return 0;                 /* write-through - see sys_fsync */
        case SYS_madvise:
            return sys_madvise();
        case SYS_umask:
            return sys_umask(frame->rdi);
        case SYS_sched_yield:
            return sys_sched_yield();
        case SYS_sched_setaffinity:
            return sys_sched_setaffinity(frame->rdi, frame->rsi, frame->rdx);
        case SYS_sched_getaffinity:
            return sys_sched_getaffinity(frame->rdi, frame->rsi, frame->rdx);
        case SYS_getcpu:
            return sys_getcpu(frame->rdi, frame->rsi);
        case SYS_setsid:
            return sys_setsid();
        case SYS_getsid:
            return sys_getsid(frame->rdi);
        case SYS_setresuid:
            return sys_setresuid(frame->rdi, frame->rsi, frame->rdx);
        case SYS_getgroups:
            return sys_getgroups(frame->rdi, frame->rsi);
        case SYS_setgroups:
            return sys_setgroups(frame->rdi, frame->rsi);
        case SYS_getresuid:
            return sys_getresuid(frame->rdi, frame->rsi, frame->rdx);
        case SYS_setresgid:
            return sys_setresgid(frame->rdi, frame->rsi, frame->rdx);
        case SYS_getresgid:
            return sys_getresgid(frame->rdi, frame->rsi, frame->rdx);
        case SYS_clock_getres:
            return sys_clock_getres(frame->rdi, frame->rsi);
        case SYS_clock_nanosleep:
            return sys_clock_nanosleep(frame->rdi, frame->rsi, frame->rdx,
                                       frame->r10);
        case SYS_times:
            return sys_times(frame->rdi);
        case SYS_rt_sigpending:
            return sys_rt_sigpending(frame->rdi, frame->rsi);
        case SYS_rt_sigsuspend:
            return sys_rt_sigsuspend(frame->rdi, frame->rsi);
        case SYS_pause:
            return sys_pause();

        case SYS_statfs:
            return sys_statfs(frame->rdi, frame->rsi);
        case SYS_fstatfs:
            return sys_fstatfs(frame->rdi, frame->rsi);

        case SYS_truncate:
            return sys_truncate(frame->rdi, frame->rsi);
        case SYS_ftruncate:
            return sys_ftruncate(frame->rdi, frame->rsi);

        case SYS_chmod:
            return sys_chmod(frame->rdi, frame->rsi);
        case SYS_fchmod:
            return sys_fchmod(frame->rdi, frame->rsi);
        case SYS_chown:
            return sys_chown(frame->rdi, frame->rsi, frame->rdx);
        case SYS_fchown:
            return sys_fchown(frame->rdi, frame->rsi, frame->rdx);

        case SYS_mkdir:
            return sys_mkdir(frame->rdi, frame->rsi);
        case SYS_rmdir:
            return sys_rmdir(frame->rdi);
        case SYS_unlink:
            return sys_unlink(frame->rdi);
        case SYS_rename:
            return sys_rename(frame->rdi, frame->rsi);
        case SYS_mkdirat:
            return sys_mkdirat(frame->rdi, frame->rsi, frame->rdx);
        case SYS_unlinkat:
            return sys_unlinkat(frame->rdi, frame->rsi, frame->rdx);
        case SYS_renameat:
            return sys_renameat(frame->rdi, frame->rsi, frame->rdx,
                                frame->r10);

        /* Accepted and ignored. Each is either a no-op given one process and
         * no memory protection changes, or something libc only probes. */
        /* rt_sigaction and rt_sigprocmask used to sit in this list, accepted
         * and ignored. They are implemented above now - a handler that was
         * silently discarded is exactly why Ctrl-C did nothing. */
        /* setuid and setgid used to sit in the accepted-and-ignored list
         * below, which was defensible while nothing read a uid and
         * indefensible the moment something did: setresuid changed the
         * credential and setuid did not, so the same intent expressed two
         * ways gave two different results. */
        case SYS_setuid:
            return sys_setuid(frame->rdi);
        case SYS_setgid:
            return sys_setgid(frame->rdi);

        case SYS_mprotect:
            /* Fell through into sys_ioctl, which read its first argument as a
             * file descriptor and answered -EBADF. A libc that checks it
             * concludes it cannot make its own data writable. Accepted and
             * ignored is what the comment above always claimed it was. */
            return 0;

        case SYS_prctl:
            /* Used to sit in the same accepted-and-ignored bucket as
             * mprotect above. It is real now - see sys_prctl - but every op
             * it does not recognise still falls through to the same 0,
             * which is what makes adding PR_GENESIS_* safe: no existing
             * caller's prctl(2) of some other op starts seeing a new
             * answer. */
            return sys_prctl(frame->rdi, frame->rsi);

        case SYS_ioctl:
            return sys_ioctl(frame->rdi, frame->rsi, frame->rdx);

        case SYS_fcntl:
            return sys_fcntl(frame->rdi, frame->rsi, frame->rdx);

        case SYS_access:
            /* access() is faccessat(AT_FDCWD, path, mode, 0) with the
             * arguments shifted, the same relationship open has to openat.
             * musl emits faccessat; busybox applets built against other
             * libcs still reach for this. */
            return sys_faccessat((uint64)(int64)AT_FDCWD, frame->rdi,
                                 frame->rsi, 0);

        case SYS_faccessat:
            return sys_faccessat(frame->rdi, frame->rsi, frame->rdx, 0);

        case SYS_faccessat2:
            /* The only difference from faccessat is that this one HAS a
             * flags argument rather than having it implied as zero. Both
             * land in the same function because there is no flag it could
             * carry that this kernel can act on - and musl tries this first
             * and falls back to faccessat on -ENOSYS, so implementing it
             * saves every access() call a failed syscall. */
            return sys_faccessat(frame->rdi, frame->rsi, frame->rdx,
                                 frame->r10);

        case SYS_poll:
            return sys_poll(frame->rdi, frame->rsi, frame->rdx);

        case SYS_ppoll:
            return sys_ppoll(frame->rdi, frame->rsi, frame->rdx, frame->r10);

        case SYS_set_robust_list:
            /* Declined, and the change from returning 0 is deliberate.
             *
             * A robust list is how a thread that dies holding a mutex lets
             * the kernel mark that mutex owner-dead, so the next thread to
             * take it learns the previous owner never released it instead of
             * blocking forever. Nothing here walks that list.
             *
             * Returning 0 said the list was registered. musl would then have
             * had every reason to believe a crashed thread's locks would be
             * released, and they would not be - a hang in an unrelated thread
             * with no trace back to the thread that actually died. -ENOSYS
             * says what is true. musl ignores the result of this call
             * entirely, so declining costs nothing today; it stops costing
             * nothing the moment a thread can die holding a lock, which is
             * now possible, which is why it changed in this commit and not a
             * later one. */
            return (uint64)-38;

        /* Declined deliberately. libc probes these and copes with failure;
         * pretending to succeed would be worse than saying no. */
        case SYS_rseq:
        case SYS_prlimit64:
            return (uint64)-38;   /* -ENOSYS */
        case SYS_readlinkat:
            return (uint64)-22;   /* -EINVAL */

        case SYS_exit:
            /* One thread. The rest of the group keeps running, which is what
             * pthread_exit needs and what makes a thread that returns from
             * its start routine not take the program with it. */
            return syscall_exit_process(frame->rdi, frame);

        case SYS_exit_group:
            /* Every thread. This is the split the old comment here promised
             * for "when clone() arrives" - the two numbers landed in the same
             * place because with one thread per process the distinction had
             * no content, and it has content now.
             *
             * Order matters: the others are retired FIRST, while this thread
             * is still the one running and still owns the tables the retire
             * path reads. Exiting self first would leave the loop walking a
             * group whose leader is already a zombie. */
            kill_thread_group(proc_current(), (int)(frame->rdi & 0xFF));
            return syscall_exit_process(frame->rdi, frame);

        case SYS_futex:
            return sys_futex(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        default:
            /* Printing the number turns "it hung" into a to-do item. This is
             * the loop that gets you to a shell: run, read the number,
             * implement it, repeat. */
            print_string("unimplemented syscall ", 0x0C);
            print_hex((uint32)frame->rax, 0x0C);
            print_string("\n", 0x0C);
            return (uint64)-38;   /* -ENOSYS */
    }
}

/* --- context switch ------------------------------------------------------
 * Save the callee-saved registers on the outgoing thread's kernel stack,
 * park its RSP, adopt the incoming thread's RSP, and pop its registers back.
 * The `ret` at the end returns to wherever the incoming thread last called
 * switch_context from - or, for a thread that has never run, to the address
 * its fabricated stack names.
 *
 * Only the callee-saved set is touched. Everything else is, by definition,
 * something the compiler already assumed a call could destroy: switch_context
 * looks like an ordinary function call to both sides, and the C around it
 * spills whatever it needs.
 *
 * The user-mode registers are NOT here. Those live in the syscall_frame
 * further up the same stack, saved on entry. This routine switches kernels;
 * the frame switches users.
 *
 * --- why RFLAGS is saved and restored -------------------------------------
 * pushfq/popfq are not decoration and were not here originally, which was a
 * latent bug the moment kernel threads arrived (kernel/include/kthread.h).
 *
 * IF is per-CPU state, not per-thread state, so without these two
 * instructions the interrupt flag simply LEAKS ACROSS THE SWITCH: whatever
 * the outgoing thread had is what the incoming one resumes with. A syscall
 * entered through SYSCALL has IF clear (MSR_SFMASK), and a kernel thread
 * must run with IF set - it has no other way to be woken, since its only
 * wakers are interrupts. So the two directions fail differently and both
 * fail quietly:
 *
 *   kthread -> user syscall   the syscall resumes with interrupts ON, in a
 *                             kernel with no locking around the structures
 *                             it is halfway through mutating
 *   user syscall -> kthread   the kernel thread runs with interrupts OFF,
 *                             the timer never fires, and the machine stops
 *                             with no output
 *
 * Restoring the flags makes the interrupt state part of the context, which
 * is what it always was. The cost is one push and one pop; popfq cannot
 * change CPL-sensitive bits from ring 0's point of view that matter here,
 * and IOPL/VM are not used by this kernel.
 *
 * thread_bootstrap_stack and kthread_bootstrap_stack both fabricate the
 * slot this pops - see them for the value each starts a new thread with. */
__asm__(
".text\n"
".globl switch_context\n"
".type switch_context, @function\n"
"switch_context:\n"          /* rdi = &prev->saved_rsp, rsi = next saved_rsp */
"    pushfq\n"
"    pushq %rbx\n"
"    pushq %rbp\n"
"    pushq %r12\n"
"    pushq %r13\n"
"    pushq %r14\n"
"    pushq %r15\n"
"    movq %rsp, (%rdi)\n"    /* park the outgoing stack pointer              */
"    movq %rsi, %rsp\n"      /* adopt the incoming one                       */
"    popq %r15\n"
"    popq %r14\n"
"    popq %r13\n"
"    popq %r12\n"
"    popq %rbp\n"
"    popq %rbx\n"
"    popfq\n"
"    ret\n"
".size switch_context, . - switch_context\n"
);
