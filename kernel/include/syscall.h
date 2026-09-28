#ifndef SYSCALL_H
#define SYSCALL_H

#include "typesk.h"

/* SYSCALL/SYSRET entry and the ring 3 transition.
 *
 * --- Why STAR looks the way it does ---
 * SYSCALL and SYSRET do not take selectors. They DERIVE them by fixed offsets
 * from two 16-bit fields in MSR_STAR, which is why the GDT layout in gdt.c is
 * not free choice:
 *
 *   SYSCALL:  CS = STAR[47:32]        SS = STAR[47:32] + 8
 *   SYSRET :  CS = STAR[63:48] + 16   SS = STAR[63:48] + 8   (both OR'd with 3)
 *
 * With STAR[47:32] = 0x08 that gives kernel CS 0x08 and SS 0x10. With
 * STAR[63:48] = 0x13 it gives user CS 0x23 and SS 0x1B. Those only line up
 * because the GDT is ordered kernel code, kernel data, USER DATA, user code -
 * user data before user code, which looks backwards until you see this.
 *
 * Get it wrong and nothing fails at boot. It fails the first time a syscall
 * RETURNS, with a #GP whose cause is three layers away from the symptom.
 *
 * --- The stack problem ---
 * SYSCALL does not switch stacks. Unlike an interrupt, there is no TSS
 * involvement: you arrive in ring 0 with RSP still pointing at whatever the
 * user process had. A hostile process therefore controls your kernel stack
 * pointer until you fix it, which is what swapgs and the per-CPU block below
 * exist for. TSS.rsp0 still matters, but only for interrupts arriving from
 * ring 3 - a different path.
 */

/* Selectors, matching gdt.c. The low 2 bits are the RPL. */
#define USER_CS   0x23   /* GDT entry 4 (0x20), RPL 3 */
#define USER_SS   0x1B   /* GDT entry 3 (0x18), RPL 3 */

#define MSR_STAR            0xC0000081u
#define MSR_LSTAR           0xC0000082u
#define MSR_SFMASK          0xC0000084u
#define MSR_GS_BASE         0xC0000101u
#define MSR_KERNEL_GS_BASE  0xC0000102u

/* Linux numbers, so binaries built against a normal toolchain work. */

/* --- added by Part 17's capability audit ---------------------------------
 * The audit compared a POSIX-ish checklist against what the table actually
 * dispatched and found 65 of 107. These close every gap that did NOT need a
 * new filesystem operation; the fourteen that do - mkdir, unlink, rename and
 * the rest of the file-NAMESPACE cluster - are recorded in ROADMAP instead,
 * because fs_ops_t has no mutation slots at all and adding them is a
 * filesystem change rather than a syscall one. */
#define SYS_readv           19
#define SYS_pread64         17
#define SYS_pwrite64        18
#define SYS_dup3           292
#define SYS_fsync           74
#define SYS_fdatasync       75
#define SYS_umask          95
#define SYS_madvise         28
#define SYS_msync           26
#define SYS_sched_yield     24
#define SYS_sched_setaffinity 203
#define SYS_sched_getaffinity 204
#define SYS_getcpu         309
#define SYS_setsid         112
#define SYS_getsid         124
#define SYS_setresuid      117
#define SYS_setresgid      119
#define SYS_getgroups      115
#define SYS_setgroups      116
#define SYS_getresuid      118
#define SYS_getresgid      120
#define SYS_clock_getres   229
#define SYS_clock_nanosleep 230
#define SYS_times          100
#define SYS_rt_sigpending   127
#define SYS_rt_sigsuspend   130
#define SYS_pause           34

/* The file-NAMESPACE calls. ROADMAP carried these as Owed after Part 17's
 * audit - not because they were hard syscalls but because fs_ops_t had no
 * mutation slots for them to reach. It does now. */
/* truncate(2) and ftruncate(2). Named in the ordered TODO's Owed list as
 * waiting on "that content path", which is what fs_ops_t::truncate is. */
#define SYS_truncate        76
#define SYS_ftruncate       77

/* chmod(2) and fchmod(2) - the ACL write path's own front door. Genuinely new
 * rather than "accepted and ignored until now": fs_ops_t had no setacl slot
 * for either to reach, the same shape truncate was in before item 7's
 * fs_ops_t::truncate landed. */
#define SYS_chmod           90
#define SYS_fchmod          91

/* chown(2) and fchown(2), on fs_ops_t::setowner and the gate chmod could not
 * share - see acl_chown_permitted (kernel/include/acl.h). */
#define SYS_chown           92
#define SYS_fchown          93

/* statfs(2) and fstatfs(2). Listed with those two and the only one of the
 * group that never actually depended on them: statfs needs FREE SPACE, which
 * on FAT means counting zero entries in the allocation table and has nothing
 * to do with writing. */
#define SYS_statfs         137
#define SYS_fstatfs        138
#define SYS_mkdir           83
#define SYS_rmdir           84
#define SYS_unlink          87
#define SYS_rename          82
#define SYS_mkdirat        258
#define SYS_unlinkat       263
#define SYS_renameat       264

#define SYS_read             0
#define SYS_write            1
#define SYS_stat             4
#define SYS_fstat            5
#define SYS_lstat            6
#define SYS_writev          20
#define SYS_open              2
#define SYS_lseek             8
#define SYS_openat          257
#define SYS_getdents64      217
#define SYS_close            3
#define SYS_dup             32
#define SYS_dup2            33
#define SYS_pipe            22
#define SYS_pipe2          293
#define SYS_mmap             9
#define SYS_munmap          11
#define SYS_newfstatat     262
#define SYS_brk             12
#define SYS_rt_sigaction    13
#define SYS_rt_sigprocmask  14
#define SYS_ioctl           16
#define SYS_readlinkat     267
#define SYS_mprotect        10
#define SYS_getpid          39
#define SYS_exit            60
#define SYS_uname           63
#define SYS_getuid         102
#define SYS_getgid         104
#define SYS_setuid         105
#define SYS_setgid         106
#define SYS_geteuid        107
#define SYS_getegid        108
#define SYS_getppid        110
#define SYS_getcwd          79
#define SYS_chdir           80
#define SYS_rt_sigreturn    15
#define SYS_kill            62
#define SYS_setpgid        109
#define SYS_getpgrp        111
#define SYS_getpgid        121
#define SYS_tgkill         234
#define SYS_reboot         169
#define SYS_execve          59
#define SYS_vfork           58
#define SYS_fork            57
#define SYS_clone           56

/* clone() flag bits, only as far as fork needs them. A libc that implements
 * fork() as clone(SIGCHLD) - glibc does, musl does not - has to land in the
 * same place as a raw fork(2), and telling the two apart is a matter of
 * checking that no sharing flag was asked for. */
#define CLONE_VM            0x00000100
#define CLONE_FS            0x00000200
#define CLONE_FILES         0x00000400
#define CLONE_SIGHAND       0x00000800
#define CLONE_VFORK         0x00004000
#define CLONE_PARENT        0x00008000
#define CLONE_THREAD        0x00010000
#define CLONE_SETTLS        0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID  0x01000000
#define CLONE_CSIGNAL       0x000000FF

/* The exact set musl's pthread_create asks for. Named rather than spelled out
 * at the test site, because "is this a thread" is a question with one answer
 * and several places that ask it. */
#define CLONE_THREAD_SET    (CLONE_VM | CLONE_FS | CLONE_FILES | \
                             CLONE_SIGHAND | CLONE_THREAD)

/* futex operations. Only the two musl's basic locking needs; FUTEX_REQUEUE
 * and the priority-inheritance operations are refused rather than
 * approximated. */
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK       (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))
#define SYS_wait4           61
#define SYS_fchdir          81
#define SYS_nanosleep       35
#define SYS_gettimeofday    96
#define SYS_clock_gettime  228
#define SYS_arch_prctl     158
#define SYS_prctl          157
#define SYS_set_tid_address 218
#define SYS_exit_group     231
#define SYS_set_robust_list 273
#define SYS_prlimit64      302
#define SYS_getrandom      318
#define SYS_rseq           334
#define SYS_fcntl           72
#define SYS_access          21
#define SYS_faccessat      269
#define SYS_faccessat2     439
#define SYS_poll             7
#define SYS_ppoll          271
#define SYS_select          23
#define SYS_pselect6       270
#define SYS_getrlimit       97
#define SYS_setrlimit      160
#define SYS_getrusage       98
#define SYS_futex          202
#define SYS_gettid         186

/* --- ROADMAP item 9: the syscalls the Owed list named -------------------- */
#define SYS_mremap          25
#define SYS_sigaltstack    131
#define SYS_waitid         247
#define SYS_execveat       322
#define SYS_socketpair      53
#define SYS_eventfd2       290

/* --- ROADMAP item 6: sockets as descriptors ------------------------------ */
#define SYS_socket          41
#define SYS_connect         42
#define SYS_sendto          44
#define SYS_recvfrom        45
#define SYS_bind            49
#define SYS_getsockname     51
#define SYS_accept          43
#define SYS_sendmsg         46
#define SYS_recvmsg         47
#define SYS_shutdown        48
#define SYS_listen          50
#define SYS_getpeername     52
#define SYS_setsockopt      54
#define SYS_getsockopt      55
#define SYS_accept4        288

/* Terminal ioctls. Only the handful a shell asks on startup; everything else
 * is answered with -ENOTTY rather than a blanket success. */
/* open() flags. Only the access mode and the few a shell actually sets;
 * anything else is accepted and ignored rather than rejected, because a libc
 * passes flags whose absence it can cope with. */
#define O_RDONLY      0x0000
#define O_WRONLY      0x0001
#define O_RDWR        0x0002
#define O_CREAT       0x0040
/* O_EXCL, which only means anything alongside O_CREAT: "create it, and fail
 * if it is already there". Added when O_CREAT stopped being refused, because
 * without it every O_CREAT is silently O_CREAT-or-open - and the one caller
 * that needs the distinction is a lock file, where getting it wrong is two
 * processes both believing they hold the lock. */
#define O_EXCL        0x0080
#define O_TRUNC       0x0200
#define O_APPEND      0x0400
#define O_NONBLOCK    0x0800
#define O_DIRECTORY   0x10000
#define O_CLOEXEC     0x80000

/* The status flags F_SETFL is allowed to change. Everything outside this mask
 * is either an access mode (fixed at open) or a creation flag (consumed by
 * open and meaningless afterwards), and POSIX says F_SETFL must IGNORE those
 * rather than fail on them - a libc passes the whole word back. */
#define O_SETFL_MASK  (O_APPEND | O_NONBLOCK)

#define AT_FDCWD      (-100)

/* fcntl commands. The numbers are Linux's; a libc passes them straight
 * through. */
#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_GETLK          5
#define F_SETLK          6
#define F_SETLKW         7
#define F_DUPFD_CLOEXEC  1030

/* The one per-descriptor flag, and it is deliberately not spelled the same as
 * HANDLE_CLOEXEC: this is the value a USER program passes, and the kernel's
 * internal bit is a separate namespace that happens to have the same value
 * today. Translating between them in one place is what keeps that an accident
 * rather than a dependency. */
#define FD_CLOEXEC       1

/* access(2) modes. */
#define F_OK  0
#define X_OK  1
#define W_OK  2
#define R_OK  4

/* poll(2) events. Only the bits with an implementation behind them: POLLPRI,
 * POLLRDBAND and the rest describe conditions no object here can be in. */
#define POLLIN     0x0001
#define POLLPRI    0x0002
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020

/* The three output-only bits. A caller does not ask for these; they are
 * reported whether or not they were requested, which is why every readiness
 * result is masked with (events | POLL_ALWAYS) rather than with events. */
#define POLL_ALWAYS (POLLERR | POLLHUP | POLLNVAL)

#define SEEK_SET      0
#define SEEK_CUR      1
#define SEEK_END      2

/* The terminal ioctl numbers (TCGETS, TIOCSPGRP...) live in kernel/dev/tty.c,
 * the only code that answers them. */

/* Linux's console display mode (<linux/kd.h>): a program that draws into
 * /dev/fb0 asks the console to stop drawing over it. */
#define KDSETMODE     0x4B3A
#define KDGETMODE     0x4B3B
#define KD_TEXT       0x00
#define KD_GRAPHICS   0x01

#define ARCH_SET_GS   0x1001
#define ARCH_SET_FS   0x1002
#define ARCH_GET_FS   0x1003
#define ARCH_GET_GS   0x1004

#define MSR_FS_BASE   0xC0000100u

/* mmap flags and protection bits we care about. */
#define PROT_NONE     0x0
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* Where anonymous mappings are placed. Below the user stack and well above
 * anything the ELF loader maps, so the three regions cannot collide. */
#define USER_MMAP_BASE  0x0000000030000000ULL
#define USER_MMAP_LIMIT 0x000000003F000000ULL

/* What syscall_entry builds on the kernel stack. Order is the reverse of the
 * push sequence, since the stack grows down. rax carries the syscall number in
 * and the return value out. */
/* A complete user context, not just the syscall arguments.
 *
 * The callee-saved registers below are new. They were not saved before
 * because nothing between kernel entry and exit ever touched them - the
 * kernel is compiled to preserve them, so they simply stayed live in the CPU
 * across a syscall.
 *
 * That stops being true the moment a DIFFERENT process runs in between. A
 * vfork child clobbers rbx, rbp and r12-r15 while the parent is suspended,
 * and the parent then resumes with six corrupted registers - most likely
 * holding its loop counters and frame pointer. Saving them turns this struct
 * from "the arguments" into "everything needed to resume", which is also what
 * a context switcher will want. */
struct syscall_frame {
    uint64 rax;
    uint64 rdi, rsi, rdx, r10, r8, r9;   /* the six argument registers */
    uint64 rip;                          /* SYSCALL leaves it in rcx    */
    uint64 rflags;                       /* SYSCALL leaves it in r11    */
    uint64 rbx, rbp, r12, r13, r14, r15; /* callee-saved: preserved for
                                          * resumption, not for the call */
};

/* Program the MSRs and point the per-CPU block at a kernel stack. Call after
 * gdt_init(), since the selectors it encodes must already exist. */
void syscall_init(uint64 kernel_stack_top);

/* The SYSCALL MSRs on an application processor - they are per CPU. */
void syscall_init_ap(void);

/* Stack the SYSCALL entry stub switches to. Must be updated on every context
 * switch alongside gdt_set_kernel_stack - they feed different entry paths
 * into the kernel and both have to name the incoming thread's stack. */
void syscall_set_kernel_stack(uint64 rsp);

/* Where the entry stub restores the user RSP from. execve rewrites this
 * together with the frame's rip to return into a different image. */
void syscall_set_user_rsp(uint64 rsp);

/* Read back what the entry stub parked, so a thread being switched away from
 * keeps its own user stack pointer rather than inheriting the next one's. */
uint64 syscall_get_user_rsp(void);

/* The user's GS base. Not a plain MSR accessor - see the comment on the
 * definition for why there are two MSRs and which one is the right one. */
void   syscall_set_user_gs_base(uint64 base);
uint64 syscall_get_user_gs_base(void);

/* Swap kernel stacks. Saves the callee-saved registers on the outgoing
 * thread's stack, stores its RSP through `save_to`, and resumes the thread
 * whose parked RSP is `load`. Returns - eventually - when someone switches
 * back. */
void switch_context(uint64 *save_to, uint64 load);

/* The single exit to user mode, in the entry stub. A thread that has never
 * run is started by pointing its fabricated kernel stack here. */
extern void syscall_return(void);

/* Build a kernel stack for a thread that has never run, so that switching to
 * it lands in syscall_return with `frame` as its user context. Returns the
 * value to store in thread.saved_rsp. */
uint64 thread_bootstrap_stack(uint64 kstack_top, const struct syscall_frame *frame);

/* Auxiliary vector tags. libc reads these to learn things it cannot ask for. */
#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
/* Where the dynamic linker itself was loaded.
 *
 * The one auxv tag an rtld cannot do without and cannot derive: it needs its
 * own base to relocate itself before it can call a single function through
 * the GOT. Zero for a static binary, which is what musl already reads it as
 * meaning - so emitting it unconditionally is safe and emitting it only
 * sometimes would change the auxv layout between two kinds of exec. */
#define AT_BASE     7
#define AT_ENTRY    9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_CLKTCK  17
#define AT_SECURE  23
#define AT_RANDOM  25

#define USER_MAX_ARGV  32
#define USER_MAX_ENVP  32

/* Map a user stack AND lay out the process start frame on it: argc, argv,
 * envp and the auxiliary vector, exactly as the System V ABI specifies. A
 * bare stack is not enough - libc reads argc from [rsp] as its first act.
 *
 * argv and envp are NULL-terminated arrays; envp may itself be NULL for an
 * empty environment. Returns the RSP to enter with, or 0 on failure. */
/* Geometry of the initial user stack, shared by the boot path and execve. */
#define USER_STACK_TOP   0x0000000040000000ULL
#define USER_STACK_SIZE  0x10000ULL           /* 64KB */

/* `interp_base` is where the dynamic linker was loaded, or 0 for a static
 * binary. `entry` stays the PROGRAM's entry point even when an interpreter
 * is present - AT_ENTRY is what the rtld jumps to when it has finished, and
 * passing it the rtld's own entry means the program never runs. Which
 * address the CPU starts at is execve's decision, not this function's. */
uint64 user_stack_create(uint64 top, uint64 size,
                         const char *const *argv,
                         const char *const *envp,
                         uint64 phdr, uint64 phnum, uint64 phentsize,
                         uint64 entry, uint64 interp_base);

/* Drop to ring 3 at `entry` with `stack` as RSP. Never returns. */
void enter_user_mode(uint64 entry, uint64 stack);

/* Where the process heap starts. Set from the ELF loader's highest mapped
 * address, so brk() grows into space nothing else claimed. */
void user_set_brk(uint64 base);

/* The one entry point the assembly stub calls. It selects a personality table
 * and calls through it; the ABI-specific work is in linux_syscall_dispatch
 * below and in nt.c. */
uint64 syscall_dispatch(struct syscall_frame *frame);

/* The Linux table's two entries. Not static, because personality.c names them
 * - but not called from anywhere else either: everything goes through
 * syscall_dispatch, which is what keeps "which ABI is this" a decision made
 * in exactly one place. */
uint64 linux_syscall_dispatch(struct syscall_frame *frame);

/* Shared mechanism, not Linux ABI. Ending a process and checking that a
 * pointer came from user space are the same operations whichever personality
 * asked for them; only the call NUMBER differs, and that is what the
 * personality table is for. */
uint64 syscall_exit_process(uint64 status, struct syscall_frame *frame);

/* exit_group: retire every other thread of the caller's group, then exit
 * the caller. What ends a PROCESS, as opposed to one thread of it - SYS_
 * exit_group, and NtTerminateProcess. */
uint64 syscall_exit_group(uint64 status, struct syscall_frame *frame);
int    user_ptr_ok(uint64 p);

/* Whether [p, p+len) is entirely inside the user half. Not the same question
 * as user_ptr_ok, which answers only for the first byte - see syscall.c. */
int    user_range_ok(uint64 p, uint64 len);
uint64 syscall_map_anonymous(uint64 addr, uint64 length, int fixed);
int    linux_is_sigreturn(uint64 nr);

#endif
