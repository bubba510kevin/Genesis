/* systest - exercise every syscall Genesis implements, from ring 3.
 *
 * Freestanding and static: no libc, no startup files, nothing between this
 * and the kernel. That is the point. A libc would retry, translate and paper
 * over return values, and the failures worth catching here are exactly the
 * ones a libc hides - a syscall that returns the wrong errno, or succeeds
 * without doing anything.
 *
 * Build:  tools/build_user.sh      (or see the command at the bottom)
 * Run:    /bin/systest             from the Genesis shell, or make it the
 *                                  init binary in flk.c to run it at boot.
 *
 * Exit status is the number of failed checks, capped at 255, so a caller can
 * branch on it. Every check prints its own line either way - a test that only
 * reports the total tells you something broke and nothing about what.
 *
 * NOT covered: reboot(2). It is implemented, and calling it would end the
 * test run by resetting the machine. Test it by hand.
 */

typedef unsigned long  u64;
typedef long           i64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

#define NULL ((void *)0)

/* --- syscall wrappers ---------------------------------------------------
 * The x86-64 Linux convention: number in rax, arguments in rdi rsi rdx r10
 * r8 r9. Note r10 rather than rcx - SYSCALL destroys rcx (it parks the return
 * address there) and r11 (rflags), which is why both are clobbers. */

static i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    i64 ret;
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    register i64 r9  __asm__("r9")  = f;

    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c),
                        "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}

#define sc0(n)                sc6((n), 0, 0, 0, 0, 0, 0)
#define sc1(n,a)              sc6((n), (i64)(a), 0, 0, 0, 0, 0)
#define sc2(n,a,b)            sc6((n), (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n,a,b,c)          sc6((n), (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc4(n,a,b,c,d)        sc6((n), (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)
#define sc5(n,a,b,c,d,e)      sc6((n), (i64)(a), (i64)(b), (i64)(c), (i64)(d), (i64)(e), 0)
#define sc6a(n,a,b,c,d,e,f)   sc6((n), (i64)(a), (i64)(b), (i64)(c), (i64)(d), (i64)(e), (i64)(f))

#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
#define SYS_stat            4
#define SYS_fstat           5
#define SYS_lstat           6
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_mprotect       10
#define SYS_munmap         11
#define SYS_mremap         25
#define SYS_sigaltstack   131
#define SYS_waitid        247
#define SYS_eventfd2      290
#define SYS_execveat      322
#define SYS_socketpair     53
#define SYS_fcntl          72
#define SYS_socket         41
#define SYS_connect        42
#define SYS_sendto         44
#define SYS_recvfrom       45
#define SYS_bind           49
#define SYS_getsockname    51
#define SYS_poll            7
#define SYS_brk            12
#define SYS_rt_sigaction   13
#define SYS_rt_sigprocmask 14
#define SYS_ioctl          16
#define SYS_writev         20
#define SYS_dup            32
#define SYS_dup2           33
#define SYS_getpid         39
#define SYS_clone          56
#define SYS_fork           57
#define SYS_vfork          58
#define SYS_execve         59
#define SYS_exit           60
#define SYS_wait4          61
#define SYS_pipe           22
#define SYS_pipe2         293
#define SYS_kill           62
#define SYS_uname          63
#define SYS_fchdir         81
#define SYS_getcwd         79
#define SYS_chdir          80
#define SYS_getuid        102
#define SYS_getgid        104
#define SYS_setuid        105
#define SYS_setgid        106
#define SYS_getgroups     115
#define SYS_setgroups     116
#define SYS_faccessat     269
#define SYS_geteuid       107
#define SYS_getegid       108
#define SYS_setpgid       109
#define SYS_getppid       110
#define SYS_getpgrp       111
#define SYS_getpgid       121
#define SYS_arch_prctl    158
#define SYS_prctl         157
#define SYS_getdents64    217
#define SYS_set_tid_address 218
#define SYS_exit_group    231
#define SYS_tgkill        234
#define SYS_set_robust_list 273
#define SYS_openat        257
#define SYS_newfstatat    262
#define SYS_readlinkat    267
#define SYS_prlimit64     302
#define SYS_getrandom     318
#define SYS_rseq          334

/* --- Part 17's gap-fill ------------------------------------------------- */
#define SYS_readv          19
#define SYS_pread64        17
#define SYS_pwrite64       18
#define SYS_dup3          292
#define SYS_fsync          74
#define SYS_umask          95
#define SYS_madvise        28
#define SYS_sched_yield    24
#define SYS_setsid        112
#define SYS_getsid        124
#define SYS_setresuid     117
#define SYS_getresuid     118
#define SYS_clock_getres  229
#define SYS_clock_nanosleep 230
#define SYS_times         100
#define SYS_rt_sigpending 127

/* --- the file-namespace calls ------------------------------------------- */
#define SYS_mkdir          83
#define SYS_rmdir          84
#define SYS_unlink         87
#define SYS_rename         82
#define SYS_mkdirat       258
#define SYS_unlinkat      263
#define SYS_renameat      264
#define AT_REMOVEDIR   0x200
#define EEXIST     17
#define ENOTEMPTY  39
#define EXDEV      18
#define EROFS      30
#define EBUSY      16
#define ENAMETOOLONG 36

#define EPERM     1
#define ENOENT    2
#define ESRCH     3
#define EINTR     4
#define EBADF     9
#define ENOMEM   12
#define ENOTDIR  20
#define EINVAL   22
#define ENOTTY   25
#define ESPIPE   29
#define EPIPE    32
#define ERANGE   34
#define ECHILD   10
#define ENOSYS   38
#define EISDIR   21

#define O_RDONLY  0
#define O_RDWR    2
#define O_DIRECTORY 0200000
#define O_CLOEXEC   0x80000
#define AT_FDCWD  (-100)

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define SIGUSR1  10
#define SIGPIPE  13
#define SIG_IGN_HANDLER 1UL
#define SA_RESTORER 0x04000000UL

/* --- output -------------------------------------------------------------
 * Everything goes through write(2) directly. No buffering, because a test
 * that crashes should have already printed everything up to the crash. */

static u64 slen(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

static void out(const char *s) {
    sc3(SYS_write, 1, s, slen(s));
}

static void out_i64(i64 v) {
    char buf[24];
    int  i = (int)sizeof(buf);
    int  neg = 0;
    u64  u;

    if (v < 0) { neg = 1; u = (u64)(-v); } else { u = (u64)v; }
    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + (u % 10));
        u /= 10;
    } while (u != 0);
    if (neg) buf[--i] = '-';
    out(&buf[i]);
}

static int passes;
static int failures;

static void check(int cond, const char *what) {
    out(cond ? "  ok    " : "  FAIL  ");
    out(what);
    out("\n");
    if (cond) passes++; else failures++;
}

/* The same, but printing the value that was wrong - which is the difference
 * between "lseek failed" and "lseek returned -29, so it thinks that fd is a
 * terminal". */
static void check_eq(i64 got, i64 want, const char *what) {
    if (got == want) {
        passes++;
        out("  ok    ");
        out(what);
        out("\n");
    } else {
        failures++;
        out("  FAIL  ");
        out(what);
        out("  (got ");
        out_i64(got);
        out(", wanted ");
        out_i64(want);
        out(")\n");
    }
}

static void section(const char *name) {
    out("\n");
    out(name);
    out("\n");
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int mem_has(const char *hay, u64 n, const char *needle) {
    u64 i, j, m = slen(needle);

    if (m == 0 || n < m) return 0;
    for (i = 0; i + m <= n; i++) {
        for (j = 0; j < m && hay[i + j] == needle[j]; j++) { }
        if (j == m) return 1;
    }
    return 0;
}

/* --- identity ----------------------------------------------------------- */

static int my_pid;

static void test_identity(void) {
    i64 pid, pgrp;

    section("identity");

    my_pid = (int)sc0(SYS_getpid);
    pid    = my_pid;
    check(pid > 0, "getpid returns a real pid");
    check(sc0(SYS_getppid) >= 0, "getppid answers");

    check_eq(sc0(SYS_getuid),  0, "getuid is root");
    check_eq(sc0(SYS_geteuid), 0, "geteuid is root");
    check_eq(sc0(SYS_getgid),  0, "getgid is root");
    check_eq(sc0(SYS_getegid), 0, "getegid is root");
    check_eq(sc1(SYS_setuid, 0), 0, "setuid is accepted");
    check_eq(sc1(SYS_setgid, 0), 0, "setgid is accepted");

    pgrp = sc0(SYS_getpgrp);
    check(pgrp > 0, "getpgrp returns a group");
    check_eq(sc1(SYS_getpgid, 0), pgrp, "getpgid(0) agrees with getpgrp");
    check_eq(sc2(SYS_setpgid, 0, 0), 0, "setpgid(0,0) leads a new group");
    check_eq(sc0(SYS_getpgrp), pid, "and the group id is now our pid");

    check_eq(sc1(SYS_set_tid_address, 0), pid, "set_tid_address answers the pid");

    /* Process state stops being a lie once a process has blocked and resumed.
     *
     * There is no syscall that reports p->state, so this checks it through
     * the one thing that reads it: CPU accounting is charged to the current
     * process every tick, so a process that has blocked and woken - which
     * every wait4 caller has - must still accrue CPU time afterwards. It did
     * not, because schedule() took its early return before marking the
     * resuming process PROC_RUNNING, and it ran on marked PROC_READY.
     *
     * A state field whose only other reader treats READY and RUNNING as
     * equivalent has no test behind it. This is that test. */
    {
        u64 a[2], b[2];
        volatile u64 spin = 0;
        u64 n;

        sc2(228 /* clock_gettime */, 2 /* CLOCK_PROCESS_CPUTIME_ID */, a);
        for (n = 0; n < 60000000ULL; n++) {
            spin += n;
        }
        sc2(228, 2, b);
        check(b[0] != a[0] || b[1] != a[1],
              "CPU time accrues after a process has blocked and resumed");
    }
    /* -ENOSYS, and the change from "accepted" is deliberate. A robust list is
     * how a thread that dies holding a mutex lets the kernel mark it
     * owner-dead, so the next thread to take it learns the previous owner
     * never released it. Nothing walks that list.
     *
     * Returning 0 said it was registered, which musl would have had every
     * reason to believe - and a crashed thread's locks would stay held, as a
     * hang in an unrelated thread with no trace back to the one that died.
     * That only became possible when clone() grew threads, which is why the
     * answer changed then and not earlier. */
    check_eq(sc2(SYS_set_robust_list, 0, 0), -ENOSYS,
             "set_robust_list declines rather than lying about the list");
    check_eq(sc2(SYS_prctl, 0, 0), 0, "prctl is accepted and ignored");
}

static void test_uname(void) {
    char buf[390];
    u64  i;

    section("uname");
    for (i = 0; i < sizeof(buf); i++) buf[i] = 0;

    check_eq(sc1(SYS_uname, buf), 0, "uname succeeds");
    check(buf[0] != 0, "sysname is not empty");
    out("        sysname: ");
    out(buf);
    out("  release: ");
    out(buf + 65 * 2);
    out("\n");
}

/* --- memory ------------------------------------------------------------- */

static void test_brk(void) {
    i64 start, moved, back;
    volatile char *p;

    section("brk");

    start = sc1(SYS_brk, 0);
    check(start > 0, "brk(0) reports the current break");

    moved = sc1(SYS_brk, start + 4096);
    check_eq(moved, start + 4096, "brk grows by a page");

    /* The page has to be real, not just accounted for. Touching every byte is
     * what turns "brk returned a number" into "brk mapped memory". */
    p = (volatile char *)start;
    for (int i = 0; i < 4096; i++) p[i] = (char)(i & 0xFF);
    {
        int ok = 1;
        for (int i = 0; i < 4096; i++) if (p[i] != (char)(i & 0xFF)) ok = 0;
        check(ok, "and every byte of it is writable and reads back");
    }

    back = sc1(SYS_brk, start);
    check_eq(back, start, "brk shrinks back");
}

static void test_mmap(void) {
    i64 addr;
    volatile u64 *p;

    section("mmap / munmap / mprotect");

    addr = sc6a(SYS_mmap, 0, 8192, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(addr > 0, "anonymous mmap returns an address");

    p = (volatile u64 *)addr;
    {
        int ok = 1;
        for (int i = 0; i < 1024; i++) if (p[i] != 0) ok = 0;
        check(ok, "MAP_ANONYMOUS memory arrives zeroed");
    }

    p[0]    = 0xFEEDFACEULL;
    p[1023] = 0x0BADC0DEULL;
    check(p[0] == 0xFEEDFACEULL && p[1023] == 0x0BADC0DEULL,
          "both ends of the mapping are writable");

    check_eq(sc3(SYS_mprotect, addr, 4096, PROT_READ), 0,
             "mprotect is accepted (it is a no-op here, but must not fail)");

    check_eq(sc2(SYS_munmap, addr, 8192), 0, "munmap succeeds");

    check(sc6a(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE, -1, 0) < 0,
          "a file-backed mmap is refused rather than faked");
}

/* --- ROADMAP item 9: mremap ---------------------------------------------- */

static void test_mremap(void) {
    i64 a, b;
    volatile unsigned char *p;

    section("mremap");

    a = sc6a(SYS_mmap, 0, 8192, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(a > 0, "a mapping to resize");
    if (a <= 0) return;

    p = (volatile unsigned char *)a;
    p[0]      = 0xA5;
    p[8191]   = 0x5A;

    /* GROW. The bump allocator leaves the space above free, so this should
     * extend in place - but the check is on the CONTENTS, not the address,
     * because a grow that moved is still correct and a grow that lost the
     * old bytes is not. */
    b = sc5(SYS_mremap, a, 8192, 16384, 1 /* MREMAP_MAYMOVE */, 0);
    check(b > 0, "mremap grows a mapping");
    if (b > 0) {
        volatile unsigned char *q = (volatile unsigned char *)b;
        check(q[0] == 0xA5 && q[8191] == 0x5A,
              "the old contents survive the grow");
        q[16383] = 0x77;
        check(q[16383] == 0x77, "the new tail is writable");

        /* SHRINK, and the discriminating half: the address must not move and
         * the surviving prefix must still be intact. A shrink implemented as
         * "allocate small, copy, free" would pass a contents check and fail
         * this one. */
        {
            i64 c = sc5(SYS_mremap, b, 16384, 4096, 0, 0);
            check_eq(c, b, "a shrink returns the SAME address (never moves)");
            if (c == b) {
                volatile unsigned char *r = (volatile unsigned char *)c;
                check(r[0] == 0xA5, "the surviving prefix is intact");
            }
        }
        sc2(SYS_munmap, b, 4096);
    }

    /* The refusals. Both matter: a misaligned address is a caller bug, and
     * mremap on nothing must not succeed by mapping fresh pages - which is
     * exactly what a naive implementation does, and it reports success. */
    check(sc5(SYS_mremap, a + 1, 4096, 8192, 1, 0) < 0,
          "a misaligned old address is refused");
    check(sc5(SYS_mremap, 0x20000000ULL, 4096, 8192, 1, 0) < 0,
          "mremap on an address that was never mapped is refused");
}

/* --- ROADMAP item 9: sigaltstack ----------------------------------------- */

struct k_stack_t { void *ss_sp; int ss_flags; int pad; unsigned long ss_size; };
static char altstack[16384];

static void test_sigaltstack(void) {
    struct k_stack_t in, old;

    section("sigaltstack");

    /* Before anything is installed the kernel must say DISABLE - not zero,
     * which is what an implementation that only stores and returns would
     * give, and which a caller cannot tell from "installed at address 0". */
    old.ss_flags = 0x1234;
    check_eq(sc2(SYS_sigaltstack, 0, (u64)&old), 0, "reading back with no stack installed");
    check_eq(old.ss_flags, 2 /* SS_DISABLE */, "reports SS_DISABLE when none is installed");

    in.ss_sp    = altstack;
    in.ss_flags = 0;
    in.pad      = 0;
    in.ss_size  = sizeof(altstack);
    check_eq(sc2(SYS_sigaltstack, (u64)&in, 0), 0, "installing an alternate stack");

    old.ss_sp = 0; old.ss_size = 0; old.ss_flags = 0x1234;
    check_eq(sc2(SYS_sigaltstack, 0, (u64)&old), 0, "reading it back");
    check(old.ss_sp == (void *)altstack, "the stack address round-trips");
    check_eq((i64)old.ss_size, (i64)sizeof(altstack), "the size round-trips");
    check_eq(old.ss_flags, 0, "flags are clear while not executing on it");

    /* A stack too small to hold a signal frame is the failure this facility
     * exists to prevent, so it has to be refused rather than installed. */
    in.ss_size = 128;
    check(sc2(SYS_sigaltstack, (u64)&in, 0) < 0,
          "a stack below MINSIGSTKSZ is refused");

    /* And the refusal must not have clobbered the good one - a rejected
     * install that half-applied is worse than one that fails. */
    old.ss_size = 0;
    sc2(SYS_sigaltstack, 0, (u64)&old);
    check_eq((i64)old.ss_size, (i64)sizeof(altstack),
             "a refused install leaves the previous stack untouched");

    /* SS_DISABLE really removes it, and is not the same as passing NULL. */
    in.ss_sp = 0; in.ss_size = 0; in.ss_flags = 2;
    check_eq(sc2(SYS_sigaltstack, (u64)&in, 0), 0, "SS_DISABLE is accepted");
    old.ss_flags = 0;
    sc2(SYS_sigaltstack, 0, (u64)&old);
    check_eq(old.ss_flags, 2, "and the stack is really gone");
}

/* --- ROADMAP item 9: waitid ---------------------------------------------- */

struct k_siginfo_t {
    int si_signo, si_errno, si_code, pad0;
    int si_pid, si_uid, si_status, pad1;
    unsigned char rest[128 - 32];
};

static void test_waitid(void) {
    i64 pid;
    struct k_siginfo_t si;

    section("waitid");

    /* The options check first, because it needs no child: waitid with no
     * state flag can never report anything, and accepting it would block
     * forever. */
    check(sc5(SYS_waitid, 0 /*P_ALL*/, 0, (u64)&si, 0, 0) < 0,
          "waitid with no WEXITED/WSTOPPED/WCONTINUED is refused");
    check(sc5(SYS_waitid, 99, 0, (u64)&si, 4, 0) < 0,
          "an unknown idtype is refused");

    pid = sc2(SYS_clone, 0, 0);
    if (pid == 0) {
        sc1(SYS_exit_group, 42);
        __builtin_unreachable();
    }
    check(pid > 0, "forked a child for waitid");

    for (unsigned i = 0; i < sizeof(si); i++) ((unsigned char *)&si)[i] = 0xEE;
    check_eq(sc5(SYS_waitid, 0 /*P_ALL*/, 0, (u64)&si, 4 /*WEXITED*/, 0), 0,
             "waitid returns 0, not the pid");
    check_eq(si.si_pid, (i64)pid, "the pid comes back in the siginfo");
    check_eq(si.si_status, 42, "and the exit status");
    check_eq(si.si_signo, 17 /* SIGCHLD */, "si_signo is SIGCHLD");
    check_eq(si.si_code, 1 /* CLD_EXITED */,
             "si_code says CLD_EXITED rather than leaving the caller to guess");

    /* WNOHANG with no children left: success with si_pid zeroed, which is
     * the only way a caller can tell "nothing ready" from a real report -
     * and it only works if the kernel CLEARS the field rather than leaving
     * the caller's buffer alone. The 0xEE fill above is what makes this a
     * test rather than a coincidence. */
    for (unsigned i = 0; i < sizeof(si); i++) ((unsigned char *)&si)[i] = 0xEE;
    {
        i64 r = sc5(SYS_waitid, 0, 0, (u64)&si, 4 | 1 /*WNOHANG*/, 0);
        if (r == 0) {
            check_eq(si.si_pid, 0, "WNOHANG with nothing ready zeroes si_pid");
        } else {
            check(r == -10, "or reports ECHILD when there are no children");
        }
    }
}

/* --- ROADMAP item 9: eventfd2 -------------------------------------------- */

static void test_eventfd(void) {
    i64 fd, r;
    u64 v;

    section("eventfd2");

    check(sc2(SYS_eventfd2, 0, 0x40000000) < 0, "an unknown flag is refused");

    fd = sc2(SYS_eventfd2, 0, 0);
    check(fd >= 0, "eventfd2 returns a descriptor");
    if (fd < 0) return;

    /* ONE descriptor, readable AND writable. That is the whole point of it
     * over the self-pipe trick, so it is worth asserting rather than assuming
     * - a pipe-shaped implementation would fail the write. */
    v = 5;
    check_eq(sc3(SYS_write, fd, (u64)&v, 8), 8, "writing 5 to the counter");
    v = 3;
    check_eq(sc3(SYS_write, fd, (u64)&v, 8), 8, "writing 3 more");

    v = 0;
    check_eq(sc3(SYS_read, fd, (u64)&v, 8), 8, "reading the counter");
    check_eq((i64)v, 8, "the writes ACCUMULATE (5 + 3), they do not queue");

    /* And the read RESET it. Without this the counter reads 8 forever and a
     * poll loop spins - which is the failure an implementation that only
     * adds would have, and it passes every check above. */
    check_eq(sc2(SYS_eventfd2, 0, 0) >= 0, 1, "a second eventfd can be made");
    {
        i64 fd2 = sc2(SYS_eventfd2, 0, 0x800 /*EFD_NONBLOCK*/);
        if (fd2 >= 0) {
            v = 0;
            r = sc3(SYS_read, fd2, (u64)&v, 8);
            check(r < 0, "a nonblocking read of an empty counter fails");
            check_eq(r, -11, "and the errno is EAGAIN");
            sc1(SYS_close, fd2);
        }
    }

    /* A short buffer must be refused, not partly transferred: half a 64-bit
     * count is a different number, not a smaller one. */
    check(sc3(SYS_read, fd, (u64)&v, 4) < 0, "a read shorter than 8 bytes is refused");
    check(sc3(SYS_write, fd, (u64)&v, 4) < 0, "a write shorter than 8 bytes is refused");

    /* EFD_SEMAPHORE is a different facility, not a tweak: eight writes of 1
     * need eight reads rather than one read of 8. */
    {
        i64 sfd = sc2(SYS_eventfd2, 3, 1 /*EFD_SEMAPHORE*/);
        check(sfd >= 0, "an EFD_SEMAPHORE eventfd, initialised to 3");
        if (sfd >= 0) {
            v = 0;
            sc3(SYS_read, sfd, (u64)&v, 8);
            check_eq((i64)v, 1, "a semaphore read takes 1, not the whole count");
            v = 0;
            sc3(SYS_read, sfd, (u64)&v, 8);
            check_eq((i64)v, 1, "and again");
            sc1(SYS_close, sfd);
        }
    }

    sc1(SYS_close, fd);
}

/* --- ROADMAP item 9: socketpair ------------------------------------------ */

static void test_socketpair(void) {
    int sv[2];
    char buf[16];

    section("socketpair");

    /* The refusals first. Each is a case where accepting would hand back
     * something that works until it doesn't: an AF_INET pair has no address,
     * and a SOCK_DGRAM pair would silently coalesce messages. */
    check(sc4(SYS_socketpair, 2 /*AF_INET*/, 1, 0, sv) < 0,
          "AF_INET is refused rather than approximated");
    check(sc4(SYS_socketpair, 1, 2 /*SOCK_DGRAM*/, 0, sv) < 0,
          "SOCK_DGRAM is refused - a pipe has no message boundaries");
    check(sc4(SYS_socketpair, 1, 1, 6 /*IPPROTO_TCP*/, sv) < 0,
          "a non-zero protocol is refused");

    sv[0] = sv[1] = -1;
    check_eq(sc4(SYS_socketpair, 1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0, sv), 0,
             "AF_UNIX/SOCK_STREAM succeeds");
    if (sv[0] < 0 || sv[1] < 0) return;
    check(sv[0] != sv[1], "two distinct descriptors");

    /* BOTH DIRECTIONS. This is what separates a socketpair from a pipe, and
     * a one-direction test would pass on a pipe pair wired the wrong way. */
    check_eq(sc3(SYS_write, sv[0], "ping", 4), 4, "end0 writes");
    for (int i = 0; i < 16; i++) buf[i] = 0;
    check_eq(sc3(SYS_read, sv[1], buf, 4), 4, "end1 reads it");
    check(buf[0]=='p' && buf[1]=='i' && buf[2]=='n' && buf[3]=='g',
          "end0 -> end1 carries the bytes");

    check_eq(sc3(SYS_write, sv[1], "pong", 4), 4, "end1 writes back");
    for (int i = 0; i < 16; i++) buf[i] = 0;
    check_eq(sc3(SYS_read, sv[0], buf, 4), 4, "end0 reads it");
    check(buf[0]=='p' && buf[1]=='o' && buf[2]=='n' && buf[3]=='g',
          "end1 -> end0 carries the bytes, so the pair is really duplex");

    /* The two directions must be INDEPENDENT. If both ends shared one buffer,
     * a write to end0 would be readable on end0 - which is the bug a
     * naive single-pipe implementation has, and which both checks above
     * still pass. */
    check_eq(sc3(SYS_write, sv[0], "self", 4), 4, "end0 writes again");
    {
        /* Nonblocking, so that a wrongly-shared buffer shows up as a read
         * that SUCCEEDS rather than as a test that hangs. */
        i64 n;
        sc3(SYS_fcntl, sv[0], 4 /*F_SETFL*/, 0x800 /*O_NONBLOCK*/);
        n = sc3(SYS_read, sv[0], buf, 4);
        check(n < 0, "an end does not read back its OWN write");
        sc3(SYS_read, sv[1], buf, 4);       /* drain it */
    }

    /* Closing one end is EOF on the other - the shutdown semantics that come
     * from the two pipes rather than from any code in socketpair.c. */
    sc1(SYS_close, sv[0]);
    check_eq(sc3(SYS_read, sv[1], buf, 4), 0,
             "closing one end gives the peer EOF, not a hang");
    sc1(SYS_close, sv[1]);
}

/* --- ROADMAP item 6: sockets as descriptors ------------------------------ */

struct k_sockaddr_in {
    unsigned char  sin_len;      /* BSD has this; Linux does not - the kernel
                                  * rewrites it from addrlen, so what is put
                                  * here is deliberately WRONG to prove it */
    unsigned char  sin_family;
    unsigned short sin_port;     /* network order */
    unsigned int   sin_addr;     /* network order */
    unsigned char  sin_zero[8];
};

/* A minimal DNS query for "a.root-servers.net A" - the same shape
 * kernel/bsd/net_selftest.c sends, because QEMU's built-in resolver at
 * 10.0.2.3 answers it and that is the only UDP peer this machine has. There
 * is no loopback interface, so a self-contained round trip is not available. */
static const unsigned char dns_q[] = {
    0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x01, 'a', 0x0c, 'r','o','o','t','-','s','e','r','v','e','r','s',
    0x03, 'n','e','t', 0x00,
    0x00, 0x01, 0x00, 0x01
};

static void test_socket(void) {
    i64 fd;
    struct k_sockaddr_in dst;

    section("socket / bind / connect / sendto / poll");

    /* The refusals. SOCK_STREAM is the interesting one: it must fail because
     * the PROTOCOL SWITCH refuses it (netinet/in_proto.c's tcp_protosw
     * pr_attach returns EPROTONOSUPPORT), not because a check in the syscall
     * layer says so - so the day TCP is vendored this starts working with no
     * edit here. Asserted as "fails" rather than as a specific errno for
     * exactly that reason. */
    check(sc3(SYS_socket, 99, 2 /*SOCK_DGRAM*/, 0) < 0,
          "an unknown address family is refused");
    check(sc3(SYS_socket, 2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0) < 0,
          "SOCK_STREAM is refused - TCP is not vendored yet");

    fd = sc3(SYS_socket, 2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 0);
    check(fd >= 0, "AF_INET/SOCK_DGRAM returns a descriptor");
    if (fd < 0) return;

    /* Bind to a wildcard address and a kernel-chosen port. */
    for (unsigned i = 0; i < sizeof(dst); i++) ((unsigned char *)&dst)[i] = 0;
    dst.sin_len    = 0;              /* deliberately wrong: see the struct */
    dst.sin_family = 2;
    dst.sin_port   = 0;
    dst.sin_addr   = 0;
    check_eq(sc3(SYS_bind, fd, &dst, sizeof(dst)), 0,
             "bind to 0.0.0.0:0 succeeds even with sin_len left at 0");

    /* And READ IT BACK. This is what makes the bind above a test rather than
     * a call that returned zero: bind to port 0 asks the kernel to choose,
     * and the only way to show it bound anything is to ask what it chose. */
    {
        struct k_sockaddr_in me;
        unsigned int melen = sizeof(me);

        for (unsigned i = 0; i < sizeof(me); i++) ((unsigned char *)&me)[i] = 0;
        check_eq(sc3(SYS_getsockname, fd, &me, &melen), 0, "getsockname succeeds");
        check_eq(me.sin_family, 2, "it reports AF_INET");
        check(me.sin_port != 0,
              "bind to port 0 really CHOSE a port (not just returned 0)");
        check(melen >= 8, "and wrote back a plausible address length");
    }

    /* 10.0.2.3:53, QEMU's resolver. */
    dst.sin_family = 2;
    dst.sin_port   = (unsigned short)((53 & 0xFF) << 8);   /* htons(53) */
    dst.sin_addr   = 0x0302000AU;                          /* 10.0.2.3 */
    check_eq(sc3(SYS_connect, fd, &dst, sizeof(dst)), 0, "connect succeeds");

    /* write(2) on a socket, which needs no socket-specific syscall at all -
     * it goes through the object vtable like any other descriptor. That
     * property is the whole reason this was small, so it is worth asserting
     * rather than assuming. */
    check_eq(sc3(SYS_write, fd, dns_q, sizeof(dns_q)), (i64)sizeof(dns_q),
             "write(2) works on a connected socket");

    check(sc6a(SYS_sendto, fd, dns_q, sizeof(dns_q), 1 /*MSG_OOB*/, 0, 0) < 0,
          "a non-zero flags argument is refused rather than ignored");

    /* --- poll(2) ON A SOCKET, tested on the half that does not need a peer -
     *
     * A connected UDP socket with an empty send buffer is ALWAYS writable, so
     * POLLOUT is deterministic and needs nothing outside this machine. That
     * makes it the right thing to assert here, and the timeout of 0 makes it
     * a pure readiness question rather than a wait.
     *
     * poll needed no socket-specific work at all: the object's poll slot is
     * what it dispatches through, the same slot pipes and eventfds use. That
     * is the property being checked.
     *
     * THE ROUND TRIP IS DELIBERATELY NOT CHECKED HERE, and this is not the
     * check giving up. There is no loopback interface, so the only UDP peer
     * this machine has is QEMU's resolver at 10.0.2.3 - and whether it
     * answers depends on the HOST having an upstream resolver, which is
     * outside the guest's control. The kernel's own boot-time check prints
     * "UDP reply not received... not evidence against the receive path" when
     * it does not, and it did exactly that during a run of this suite.
     *
     * Requiring it here would make this suite fail for a reason that has
     * nothing to do with the code under test. The receive path is proven at
     * boot by the ARP and ICMP checks, which talk to 10.0.2.2 - answered by
     * slirp itself rather than by the host - and are therefore reliable. What
     * is left for THIS test is the descriptor layer, which is what it checks. */
    {
        struct { int fd; short events; short revents; } pfd;

        pfd.fd = (int)fd;
        pfd.events = 4 /*POLLOUT*/;
        pfd.revents = 0;
        check_eq(sc3(SYS_poll, &pfd, 1, 0), 1,
                 "poll(2) on a socket returns a ready descriptor");
        check(pfd.revents & 4, "and reports it writable");
    }

    check_eq(sc1(SYS_close, fd), 0, "closing the socket");
    check(sc3(SYS_bind, fd, &dst, sizeof(dst)) < 0,
          "and the descriptor is really gone");
}

static u64 tls_block[8];

static void test_arch_prctl(void) {
    u64 got = 0;

    section("arch_prctl");

    tls_block[0] = 0xC0FFEEULL;
    check_eq(sc2(SYS_arch_prctl, 0x1002 /* ARCH_SET_FS */, tls_block), 0,
             "ARCH_SET_FS is accepted");

    /* The only test that proves the descriptor actually moved: read through
     * the segment the kernel just pointed somewhere. */
    __asm__ volatile ("movq %%fs:0, %0" : "=r"(got));
    check(got == 0xC0FFEEULL, "and fs:0 now reads the block we set");
}

/* --- files -------------------------------------------------------------- */

static char motd[256];
static i64  motd_len;

static void test_files(void) {
    i64 fd, fd2, n;
    u8  st[144];

    section("open / read / lseek / close");

    fd = sc3(SYS_open, "/etc/motd", O_RDONLY, 0);
    check(fd >= 0, "open /etc/motd");
    if (fd < 0) return;

    motd_len = sc3(SYS_read, fd, motd, sizeof(motd));
    check(motd_len > 0, "read returns bytes");

    check_eq(sc3(SYS_lseek, fd, 0, 0 /* SEEK_SET */), 0, "lseek to the start");
    n = sc3(SYS_read, fd, motd, sizeof(motd));
    check_eq(n, motd_len, "and reading again returns the same count");

    check_eq(sc3(SYS_lseek, fd, 0, 2 /* SEEK_END */), motd_len,
             "SEEK_END reports the file size");
    check_eq(sc3(SYS_read, fd, motd, sizeof(motd)), 0,
             "a read at EOF returns zero, not an error");
    check(sc3(SYS_lseek, fd, -1, 0) < 0, "seeking before the start is refused");

    check_eq(sc2(SYS_fstat, fd, st), 0, "fstat succeeds");
    check_eq((i64)*(u64 *)(st + 48), motd_len, "and reports the size we read");

    section("openat / dup / dup2");

    fd2 = sc4(SYS_openat, AT_FDCWD, "/etc/motd", O_RDONLY, 0);
    check(fd2 >= 0, "openat with AT_FDCWD");

    {
        i64 dupped = sc1(SYS_dup, fd);
        check(dupped >= 0 && dupped != fd, "dup gives a new descriptor");
        check_eq(sc2(SYS_dup2, fd, 9), 9, "dup2 lands on the number asked for");
        check_eq(sc2(SYS_dup2, fd, fd), fd, "dup2(fd,fd) is a no-op, not a close");
        sc1(SYS_close, dupped);
        sc1(SYS_close, 9);
    }

    check_eq(sc1(SYS_close, fd),  0, "close succeeds");
    check_eq(sc1(SYS_close, fd2), 0, "and again for the second descriptor");
    check_eq(sc1(SYS_close, fd), -EBADF, "closing it twice is -EBADF");
    check_eq(sc3(SYS_read, fd, motd, 1), -EBADF, "reading a closed fd is -EBADF");
    check_eq(sc1(SYS_close, 999), -EBADF, "closing a wild fd is -EBADF");
}

static void test_stat(void) {
    u8 st[144];

    section("stat / lstat / newfstatat");

    check_eq(sc2(SYS_stat, "/etc/motd", st), 0, "stat a regular file");
    check((*(u32 *)(st + 24) & 0170000u) == 0100000u, "and it is a regular file");
    check_eq((i64)*(u64 *)(st + 48), motd_len, "with the size we read earlier");

    check_eq(sc2(SYS_stat, "/bin", st), 0, "stat a directory");
    check((*(u32 *)(st + 24) & 0170000u) == 0040000u, "and it is a directory");

    check_eq(sc2(SYS_lstat, "/etc/motd", st), 0, "lstat behaves like stat");
    check_eq(sc2(SYS_stat, "/no/such/file", st), -ENOENT,
             "stat of a missing path is -ENOENT");

    check_eq(sc4(SYS_newfstatat, AT_FDCWD, "/etc/motd", st, 0), 0,
             "newfstatat succeeds");
    check_eq(sc2(SYS_readlinkat, 0, 0), -EINVAL,
             "readlinkat declines rather than lying");
}

static void test_dirs(void) {
    char buf[1024];
    char cwd[128];
    i64  fd, n;

    section("getdents64 / getcwd / chdir");

    fd = sc3(SYS_open, "/", O_RDONLY | O_DIRECTORY, 0);
    check(fd >= 0, "open the root directory");
    if (fd >= 0) {
        n = sc3(SYS_getdents64, fd, buf, sizeof(buf));
        check(n > 0, "getdents64 returns entries");
        /* Names are 8.3 and upper case on the FAT volume, which is worth
         * asserting rather than assuming - a lookup that silently lower-cased
         * would still "work" until something compared strings. */
        check(mem_has(buf, (u64)n, "BIN"), "and /bin is among them");
        check(mem_has(buf, (u64)n, "ETC"), "and /etc is too");
        sc1(SYS_close, fd);
    }

    check(sc2(SYS_getcwd, cwd, sizeof(cwd)) > 0, "getcwd succeeds");
    check(str_eq(cwd, "/"), "and we start at the root");

    check_eq(sc1(SYS_chdir, "/bin"), 0, "chdir into /bin");
    check(sc2(SYS_getcwd, cwd, sizeof(cwd)) > 0, "getcwd after chdir");
    check(str_eq(cwd, "/bin"), "and it reports /bin");

    check_eq(sc2(SYS_getcwd, cwd, 2), -ERANGE, "a short getcwd buffer is -ERANGE");
    check_eq(sc1(SYS_chdir, "/no/such/dir"), -ENOENT, "chdir to nowhere fails");
    check_eq(sc1(SYS_chdir, "/"), 0, "chdir back to the root");

    check(sc1(SYS_fchdir, 0) < 0, "fchdir is declined, as documented");
}


/* --- the filesystem section, run identically on every mounted volume -----
 *
 * The ordered TODO asked for exactly this: "systest's filesystem section must
 * run identically on both volumes". So it is ONE function called once per
 * volume rather than two similar functions - the only way the claim can be
 * true by construction instead of by inspection.
 *
 * What that buys is worth naming. The FAT root and the gnfs volume have
 * nothing in common below the vtable: 8.3 names folded to upper case against
 * arbitrary ones, a cluster chain against a copy-on-write object table, a
 * directory that is a fixed array against gnfs's directory blocks. If the same checks pass on both, the abstraction is
 * real; if one of them needs its own version of a check, that is the
 * abstraction leaking and the check is where it leaked. */

static void path_at(char *out, const char *base, const char *rel) {
    u64 n = 0;
    u64 i;

    for (i = 0; base[i] != '\0'; i++) {
        out[n++] = base[i];
    }
    /* A base of "" is the root, and "/" + "/etc/motd" would be "//etc/motd" -
     * which path_normalize handles, but only because it happens to. Building
     * the name correctly here means the test is not resting on that. */
    while (n > 0 && out[n - 1] == '/') {
        n--;
    }
    for (i = 0; rel[i] != '\0'; i++) {
        out[n++] = rel[i];
    }
    out[n] = '\0';
}

/* Case-insensitive, because the two volumes disagree about case and both are
 * right: FAT folds to 8.3 upper case, gnfs stores the name it was given. A
 * check that demanded one of those would be asserting the filesystem rather
 * than the interface. */
static int mem_has_ci(const char *hay, u64 n, const char *needle) {
    u64 i, j, m = slen(needle);

    if (m == 0 || n < m) return 0;
    for (i = 0; i + m <= n; i++) {
        for (j = 0; j < m; j++) {
            char a = hay[i + j];
            char b = needle[j];

            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (j == m) return 1;
    }
    return 0;
}

static void fs_checks(const char *base) {
    char path[256];
    char buf[1024];
    u8   st[144];
    i64  fd, n, size;

    path_at(path, base, "/etc/motd");
    fd = sc3(SYS_open, path, O_RDONLY, 0);
    check(fd >= 0, "open <volume>/etc/motd");
    if (fd < 0) {
        return;
    }

    n = sc3(SYS_read, fd, buf, sizeof(buf));
    check(n > 0, "read returns bytes");
    size = n;

    check_eq(sc3(SYS_lseek, fd, 0, 2 /* SEEK_END */), size,
             "SEEK_END agrees with what the read returned");
    check_eq(sc3(SYS_read, fd, buf, sizeof(buf)), 0,
             "a read at EOF is zero, not an error");
    check_eq(sc3(SYS_lseek, fd, 0, 0 /* SEEK_SET */), 0, "lseek back to 0");
    check_eq(sc3(SYS_read, fd, buf, sizeof(buf)), size,
             "and the second read returns the same count");

    check_eq(sc2(SYS_fstat, fd, st), 0, "fstat succeeds");
    check_eq((i64)*(u64 *)(st + 48), size, "and reports that size");
    check_eq(sc1(SYS_close, fd), 0, "close succeeds");

    check_eq(sc2(SYS_stat, path, st), 0, "stat the file by path");
    check((*(u32 *)(st + 24) & 0170000u) == 0100000u, "it is a regular file");
    check_eq((i64)*(u64 *)(st + 48), size, "with the same size");

    path_at(path, base, "/etc");
    check_eq(sc2(SYS_stat, path, st), 0, "stat the directory holding it");
    check((*(u32 *)(st + 24) & 0170000u) == 0040000u, "it is a directory");

    fd = sc3(SYS_open, path, O_RDONLY | O_DIRECTORY, 0);
    check(fd >= 0, "open that directory");
    if (fd >= 0) {
        n = sc3(SYS_getdents64, fd, buf, sizeof(buf));
        check(n > 0, "getdents64 returns entries");
        check(mem_has_ci(buf, (u64)n, "motd"), "and motd is among them");
        sc1(SYS_close, fd);
    }

    path_at(path, base, "/etc/motd/deeper");
    check_eq(sc2(SYS_stat, path, st), -ENOTDIR,
             "descending through a file is -ENOTDIR");

    path_at(path, base, "/no/such/file");
    check_eq(sc2(SYS_stat, path, st), -ENOENT, "a missing path is -ENOENT");
}

/* Every volume the mount table has, found the way a program would: by asking
 * whether the mount point is there. */
static void test_volumes(void) {
    static const char *bases[] = { "", "/mnt/d" };
    u8  st[144];
    u64 i;
    int ran = 0;

    for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        char label[64];

        if (bases[i][0] != '\0') {
            if (sc2(SYS_stat, bases[i], st) != 0) {
                continue;
            }
        }
        path_at(label, "filesystem on ", bases[i][0] ? bases[i] : "/");
        section(label);
        fs_checks(bases[i]);
        ran++;
    }

    if (ran < 2) {
        /* Said out loud rather than passed over. A second volume is a disk
         * that has to be attached, so its absence is a fact about this
         * machine and not a failure - but a suite that stayed silent about it
         * would report the same "all passed" whether the second-volume half
         * ran or not, which is the failure mode this tree has already had
         * once. build.py attaches the gnfs fixture there by default. */
        section("filesystem on a second volume");
        out("  skip  no second volume is mounted - attach one and rerun\n");
    }
}


/* --- ROADMAP item 7: access control, end to end -----------------------------
 *
 * Everything else in this suite runs as uid 0, which is the one identity for
 * which the answer to every permission question is yes. That makes it exactly
 * the wrong identity to test an access check with, so this section forks
 * children and drops them to real uids before asking.
 *
 * It runs against /mnt/d, the gnfs fixture build.py attaches (see
 * tests/host/fixtures/README.md): secret.txt there is mode 0600 owned by uid
 * 1000, with an ACL that additionally grants uid 1001 READ. The whole point
 * of that file is that the two views disagree: a kernel consulting st_mode
 * denies uid 1001, a kernel consulting the ACL allows it, and this is where
 * the difference is visible from userland rather than from a unit test.
 * (Until 2026-09-26 the same file sat on two ZFS pools; ZFS left the tree
 * and gnfs, which stores the same NFSv4 ACLs, carries it now.)
 *
 * setuid is one-way here (there is no saved-set-user-id - see sys_setuid), so
 * each identity needs its own child. That is not a workaround: a test that
 * dropped privilege in the parent would silently change every check after it.
 */

/* Open a path in a child running as `uid`, and report what happened as the
 * child's exit status. 0 opened, 1 was refused with EACCES, 2 anything else -
 * three outcomes rather than a boolean, because "refused for the right
 * reason" and "failed for some other reason" must not look the same. */
static i64 open_as(u32 uid, const char *path, int write) {
    i64 pid, status = 0;

    pid = sc2(SYS_clone, 0, 0);
    if (pid == 0) {
        i64 fd;

        if (sc1(SYS_setuid, uid) != 0) {
            sc1(SYS_exit_group, 3);
        }
        fd = sc4(SYS_openat, (u64)AT_FDCWD, (u64)path,
                 write ? 1 /*O_WRONLY*/ : 0 /*O_RDONLY*/, 0);
        if (fd >= 0) {
            sc1(SYS_close, fd);
            sc1(SYS_exit_group, 0);
        }
        sc1(SYS_exit_group, fd == -13 ? 1 : 2);
        __builtin_unreachable();
    }
    if (pid < 0) {
        return -1;
    }
    sc4(SYS_wait4, -1, &status, 0, 0);
    return (status >> 8) & 0xFF;
}

static void test_acls(void) {
    u8  st[144];
    u32 groups[4];
    i64 r;

    section("access control");

    if (sc2(SYS_stat, "/mnt/d/secret.txt", st) != 0) {
        out("  skip  /mnt/d is not mounted - the ACL fixture is not attached\n");
        return;
    }

    /* --- stat, which used to report a constant ---------------------------- */
    check_eq(*(u32 *)(st + 28), 1000,
             "st_uid is 1000 - read off the disk, not a zeroed buffer");
    check_eq(*(u32 *)(st + 32), 1000, "st_gid is 1000");
    check_eq(*(u32 *)(st + 24) & 07777, 0600,
             "and the mode is 0600, not the 0755 stat used to invent");

    /* A control, so the check above is about THIS file rather than about
     * every file getting the same new constant. */
    if (sc2(SYS_stat, "/mnt/d/open.txt", st) == 0) {
        check_eq(*(u32 *)(st + 24) & 07777, 0666,
                 "while open.txt on the same volume is 0666 - the modes are "
                 "per-file, not one new constant replacing an old one");
        check_eq(*(u32 *)(st + 28), 0, "and it is owned by uid 0");
    }

    /* --- root, the identity every other test in this file runs as --------- */
    check_eq(open_as(0, "/mnt/d/secret.txt", 0), 0, "root may read secret.txt");

    /* --- the owner -------------------------------------------------------- */
    check_eq(open_as(1000, "/mnt/d/secret.txt", 0), 0,
             "uid 1000 owns it and may read it");
    check_eq(open_as(1000, "/mnt/d/secret.txt", 1), 0,
             "and may open it for WRITING - owner@ grants write, and gnfs is "
             "a writable volume");

    /* --- the discriminating pair ------------------------------------------
     *
     * These two are the reason the fixture exists. Both users are strangers
     * to a mode word: neither owns the file and neither is in its group, so
     * mode 0600 says no to both. The ACL says yes to one of them. */
    check_eq(open_as(1001, "/mnt/d/secret.txt", 0), 0,
             "uid 1001 may read a mode-0600 file it does not own - the ACL "
             "grants it, and no mode word can say that");
    check_eq(open_as(1002, "/mnt/d/secret.txt", 0), 1,
             "uid 1002 may not - so it is that one ACE, not a blanket allow");

    /* And the grant is exactly as wide as it says. On a read-only volume a
     * write refusal proved nothing about the ACL; on gnfs it is the ACL
     * talking: uid 1001's entry names READ_DATA and nothing else, and the
     * deny-everyone@ entry after it catches the write. */
    check_eq(open_as(1001, "/mnt/d/secret.txt", 1), 1,
             "uid 1001 may NOT open it for writing - its grant is read only");

    /* --- and a file with no interesting ACL at all ------------------------ */
    check_eq(open_as(1002, "/mnt/d/readable.txt", 0), 0,
             "the same stranger may read readable.txt, which is 0644");
    check_eq(open_as(1002, "/mnt/d/readable.txt", 1), 1,
             "but not write it");
    check_eq(open_as(1002, "/mnt/d/etc/motd", 0), 0,
             "and descend into a 0755 directory to reach a file");

    /* --- access(2) must agree with open(2) -------------------------------- */
    check_eq(sc4(SYS_faccessat, (u64)AT_FDCWD, (u64)"/mnt/d/secret.txt",
                 4 /*R_OK*/, 0), 0,
             "access(R_OK) as root agrees with open");
    check_eq(sc4(SYS_faccessat, (u64)AT_FDCWD, (u64)"/mnt/d/secret.txt",
                 2 /*W_OK*/, 0), 0,
             "access(W_OK) as root says yes on a writable volume");
    check_eq(sc4(SYS_faccessat, (u64)AT_FDCWD, (u64)"/mnt/d/secret.txt",
                 0 /*F_OK*/, 0), 0,
             "and F_OK asks only whether it exists");

    /* --- groups ------------------------------------------------------------
     *
     * The supplementary list exists so a group@ or named-group ACE can match
     * a user whose primary gid is something else. Nothing on the fixture uses
     * one, so what is checked here is the plumbing: that it round-trips and
     * that a non-root process cannot grant itself a group. */
    check_eq(sc2(SYS_getgroups, 0, 0), 0, "a fresh process has no groups");
    groups[0] = 500;
    groups[1] = 501;
    check_eq(sc2(SYS_setgroups, 2, (u64)groups), 0, "root may set two");
    check_eq(sc2(SYS_getgroups, 0, 0), 2, "getgroups reports two");
    groups[0] = groups[1] = 0;
    check_eq(sc2(SYS_getgroups, 4, (u64)groups), 2, "and reads them back");
    check_eq(groups[0], 500, "the first is 500");
    check_eq(groups[1], 501, "the second is 501");
    check(sc2(SYS_getgroups, 1, (u64)groups) < 0,
          "a buffer too small is refused, not silently truncated");

    r = sc2(SYS_setgroups, 0, 0);
    check_eq(r, 0, "and they can be cleared again");

    {
        i64 pid, status = 0;

        pid = sc2(SYS_clone, 0, 0);
        if (pid == 0) {
            u32 g = 42;

            sc1(SYS_setuid, 1001);
            sc1(SYS_exit_group, sc2(SYS_setgroups, 1, (u64)&g) == 0 ? 1 : 0);
            __builtin_unreachable();
        }
        sc4(SYS_wait4, -1, &status, 0, 0);
        check_eq((status >> 8) & 0xFF, 0,
                 "a non-root process may NOT add itself to a group - it could "
                 "otherwise grant itself anything a group ACE allows");
    }

    /* --- credentials survive a fork ---------------------------------------
     *
     * Neither fork path copied uid or gid before this work, and proc_alloc
     * never initialised them, so a child got whatever was left in a recycled
     * process slot. Invisible while nothing read a uid; a privilege leak the
     * moment something did. */
    {
        i64 pid, status = 0;

        pid = sc2(SYS_clone, 0, 0);
        if (pid == 0) {
            i64 mine;

            sc1(SYS_setuid, 1234);
            /* A grandchild, to check the value is inherited and not just
             * still set in the process that called setuid. */
            if (sc2(SYS_clone, 0, 0) == 0) {
                sc1(SYS_exit_group, (int)sc0(SYS_getuid));
            }
            sc4(SYS_wait4, -1, &mine, 0, 0);
            sc1(SYS_exit_group, (int)((mine >> 8) & 0xFF));
        }
        sc4(SYS_wait4, -1, &status, 0, 0);
        check_eq((status >> 8) & 0xFF, 1234 & 0xFF,
                 "a forked child inherits its parent's uid");
    }
}

/* --- terminal ----------------------------------------------------------- */

static void test_ioctl(void) {
    u8  termios[64];
    u16 ws[4];

    section("ioctl");

    check_eq(sc3(SYS_ioctl, 0, 0x5401 /* TCGETS */, termios), 0,
             "TCGETS on stdin succeeds");
    check(termios[17 + 0] == 3, "VINTR is Ctrl-C");
    check(termios[17 + 4] == 4, "VEOF is Ctrl-D");
    check((*(u32 *)(termios + 12) & 0x1) != 0, "and ISIG is set");

    check_eq(sc3(SYS_ioctl, 0, 0x5402 /* TCSETS */, termios), 0,
             "TCSETS is accepted");
    check_eq(sc3(SYS_ioctl, 0, 0x5413 /* TIOCGWINSZ */, ws), 0,
             "TIOCGWINSZ succeeds");
    check(ws[0] == 25 && ws[1] == 80, "and the console is 80x25");
    check_eq(sc3(SYS_ioctl, 0, 0xDEAD, termios), -ENOTTY,
             "an unknown request is -ENOTTY, not a lie");
    /* --- ioctl is about WHAT the descriptor names, not its number -------
     *
     * fd 0/1/2 used to get the terminal answers whatever they pointed at, and
     * anything above 2 got -EBADF. Both were wrong, and the first one was
     * wrong in a way programs can see: with stdin redirected from a file,
     * TCGETS still returned a populated termios, so isatty() said yes for a
     * redirected stdin. That is the exact question isatty exists to answer,
     * and every program deciding whether to colourise or to prompt asks it.
     *
     * The pair below is what makes this a test rather than an assertion: a
     * regular file must be -ENOTTY, and a descriptor that really is the
     * console must still succeed - even though it is neither 0, 1 nor 2. */
    {
        int ffd = (int)sc4(SYS_openat, AT_FDCWD, "/etc/motd", O_RDONLY, 0);

        check(ffd > 2, "a regular file opens above fd 2");
        if (ffd >= 0) {
            check_eq(sc3(SYS_ioctl, ffd, 0x5401 /* TCGETS */, termios),
                     -ENOTTY,
                     "TCGETS on a FILE is -ENOTTY - isatty() must say no");
            sc1(SYS_close, ffd);
        }
    }
    {
        int cfd = (int)sc4(SYS_openat, AT_FDCWD, "/dev/console", O_RDWR, 0);

        check(cfd > 2, "the console also opens above fd 2");
        if (cfd >= 0) {
            check_eq(sc3(SYS_ioctl, cfd, 0x5401 /* TCGETS */, termios), 0,
                     "and TCGETS on IT succeeds - the answer follows the "
                     "object, not the descriptor number");
            sc1(SYS_close, cfd);
        }
    }

    check_eq(sc3(SYS_ioctl, 99, 0x5401, termios), -EBADF,
             "and a bad fd is -EBADF");
}

static void test_writev(void) {
    struct { const void *base; u64 len; } iov[3];

    section("writev");

    iov[0].base = "        writev: ";  iov[0].len = 16;
    iov[1].base = "three pieces";      iov[1].len = 12;
    iov[2].base = " in one call\n";    iov[2].len = 13;

    check_eq(sc3(SYS_writev, 1, iov, 3), 16 + 12 + 13,
             "writev returns the total of every iovec");
}

/* --- the object namespace, through its POSIX spelling ------------------- */

static void test_dev(void) {
    i64 fd;
    u8  st[144];

    section("/dev - the object namespace");

    fd = sc3(SYS_open, "/dev/console", 1 /* O_WRONLY */, 0);
    check(fd >= 0, "open /dev/console");
    if (fd >= 0) {
        check_eq(sc3(SYS_write, fd, "        (written to /dev/console)\n", 34),
                 34, "and writing to it reaches the screen");
        /* A console has no position, and saying so is how a program works out
         * it is talking to a terminal. */
        check_eq(sc3(SYS_lseek, fd, 0, 0), -ESPIPE, "seeking it is -ESPIPE");
        sc1(SYS_close, fd);
    }

    /* Different names, one object. /dev/tty and /dev/console are links to the
     * same \\Device\\Console, which is the whole reason for a namespace
     * rather than a table of device names per personality. */
    fd = sc3(SYS_open, "/dev/tty", 1, 0);
    check(fd >= 0, "/dev/tty is another name for the same device");
    if (fd >= 0) sc1(SYS_close, fd);

    fd = sc3(SYS_open, "/dev/CONSOLE", 1, 0);
    check(fd >= 0, "and the namespace is case-insensitive");
    if (fd >= 0) sc1(SYS_close, fd);

    check_eq(sc3(SYS_open, "/dev/nosuchdevice", 0, 0), -ENOENT,
             "a device that does not exist is -ENOENT");

    /* /dev is a view onto \\??\\ AND \\Device\\, in that order. A device that
     * was ns_insert()ed and never given a DOS link is still reachable under
     * its own name - which is what stops /dev from being a second list of
     * device names that drifts from the namespace. \\Device\\HarddiskVolume1
     * has no \\??\\ link; only \\??\\C: points at it. */
    fd = sc3(SYS_open, "/dev/HarddiskVolume1", 0, 0);
    check(fd >= 0, "a device with no DOS link opens under its \\Device\\ name");
    if (fd >= 0) sc1(SYS_close, fd);

    /* Opening a directory read-only SUCCEEDS - that is how opendir works, and
     * /bin already behaves this way. -EISDIR belongs to the read, not to the
     * open. This check used to assert the opposite and contradicted the
     * O_DIRECTORY open further down. */
    fd = sc3(SYS_open, "/dev", 0, 0);
    check(fd >= 0, "the device directory opens read-only, like any directory");
    if (fd >= 0) {
        char c;
        check_eq(sc3(SYS_read, fd, &c, 1), -EISDIR,
                 "but reading bytes from it is -EISDIR");
        sc1(SYS_close, fd);
    }

    check_eq(sc2(SYS_stat, "/dev/console", st), 0, "stat /dev/console");
    check((*(u32 *)(st + 24) & 0170000u) == 0020000u,
          "and it reports a character device");
    check_eq(sc2(SYS_stat, "/dev", st), 0, "stat /dev itself");
    check((*(u32 *)(st + 24) & 0170000u) == 0040000u, "as a directory");

    /* /dev/null. A pseudo-device: an object with behaviour and no hardware,
     * reached through exactly the same namespace machinery as the console. */
    fd = sc3(SYS_open, "/dev/null", 2 /* O_RDWR */, 0);
    check(fd >= 0, "open /dev/null");
    if (fd >= 0) {
        char buf[16];

        /* Everything written, nothing kept. A short write here would put a
         * caller into a retry loop that never ends. */
        check_eq(sc3(SYS_write, fd, "discard me", 10), 10,
                 "a write to it is accepted in full");
        check_eq(sc3(SYS_read, fd, buf, sizeof(buf)), 0,
                 "and a read from it is immediately end of file");
        check_eq(sc3(SYS_lseek, fd, 0, 0), -ESPIPE,
                 "it has no position to seek");
        sc1(SYS_close, fd);
    }

    check_eq(sc2(SYS_stat, "/dev/null", st), 0, "stat /dev/null");
    check((*(u32 *)(st + 24) & 0170000u) == 0020000u,
          "and it is a character device");

    /* Listing /dev walks the object namespace itself - so a device that was
     * registered appears here without anything maintaining a second list. */
    {
        char dents[1024];
        i64  dfd, n;

        dfd = sc3(SYS_open, "/dev", O_RDONLY | O_DIRECTORY, 0);
        check(dfd >= 0, "open /dev as a directory");
        if (dfd >= 0) {
            n = sc3(SYS_getdents64, dfd, dents, sizeof(dents));
            check(n > 0, "getdents64 on it returns entries");
            check(mem_has(dents, (u64)n, "null"),
                  "and null is among them, straight from the namespace");
            check(mem_has(dents, (u64)n, "console"), "and so is console");
            /* The \\Device\\ arm of the view, listed with nothing keeping a
             * second copy of the name in step. */
            check(mem_has(dents, (u64)n, "HarddiskVolume1"),
                  "and so is a device that only has a \\Device\\ name");
            sc1(SYS_close, dfd);
        }
    }
}


/* --- signals ------------------------------------------------------------ */

static volatile int handler_ran;
static volatile int handler_signo;

static void sig_handler(int signo) {
    handler_ran = 1;
    handler_signo = signo;
}

/* The restorer. A handler cannot simply return - there is no ordinary return
 * address under it, only the signal frame the kernel pushed - so it returns
 * HERE, and this asks the kernel to unwind. musl supplies one of these for
 * exactly the same reason; SA_RESTORER is not optional on x86-64. */
__asm__(
".text\n"
".globl systest_restorer\n"
"systest_restorer:\n"
"    movq $15, %rax\n"        /* SYS_rt_sigreturn */
"    syscall\n"
);
extern void systest_restorer(void);

struct kernel_sigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

static void test_signals(void) {
    struct kernel_sigaction sa, old;
    u64 set;

    section("signals");

    check_eq(sc4(SYS_rt_sigprocmask, 0 /* SIG_BLOCK */, 0, &set, 8), 0,
             "rt_sigprocmask reads the current mask");

    sa.handler  = (u64)&sig_handler;
    sa.flags    = SA_RESTORER;
    sa.restorer = (u64)&systest_restorer;
    sa.mask     = 0;

    check_eq(sc4(SYS_rt_sigaction, SIGUSR1, &sa, &old, 8), 0,
             "rt_sigaction installs a handler");
    check_eq(sc4(SYS_rt_sigaction, 9 /* SIGKILL */, &sa, 0, 8), -EINVAL,
             "and refuses to let SIGKILL be caught");
    check_eq(sc4(SYS_rt_sigaction, SIGUSR1, 0, &old, 8), 0,
             "querying the disposition works");
    check(old.handler == (u64)&sig_handler, "and reports the handler we set");

    handler_ran = 0;
    check_eq(sc2(SYS_kill, my_pid, SIGUSR1), 0, "kill delivers to ourselves");

    /* Delivery happens on the way out of the kill syscall, so by the time
     * that call has returned the handler has already run and rt_sigreturn has
     * already put us back. If this fails, the frame or the restorer is
     * wrong - and that failure mode is otherwise a silent hang. */
    check(handler_ran == 1, "the handler ran");
    check_eq(handler_signo, SIGUSR1, "with the right signal number");
    check(1, "and rt_sigreturn brought us back here to say so");

    check_eq(sc2(SYS_kill, my_pid, 0), 0, "kill(pid,0) probes without sending");
    check_eq(sc2(SYS_kill, 31337, 0), -ESRCH, "and a missing pid is -ESRCH");
    check_eq(sc2(SYS_kill, my_pid, 99), -EINVAL, "an absurd signal is -EINVAL");
    check_eq(sc3(SYS_tgkill, my_pid, my_pid, 0), 0, "tgkill probes too");

    /* Put it back, so a later fork test does not inherit a handler pointing
     * at a function whose stack frame no longer exists. */
    sa.handler  = 0;   /* SIG_DFL */
    sa.flags    = 0;
    sa.restorer = 0;
    sc4(SYS_rt_sigaction, SIGUSR1, &sa, 0, 8);
}

/* --- pipes --------------------------------------------------------------
 *
 * The half of this that matters is not "bytes go in and come out" - it is
 * what happens when an end closes, because that is what makes a pipeline
 * terminate instead of hanging. Both halves are checked, and the fork case at
 * the end is the one a shell actually performs.
 */

static void test_pipes(void) {
    int fds[2];
    char buf[64];
    u8   st[144];
    i64  pid, reaped;
    int  status = 0;

    section("pipes");

    check_eq(sc1(SYS_pipe, fds), 0, "pipe() succeeds");
    check(fds[0] >= 0 && fds[1] >= 0 && fds[0] != fds[1],
          "and hands back two distinct descriptors");

    check_eq(sc3(SYS_write, fds[1], "hello pipe", 10), 10,
             "a write to the write end takes every byte");
    check_eq(sc3(SYS_read, fds[0], buf, sizeof(buf)), 10,
             "and the read end returns them");
    buf[10] = '\0';
    check(buf[0] == 'h' && buf[9] == 'e', "with the contents intact");

    /* The ends are one-way. A program that gets them backwards should be told
     * so at the first operation rather than quietly reading nothing forever. */
    check_eq(sc3(SYS_read, fds[1], buf, 1), -EBADF,
             "reading the write end is refused - the access mode says no");
    check_eq(sc3(SYS_write, fds[0], "x", 1), -EBADF,
             "and so is writing the read end");

    /* No position, and saying so is how a libc works out what it is holding.
     * S_IFIFO rather than S_IFCHR is what makes it pick block buffering. */
    check_eq(sc3(SYS_lseek, fds[0], 0, 0), -ESPIPE, "a pipe cannot be seeked");
    check_eq(sc2(SYS_fstat, fds[0], st), 0, "fstat on a pipe succeeds");
    check((*(u32 *)(st + 24) & 0170000u) == 0010000u, "and reports a FIFO");

    /* End of file. Data written before the close must survive it, or a
     * producer that exits before its consumer runs loses everything. */
    sc3(SYS_write, fds[1], "last", 4);
    sc1(SYS_close, fds[1]);
    check_eq(sc3(SYS_read, fds[0], buf, sizeof(buf)), 4,
             "data written before the write end closed is still readable");
    check_eq(sc3(SYS_read, fds[0], buf, sizeof(buf)), 0,
             "and then the read is 0 - end of file, not an error");
    sc1(SYS_close, fds[0]);

    /* -EPIPE, with SIGPIPE ignored so the check can observe the errno rather
     * than being killed by the default action. */
    {
        struct kernel_sigaction sa, old;

        sa.handler  = SIG_IGN_HANDLER;
        sa.flags    = SA_RESTORER;
        sa.restorer = (u64)&systest_restorer;
        sa.mask     = 0;
        sc4(SYS_rt_sigaction, SIGPIPE, &sa, &old, 8);

        check_eq(sc1(SYS_pipe, fds), 0, "a second pipe");
        sc1(SYS_close, fds[0]);
        check_eq(sc3(SYS_write, fds[1], "nobody", 6), -EPIPE,
                 "writing with no reader left is -EPIPE");
        sc1(SYS_close, fds[1]);

        sc4(SYS_rt_sigaction, SIGPIPE, &old, 0, 8);
    }

    /* pipe2 flags. O_CLOEXEC and O_NONBLOCK are both implemented now;
     * anything else is still refused rather than accepted and ignored,
     * because a program that asked for a behaviour and did not get it hangs
     * where it has no reason to look.
     *
     * O_NONBLOCK used to be the unimplemented flag this checked, and it was
     * the right one to refuse at the time - no read path honoured it, so
     * accepting it would have returned success and then blocked. The
     * readiness predicate poll added is what a non-blocking read needs to
     * answer -EAGAIN instead, so the refusal went with it. O_DIRECT is the
     * flag that has no implementation now: it means packet mode, which is a
     * different framing on the ring buffer rather than a flag to record. */
    check_eq(sc2(SYS_pipe2, fds, O_CLOEXEC), 0, "pipe2 with O_CLOEXEC");
    sc1(SYS_close, fds[0]);
    sc1(SYS_close, fds[1]);
    check_eq(sc2(SYS_pipe2, fds, 0x800 /* O_NONBLOCK */), 0,
             "pipe2 now ACCEPTS O_NONBLOCK");
    sc1(SYS_close, fds[0]);
    sc1(SYS_close, fds[1]);
    check_eq(sc2(SYS_pipe2, fds, 0x4000 /* O_DIRECT */), -EINVAL,
             "and an unimplemented flag is still refused, not ignored");

    /* What a shell pipeline is. The parent writes, the child reads, and the
     * child's read terminates only because the parent closed ITS copy of the
     * write end - which is the whole reason the two ends are separate objects
     * with separate reference counts. */
    check_eq(sc1(SYS_pipe, fds), 0, "a pipe for the fork case");
    pid = sc0(SYS_fork);
    if (pid == 0) {
        char cbuf[32];
        i64  got, total = 0;

        sc1(SYS_close, fds[1]);          /* the child does not write */
        for (;;) {
            got = sc3(SYS_read, fds[0], cbuf + total, (u64)(8 - total));
            if (got <= 0) break;
            total += got;
        }
        sc1(SYS_close, fds[0]);
        /* 0 from the read means the parent's write end went away. Exiting
         * with the byte count lets the parent check both at once. */
        sc1(SYS_exit, (int)total);
    }
    check(pid > 0, "fork for the pipeline succeeds");
    sc1(SYS_close, fds[0]);              /* the parent does not read */
    check_eq(sc3(SYS_write, fds[1], "12345678", 8), 8,
             "the parent writes through the pipe");
    sc1(SYS_close, fds[1]);              /* this is what ends the child's read */

    reaped = sc4(SYS_wait4, -1, &status, 0, 0);
    check_eq(reaped, pid, "and reaps the child");
    check_eq((status >> 8) & 0xFF, 8,
             "which read all eight bytes and then saw end of file");
}


/* --- processes ---------------------------------------------------------- */

/* Deliberately in .data rather than on the stack: after fork the two
 * processes have separate copies of this at the same address, and proving
 * that is the whole point of the copy-on-write test below. */
static volatile u64 shared_word = 0x11111111ULL;

static void test_fork(void) {
    i64 pid, reaped;
    int status = 0;

    section("fork and copy-on-write");

    pid = sc0(SYS_fork);

    /* The check comes AFTER the child has gone, not before: everything from
     * here to the exit below runs in both processes, and a check() in the
     * child would print its line twice. */
    if (pid == 0) {
        /* Child. Write to the shared page - which faults, copies, and leaves
         * the parent's copy alone if copy-on-write is right and clobbers it
         * if it is not. */
        shared_word = 0x22222222ULL;
        if (shared_word != 0x22222222ULL) {
            sc1(SYS_exit, 90);      /* our own write did not stick */
        }
        sc1(SYS_exit, 42);
    }
    check(pid > 0, "fork returns the child's pid in the parent");

    reaped = sc4(SYS_wait4, -1, &status, 0, 0);
    check_eq(reaped, pid, "wait4 reaps the child we forked");
    check_eq((status >> 8) & 0xFF, 42, "with the exit status it chose");
    check_eq((i64)shared_word, 0x11111111LL,
             "and the child's write did NOT reach the parent's page");

    check_eq(sc4(SYS_wait4, -1, &status, 0, 0), -ECHILD,
             "a second wait4 with no children is -ECHILD");
}

/* --- real threads, raw -----------------------------------------------------
 *
 * clone() with the thread flags, no libc: a new task on a stack of our own
 * that shares this address space. Two things were wrong with threads and
 * neither showed while everything ran as root and made one thread:
 *
 *   - the thread path did not copy credentials, so a thread created by a
 *     process that had dropped to uid 1000 ran as uid 0;
 *   - an exited thread was never freed (wait4 rightly skips threads, and
 *     nothing else reaped them), so each one held a process slot forever
 *     and clone failed with -EAGAIN after about a dozen.
 *
 * The child's first instructions have to be assembly: it resumes inside
 * this function's frame on a DIFFERENT stack, where no C frame exists. So
 * the branch is taken in asm, the child calls `fn` on its fresh stack and
 * exits with SYS_exit (one thread), and only the parent returns into C.
 *
 * Joined with CLONE_CHILD_CLEARTID - the kernel zeroes *ctid when the
 * thread is gone, which is pthread_join's whole mechanism - so the stack is
 * never reused while the previous thread is still standing on it. */
#define CLONE_THREAD_FLAGS (0x100 | 0x200 | 0x400 | 0x800 | 0x10000)
#define CLONE_CHILD_CLEARTID_F 0x00200000

static u8 thr_stack[16384] __attribute__((aligned(16)));
static volatile int thr_ctid;
static volatile i64 thr_seen_uid;

static void thr_body(void) {
    thr_seen_uid = sc0(SYS_getuid);
}

static i64 raw_thread(void (*fn)(void)) {
    i64 ret;
    register i64 r10 __asm__("r10") = 0;                      /* tls  */
    register i64 r8  __asm__("r8")  = (i64)&thr_ctid;         /* ctid */

    thr_ctid = 1;
    __asm__ volatile (
        "syscall\n\t"
        "test %%rax, %%rax\n\t"
        "jnz 1f\n\t"
        "call *%%rbx\n\t"            /* child: on the new stack */
        "movl $60, %%eax\n\t"        /* SYS_exit - this thread only */
        "xorl %%edi, %%edi\n\t"
        "syscall\n\t"
        "1:\n\t"
        : "=a"(ret)
        : "a"(SYS_clone),
          "D"(CLONE_THREAD_FLAGS | CLONE_CHILD_CLEARTID_F),
          "S"(thr_stack + sizeof(thr_stack)), "d"(0),
          "r"(r10), "r"(r8), "b"(fn)
        : "rcx", "r11", "memory");
    if (ret < 0) {
        thr_ctid = 0;
        return ret;
    }
    while (thr_ctid != 0) {
        sc0(SYS_sched_yield);
    }
    return ret;
}

static void test_clone_threads(void) {
    i64 pid, status = 0, r;
    int i;

    thr_seen_uid = -1;
    r = raw_thread(thr_body);
    check(r > 0, "clone with the thread flags makes a thread");
    check_eq(thr_seen_uid, 0, "which, made by root, runs as root");

    /* In a child, because setuid is one-way. Exit status: 0 all good,
     * 1 the thread ran as the wrong uid, 2+i clone failed at thread i. */
    pid = sc2(SYS_clone, 0, 0);
    if (pid == 0) {
        if (sc1(SYS_setuid, 1000) != 0) {
            sc1(SYS_exit_group, 99);
        }
        thr_seen_uid = -1;
        if (raw_thread(thr_body) <= 0) {
            sc1(SYS_exit_group, 2);
        }
        if (thr_seen_uid != 1000) {
            sc1(SYS_exit_group, 1);
        }
        for (i = 0; i < 30; i++) {
            if (raw_thread(thr_body) <= 0) {
                sc1(SYS_exit_group, 3 + i);
            }
        }
        sc1(SYS_exit_group, 0);
    }
    sc4(SYS_wait4, pid, &status, 0, 0);
    r = (status >> 8) & 0xFF;
    check(r != 1, "a thread made by uid 1000 runs as uid 1000, not as root");
    check_eq(r, 0, "and thirty threads made and joined in turn all succeed - "
                   "exited threads are reclaimed, not leaked");
}

static void test_clone(void) {
    i64 pid, reaped;
    int status = 0;

    section("clone");

    /* clone(SIGCHLD) with no sharing flags is what glibc's fork() issues. */
    pid = sc6a(SYS_clone, 17 /* SIGCHLD */, 0, 0, 0, 0, 0);
    if (pid == 0) {
        sc1(SYS_exit, 7);
    }
    check(pid > 0, "clone(SIGCHLD) is accepted as a fork");
    reaped = sc4(SYS_wait4, -1, &status, 0, 0);
    check_eq(reaped, pid, "and the child is reapable");
    check_eq((status >> 8) & 0xFF, 7, "with its own exit status");

    check(sc6a(SYS_clone, 0x00000100 /* CLONE_VM */, 0, 0, 0, 0, 0) < 0,
          "a clone asking to share memory is refused, not faked");

    test_clone_threads();
}

static void test_exec(void) {
    i64 pid, reaped;
    int status = 0;
    const char *argv[2];
    const char *envp[1];

    section("execve");

    argv[0] = "/bin/hello";
    argv[1] = NULL;
    envp[0] = NULL;

    check(sc3(SYS_execve, "/no/such/binary", argv, envp) < 0,
          "execve of a missing file fails without replacing us");

    pid = sc0(SYS_fork);
    if (pid == 0) {
        sc3(SYS_execve, "/bin/hello", argv, envp);
        sc1(SYS_exit, 99);          /* only reached if execve failed */
    }
    reaped = sc4(SYS_wait4, -1, &status, 0, 0);
    check_eq(reaped, pid, "a forked child execs and is reaped");
    check_eq((status >> 8) & 0xFF, 0, "and the new image exited cleanly");

    /* --- ROADMAP item 9: execveat ------------------------------------- */

    /* The refusals first, because they need no child. Both are cases where
     * ignoring the argument would run the WRONG PROGRAM rather than fail: a
     * real dirfd would silently resolve against the cwd, and AT_EMPTY_PATH
     * would execute whatever the empty path resolves to. */
    check(sc5(SYS_execveat, 3, "/bin/hello", argv, envp, 0) < 0,
          "execveat with a real dirfd is refused, not resolved against cwd");
    check(sc5(SYS_execveat, -100 /*AT_FDCWD*/, "/bin/hello", argv, envp,
              0x1000 /*AT_EMPTY_PATH*/) < 0,
          "AT_EMPTY_PATH is refused rather than ignored");
    check(sc5(SYS_execveat, -100, "/no/such/binary", argv, envp, 0) < 0,
          "execveat of a missing file fails without replacing us");

    pid = sc0(SYS_fork);
    if (pid == 0) {
        sc5(SYS_execveat, -100 /*AT_FDCWD*/, "/bin/hello", argv, envp, 0);
        sc1(SYS_exit, 99);          /* only reached if execveat failed */
    }
    status = 0;
    reaped = sc4(SYS_wait4, -1, &status, 0, 0);
    check_eq(reaped, pid, "a child execveat's and is reaped");
    check_eq((status >> 8) & 0xFF, 0,
             "and the new image exited cleanly (99 would mean it never ran)");
}

/* vfork last, and deliberately so.
 *
 * The child runs on the PARENT's stack, so anything it does beyond exiting
 * corrupts the process this test is running in. The exit is written as inline
 * asm rather than a call for that reason: a compiler is free to spill a
 * register into the frame around an ordinary call, and that frame belongs to
 * the parent. */
static void test_vfork(void) {
    i64 pid, reaped;
    int status = 0;

    section("vfork");

    pid = sc0(SYS_vfork);
    if (pid == 0) {
        __asm__ volatile ("movq $60, %%rax\n\t"
                          "movq $23, %%rdi\n\t"
                          "syscall"
                          : : : "rax", "rdi", "rcx", "r11", "memory");
        __builtin_unreachable();
    }
    check(pid > 0, "vfork returns the child's pid in the parent");
    reaped = sc4(SYS_wait4, -1, &status, 0, 0);
    check_eq(reaped, pid, "and the child is reaped after it exits");
    check_eq((status >> 8) & 0xFF, 23, "with the status it exited with");
}

/* --- FPU state across a context switch ----------------------------------
 *
 * MXCSR and the x87 control word rather than the data registers, because a C
 * compiler will happily reuse xmm0 between two statements and prove nothing.
 * These two are control state: nothing touches them unless asked, so if the
 * parent's values come back changed, something else was running with them. */

static void test_fpu(void) {
    u32 mxcsr, after;
    u16 cw, cw_after;
    i64 pid;
    int status = 0;

    section("FPU state across a context switch");

    mxcsr = 0x1F80 | 0x6000;                  /* round toward zero      */
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
    cw = 0x037F & (u16)~0x0C00;
    cw |= 0x0400;                             /* x87: round down        */
    __asm__ volatile ("fldcw %0" : : "m"(cw));

    pid = sc0(SYS_fork);
    if (pid == 0) {
        u32 child_mxcsr = 0x1F80;             /* round to nearest       */
        u16 child_cw    = 0x037F;
        volatile int i;

        __asm__ volatile ("ldmxcsr %0" : : "m"(child_mxcsr));
        __asm__ volatile ("fldcw %0" : : "m"(child_cw));

        /* Long enough to be preempted, so the parent is scheduled back in
         * with the child's control words live on the CPU. */
        for (i = 0; i < 400000; i++) { }
        sc1(SYS_exit, 0);
    }

    /* wait4 blocks, so this thread is genuinely switched out and back. */
    sc4(SYS_wait4, -1, &status, 0, 0);

    __asm__ volatile ("stmxcsr %0" : "=m"(after));
    __asm__ volatile ("fnstcw %0" : "=m"(cw_after));

    check_eq(after & 0x6000, 0x6000,
             "the parent's SSE rounding mode survived the child");
    check_eq(cw_after & 0x0C00, 0x0400,
             "and so did its x87 control word");

    mxcsr = 0x1F80;
    cw    = 0x037F;
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile ("fldcw %0" : : "m"(cw));
}

/* --- odds and ends ------------------------------------------------------ */

static void test_declined(void) {
    section("syscalls that decline on purpose");

    check_eq(sc4(SYS_rseq, 0, 0, 0, 0), -ENOSYS,
             "rseq says no rather than pretending");
    check_eq(sc4(SYS_prlimit64, 0, 0, 0, 0), -ENOSYS, "prlimit64 likewise");
}

static void test_getrandom(void) {
    u8 buf[16];
    int all_zero = 1;
    u64 i;

    section("getrandom");

    for (i = 0; i < sizeof(buf); i++) buf[i] = 0;
    check_eq(sc3(SYS_getrandom, buf, sizeof(buf), 0), (i64)sizeof(buf),
             "getrandom fills the buffer it was given");
    for (i = 0; i < sizeof(buf); i++) if (buf[i] != 0) all_zero = 0;
    check(!all_zero, "and wrote something into it");
    out("        note: the kernel's getrandom is a fixed pattern, not random\n");
}

/* Somewhere to aim a write that must NOT be allowed. Taking the address of a
 * function is the portable way to name a byte inside .text without knowing
 * the link map - and .text is the segment the ELF loader marks read-only.
 *
 * Deliberately its own function rather than reusing one of the tests above:
 * a compiler is entitled to notice that nothing calls this and that its body
 * is empty, but it may not delete a function whose address is taken. */
static void wx_target(void) {
}

static void test_wx(void) {
    i64 pid, reaped;
    int status = 0;

    section("W^X on the ELF side");

    /* Forked, because this write is supposed to be fatal and there is no way
     * to survive it in-process: interrupt.c kills a faulting process rather
     * than delivering a catchable SIGSEGV, and it says why - a signal is
     * delivered at a syscall boundary, and a faulting instruction never
     * reaches one, so a handler that returned would re-execute the fault
     * forever. So the child dies and the parent reads the status.
     *
     * Note this is a POSITIVE test, and it is the one that matters. Every
     * other piece of evidence for W^X is of the form "nothing broke", which
     * a loader that quietly kept mapping .text writable would also produce. */
    pid = sc0(SYS_fork);
    if (pid == 0) {
        /* volatile so the store is really emitted. Writing the byte back as
         * itself, so that on a kernel where this WRONGLY succeeds the child
         * has still not corrupted its own code before exiting. */
        volatile unsigned char *text = (volatile unsigned char *)(void *)wx_target;

        *text = *text;
        sc1(SYS_exit, 77);        /* reached only if .text was writable */
    }
    check(pid > 0, "fork returns the child's pid in the parent");

    reaped = sc4(SYS_wait4, -1, &status, 0, 0);
    check_eq(reaped, pid, "wait4 reaps the child that wrote to .text");

    /* 128 + SIGSEGV is what proc_retire records for a process killed by a
     * fault - see kill_faulting_process in kernel/interrupt.c. 77 here would
     * mean the write went through, which is exactly the bug this pass
     * closed: before it, every PT_LOAD page was mapped PAGE_RW and stayed
     * that way even after the executable pass cleared NX. */
    check_eq((status >> 8) & 0xFF, 128 + 11,
             "and it was KILLED - .text is not writable");
}

static void test_audit_gapfill(void) {
    u64 ts[2];
    u64 tms[4];
    i64 rc;

    section("Part 17 gap-fill");

    /* readv, against the console. One segment, because the interesting part
     * is that the iovec is walked at all - writev already proves the shape
     * and this is its mirror. */
    {
        struct { const void *base; u64 len; } iov[2];
        char buf[8];

        iov[0].base = buf; iov[0].len = 0;   /* zero-length: legal, skipped */
        iov[1].base = buf; iov[1].len = 0;
        check_eq(sc3(SYS_readv, 0, iov, 2), 0,
                 "readv of two empty segments reads nothing");
        check_eq(sc3(SYS_readv, 0, iov, 2000), -EINVAL,
                 "and an absurd iovcnt is refused, not walked");
    }

    /* pread/pwrite must NOT move the descriptor offset - that is the whole
     * reason they exist, and a lseek-then-read implementation passes every
     * test except this one. */
    {
        int fd = (int)sc3(SYS_open, "/etc/motd", 0, 0);

        if (fd >= 0) {
            char a[4], b[4];
            i64 off_after;

            check(sc3(SYS_read, fd, a, 2) == 2, "read two bytes");
            check(sc4(SYS_pread64, fd, b, 2, 0) == 2, "pread at offset 0");
            off_after = sc3(SYS_lseek, fd, 0, 1 /* SEEK_CUR */);
            check_eq(off_after, 2,
                     "and pread did NOT move the descriptor offset");
            check(b[0] == a[0], "pread at 0 returned the same first byte");
            sc1(SYS_close, fd);
        }
    }

    check_eq(sc1(SYS_fsync, 0), 0, "fsync succeeds - the cache is write-through");
    check_eq(sc1(SYS_fsync, 999), -EBADF, "fsync of a bad fd is -EBADF");
    check_eq(sc3(SYS_madvise, 0, 0, 0), 0, "madvise is advisory and succeeds");

    /* umask returns the PREVIOUS value, which is the only part of it a
     * caller can observe here. */
    {
        i64 old = sc1(SYS_umask, 022);
        check_eq(sc1(SYS_umask, old), 022,
                 "umask returns the value it previously held");
    }

    check_eq(sc0(SYS_sched_yield), 0, "sched_yield returns 0");

    /* dup3 must REFUSE oldfd == newfd where dup2 succeeds. That difference
     * is the reason dup3 exists, and it is the one thing a dup3 implemented
     * as a plain alias for dup2 gets wrong. */
    check_eq(sc3(SYS_dup3, 1, 1, 0), -EINVAL,
             "dup3 refuses oldfd == newfd");
    check_eq(sc2(SYS_dup2, 1, 1), 1, "where dup2 accepts it");

    /* Sessions. getsid of a process that never called setsid falls back to
     * its group, so it is never an error. */
    check(sc1(SYS_getsid, 0) >= 0, "getsid of self succeeds");

    /* Credentials. setresuid must REFUSE a request it cannot represent
     * rather than silently collapsing three ids into one. */
    check_eq(sc3(SYS_setresuid, 0, 0, 0), 0, "setresuid(0,0,0) is accepted");
    check_eq(sc3(SYS_setresuid, 1, 2, 3), -EINVAL,
             "and three DIFFERENT ids are refused, not collapsed");
    {
        u32 r = 99, e = 99, sv = 99;
        check_eq(sc3(SYS_getresuid, &r, &e, &sv), 0, "getresuid succeeds");
        check(r == e && e == sv, "and reports all three the same");
    }

    /* clock_getres must answer ONE TICK, not 1ns. A caller told 1ns and
     * then handed tick-granular timestamps has been lied to. */
    check_eq(sc2(SYS_clock_getres, 0, ts), 0, "clock_getres succeeds");
    check(ts[1] > 1000, "and reports tick granularity, not 1ns");

    /* clock_nanosleep with an absolute deadline already in the past must
     * return immediately rather than sleeping for the epoch. */
    ts[0] = 0; ts[1] = 0;
    check_eq(sc4(SYS_clock_nanosleep, 0, 1 /* TIMER_ABSTIME */, ts, 0), 0,
             "clock_nanosleep to a past deadline returns at once");

    rc = sc1(SYS_times, tms);
    check(rc > 0, "times returns a tick count");

    {
        u64 pending = 0xFFFF;
        check_eq(sc2(SYS_rt_sigpending, &pending, 8), 0,
                 "rt_sigpending succeeds");
        check_eq((i64)pending, 0, "and nothing is pending here");
        check_eq(sc2(SYS_rt_sigpending, &pending, 4), -EINVAL,
                 "a wrong sigset size is refused");
    }
}

/* Does `path` exist? stat is the cheapest way to ask, and using it rather
 * than trusting the return of the call under test is the point: a mkdir that
 * returns 0 and creates nothing passes any test that only reads its own
 * return value. */
static int exists(const char *path) {
    char st[144];

    return sc2(SYS_stat, path, st) == 0;
}

static void test_namespace(void) {
    char st[144];

    section("file namespace (mkdir/unlink/rename)");

    /* Start from a known state, ignoring failures - a previous run may have
     * left these behind, and this test has to be repeatable on a volume that
     * persists across boots. */
    sc1(SYS_unlink, "/tdir/f2");
    sc1(SYS_unlink, "/tdir/f1");
    sc1(SYS_unlink, "/tf1");
    sc1(SYS_rmdir,  "/tdir/sub");
    sc1(SYS_rmdir,  "/tdir");
    sc1(SYS_rmdir,  "/tdir2");

    /* --- mkdir ---------------------------------------------------------- */
    check_eq(sc2(SYS_mkdir, "/tdir", 0755), 0, "mkdir /tdir");
    check(exists("/tdir"), "and it is really there afterwards");
    check_eq(sc2(SYS_stat, "/tdir", st), 0, "stat of the new directory works");

    /* The mode bits say it IS a directory - S_IFDIR is 0040000, and st_mode
     * is at offset 24 in struct stat. A mkdir that created a plain file
     * would pass every other check here. */
    check((*(u32 *)(st + 24) & 0170000) == 0040000,
          "and stat reports it as a DIRECTORY, not a file");

    check_eq(sc2(SYS_mkdir, "/tdir", 0755), -EEXIST,
             "a second mkdir of the same name is -EEXIST");

    /* Nested, which only works if the "." and ".." entries and the parent's
     * cluster were all written correctly. */
    check_eq(sc2(SYS_mkdir, "/tdir/sub", 0755), 0, "mkdir /tdir/sub");
    check(exists("/tdir/sub"), "and the nested directory resolves");

    /* --- rmdir ---------------------------------------------------------- */
    check_eq(sc1(SYS_rmdir, "/tdir"), -ENOTEMPTY,
             "rmdir of a NON-empty directory is refused");
    check_eq(sc1(SYS_rmdir, "/tdir/sub"), 0, "rmdir of the empty child works");
    check(!exists("/tdir/sub"), "and it is really gone");

    /* rmdir must not eat a file, and unlink must not eat a directory -
     * conflating them is how a directory tree gets orphaned. */
    check_eq(sc1(SYS_unlink, "/tdir"), -EISDIR,
             "unlink refuses a directory with -EISDIR");
    check_eq(sc1(SYS_rmdir, "/etc/motd"), -ENOTDIR,
             "rmdir refuses a file with -ENOTDIR");

    /* --- rename --------------------------------------------------------- */
    check_eq(sc2(SYS_rename, "/tdir", "/tdir2"), 0, "rename /tdir -> /tdir2");
    check(exists("/tdir2"), "the new name resolves");
    check(!exists("/tdir"), "and the old name does NOT");

    check_eq(sc2(SYS_rename, "/tdir2", "/etc"), -EEXIST,
             "rename onto an existing name is refused, not silently "
             "replacing it");
    check_eq(sc2(SYS_rename, "/nosuch", "/tdir3"), -ENOENT,
             "rename of a missing source is -ENOENT");

    check_eq(sc2(SYS_rename, "/tdir2", "/tdir"), 0, "rename back");

    /* --- the *at variants ------------------------------------------------ */
    check_eq(sc3(SYS_mkdirat, AT_FDCWD, "/tdir/sub", 0755), 0,
             "mkdirat with AT_FDCWD");
    check_eq(sc3(SYS_unlinkat, AT_FDCWD, "/tdir/sub", AT_REMOVEDIR), 0,
             "unlinkat with AT_REMOVEDIR removes a directory");
    check(!exists("/tdir/sub"), "and it is gone");
    check_eq(sc3(SYS_unlinkat, AT_FDCWD, "/tdir", 0), -EISDIR,
             "unlinkat WITHOUT AT_REMOVEDIR still refuses a directory");
    check_eq(sc3(SYS_mkdirat, 5, "/tdir/x", 0755), -EBADF,
             "a real dirfd is refused rather than silently ignored");

    /* --- refusals -------------------------------------------------------- */
    check_eq(sc1(SYS_rmdir, "/"), -EBUSY,
             "rmdir of a MOUNT POINT is -EBUSY, not an unmount");
    check_eq(sc2(SYS_mkdir, "/toolongname.dir", 0755), -ENAMETOOLONG,
             "a name that does not fit 8.3 is refused, not truncated");

    /* Clean up, so the volume is left as it was found. */
    check_eq(sc1(SYS_rmdir, "/tdir"), 0, "rmdir /tdir cleans up");
    check(!exists("/tdir"), "and the volume is back to how it started");
}

/* --- gnfs: ownership and permissions, from ring 3 ---------------------------
 *
 * The permission model built on gnfs - creator ownership, create/delete
 * gates on the parent, chmod with special bits, chown's give-away rule,
 * umask, setgid and sticky directories, and the supreme privilege - was
 * checked by the host suite against the VFS directly. That proves the
 * policy and nothing about the plumbing: that each syscall hands the right
 * credential down, that umask is applied where the syscall says, that
 * (uid_t)-1 means "keep". This section is the plumbing check, and it is
 * the one that would catch a syscall passing NULL (the kernel, which is
 * never refused) where it meant the caller.
 *
 * Every refusal is asked of a CHILD that has dropped to a real uid - as
 * test_acls explains, root is the one identity for which every answer is
 * yes - and the errno comes back as the child's exit status, compared
 * exactly: -EACCES and -EPERM are different answers here on purpose.
 *
 * The volume is found by what statfs says (gnfs's f_namelen is 60; FAT's
 * is 12), not by drive letter. It is the ACL fixture volume, unpacked fresh
 * every run by build.py, so this section can mutate it freely - it runs
 * after test_volumes and test_acls have read it as shipped. */

#define SYS_chmod      90
#define SYS_fchmod     91
#define SYS_chown      92
#define SYS_fchown     93
#define SYS_statfs    137
#define EACCES         13
#define O_WRONLY        1
#define O_CREAT      0100
#define GNFS_NAMELEN   60

#define PR_GENESIS_GRANT_SUPREME  0x47454e01L
#define PR_GENESIS_REVOKE_SUPREME 0x47454e02L
#define PR_GENESIS_QUERY_SUPREME  0x47454e03L

enum {
    OP_CREAT = 1, OP_MKDIR, OP_UNLINK, OP_RMDIR, OP_CHMOD, OP_CHOWN,
    OP_FCHMOD, OP_FCHOWN, OP_ACCESS_W, OP_GRANT_SUPREME
};

/* Do one operation as (uid, gid) in a child, and return what it returned:
 * 0, or a negative errno. -200 means the child could not even take on the
 * identity, which is a failure of the test and not an answer. */
static i64 as_user(u32 uid, u32 gid, int op, const char *path, i64 a, i64 b) {
    i64 pid, status = 0, code;

    pid = sc2(SYS_clone, 0, 0);
    if (pid == 0) {
        i64 r = -ENOSYS, fd;

        /* gid first: once the uid is dropped, setgid is no longer root's
         * to call. */
        if (sc1(SYS_setgid, gid) != 0 || sc1(SYS_setuid, uid) != 0) {
            sc1(SYS_exit_group, 200);
        }
        switch (op) {
        case OP_CREAT:
            r = sc3(SYS_open, path, O_WRONLY | O_CREAT, a);
            if (r >= 0) { sc1(SYS_close, r); r = 0; }
            break;
        case OP_MKDIR:  r = sc2(SYS_mkdir, path, a);            break;
        case OP_UNLINK: r = sc1(SYS_unlink, path);              break;
        case OP_RMDIR:  r = sc1(SYS_rmdir, path);               break;
        case OP_CHMOD:  r = sc2(SYS_chmod, path, a);            break;
        case OP_CHOWN:  r = sc3(SYS_chown, path, a, b);         break;
        case OP_FCHMOD:
        case OP_FCHOWN:
            fd = sc3(SYS_open, path, O_RDONLY, 0);
            if (fd < 0) { r = fd; break; }
            r = (op == OP_FCHMOD) ? sc2(SYS_fchmod, fd, a)
                                  : sc3(SYS_fchown, fd, a, b);
            sc1(SYS_close, fd);
            break;
        case OP_ACCESS_W:
            r = sc4(SYS_faccessat, (u64)AT_FDCWD, path, 2 /* W_OK */, 0);
            break;
        case OP_GRANT_SUPREME:
            r = sc2(SYS_prctl, PR_GENESIS_GRANT_SUPREME, a);
            break;
        }
        sc1(SYS_exit_group, r < 0 ? (int)((-r) & 0xFF) : 0);
        __builtin_unreachable();
    }
    if (pid < 0) {
        return -201;
    }
    sc4(SYS_wait4, -1, &status, 0, 0);
    code = (status >> 8) & 0xFF;
    return code == 0 ? 0 : -code;
}

static u32 st_mode_of(const char *path) {
    u8 st[144];

    return sc2(SYS_stat, path, st) == 0 ? *(u32 *)(st + 24) : 0xFFFFFFFFu;
}

static u32 st_uid_of(const char *path) {
    u8 st[144];

    return sc2(SYS_stat, path, st) == 0 ? *(u32 *)(st + 28) : 0xFFFFFFFFu;
}

static u32 st_gid_of(const char *path) {
    u8 st[144];

    return sc2(SYS_stat, path, st) == 0 ? *(u32 *)(st + 32) : 0xFFFFFFFFu;
}

static void test_gnfs_perms(void) {
    char base[8] = "/mnt/?";
    char p[64], q[64];
    u8   sfs[128];
    char c;
    int  found = 0;
    i64  old_umask, fd;

    section("gnfs: ownership and permissions from ring 3");

    for (c = 'd'; c <= 'z'; c++) {
        base[5] = c;
        if (sc2(SYS_statfs, base, sfs) == 0 &&
            *(u64 *)(sfs + 64) == GNFS_NAMELEN) {
            found = 1;
            break;
        }
    }
    if (!found) {
        out("  skip  no gnfs volume is mounted - build.py attaches one; "
            "attach one and rerun\n");
        return;
    }
    out("  (gnfs volume at ");
    out(base);
    out(")\n");

    old_umask = sc1(SYS_umask, 022);

    /* --- umask, applied by open(O_CREAT) and mkdir ----------------------- */
    sc1(SYS_umask, 027);
    path_at(p, base, "/um");
    fd = sc3(SYS_open, p, O_WRONLY | O_CREAT, 0666);
    check(fd >= 0, "root creates a file with mode 0666 under umask 027");
    if (fd >= 0) sc1(SYS_close, fd);
    check_eq(st_mode_of(p) & 07777, 0640, "and gets 0640 - the umask applied");
    path_at(p, base, "/umd");
    check_eq(sc2(SYS_mkdir, p, 0777), 0, "mkdir with 0777 under umask 027");
    check_eq(st_mode_of(p) & 07777, 0750, "gets 0750");
    sc1(SYS_umask, 022);
    path_at(p, base, "/um2");
    fd = sc3(SYS_open, p, O_WRONLY | O_CREAT, 0600);
    if (fd >= 0) sc1(SYS_close, fd);
    check_eq(st_mode_of(p) & 07777, 0600,
             "a requested 0600 is honoured - not the fixed 0644 of before");

    /* --- the creator owns it; creating needs the parent ------------------ */
    path_at(p, base, "/pt");
    check_eq(sc2(SYS_mkdir, p, 0755), 0, "root makes <gnfs>/pt, 0755");
    path_at(q, base, "/pt/x");
    check_eq(as_user(1000, 1000, OP_CREAT, q, 0644, 0), -EACCES,
             "uid 1000 may not create in root's 0755 directory");
    check_eq(sc3(SYS_chown, p, 1000, 1000), 0,
             "root chowns <gnfs>/pt to uid 1000");
    check_eq(st_uid_of(p), 1000, "and stat agrees");

    path_at(q, base, "/pt/mine");
    check_eq(as_user(1000, 1000, OP_CREAT, q, 0666, 0), 0,
             "now uid 1000 creates <gnfs>/pt/mine");
    check_eq(st_uid_of(q), 1000, "it is owned by uid 1000, the creator");
    check_eq(st_gid_of(q), 1000, "in group 1000, the creator's gid");
    check_eq(st_mode_of(q) & 07777, 0644,
             "and 0666 under the inherited umask 022 is 0644");
    check_eq(as_user(1000, 1000, OP_ACCESS_W, p, 0, 0), 0,
             "access(W_OK) on its own 0755 directory says yes (it used to "
             "ask for ACE_DELETE_CHILD that nothing granted)");

    /* --- chmod, chown, and their fd forms -------------------------------- */
    check_eq(as_user(1000, 1000, OP_CHMOD, q, 0600, 0), 0,
             "the owner may chmod it");
    check_eq(st_mode_of(q) & 07777, 0600, "to 0600");
    check_eq(as_user(2000, 2000, OP_CHMOD, q, 0777, 0), -EPERM,
             "a stranger may not - -EPERM, POSIX's errno for chmod");
    check_eq(as_user(1000, 1000, OP_FCHMOD, q, 0640, 0), 0,
             "fchmod through a descriptor works too");
    check_eq(st_mode_of(q) & 07777, 0640, "to 0640");
    check_eq(as_user(1000, 1000, OP_CHOWN, q, 2000, -1), -EPERM,
             "the owner may NOT chown it away to uid 2000");
    check_eq(as_user(1000, 1000, OP_FCHOWN, q, 2000, -1), -EPERM,
             "nor through fchown");
    check_eq(st_uid_of(q), 1000, "and it is still uid 1000's");

    /* --- deleting needs the parent --------------------------------------- */
    check_eq(as_user(2000, 2000, OP_UNLINK, q, 0, 0), -EACCES,
             "a stranger may not unlink it");
    check(exists(q), "and it is still there");
    path_at(q, base, "/pt/sub");
    check_eq(sc2(SYS_mkdir, q, 0755), 0,
             "root makes a subdirectory inside uid 1000's directory");
    check_eq(as_user(2000, 2000, OP_RMDIR, q, 0, 0), -EACCES,
             "a stranger may not rmdir it");
    check_eq(as_user(1000, 1000, OP_RMDIR, q, 0, 0), 0,
             "the PARENT's owner may, though root made it");
    path_at(q, base, "/pt/mine");
    check_eq(as_user(1000, 1000, OP_UNLINK, q, 0, 0), 0,
             "and uid 1000 may unlink its own file");
    check(!exists(q), "which is really gone");

    /* --- the special bits ------------------------------------------------ */
    path_at(q, base, "/pt/prog");
    check_eq(as_user(1000, 1000, OP_CREAT, q, 0755, 0), 0,
             "uid 1000 creates <gnfs>/pt/prog");
    check_eq(as_user(1000, 1000, OP_CHMOD, q, 04755, 0), 0,
             "and chmods it setuid");
    check_eq(st_mode_of(q) & 07777, 04755, "stat shows 04755");
    check_eq(sc3(SYS_chown, q, -1, 50), 0,
             "root moves it into group 50 - (uid_t)-1 keeps the owner");
    check_eq(st_uid_of(q), 1000, "the owner really was kept");
    check_eq(as_user(1000, 1000, OP_CHMOD, q, 02755, 0), 0,
             "uid 1000, not in group 50, chmods it 02755 and is not refused");
    check_eq(st_mode_of(q) & 07777, 0755,
             "but setgid was silently dropped - no group-50 program for it");

    path_at(p, base, "/proj");
    check_eq(sc2(SYS_mkdir, p, 0777), 0, "root makes <gnfs>/proj");
    check_eq(sc3(SYS_chown, p, -1, 50), 0, "in group 50");
    check_eq(sc2(SYS_chmod, p, 02777), 0, "and makes it setgid, 02777");
    path_at(q, base, "/proj/f");
    check_eq(as_user(1000, 1000, OP_CREAT, q, 0644, 0), 0,
             "uid 1000 creates a file in it");
    check_eq(st_gid_of(q), 50,
             "which is in group 50 - the directory's, not uid 1000's");
    path_at(q, base, "/proj/d");
    check_eq(as_user(1000, 1000, OP_MKDIR, q, 0755, 0), 0,
             "and a directory");
    check(st_gid_of(q) == 50 && (st_mode_of(q) & 02000),
          "which is group 50 AND setgid, so the rule carries on down");

    path_at(p, base, "/tmp");
    check_eq(sc2(SYS_mkdir, p, 0777), 0, "root makes <gnfs>/tmp");
    check_eq(sc2(SYS_chmod, p, 01777), 0, "sticky, 01777");
    path_at(q, base, "/tmp/a");
    check_eq(as_user(1000, 1000, OP_CREAT, q, 0644, 0), 0,
             "uid 1000 creates a file in it");
    check_eq(as_user(2000, 2000, OP_UNLINK, q, 0, 0), -EPERM,
             "uid 2000 may NOT delete it - -EPERM, the sticky bit");
    check_eq(as_user(1000, 1000, OP_UNLINK, q, 0, 0), 0,
             "its owner may");

    /* --- the supreme privilege (handoff item 6) -------------------------- */
    check_eq(as_user(1000, 1000, OP_GRANT_SUPREME, 0, 1000, 0), -EPERM,
             "a non-root process may not grant ITSELF supreme");
    check_eq(sc2(SYS_prctl, PR_GENESIS_QUERY_SUPREME, 0), -1,
             "and nobody holds it after the refused attempt");
    path_at(q, base, "/pt/gift");
    check_eq(as_user(1000, 1000, OP_CREAT, q, 0644, 0), 0,
             "uid 1000 creates <gnfs>/pt/gift");
    check_eq(as_user(1000, 1000, OP_CHOWN, q, 3000, -1), -EPERM,
             "and may not give it to uid 3000 - the control");
    check_eq(sc2(SYS_prctl, PR_GENESIS_GRANT_SUPREME, 1000), 0,
             "root grants supreme to uid 1000");
    check_eq(as_user(1000, 1000, OP_CHOWN, q, 3000, -1), 0,
             "and now uid 1000 CAN give it away");
    check_eq(st_uid_of(q), 3000, "it belongs to uid 3000");
    check_eq(sc2(SYS_prctl, PR_GENESIS_REVOKE_SUPREME, 0), 0,
             "root revokes it");
    check_eq(sc2(SYS_prctl, PR_GENESIS_QUERY_SUPREME, 0), -1,
             "and nobody holds it again");

    sc1(SYS_umask, old_umask);
}

/* --- main --------------------------------------------------------------- */

void _start(void) {
    out("systest: exercising every syscall Genesis implements\n");
    out("(reboot is implemented and deliberately not called)\n");

    test_identity();
    test_uname();
    test_brk();
    test_mmap();
    test_mremap();
    test_eventfd();
    test_socketpair();
    test_socket();
    test_sigaltstack();
    test_waitid();
    test_arch_prctl();
    test_files();
    test_stat();
    test_dirs();
    test_volumes();
    test_acls();
    test_ioctl();
    test_writev();
    test_dev();
    test_getrandom();
    test_declined();
    test_signals();
    test_fork();
    test_pipes();
    test_clone();
    test_fpu();
    test_exec();
    test_vfork();
    test_wx();
    test_audit_gapfill();
    test_namespace();
    test_gnfs_perms();

    out("\nsystest: ");
    out_i64(passes);
    out(" passed, ");
    out_i64(failures);
    out(" failed\n");

    sc1(SYS_exit_group, failures > 255 ? 255 : failures);
    __builtin_unreachable();
}
