/* verification - the queue of things written but never run.
 *
 * Genesis's problem is not that code is wrong, it is that code is UNVERIFIED.
 * systest is the standing regression suite: it runs after every change and
 * everything in it has passed at least once. This is the other list - the
 * work that compiles or assembles and has never executed, plus the checks
 * that cannot be made from ring 3 at all and have to be made by a human.
 *
 * Two halves, and the split is the point:
 *
 *   PART A   checks this program can make itself. They run and print
 *            ok/FAIL like systest. When one of these passes it should be
 *            MOVED into systest and deleted from here - this file is a queue,
 *            not a second test suite, and a check that lives in both is a
 *            check that will be updated in one.
 *
 *   PART B   checks nobody in ring 3 can make. Printed as a checklist at the
 *            end, with the exact command or observation for each. These are
 *            here because "I could not test it" belongs in the tree next to
 *            the thing that was not tested, rather than in a message that
 *            scrolls away.
 *
 * Build:  tools/build_user.sh    (stages as /bin/verify)
 * Run:    /bin/verify            from the Genesis shell
 *
 * Exit status is the number of failed PART A checks, capped at 255.
 *
 * Freestanding and static, for systest's reason: a libc retries, translates
 * and papers over return values, and the failures worth catching here are
 * exactly the ones it hides.
 */

typedef unsigned long  u64;
typedef long           i64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef short          i16;
typedef unsigned char  u8;

#define NULL ((void *)0)

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

#define sc0(n)           sc6((n), 0, 0, 0, 0, 0, 0)
#define sc1(n,a)         sc6((n), (i64)(a), 0, 0, 0, 0, 0)
#define sc2(n,a,b)       sc6((n), (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sc3(n,a,b,c)     sc6((n), (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sc4(n,a,b,c,d)   sc6((n), (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)

#define SYS_read            0
/* Added for the volume checks. getdents64 is how /dev is listed, and
 * O_DIRECTORY is what opendir passes - the flag that once made `ls /dev` fail
 * with -ENOTDIR, so it is worth exercising rather than avoiding. */
#define SYS_getdents64    217
#define O_DIRECTORY  0200000
#define SYS_write           1
#define SYS_close           3
#define SYS_poll            7
#define SYS_pipe           22
#define SYS_access         21
#define SYS_dup            32
#define SYS_dup2           33
#define SYS_fork           57
#define SYS_execve         59
#define SYS_exit           60
#define SYS_exit_group    231
#define SYS_wait4          61
#define SYS_fcntl          72
#define SYS_nanosleep      35
#define SYS_clock_gettime 228
#define SYS_openat        257
#define SYS_faccessat     269
#define SYS_faccessat2    439
#define SYS_ppoll         271
#define SYS_pipe2         293
#define SYS_futex         202
#define SYS_gettid        186
#define SYS_getpid         39
#define SYS_clone          56
#define SYS_mmap            9
#define SYS_tgkill        234

#define CLONE_VM            0x00000100
#define CLONE_FS            0x00000200
#define CLONE_FILES         0x00000400
#define CLONE_SIGHAND       0x00000800
#define CLONE_THREAD        0x00010000
#define CLONE_SETTLS        0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#define PROT_READ     0x1
#define PROT_EXEC     0x4
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define ETIMEDOUT 110
#define ESRCH       3
#define EFAULT     14
#define ENODEV     19
#define EISDIR     21
#define ENOTDIR    20

#define SYS_stat            4
#define SYS_lseek           8
#define SYS_clock_getres  229
#define SEEK_SET 0
#define SEEK_END 2

#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

#define S_IFMT  0170000
#define S_IFBLK 0060000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFREG 0100000

#define EINTR      4
#define EBADF      9
#define EAGAIN    11
#define EACCES    13
#define EINVAL    22
#define EROFS     30
#define ENOSYS    38
#define ENOENT     2

#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_EXCL     0x0080
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define SYS_truncate       76
#define SYS_statfs        137
#define SYS_ftruncate      77
#define SYS_unlink         87
#define EEXIST     17
#define O_NONBLOCK 0x0800
#define O_CLOEXEC  0x80000
#define AT_FDCWD   (-100)

#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_GETLK          5
#define F_SETLK          6
#define F_DUPFD_CLOEXEC  1030
#define FD_CLOEXEC       1

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

#define CLOCK_MONOTONIC 1

struct pollfd {
    int fd;
    i16 events;
    i16 revents;
};

/* --- output -------------------------------------------------------------- */

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

/* ========================================================================
 * PART A - checks this program can make
 * ===================================================================== */

/* --- fcntl ---------------------------------------------------------------
 *
 * The three flag sets, and the checks that catch each one being stored at the
 * wrong level. That is the whole bug class here: every wrong answer below
 * looks like a working fcntl until a program dups a descriptor. */

static void test_fcntl(void) {
    int fd, fd2, fd3;
    i64 rc;

    section("fcntl");

    fd = (int)sc4(SYS_openat, AT_FDCWD, "/etc/motd", O_RDONLY, 0);
    if (fd < 0) {
        check(0, "open /etc/motd for the fcntl tests");
        return;
    }

    check_eq(sc3(SYS_fcntl, fd, F_GETFD, 0), 0,
             "F_GETFD on a plain open descriptor is clear");
    check_eq(sc3(SYS_fcntl, fd, F_SETFD, FD_CLOEXEC), 0, "F_SETFD accepts it");
    check_eq(sc3(SYS_fcntl, fd, F_GETFD, 0), FD_CLOEXEC,
             "and F_GETFD reads it back");

    /* THE check for FD_CLOEXEC being stored in the wrong one of the three
     * flag sets. dup(2) is defined NOT to copy it - if it lands on the open
     * instance instead of on the descriptor, this reports 1 and a shell's
     * redirection of a close-on-exec descriptor silently closes in the
     * child. */
    fd2 = (int)sc1(SYS_dup, fd);
    check(fd2 >= 0, "dup of a close-on-exec descriptor succeeds");
    check_eq(sc3(SYS_fcntl, fd2, F_GETFD, 0), 0,
             "and the duplicate does NOT inherit FD_CLOEXEC");
    sc3(SYS_fcntl, fd, F_SETFD, 0);

    /* F_GETFL reconstructs the access mode. O_RDONLY is zero, so a naive
     * implementation that tests the read bit before the read+write case
     * reports every O_RDWR descriptor as read-only. Nothing here can open
     * O_RDWR on a read-only volume, so the console is used instead. */
    check_eq(sc3(SYS_fcntl, fd, F_GETFL, 0) & 3, O_RDONLY,
             "F_GETFL reports O_RDONLY for a file opened read-only");

    fd3 = (int)sc4(SYS_openat, AT_FDCWD, "/dev/console", O_RDWR, 0);
    if (fd3 >= 0) {
        check_eq(sc3(SYS_fcntl, fd3, F_GETFL, 0) & 3, O_RDWR,
                 "F_GETFL reports O_RDWR for the console (the O_RDONLY==0 trap)");
        sc1(SYS_close, fd3);
    } else {
        check(0, "open /dev/console O_RDWR");
    }

    /* F_DUPFD's floor is its entire content. A shell uses it to park a
     * descriptor clear of the 0/1/2 it is about to rearrange, so answering
     * with the lowest free index regardless hands back the one it was trying
     * to avoid. */
    rc = sc3(SYS_fcntl, fd, F_DUPFD, 10);
    check(rc >= 10, "F_DUPFD respects its floor");
    if (rc >= 0) {
        check_eq(sc3(SYS_fcntl, (int)rc, F_GETFD, 0), 0,
                 "F_DUPFD does not set close-on-exec");
        sc1(SYS_close, (int)rc);
    }
    rc = sc3(SYS_fcntl, fd, F_DUPFD_CLOEXEC, 10);
    check(rc >= 10, "F_DUPFD_CLOEXEC respects its floor too");
    if (rc >= 0) {
        check_eq(sc3(SYS_fcntl, (int)rc, F_GETFD, 0), FD_CLOEXEC,
                 "and DOES set close-on-exec - the only difference between them");
        sc1(SYS_close, (int)rc);
    }

    /* Accepted now. This check has been -EROFS through three phases: no write
     * path, then a write path with no append policy, and now both. The flag
     * is a STATUS flag, so setting it here affects every descriptor dup'd
     * from this one - which is why F_GETFL has to report it back. */
    check_eq(sc3(SYS_fcntl, fd, F_SETFL, O_APPEND), 0,
             "F_SETFL accepts O_APPEND now that do_write honours it");
    check_eq(sc3(SYS_fcntl, fd, F_GETFL, 0) & O_APPEND, O_APPEND,
             "and F_GETFL reports it back");
    check_eq(sc3(SYS_fcntl, fd, F_SETFL, 0), 0, "and it can be cleared");
    check_eq(sc3(SYS_fcntl, fd, F_GETFL, 0) & O_APPEND, 0,
             "which F_GETFL also reports - the flag is not write-once");
    check_eq(sc3(SYS_fcntl, fd, F_GETLK, 0), -EINVAL,
             "F_GETLK declines rather than lying about a lock");
    check_eq(sc3(SYS_fcntl, fd, F_SETLK, 0), -EINVAL, "F_SETLK likewise");
    check_eq(sc3(SYS_fcntl, 999, F_GETFD, 0), -EBADF,
             "fcntl on a closed descriptor is -EBADF");

    sc1(SYS_close, fd2);
    sc1(SYS_close, fd);
}

/* --- access -------------------------------------------------------------- */

static void test_access(void) {
    section("access / faccessat / faccessat2");

    check_eq(sc2(SYS_access, "/etc/motd", F_OK), 0, "access(F_OK) on a file");
    check_eq(sc2(SYS_access, "/etc/motd", R_OK), 0, "access(R_OK) on a file");
    check_eq(sc2(SYS_access, "/etc/motd", X_OK), 0,
             "access(X_OK) - FAT has no permission bits to say otherwise");

    /* This check said "-EACCES here is the truth, not a placeholder: the
     * volume really is read-only. When it becomes writable this check has to
     * change with it, and it is here so that it does."
     *
     * It became writable. fatfs's .write slot is no longer NULL, fs_writable
     * derives its answer from that pointer, and access(W_OK) derives its
     * answer from fs_writable - so this flipped with no change to access(2)
     * itself, which is exactly what having one source of the fact bought. */
    check_eq(sc2(SYS_access, "/etc/motd", W_OK), 0,
             "access(W_OK) succeeds now that the volume has a write path");

    check_eq(sc2(SYS_access, "/no/such/file", F_OK), -ENOENT,
             "access on a missing file is -ENOENT");
    check_eq(sc2(SYS_access, "/dev/console", W_OK), 0,
             "a device answers W_OK - the console really is writable");
    check_eq(sc2(SYS_access, "/etc/motd", 0x40), -EINVAL,
             "an undefined mode bit is refused, not ignored");

    check_eq(sc4(SYS_faccessat, AT_FDCWD, "/etc/motd", F_OK, 0), 0,
             "faccessat agrees with access");
    check_eq(sc4(SYS_faccessat2, AT_FDCWD, "/etc/motd", F_OK, 0), 0,
             "faccessat2 is implemented, so musl skips its -ENOSYS fallback");
    check_eq(sc4(SYS_faccessat, 3, "/etc/motd", F_OK, 0), -EINVAL,
             "faccessat with a real dirfd declines rather than ignoring it");
}

/* --- poll ---------------------------------------------------------------- */

static void test_poll_basics(void) {
    struct pollfd pfd[3];
    int fd;

    section("poll - the non-blocking probe");

    fd = (int)sc4(SYS_openat, AT_FDCWD, "/etc/motd", O_RDONLY, 0);
    if (fd < 0) {
        check(0, "open /etc/motd for the poll tests");
        return;
    }

    /* POSIX: a regular file is always ready for both. A disk read that takes
     * a millisecond is not "blocking" in the sense poll means. */
    pfd[0].fd = fd;
    pfd[0].events = POLLIN | POLLOUT;
    pfd[0].revents = 0;
    check_eq(sc3(SYS_poll, pfd, 1, 0), 1, "poll on a file returns 1");
    check_eq(pfd[0].revents & POLLIN, POLLIN, "a file is always readable");
    check_eq(pfd[0].revents & POLLOUT, POLLOUT, "and always writable");

    /* A negative fd is IGNORED, not an error. Programs punch holes in a
     * persistent array rather than rebuilding it; reporting POLLNVAL would
     * give every such program a permanent error. */
    pfd[1].fd = -1;
    pfd[1].events = POLLIN;
    pfd[1].revents = 0x7F;
    check_eq(sc3(SYS_poll, pfd, 2, 0), 1, "a negative fd does not count as ready");
    check_eq(pfd[1].revents, 0, "and its revents is cleared, not POLLNVAL");

    /* A closed descriptor DOES get POLLNVAL, and it is reported even though
     * it was not asked for - that is what an output-only bit means. */
    pfd[2].fd = 999;
    pfd[2].events = POLLIN;
    pfd[2].revents = 0;
    check_eq(sc3(SYS_poll, &pfd[2], 1, 0), 1, "a closed fd is reported");
    check_eq(pfd[2].revents, POLLNVAL, "as POLLNVAL, which was not requested");

    check_eq(sc3(SYS_poll, pfd, 0, 0), 0, "poll with nfds 0 answers 0");
    check_eq(sc3(SYS_poll, pfd, 4096, 0), -EINVAL,
             "poll refuses more descriptors than the table can hold");

    sc1(SYS_close, fd);
}

static void test_poll_pipe(void) {
    int fds[2];
    struct pollfd pfd[2];
    char buf[16];

    section("poll - a pipe, both ends");

    if (sc1(SYS_pipe, fds) != 0) {
        check(0, "pipe() for the poll tests");
        return;
    }

    pfd[0].fd = fds[0]; pfd[0].events = POLLIN;  pfd[0].revents = 0;
    pfd[1].fd = fds[1]; pfd[1].events = POLLOUT; pfd[1].revents = 0;

    check_eq(sc3(SYS_poll, pfd, 2, 0), 1, "an empty pipe has one ready end");
    check_eq(pfd[0].revents, 0, "the read end is not readable when empty");
    check_eq(pfd[1].revents, POLLOUT, "the write end is writable when it has room");

    sc3(SYS_write, fds[1], "hi", 2);
    pfd[0].revents = 0;
    pfd[1].revents = 0;
    check_eq(sc3(SYS_poll, pfd, 2, 0), 2, "after a write, both ends are ready");
    check_eq(pfd[0].revents, POLLIN, "the read end is readable");

    sc3(SYS_read, fds[0], buf, sizeof(buf));
    pfd[0].revents = 0;
    check_eq(sc3(SYS_poll, pfd, 1, 0), 0, "and not readable again once drained");

    /* The check the whole design turns on. End of file is a READABLE event.
     * A poll that reports "nothing to read" on a pipe whose writer has gone
     * hangs on exactly the case poll was added to handle. */
    sc1(SYS_close, fds[1]);
    pfd[0].revents = 0;
    check_eq(sc3(SYS_poll, pfd, 1, 0), 1, "closing the write end wakes the poll");
    check_eq(pfd[0].revents & POLLHUP, POLLHUP, "the read end reports POLLHUP");
    check_eq(pfd[0].revents & POLLIN, POLLIN,
             "and POLLIN too - a read really would return, with zero");
    check_eq(sc3(SYS_read, fds[0], buf, sizeof(buf)), 0,
             "and that read is end of file, not a block");

    sc1(SYS_close, fds[0]);

    /* The other end of the same question. A write end with no readers left
     * reports POLLERR rather than POLLHUP, because a write there does not end
     * quietly - it raises SIGPIPE and returns -EPIPE. */
    if (sc1(SYS_pipe, fds) == 0) {
        sc1(SYS_close, fds[0]);
        pfd[0].fd = fds[1];
        pfd[0].events = POLLOUT;
        pfd[0].revents = 0;
        check_eq(sc3(SYS_poll, pfd, 1, 0), 1, "a pipe with no readers is ready");
        check_eq(pfd[0].revents & POLLERR, POLLERR,
                 "and reports POLLERR, not POLLHUP - a write will FAIL, not end");
        sc1(SYS_close, fds[1]);
    }
}

static void test_poll_timeout(void) {
    struct pollfd pfd;
    int fds[2];
    u64 t0[2], t1[2];
    i64 elapsed_ms;

    section("poll - the timeout");

    if (sc1(SYS_pipe, fds) != 0) {
        check(0, "pipe() for the timeout test");
        return;
    }
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    sc2(SYS_clock_gettime, CLOCK_MONOTONIC, t0);
    /* Nothing will ever write to this pipe, so the only way out is the
     * deadline. A zero return is the answer, not an error: a poll that times
     * out with nothing ready has not failed. */
    check_eq(sc3(SYS_poll, &pfd, 1, 50), 0, "a poll that times out returns 0");
    sc2(SYS_clock_gettime, CLOCK_MONOTONIC, t1);

    elapsed_ms = (i64)((t1[0] - t0[0]) * 1000 +
                       (t1[1] / 1000000) - (t0[1] / 1000000));
    check(elapsed_ms >= 50, "and really waited at least the 50ms it was given");
    if (elapsed_ms < 50) {
        out("        elapsed was ");
        out_i64(elapsed_ms);
        out("ms - a timeout that expires early is invisible to the caller\n");
    }

    check_eq(sc3(SYS_poll, &pfd, 1, 0), 0,
             "a zero timeout returns immediately rather than spinning");

    sc1(SYS_close, fds[0]);
    sc1(SYS_close, fds[1]);
}

/* The blocking case, which is the one the shared readiness queue exists for.
 * A child writes after a delay; the parent must be woken by it rather than
 * sitting until the timeout. If the readiness broadcast is missing this still
 * PASSES on the return value and fails on the elapsed time - which is why the
 * time is checked. */
static void test_poll_blocking(void) {
    int fds[2];
    struct pollfd pfd;
    u64 t0[2], t1[2];
    i64 pid, elapsed_ms, rc;
    u64 req[2];

    section("poll - blocking, woken by another process");

    if (sc1(SYS_pipe, fds) != 0) {
        check(0, "pipe() for the blocking test");
        return;
    }

    pid = sc0(SYS_fork);
    if (pid == 0) {
        sc1(SYS_close, fds[0]);
        req[0] = 0;
        req[1] = 60000000ULL;              /* 60ms */
        sc2(SYS_nanosleep, req, 0);
        sc3(SYS_write, fds[1], "x", 1);
        sc1(SYS_close, fds[1]);
        sc1(SYS_exit, 0);
    }
    if (pid < 0) {
        check(0, "fork for the blocking poll test");
        sc1(SYS_close, fds[0]);
        sc1(SYS_close, fds[1]);
        return;
    }

    sc1(SYS_close, fds[1]);
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    sc2(SYS_clock_gettime, CLOCK_MONOTONIC, t0);
    rc = sc3(SYS_poll, &pfd, 1, 5000);
    sc2(SYS_clock_gettime, CLOCK_MONOTONIC, t1);
    elapsed_ms = (i64)((t1[0] - t0[0]) * 1000 +
                       (t1[1] / 1000000) - (t0[1] / 1000000));

    check_eq(rc, 1, "a blocking poll returns when the writer writes");
    check_eq(pfd.revents & POLLIN, POLLIN, "and reports the read end readable");
    check(elapsed_ms < 4000,
          "woken by the write rather than by the 5s timeout");
    if (elapsed_ms >= 4000) {
        out("        it timed out instead - the readiness broadcast in\n");
        out("        waitq_wake_all is not reaching the poller\n");
    }

    sc4(SYS_wait4, pid, 0, 0, 0);
    sc1(SYS_close, fds[0]);
}

/* --- O_NONBLOCK ---------------------------------------------------------- */

static void test_nonblock(void) {
    int fds[2];
    char buf[16];
    i64 rc;

    section("O_NONBLOCK");

    if (sc1(SYS_pipe, fds) != 0) {
        check(0, "pipe() for the O_NONBLOCK tests");
        return;
    }

    check_eq(sc3(SYS_fcntl, fds[0], F_SETFL, O_NONBLOCK), 0,
             "F_SETFL now ACCEPTS O_NONBLOCK");
    check_eq(sc3(SYS_fcntl, fds[0], F_GETFL, 0) & O_NONBLOCK, O_NONBLOCK,
             "and F_GETFL reads it back");

    /* The check that the flag means something. Before the readiness
     * predicate existed, accepting this returned success and then blocked -
     * the one failure a program using it cannot diagnose. */
    check_eq(sc3(SYS_read, fds[0], buf, sizeof(buf)), -EAGAIN,
             "a non-blocking read of an empty pipe is -EAGAIN, not a hang");

    sc3(SYS_write, fds[1], "hi", 2);
    check_eq(sc3(SYS_read, fds[0], buf, sizeof(buf)), 2,
             "and reads normally once there is data");

    /* O_NONBLOCK is a STATUS flag: shared by every descriptor for one open
     * instance. This is the mirror of the FD_CLOEXEC check above, and the two
     * together are what pin all three flag sets to the right level. */
    rc = sc1(SYS_dup, fds[0]);
    check(rc >= 0, "dup of a non-blocking descriptor");
    if (rc >= 0) {
        check_eq(sc3(SYS_fcntl, (int)rc, F_GETFL, 0) & O_NONBLOCK, O_NONBLOCK,
                 "the duplicate DOES share O_NONBLOCK - it is an open-file flag");
        sc1(SYS_close, (int)rc);
    }

    /* End of file must still arrive as zero. -EAGAIN here would be a program
     * spinning forever on a pipe that will never have data again. */
    sc1(SYS_close, fds[1]);
    check_eq(sc3(SYS_read, fds[0], buf, sizeof(buf)), 0,
             "a non-blocking read at end of file is 0, not -EAGAIN");
    sc1(SYS_close, fds[0]);

    /* pipe2 applies the flag to both ends at creation. */
    if (sc2(SYS_pipe2, fds, O_NONBLOCK) == 0) {
        check_eq(sc3(SYS_fcntl, fds[0], F_GETFL, 0) & O_NONBLOCK, O_NONBLOCK,
                 "pipe2(O_NONBLOCK) sets it on the read end");
        check_eq(sc3(SYS_fcntl, fds[1], F_GETFL, 0) & O_NONBLOCK, O_NONBLOCK,
                 "and on the write end");
        sc1(SYS_close, fds[0]);
        sc1(SYS_close, fds[1]);
    } else {
        check(0, "pipe2 accepts O_NONBLOCK");
    }
}

/* --- ppoll --------------------------------------------------------------- */

static void test_ppoll(void) {
    struct pollfd pfd;
    u64 ts[2];
    int fd;

    section("ppoll");

    fd = (int)sc4(SYS_openat, AT_FDCWD, "/etc/motd", O_RDONLY, 0);
    if (fd < 0) {
        check(0, "open /etc/motd for the ppoll test");
        return;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    ts[0] = 0;
    ts[1] = 0;
    check_eq(sc4(SYS_ppoll, &pfd, 1, ts, 0), 1,
             "ppoll with a NULL mask is poll");

    /* The mask is applied now (systest's "phase 1" section checks that it
     * really is - the signal it unblocks is delivered and the caller's mask
     * comes back). Here: a mask with the wrong sigsetsize is -EINVAL, not
     * accepted and misread. This check used to assert -ENOSYS, from when a
     * mask was declined because nothing could apply one. */
    check_eq(sc6(SYS_ppoll, (i64)&pfd, 1, (i64)ts, 0x1234, 4, 0), -EINVAL,
             "ppoll refuses a signal mask of the wrong size");

    sc1(SYS_close, fd);
}

/* --- W^X ----------------------------------------------------------------
 *
 * NX is checked by watching a child DIE. There is no way to test a page fault
 * from inside the process that takes it, and catching SIGSEGV would test the
 * signal path as much as the mapping. So each attempt runs in a forked child
 * and the parent asserts the child did not survive.
 *
 * A child that survives is the failure, and it is the failure that reports
 * nothing on its own: an executable stack works perfectly until somebody
 * exploits it. */

static int child_survives_executing(void *addr) {
    i64 pid, status = 0;

    /* 0xC3 is `ret`. If the page is executable the child returns from the
     * call and exits 0; if it is not, the fetch faults and the child dies
     * without reaching the exit. */
    ((volatile u8 *)addr)[0] = 0xC3;

    pid = sc0(SYS_fork);
    if (pid == 0) {
        ((void (*)(void))addr)();
        sc1(SYS_exit, 0);
    }
    if (pid < 0) {
        return -1;
    }
    sc4(SYS_wait4, pid, &status, 0, 0);

    /* The wait status encoding: a normal exit puts the code in bits 15:8, so
     * a child that ran to completion reports 0 here. Anything else means it
     * did not get there. */
    return (status & 0xFF00) == 0 && (status & 0x7F) == 0;
}

static void test_wx(void) {
    u8 stack_buf[64];
    i64 heap;
    int rc;

    section("W^X - NX on the stack, the break and anonymous mappings");

    rc = child_survives_executing(stack_buf);
    if (rc < 0) {
        check(0, "fork for the executable-stack test");
    } else {
        check(rc == 0, "executing from the STACK faults");
    }

    /* The break. brk(0) reports the current end; asking for a page more and
     * executing out of the old end tests the heap. */
    heap = sc1(0x0C /* SYS_brk */, 0);
    if (heap > 0 && sc1(0x0C, heap + 0x2000) > heap) {
        rc = child_survives_executing((void *)(u64)heap);
        if (rc < 0) {
            check(0, "fork for the executable-break test");
        } else {
            check(rc == 0, "executing from the BREAK faults");
        }
    } else {
        check(0, "grow the break for the W^X test");
    }

    /* Anonymous mmap, which the section header promised and the first
     * version of this test did not actually check.
     *
     * This is the case that changed most: sys_mmap ignored `prot` entirely,
     * so EVERY anonymous mapping was executable - including everything malloc
     * hands out. An executable heap is the largest of the three holes and the
     * only one a program can grow at will, so leaving it untested while
     * claiming to test it was the worst of the three to have missed. */
    {
        i64 addr = sc6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (addr <= 0) {
            check(0, "mmap for the executable-mapping test");
        } else {
            rc = child_survives_executing((void *)(u64)addr);
            if (rc < 0) {
                check(0, "fork for the executable-mapping test");
            } else {
                check(rc == 0,
                      "executing an anonymous mapping without PROT_EXEC faults");
            }
        }
    }

    /* The complement, and it is the half that proves PROT_EXEC is HONOURED
     * rather than that NX is merely always on. Without this, a sys_mmap that
     * set PAGE_NX unconditionally would pass every check above - and would
     * break the ELF rtld in roadmap item 4 the first time it mapped a text
     * segment. A test that only ever asserts "this faults" cannot tell
     * enforcement from a blanket refusal. */
    {
        i64 addr = sc6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (addr <= 0) {
            check(0, "mmap PROT_EXEC for the positive W^X test");
        } else {
            rc = child_survives_executing((void *)(u64)addr);
            if (rc < 0) {
                check(0, "fork for the PROT_EXEC test");
            } else {
                check(rc == 1,
                      "but WITH PROT_EXEC it runs - the flag is honoured");
            }
        }
    }

    out("        note: on a CPU without NX the three negative checks above\n");
    out("        cannot fail, because paging.c strips PAGE_NX when EFER.NXE\n");
    out("        is off. Under QEMU with the default cpu model NX is\n");
    out("        present, so a pass here is real.\n");
}

/* --- futex --------------------------------------------------------------
 *
 * Testable single-threaded, which is worth doing before any thread exists:
 * every one of these is a wrong answer the kernel can give without another
 * thread being involved, and finding them here means not finding them
 * afterwards while also debugging clone. */

static void test_futex(void) {
    static volatile u32 word;
    u64 ts[2];

    section("futex");

    /* The value check, which is the whole protocol. Userspace has already
     * decided the lock is contended; the kernel re-tests because the state
     * can change in the gap. A word that does not hold the expected value
     * means the contention is already over. */
    word = 1;
    check_eq(sc4(SYS_futex, &word, FUTEX_WAIT, 999, 0), -EAGAIN,
             "FUTEX_WAIT on a word that changed returns -EAGAIN, not a sleep");

    /* The lost-wakeup guard. If this actually slept, the test would hang -
     * which is itself the result, and better than a wrong answer. */
    ts[0] = 0;
    ts[1] = 30000000ULL;                    /* 30ms */
    word = 7;
    check_eq(sc4(SYS_futex, &word, FUTEX_WAIT, 7, ts), -ETIMEDOUT,
             "FUTEX_WAIT with a matching value sleeps and times out");

    check_eq(sc4(SYS_futex, &word, FUTEX_WAKE, 1, 0), 1,
             "FUTEX_WAKE on a queue with nobody on it is not an error");

    /* Misalignment is refused rather than hashed. An unaligned futex can
     * straddle a page boundary, which would give one word two keys. */
    check_eq(sc4(SYS_futex, ((u8 *)&word) + 1, FUTEX_WAKE, 1, 0), -EFAULT,
             "an unaligned futex address is -EFAULT");
    check_eq(sc4(SYS_futex, 0, FUTEX_WAKE, 1, 0), -EFAULT,
             "a null futex address is -EFAULT");

    /* Refused rather than approximated. FUTEX_REQUEUE is a distinct protocol,
     * and something built out of the wrong primitives works under low
     * contention and deadlocks under high. */
    check_eq(sc4(SYS_futex, &word, 3 /* FUTEX_REQUEUE */, 1, 0), -ENOSYS,
             "an unimplemented futex op says -ENOSYS, not -EINVAL");
}

/* --- threads -------------------------------------------------------------
 *
 * The clone() argument checks, which need no thread to run. The thread shape
 * ITSELF is deliberately not exercised here: a real pthread needs a stack
 * this program would have to mmap and a TLS block it would have to build, and
 * a hand-rolled version would test this program's idea of a thread rather
 * than musl's. That check is in PART B, against a real pthreads binary. */

static void test_clone_args(void) {
    section("clone - argument checking");

    check(sc0(SYS_gettid) > 0, "gettid answers");
    check_eq(sc0(SYS_gettid), sc0(SYS_getpid),
             "for a single-threaded process gettid and getpid agree");

    /* A partial thread shape describes something this kernel does not build,
     * and giving a 'thread' its own copy of memory produces a program that
     * runs and is wrong - two threads incrementing what they both believe is
     * one counter and getting two. */
    check_eq(sc6((i64)SYS_clone, CLONE_VM | CLONE_THREAD, 0x1000, 0, 0, 0, 0),
             -EINVAL, "CLONE_VM|CLONE_THREAD without the rest is refused");
    check_eq(sc6((i64)SYS_clone, CLONE_THREAD, 0x1000, 0, 0, 0, 0),
             -EINVAL, "CLONE_THREAD without CLONE_VM is refused");

    /* A thread sharing an address space with no stack of its own would push
     * onto its creator's, and each would corrupt the other's frames. */
    check_eq(sc6((i64)SYS_clone,
                 CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                 CLONE_THREAD, 0, 0, 0, 0, 0),
             -EINVAL, "a thread with no stack pointer is refused");

    check_eq(sc3(SYS_tgkill, 999999, 999999, 0), -ESRCH,
             "tgkill on a tid that does not exist is -ESRCH");
    /* The tgid is checked rather than ignored, which is the entire reason
     * tgkill exists in preference to tkill: a recycled tid must not receive a
     * signal aimed at the thread that used to hold it. */
    check_eq(sc3(SYS_tgkill, 999999, sc0(SYS_gettid), 0), -ESRCH,
             "tgkill with the wrong tgid is refused even for a live tid");
}

/* --- the filesystem vtable ----------------------------------------------
 *
 * These are not new behaviour, and that is exactly what makes them worth
 * running: the vtable was supposed to change nothing a program can see. Every
 * check here passed before it and must still pass after, which is the only
 * way to tell an abstraction from a rewrite. */

static void test_fs_vtable(void) {
    char buf[64];
    u8 st[144];
    int fd;
    i64 rc;
    u64 i;

    section("filesystem vtable - behaviour that must NOT have changed");

    fd = (int)sc4(SYS_openat, AT_FDCWD, "/etc/motd", O_RDONLY, 0);
    check(fd >= 0, "a file still opens");
    if (fd >= 0) {
        rc = sc3(SYS_read, fd, buf, sizeof(buf));
        check(rc > 0, "and still reads");

        /* Position advancing through the vtable rather than in the caller is
         * what makes two descriptors from dup share progress. */
        check_eq(sc3(SYS_lseek, fd, 0, SEEK_SET), 0, "lseek to the start");
        check(sc3(SYS_lseek, fd, 0, SEEK_END) > 0, "SEEK_END finds a size");

        for (i = 0; i < sizeof(st); i++) st[i] = 0;
        if (sc2(SYS_stat, "/etc/motd", st) == 0) {
            check_eq(*(u32 *)(st + 24) & S_IFMT, S_IFREG,
                     "stat reports a regular file");
            check(*(u64 *)(st + 48) > 0, "with a non-zero size");
            check(*(u64 *)(st + 56) > 0,
                  "and a block size from the volume, not a constant");
        } else {
            check(0, "stat /etc/motd");
        }
        sc1(SYS_close, fd);
    }

    /* The errno distinctions the vtable had to carry across the boundary.
     * ash prints different messages for these and they mean different things
     * about what the user typed, so collapsing them all to -EIO would be a
     * regression a compile cannot catch. */
    check_eq(sc4(SYS_openat, AT_FDCWD, "/no/such/thing", O_RDONLY, 0), -ENOENT,
             "a missing file is still -ENOENT, not -EIO");
    check_eq(sc4(SYS_openat, AT_FDCWD, "/etc/motd/nope", O_RDONLY, 0), -ENOTDIR,
             "a non-directory component is still -ENOTDIR");

    /* Reading a directory is -EISDIR, which now comes from fs_read rather
     * than from each filesystem remembering to say so. */
    fd = (int)sc4(SYS_openat, AT_FDCWD, "/etc", O_RDONLY, 0);
    if (fd >= 0) {
        check_eq(sc3(SYS_read, fd, buf, sizeof(buf)), -EISDIR,
                 "reading a directory is -EISDIR");
        sc1(SYS_close, fd);
    } else {
        check(0, "open /etc as a directory");
    }
}

/* --- block devices ------------------------------------------------------- */

static void test_block_device(void) {
    u8 st[144];
    char buf[512];
    int fd;
    u64 i;
    i64 rc;

    section("raw disk - S_IFBLK and byte-addressed reads");

    for (i = 0; i < sizeof(st); i++) st[i] = 0;
    if (sc2(SYS_stat, "/dev/sda", st) != 0) {
        out("        no /dev/sda - skipping (is a second ATA disk attached?)\n");
        return;
    }

    /* Everything under /dev used to report S_IFCHR. dd picks a transfer size
     * from this and a partition tool refuses to touch a character device, so
     * the wrong answer here is one programs act on. */
    check_eq(*(u32 *)(st + 24) & S_IFMT, S_IFBLK,
             "/dev/sda reports S_IFBLK, not S_IFCHR");
    check(*(u64 *)(st + 48) > 0,
          "with a real st_size - zero makes dd stop immediately");
    check_eq(*(u64 *)(st + 56), 512, "and a 512-byte block size");

    /* The console still reports S_IFCHR. The point of adding S_IFBLK was to
     * tell two kinds of device apart, not to relabel all of them. */
    for (i = 0; i < sizeof(st); i++) st[i] = 0;
    if (sc2(SYS_stat, "/dev/console", st) == 0) {
        check_eq(*(u32 *)(st + 24) & S_IFMT, S_IFCHR,
                 "the console is still S_IFCHR");
    }

    fd = (int)sc4(SYS_openat, AT_FDCWD, "/dev/sda", O_RDONLY, 0);
    if (fd < 0) {
        check(0, "open /dev/sda");
        return;
    }
    rc = sc3(SYS_read, fd, buf, sizeof(buf));
    check_eq(rc, 512, "a raw sector reads");

    /* A disk HAS a position, which is most of what makes it different from
     * every other device here. -ESPIPE would be the wrong answer. */
    check_eq(sc3(SYS_lseek, fd, 0, SEEK_SET), 0, "and it is seekable");

    /* The unaligned read is the one worth checking: it goes through the
     * bounce buffer, and getting it wrong returns the right COUNT with the
     * wrong bytes, which no caller can detect. */
    check_eq(sc3(SYS_lseek, fd, 3, SEEK_SET), 3, "seek to an unaligned offset");
    check_eq(sc3(SYS_read, fd, buf, 10), 10, "an unaligned short read works");

    check(sc3(SYS_lseek, fd, 0, SEEK_END) > 0, "SEEK_END gives the disk size");
    sc1(SYS_close, fd);
}

/* --- CPU time ------------------------------------------------------------ */

static void test_cputime(void) {
    u64 c0[2], c1[2], m0[2], m1[2];
    volatile u64 spin;
    u64 i;
    i64 cpu_ms, wall_ms;

    section("per-process CPU accounting");

    /* These used to be refused with -EINVAL, because answering with wall time
     * would give a program timing itself a plausible number that is wrong
     * under load - and it would never think to check. The accounting exists
     * now. */
    check_eq(sc2(SYS_clock_gettime, CLOCK_PROCESS_CPUTIME_ID, c0), 0,
             "CLOCK_PROCESS_CPUTIME_ID is answered rather than refused");
    check_eq(sc2(SYS_clock_gettime, CLOCK_THREAD_CPUTIME_ID, c1), 0,
             "CLOCK_THREAD_CPUTIME_ID likewise");

    /* MONOTONIC is read across the SAME loop, and it is the whole point of
     * this test rather than a decoration.
     *
     * The previous version measured only CPU time, saw zero, and printed a
     * note blaming the 10ms tick resolution. That note assumed its own
     * conclusion: a zero here has two possible causes - a loop too short to
     * cross a tick, or a run_ticks counter that is not advancing - and
     * measuring one clock cannot tell them apart. It is the second cause that
     * matters, and the version that explains a zero away is precisely the one
     * that would never report it.
     *
     * With both clocks, the two are distinguishable in one run:
     *   wall large, cpu zero   -> the counter is broken
     *   wall also ~zero        -> the loop really is that fast
     * and the test says which. */
    sc2(SYS_clock_gettime, CLOCK_MONOTONIC, m0);
    sc2(SYS_clock_gettime, CLOCK_PROCESS_CPUTIME_ID, c0);

    spin = 0;
    for (i = 0; i < 200000000ULL; i++) {
        spin += i;
    }

    sc2(SYS_clock_gettime, CLOCK_PROCESS_CPUTIME_ID, c1);
    sc2(SYS_clock_gettime, CLOCK_MONOTONIC, m1);

    wall_ms = (i64)((m1[0] - m0[0]) * 1000 +
                    (m1[1] / 1000000) - (m0[1] / 1000000));
    cpu_ms  = (i64)((c1[0] - c0[0]) * 1000 +
                    (c1[1] / 1000000) - (c0[1] / 1000000));

    out("        wall ");
    out_i64(wall_ms);
    out("ms, cpu ");
    out_i64(cpu_ms);
    out("ms\n");

    check(cpu_ms >= 0, "CPU time does not go backwards");

    /* The loop has to be long enough for the question to mean anything. If it
     * is not, say so instead of drawing a conclusion from it. */
    if (wall_ms < 50) {
        out("        the loop finished in under 50ms, so this cannot\n");
        out("        distinguish a stuck counter from tick resolution.\n");
        out("        Raise the iteration count and re-run.\n");
    } else {
        /* Nothing else is competing for the CPU here, so a process that ran
         * for `wall` should have been charged essentially all of it.
         *
         * The floor was HALF, described as "generous", and that generosity is
         * exactly what hid a real bug for as long as it did: the counter sat
         * at 46-51% and the check flickered between pass and fail, which
         * reads as a flaky test rather than as a broken clock. It was broken.
         * clock_gettime was reading run_ticks, which kernel/proc/sched_ule.c
         * HALVES every two seconds as a scheduling heuristic, so the answer
         * depended on where the decay boundary fell inside the measurement.
         * process.h's cpu_ticks is the fix.
         *
         * Eighty per cent now. It measures 100% on an idle machine, and the
         * remaining margin is for a boot that happens to have a kernel thread
         * wake up mid-loop - not for an order-of-magnitude error, which is
         * what the old floor was quietly tolerating. A threshold set where
         * nothing can reach it is a threshold that never fails; one set just
         * below the real value is one that catches the next regression. */
        check(cpu_ms * 10 >= wall_ms * 8,
              "CPU time tracks wall time for a process with the CPU to itself");
        if (cpu_ms * 2 < wall_ms) {
            out("        the process ran for ");
            out_i64(wall_ms);
            out("ms and was charged ");
            out_i64(cpu_ms);
            out("ms.\n");
            out("        run_ticks is not advancing. sched_tick charges the\n");
            out("        CURRENT process every tick; check that it is reached\n");
            out("        on the IRQ 0 path and that proc_current() is not NULL\n");
            out("        there.\n");
        }
    }
}

/* ========================================================================
 * PART B - what nobody in ring 3 can check
 * ===================================================================== */

static void print_manual_queue(void) {
    out("\n");
    out("========================================================\n");
    out("PART B - checks that cannot be made from here\n");
    out("========================================================\n");
    out("\n");

    out("These are not failures. They are the things that were written\n");
    out("and never run, in the order they are cheapest to clear.\n");
    out("\n");

    out("[ ] kernel32.dll compiles and links.\n");
    out("      tools/build_user.sh, and watch for a LINK error rather than\n");
    out("      a compile one. -Os can synthesise calls to memcpy/memset with\n");
    out("      no CRT to satisfy them, which is why err.c defines both. That\n");
    out("      is a guess at a failure nobody could reproduce.\n");
    out("\n");

    out("[x] k32.exe runs - the loader at DEPENDENCY DEPTH TWO.\n");
    out("      k32.exe -> kernel32.dll -> ntdll.dll. This entry carried\n");
    out("      'inspection is not a run' for several passes. It has now\n");
    out("      been run, and inspection had in fact been wrong - see the\n");
    out("      two bug entries below. Expect all of:\n");
    out("        hello from a PE that imports only kernel32.dll\n");
    out("        GetCommandLineW: /bin/k32.exe\n");
    out("        GetCurrentDirectoryW: C:\\\n");
    out("        ReadFile through CreateFileW: Genesis\n");
    out("\n");

    out("[ ] hand.exe is the ASSEMBLED one, not the mkpe.py fallback.\n");
    out("      The two print different banners on purpose. 'a hand-written\n");
    out("      PE' is src/hand/hand.S; anything else is the generator, and\n");
    out("      you are debugging a different binary than you think.\n");
    out("\n");

    out("[ ] ash pipelines end to end:  echo hi | cat\n");
    out("      Carried as 'not yet confirmed' through several phases now.\n");
    out("\n");

    out("[x] systest still passes: 234 passed, 0 failed, run from the shell\n");
    out("      with kernel threads, condvars and the deferred taskqueue all\n");
    out("      in place. It could not be run at all while the data disk\n");
    out("      halted the boot, which is why the entry below about that is\n");
    out("      the one that unblocked this one.\n");
    out("      Driving the shell without a keyboard: QEMU's monitor can type\n");
    out("      for you - -display none -monitor stdio, then feed it\n");
    out("      'sendkey slash', 'sendkey b', ... 'sendkey ret'. The kernel\n");
    out("      mirrors output to COM1 but reads input from the emulated PS/2\n");
    out("      controller, so serial alone leaves the shell unreachable.\n");
    out("\n");

    out("[x] The host tests:  tests/host/run.sh - RUN, and the C half now\n");
    out("      passes end to end ('all checks passed').\n");
    out("      Getting there meant repairing the harness, and both repairs\n");
    out("      are the SAME BUG in two spellings - a rewrite that silently\n");
    out("      does not apply:\n");
    out("        - the cp line still named kernel/*.c, from before the tree\n");
    out("          was split into mm/ dev/ fs/ proc/ obj/ exec/ arch/. All\n");
    out("          twenty-two failed, the script did not stop, and the suite\n");
    out("          reported on a subset without saying which.\n");
    out("        - the seds spelled out VALUES (KERNEL_MAP_SIZE 0x00400000)\n");
    out("          which the kernel had since changed. sed does not fail on\n");
    out("          a pattern that does not match, so the direct map was\n");
    out("          built into a window a quarter the size it needed and\n");
    out("          three failures were reported against paging.c that had\n");
    out("          nothing to do with paging.c. They are anchored on the\n");
    out("          macro NAME now, with a grep after each that fails loudly.\n");
    out("      tests/host/kernel_stub.c is new: smp_tlb_shootdown,\n");
    out("      kvm_alloc_range and the two nt_* resolvers, all answering the\n");
    out("      way a machine without that facility answers rather than\n");
    out("      simulating one.\n");
    out("\n");

    out("[ ] tests/host/check_zfs_boundary.py - 287 failures, ALL PRE-\n");
    out("      EXISTING and none of them about kernel/zfs/. It flags every\n");
    out("      kernel/bsd/*.c for including <sys/param.h> and every\n");
    out("      kernel/include/linux/*.h for including <linux/types.h> -\n");
    out("      which is what vendored FreeBSD and LinuxKPI code IS. The\n");
    out("      check was written when kernel/zfs/ was the only vendored\n");
    out("      tree and its rule 3 has not been told about the other two.\n");
    out("      Either scope it back to kernel/zfs/ or give the other trees\n");
    out("      their own whitelists; leaving it at 287 means the run always\n");
    out("      ends in FAILURES and nobody reads it, which is the same as\n");
    out("      not having it.\n");
    out("\n");

    out("[ ] poll(2) on the CONSOLE, by hand. Nothing here can type.\n");
    out("      The predicate is kbd_has_line(), not kbd_has_input(): the\n");
    out("      terminal is readable when the ring holds a COMPLETE line, and\n");
    out("      reporting readable on a partial one is the exact failure poll\n");
    out("      exists to prevent - poll says ready, read blocks anyway.\n");
    out("      To check it: a program that polls fd 0 with a 5s timeout,\n");
    out("      then type a few characters WITHOUT pressing Enter. It must\n");
    out("      time out. Press Enter and it must return immediately.\n");
    out("      Ctrl-C must also make it return, because -EINTR is a return.\n");
    out("\n");

    out("[ ] mhello and busybox still run, now that the stack, the break\n");
    out("      and every anonymous mapping are PAGE_NX and the ELF loader\n");
    out("      maps a page executable only if an executable segment covers\n");
    out("      it. This is the change most likely to break something that\n");
    out("      worked yesterday, and it breaks it as a page fault at a\n");
    out("      valid address - which reads like memory corruption, not like\n");
    out("      a permission change. If mhello faults on a fetch, suspect\n");
    out("      the per-page union in elf_load_into first.\n");
    out("\n");

    out("[ ] A REAL pthreads program. Nothing here can test the thread\n");
    out("      shape itself - a hand-rolled clone would test this program's\n");
    out("      idea of a thread rather than musl's. Build a musl binary that\n");
    out("      does pthread_create / pthread_join / pthread_mutex_lock and\n");
    out("      run it. In order, what it proves:\n");
    out("        - clone with the thread flags returns a tid in the parent\n");
    out("          and 0 in the child, on the child's own stack\n");
    out("        - CLONE_SETTLS worked, or every __thread access in the new\n");
    out("          thread reads another thread's TLS\n");
    out("        - pthread_join returns, which means CLONE_CHILD_CLEARTID\n");
    out("          was recorded AND the exit path cleared the word and\n");
    out("          futex-woke it. If join hangs, look there first\n");
    out("        - getpid() is the SAME in both threads and gettid() differs\n");
    out("        - returning from main ends the whole program, not one\n");
    out("          thread (that is the exit vs exit_group split)\n");
    out("\n");

    out("[ ] MAX_PROCESSES is 16, and a thread now occupies a slot.\n");
    out("      A four-thread program plus a shell plus init is most of the\n");
    out("      table. Exhaustion arrives as clone returning -EAGAIN, which\n");
    out("      musl reports as EAGAIN from pthread_create - correct, but\n");
    out("      surprising at four threads. Raising it is a constant; the\n");
    out("      kernel stacks it implies are the real cost.\n");
    out("\n");

    out("[ ] The in-kernel trace now COMPILES. It did not before - a block\n");
    out("      of the dispatch switch had been spliced into the middle of\n");
    out("      syscall_name(), inside the #if, so nothing ever built it.\n");
    out("      Set SYSCALL_TRACE to 1 in syscall.c and confirm it builds and\n");
    out("      prints, while it is fresh. A debug tool you only reach for\n");
    out("      when something is already wrong is the worst place to keep a\n");
    out("      build error.\n");
    out("\n");

    out("[ ] systest's filesystem section, unchanged, still passes.\n");
    out("      That is the real test of the filesystem vtable: it was\n");
    out("      supposed to change NOTHING a program can see. If any file\n");
    out("      test behaves differently, the abstraction moved behaviour\n");
    out("      rather than hiding it, and the ext4 work would inherit the\n");
    out("      difference.\n");
    out("\n");

    out("[ ] The boot line from disk_register. It prints each disk, its\n");
    out("      sector count, and either a partition count or '(no partition\n");
    out("      table)'. The current image has no MBR, so '(no partition\n");
    out("      table)' is the CORRECT output - it is said out loud rather\n");
    out("      than left blank precisely so a silent absence does not read\n");
    out("      like a failed parse.\n");
    out("\n");

    out("[ ] Writing to /dev/sda, which this program deliberately does NOT\n");
    out("      test - it would corrupt the disk it is running from. The\n");
    out("      read-modify-write path for a partial sector is the risky\n");
    out("      part: skipping the read would zero the rest of the sector,\n");
    out("      destroying data the caller never addressed. Test it on a\n");
    out("      scratch image, not on the boot disk.\n");
    out("\n");

    /* The BUILD item for part_crc32 and the DESTRUCTIVE one for a real GPT
     * disk were both here and are both gone: they are host tests now
     * (tests/host/part_test.c, fixtures from tests/host/mkimg.py). Deleted
     * rather than left ticked, because this file is a queue and a checklist
     * line that duplicates a test that already runs is how the two drift.
     *
     * What they found, recorded here because the queue is also where a real
     * cost gets written down:
     *
     *   part_scan treated a 0xAA55 signature as proof of a partition table.
     *   Every FAT boot sector has one, so the data disk - a filesystem at
     *   sector 0 with no table - was parsed as partitioned, produced zero
     *   volumes, and the kernel booted with nothing mounted. Silent at every
     *   layer: the scan succeeded, it just found nothing.
     *
     *   The GPT path had never executed at all. It does now, against a
     *   generated GPT, a hybrid MBR, and one with the primary header zeroed
     *   to force the backup - and the checksums in those fixtures are
     *   computed by Python's zlib rather than by the code under test, which
     *   is the only arrangement in which a CRC bug is visible.
     */

    out("[ ] JUDGEMENT - 8.3 is a constraint on every staged NAME, not just\n");
    out("      on the FAT driver. The rtld was first staged following the\n");
    out("      musl convention, ld-genesis-x86_64.so.1, which fatfs.py\n");
    out("      refuses - seventeen-character stem, two dots. It is\n");
    out("      /lib/ld-gen.so now, and tools/check_root.py runs at the end of\n");
    out("      build_user.sh so the next one fails at the line that named the\n");
    out("      file rather than three tools later at image build.\n");
    out("      That check also found the /wsr System32-vs-system32 collision\n");
    out("      still present in the staged root - the lowercase directory was\n");
    out("      empty debris from the first time it happened.\n");
    out("\n");

    out("[ ] BOOT - a GPT disk as the ROOT volume.\n");
    out("      part_scan is host-tested against real GPT layouts now, but\n");
    out("      no machine here boots from one: every disk in the build takes\n");
    out("      the PART_TABLE_NONE path. What is still unproven is the whole\n");
    out("      chain - scan, probe, letter, mount, execute init - on a disk\n");
    out("      whose root came from a GPT entry rather than from sector 0.\n");
    out("\n");

    out("[ ] BOOT - PCI enumeration matches QEMU's default topology.\n");
    out("      pci_report's raw scan and pci_generic's attach-time print\n");
    out("      (kernel/pci.c, kernel/bus.c, kernel/pci_generic.c) both run\n");
    out("      at boot with no ring-3 equivalent to check them against.\n");
    out("      What to look for: pci_report lists at least the i440FX host\n");
    out("      bridge (00:00.0), the PIIX3 ISA bridge (00:01.0), and the\n");
    out("      PIIX3 IDE controller (00:01.1) - vendor 0x8086 throughout on\n");
    out("      QEMU's default machine - and pci_generic's own print lists\n");
    out("      the SAME functions, proving bus_attach_children's probe/\n");
    out("      attach loop found what the raw scan found rather than\n");
    out("      silently matching fewer (or more) devices.\n");
    out("\n");

    out("[ ] BOOT - all four driver models (Newbus native, Newbus\n");
    out("      source-compat, Linux-shaped, WDM-shaped) attach against\n");
    out("      the same PCI topology.\n");
    out("      kernel/pci_generic.c, kernel/newbus_compat_demo.c,\n");
    out("      kernel/lkpi_demo.c, and kernel/wdm_demo.c are all\n");
    out("      registered on the \"pci\" devclass before the one\n");
    out("      bus_attach_children(pci_root()) call in flk.c - no ring-3\n");
    out("      equivalent to check them against, same as the PCI-\n");
    out("      enumeration entry above. What to look for:\n");
    out("      newbus_compat_demo's \"newbus_compat_demo: matched\n");
    out("      8086:7010\" line (the emulated PIIX3 IDE controller,\n");
    out("      matched through the real DEVMETHOD/DRIVER_MODULE idiom -\n");
    out("      kernel/newbus_compat.c), lkpi_demo's \"lkpi_demo: matched\n");
    out("      8086:100e\" line (the emulated e1000, matched by exact\n");
    out("      vendor/device ID through linux_pci_register_driver -\n");
    out("      kernel/lkpi.c), wdm_demo's \"wdm_demo: matched 1234:1111\"\n");
    out("      line (the emulated std VGA, matched through\n");
    out("      IoCreateDriver/AddDevice - kernel/wdm.c), and pci_generic's\n");
    out("      own print for every OTHER function pci_report listed\n");
    out("      (host bridge, ISA, ACPI) - proving the priority-ranked\n");
    out("      catch-all still wins whatever the other three didn't\n");
    out("      specifically claim, rather than losing the devclass race\n");
    out("      to whichever driver ran last.\n");
    out("\n");

    out("[ ] BOOT - a real kernel-mode .sys loads and runs.\n");
    out("      /wsr/Windows/System32/Drivers/test.sys (built by\n");
    out("      src/mkpe.py --sys, staged onto disk.img by\n");
    out("      python3 build.py disk) is loaded by kernel/sysload.c's\n");
    out("      sys_load_driver, called from flk.c right after\n");
    out("      storage_init - no ring-3 equivalent, same posture as\n");
    out("      every other BOOT entry here. What to look for: both\n");
    out("      \"test.sys: DriverEntry running\" and \"test.sys:\n");
    out("      IoCreateDevice returned\" lines - the first proves\n");
    out("      kernel/pe.c's new pe_load_driver correctly mapped and\n");
    out("      relocated a real IMAGE_FILE_DLL + IMAGE_SUBSYSTEM_NATIVE\n");
    out("      PE image into kernel address space (PE_DRIVER_BASE,\n");
    out("      pe.h) and resolved its first import (DbgPrint) against\n");
    out("      the synthetic ntoskrnl.exe table (kernel/\n");
    out("      ntoskrnl_exports.c); the second proves a SECOND, distinct\n");
    out("      import (IoCreateDevice) also resolved and was called\n");
    out("      through a real IAT slot using the real Win64 calling\n");
    out("      convention (WDM_ABI, kernel/include/wdm.h) without\n");
    out("      crashing - not just that the first call got lucky. Both\n");
    out("      lines appearing, followed by the rest of boot completing\n");
    out("      unchanged (bcache report, \"Interrupts enabled\", the\n");
    out("      BusyBox shell), is the whole proof.\n");
    out("\n");

    out("[ ] BOOT - a kernel-mode fault prints real symbol names and a\n");
    out("      backtrace, not a bare RIP. ROADMAP item 11's kernel\n");
    out("      self-symbol-table gap: build.py's gen_ksyms() now patches\n");
    out("      kernel.elf's real nm -n output straight into kernel.bin's\n");
    out("      reserved .ksyms bytes (linker.ld, kernel/ksyms_data.c) -\n");
    out("      no second link - and kernel/ksyms.c reads it back at\n");
    out("      runtime. To check: force a kernel-mode page fault (a null\n");
    out("      write from a few real stack frames deep is enough - this\n");
    out("      was verified during development by a temporary call chain\n");
    out("      in flk.c, removed afterward) and confirm the fatal-\n");
    out("      exception path in kernel/interrupt.c prints \"#0\" through\n");
    out("      \"#N\"\n");
    out("      lines through kernel/backtrace.c, each with a real\n");
    out("      function name and byte offset (e.g. \"flk+5a9\"), not just\n");
    out("      the \"RIP 0x...\" line that used to be the entire report.\n");
    out("\n");

    out("[ ] BOOT - the mbuf subsystem is FreeBSD's, not a lookalike.\n");
    out("      ROADMAP item 6 calls mbuf \"a second subsystem, not a\n");
    out("      header\", and item 11's rule for this pass was to vendor\n");
    out("      rather than hand-roll. kernel/bsd/compat/sys/mbuf.h is\n");
    out("      byte-for-byte vendsrc/sys/sys/mbuf.h - verify with md5sum\n");
    out("      against vendsrc, which is the check that catches this\n");
    out("      quietly drifting into a reimplementation - and the\n");
    out("      allocation core, the tag code and the chain routines are\n");
    out("      function-for-function copies in kernel/bsd/vendor/.\n");
    out("      What is Genesis's is the seam: kernel/bsd/uma.c (a slab\n");
    out("      allocator standing in for vm/uma_core.c) and the shim\n");
    out("      headers in kernel/bsd/compat/. That seam is PROVISIONAL -\n");
    out("      the plan's Part 12 vendors the real 6042-line uma_core.c\n");
    out("      and deletes uma.c, gated on per-CPU data and real mutexes\n");
    out("      because uma_core.c's fast path and locking need both. When\n");
    out("      that lands, THIS CHECK IS THE ACCEPTANCE TEST: the mbuf\n");
    out("      selftest below must keep passing with no edit to it.\n");
    out("      To check at boot: the log must show\n");
    out("        mbuf: FreeBSD mbuf(9) up - MSIZE 256, MLEN 224,\n");
    out("        MHLEN 160, MCLBYTES 2048\n");
    out("      Those four numbers are the evidence, not decoration: 224\n");
    out("      and 160 are MSIZE minus the real offsetof(struct mbuf,\n");
    out("      m_dat) and m_pktdat. A struct that had been retyped or\n");
    out("      repacked by a wrong compat header prints different ones,\n");
    out("      and kernel/bsd/mbuf.c's CTASSERT block (upstream's own,\n");
    out("      from uipc_mbuf.c) fails the build before it gets that far.\n");
    out("      Then \"mbuf: selftest passed\" - net_mbuf_selftest() in\n");
    out("      kernel/bsd/mbuf.c, which round-trips m_get, m_gethdr,\n");
    out("      m_getcl (the secondary zone, so mb_zinit_pack really\n");
    out("      attached a cluster), m_copydata, m_prepend, m_length and\n");
    out("      free-list reuse. It is a BOOT check because there is no\n");
    out("      socket layer yet, by design - nothing in ring 3 can reach\n");
    out("      an mbuf, so boot is the only place this runs at all.\n");
    out("\n");

    out("[ ] BOOT - bus.c's five Newbus gaps, closed. ROADMAP item 11\n");
    out("      named them: no KOBJ, no bus_alloc_resource, no multi-pass\n");
    out("      boot ordering, no devclass inheritance, and a BUS_PROBE_*\n");
    out("      scale narrower than upstream's.\n");
    out("      At boot the log must show, in order:\n");
    out("        bus selftest: the next line is expected - a deliberate\n");
    out("        overlapping allocation\n");
    out("        bus: resource conflict - type 3 [...] wanted by ...\n");
    out("        bus: selftest passed\n");
    out("      The middle line is RED and is supposed to be. It is the\n");
    out("      gap item 11 complains about in as many words - \"a driver\n");
    out("      reads pci_bar_size straight off the raw pci_ivars_t with\n");
    out("      no bus-mediated conflict detection at all\" - being caught.\n");
    out("      A boot WITHOUT that line is the failure, not a boot with\n");
    out("      it.\n");
    out("      kernel/bus_selftest.c covers all five: pass ordering (the\n");
    out("      late-pass driver has the BETTER probe priority and is\n");
    out("      registered FIRST, so only a working pass gate makes the\n");
    out("      early-pass one win), devclass inheritance including cycle\n");
    out("      and self-link refusal, overlapping vs ADJACENT allocation\n");
    out("      (a checker that refuses the adjacent case is as broken as\n");
    out("      one that accepts the overlap), release-then-reallocate,\n");
    out("      refusing to release another device's resource, and custom\n");
    out("      KOBJ-shaped method dispatch through a desc token bus.c and\n");
    out("      newbus_compat.c have never heard of.\n");
    out("      Then the REGRESSION, which matters as much: all four\n");
    out("      driver models must still match exactly what they matched\n");
    out("      before - pci_generic on 1237/7000/7113, newbus_compat_demo\n");
    out("      on 8086:7010, wdm_demo on 1234:1111, lkpi_demo on\n");
    out("      8086:100e. Plus two new lines proving the real API works\n");
    out("      end to end on real hardware rather than only in the\n");
    out("      selftest's synthetic devices:\n");
    out("        newbus_compat_demo: BAR4 io c040 size 10\n");
    out("        bus resources:\n");
    out("          io rid 32  c040 + 10  newbus_compat_demo\n");
    out("      That is FreeBSD driver source calling bus_alloc_resource_\n");
    out("      any with PCIR_BAR(4) and reading it back with\n");
    out("      rman_get_start, resolved through pci_bar_size and recorded\n");
    out("      by the bus. rid 32 is 0x20, which is PCIR_BAR(4) - bus.c\n");
    out("      accepts both that config-offset form and a bare 0-5 index.\n");
    out("\n");

    out("[ ] BOOT - BUS_PROBE_NOWILDCARD is FULLY implemented now. This\n");
    out("      entry was a JUDGEMENT recording a half-done feature; Part 16\n");
    out("      finished it and this is the real check.\n");
    out("      Part 4 got the NUMERIC half right - such a driver ranks\n");
    out("      below everything, including BUS_PROBE_HOOVER. Upstream means\n");
    out("      more: subr_bus.c's device_probe_child skips any driver\n");
    out("      returning <= BUS_PROBE_NOWILDCARD UNLESS the device\n");
    out("      `hasclass` - unless something explicitly NAMED which driver\n");
    out("      this device is, rather than it being matched by wildcard\n");
    out("      probing. That test needs device identity and a config\n");
    out("      source, neither of which existed until Part 15, which is\n");
    out("      why this was scheduled rather than faked.\n");
    out("      It is covered by bus_selftest, so:\n");
    out("        bus: selftest passed\n");
    out("      is the whole evidence. What it checks is BOTH halves, and\n");
    out("      that matters: a test that only checked the refusal cannot\n");
    out("      tell correct gating from a driver that never attaches at\n");
    out("      all. The same driver is run against the same kind of device\n");
    out("      twice, differing ONLY in whether it was named - it must lose\n");
    out("      the first and win the second. Confirmed by deleting the gate\n");
    out("      and watching both halves fail:\n");
    out("        bus selftest: a NOWILDCARD driver attached to a device\n");
    out("        nobody named\n");
    out("        bus selftest: a NOWILDCARD driver did not attach to a\n");
    out("        device that WAS named\n");
    out("      Note the direction, which is easy to get backwards: this\n");
    out("      does NOT mean \"only try NOWILDCARD drivers on named\n");
    out("      devices\". The driver's probe still RUNS on an unnamed\n");
    out("      device - and returns success, enthusiastically. What is\n");
    out("      refused is letting it win. A test whose probe answered \"not\n");
    out("      mine\" would pass for the wrong reason.\n");
    out("      Upstream's users of this are drivers that would probe\n");
    out("      DESTRUCTIVELY if auto-attached, which is why the mechanism\n");
    out("      exists and why half-implementing it was worth recording.\n");
    out("\n");

    out("[ ] BOOT - callout(9): vendored API, hand-written engine, and\n");
    out("      the split is the thing to check. kernel/bsd/compat/sys/\n");
    out("      callout.h and _callout.h are byte-for-byte vendsrc (md5sum\n");
    out("      them), so struct callout has FreeBSD's layout and\n");
    out("      callout_reset/stop/drain/pending are FreeBSD's macros. The\n");
    out("      wheel behind them is kernel/bsd/callout.c, Genesis's,\n");
    out("      because vendsrc/sys/kern/kern_timeout.c needs per-CPU state,\n");
    out("      softclock kernel threads, sleepqueue(9) and mtx(9) - Parts\n");
    out("      10 and 11 of this pass. When those land, kern_timeout.c\n");
    out("      becomes vendorable and this file is what it replaces, which\n");
    out("      is why every signature and return value matches upstream\n");
    out("      rather than a convenient subset.\n");
    out("      To check at boot, two lines:\n");
    out("        callout: wheel up - 512 buckets, 100 Hz, tick 9999 us,\n");
    out("        wheel spans 4000 ms\n");
    out("      9999 and not 10000 is not a rounding bug in the print: it\n");
    out("      is tick_sbt itself, SBT_1S / hz truncated exactly the way\n");
    out("      upstream computes it. Matching upstream's arithmetic was\n");
    out("      the choice over a rounder number.\n");
    out("        callout: selftest passed\n");
    out("      The selftest is net_callout_selftest() and it runs AFTER\n");
    out("      sti on purpose - it spins on real timer ticks rather than\n");
    out("      calling callout_process() by hand, so it tests the wiring\n");
    out("      through timer.c's tick handler and not just the sweep. What\n");
    out("      it covers: deadline ORDER (the later callout is scheduled\n");
    out("      first, so insertion order cannot be what decides), a\n");
    out("      600-second timeout that must NOT fire - it hashes into a\n");
    out("      bucket the sweep visits every four seconds, which is the\n");
    out("      classic hand-rolled-wheel bug - callout_stop's return value\n");
    out("      in both directions, and a handler that reschedules its own\n");
    out("      callout, which breaks if the sweep runs handlers while\n");
    out("      still walking the bucket they re-insert into.\n");
    out("\n");

    out("[ ] BOOT - the Local APIC is up, and Part 5 needed it before\n");
    out("      Part 10 did. The plan said Part 5 'depends on nothing else\n");
    out("      here'. That was wrong, and the work corrected it rather than\n");
    out("      the text: an MSI IS a memory write to 0xFEE00000 that the\n");
    out("      LAPIC decodes, so there is no MSI on an 8259 alone. What\n");
    out("      Part 5 brings up is the RECEIVE half - enable, SVR, TPR,\n");
    out("      EOI, self-IPI. Part 10 adds the send/AP-startup half on top,\n");
    out("      additively; see kernel/include/lapic.h. One line:\n");
    out("        lapic: id 0  version 14  max lvt 5  at ffffffff86100000\n");
    out("      The address is the MMIO base mapped PCD|PWT. It is NOT the\n");
    out("      MSI message address, which is a separate architectural\n");
    out("      window - lapic_msi_address() writes 0xFEE00000 out longhand\n");
    out("      for exactly that reason, even though the two numbers happen\n");
    out("      to coincide on a default configuration.\n");
    out("\n");

    out("[ ] BOOT - dynamic IDT vectors 48-254, allocated and really\n");
    out("      delivered. idt.c hard-wired 48 gates (0-31 exceptions,\n");
    out("      32-47 the remapped PIC) with no machinery for the other 208;\n");
    out("      all 256 are now filled from a generated stub table. One\n");
    out("      line:\n");
    out("        idt: selftest passed\n");
    out("      The delivery check is a LAPIC SELF-IPI, not a direct call\n");
    out("      and not int $N. A direct call would test a function pointer;\n");
    out("      int $N tests the gate but its operand must be a compile-time\n");
    out("      constant, so it cannot use whatever vector the allocator\n");
    out("      actually returned. The self-IPI walks allocator -> gate ->\n");
    out("      stub -> interrupt_dispatch's >47 branch -> handler -> EOI,\n");
    out("      which is an MSI's path minus the device write. It also\n");
    out("      checks allocation is TOP-DOWN (first vector out must be\n");
    out("      254): direction decides LAPIC priority class, and a test\n");
    out("      that only asserted 'in range' would not notice it flip.\n");
    out("      Expect this immediately after, and expect the 1:\n");
    out("        interrupts: spurious 0x0  unhandled dynamic vectors 0x1\n");
    out("      That 1 is deliberate - the selftest frees a vector and then\n");
    out("      self-IPIs it to prove a freed vector stops dispatching. The\n");
    out("      boot log says so on the preceding line rather than leaving a\n");
    out("      non-zero counter to be read as a defect.\n");
    out("\n");

    out("[ ] BOOT - PCI bridge recursion and MSI, both of which are\n");
    out("      correct-and-untaken on QEMU's default machine. This is the\n");
    out("      entry to read carefully, because the default boot log shows\n");
    out("      neither feature doing anything:\n");
    out("        PCI MSI-capable functions: none\n");
    out("        pci msi selftest: no MSI-capable function - programming\n");
    out("        not exercised (try -device ich9-ahci)\n");
    out("      i440fx exposes no P2P bridge, and QEMU's e1000 (82540em)\n");
    out("      genuinely implements no MSI capability - so 'none' there is\n");
    out("      a true negative, not a broken capability walker. To actually\n");
    out("      take both paths:\n");
    out("        qemu-system-x86_64 ... -device pci-bridge,chassis_nr=1,\n");
    out("        id=br0 -device vmxnet3,bus=br0,addr=0x1 -device ich9-ahci\n");
    out("      Then the recursion shows up as a device on a bus that is not\n");
    out("      bus 0 - read the FIRST column:\n");
    out("        0:4.0  1b36:1    class 06.04  bridge\n");
    out("        1:1.0  15ad:7b0  class 02.00  network\n");
    out("      and three functions appear under MSI-capable, one of them\n");
    out("      (1:1.0) found through the bridge - so the capability walk\n");
    out("      works on a recursed bus too, which scanning bus 0 alone\n");
    out("      could never show. Then:\n");
    out("        pci msi: selftest passed - 1b36:1 programmed on vector 254\n");
    out("        and released\n");
    out("      What that proves is a config-space ROUND TRIP: every\n");
    out("      register is read back OFF THE DEVICE and compared with what\n");
    out("      was written - address lo, address hi on the 64-bit layout,\n");
    out("      the full 16-bit data register (bits 15:8 must be zero, ie\n");
    out("      fixed/edge), the enable bit, MME granting exactly one\n");
    out("      vector, and the command register's INTx-disable. Release is\n");
    out("      checked as its own half, because a release that frees the\n");
    out("      vector but leaves the device enabled is worse than none.\n");
    out("      What it does NOT prove is that an MSI ever arrives. Nothing\n");
    out("      here programs a device into raising one - that needs a real\n");
    out("      driver, which is item 12. The delivery chain is covered by\n");
    out("      the self-IPI above; the untested step is strictly the device\n");
    out("      writing the message. Do not read 'selftest passed' as more.\n");
    out("\n");

    out("[ ] BOOT - W^X is enforced on the ELF side now, both halves.\n");
    out("      elf.c used to map every PT_LOAD page PAGE_RW and leave it\n");
    out("      that way even after clearing NX on the executable ones, so\n");
    out("      .text was writable and the loader's own comment said so.\n");
    out("      The check is a systest case, and it is a POSITIVE one:\n");
    out("        W^X on the ELF side\n");
    out("          ok    wait4 reaps the child that wrote to .text\n");
    out("          ok    and it was KILLED - .text is not writable\n");
    out("      It forks, writes one byte to the address of a function, and\n");
    out("      requires the child to DIE. Forked because interrupt.c kills\n");
    out("      a faulting process rather than delivering a catchable\n");
    out("      SIGSEGV - it explains why: a signal arrives at a syscall\n");
    out("      boundary and a faulting instruction never reaches one. So\n");
    out("      the parent reads 128+SIGSEGV=139 out of wait4. Expect the\n");
    out("      kernel to print the fault as it happens; that line is the\n");
    out("      test working, not a defect:\n");
    out("        cause protection violation, write, user\n");
    out("      'protection violation' and not 'page not present' is the\n");
    out("      whole point - the page IS mapped, and the write is what was\n");
    out("      refused.\n");
    out("      Why a positive test and not just a clean boot: every other\n");
    out("      piece of evidence here is of the form 'nothing broke', and a\n");
    out("      loader that quietly went on mapping .text writable would\n");
    out("      produce exactly that too. This one was confirmed to FAIL\n");
    out("      (got 77, wanted 139) against the old always-writable\n");
    out("      mapping, so it discriminates rather than merely passing.\n");
    out("      The regression half was run as an A/B: the full boot log\n");
    out("      plus busybox ash, /bin/hello, /bin/ls, /bin/mhello and\n");
    out("      systest were compared old-vs-new and came out identical\n");
    out("      line for line. Note in passing that busybox applets resolve\n");
    out("      by absolute path only - 'echo: not found' appears the same\n");
    out("      number of times either way. That is the stat()/PATH gap\n");
    out("      flk.c already documents, not this change.\n");
    out("      One implementation note worth knowing before editing that\n");
    out("      loop: both permission bits are COMPUTED per page as a union\n");
    out("      over every segment covering it, rather than built up\n");
    out("      incrementally. Segments share boundary pages - ld only\n");
    out("      aligns to p_align - and the two unions run in OPPOSITE\n");
    out("      directions: the X half must end up set if any segment says\n");
    out("      X, the W half must end up clear unless some segment says W.\n");
    out("      Incremental passes would have to run in an order neither\n");
    out("      states, and vmm_map_page_in writes flags absolutely, so it\n");
    out("      cannot preserve the bit the other pass just decided.\n");
    out("\n");

    out("[ ] BOOT - PE ordinal imports and forwarder exports, both real.\n");
    out("      pe.c used to refuse an ordinal import outright and return 0\n");
    out("      for any export whose RVA landed inside the export directory.\n");
    out("      Both now resolve. Run /bin/k32.exe and expect:\n");
    out("        K32OrdinalProbe (imported by ordinal 42) = 0xC0DE0042 -\n");
    out("        correct\n");
    out("        K32CurrentTeb (forwarded to ntdll.NtCurrentTeb) =\n");
    out("        non-NULL - followed\n");
    out("      The test material is in src/kernel32/kernel32.def. Two\n");
    out("      details there are the actual test and are easy to undo by\n");
    out("      accident:\n");
    out("      1. K32OrdinalProbe is NONAME, so it appears in NO name\n");
    out("         table - a loader that 'resolves' an ordinal by falling\n");
    out("         back to a name lookup cannot find it at all.\n");
    out("      2. Numbering it @42 pushes kernel32's ORDINAL BASE off 1\n");
    out("         (it is 19). An ordinal import is\n");
    out("         AddressOfFunctions[ordinal - ordinal_base], and every\n");
    out("         DLL with a base of 1 hides a missing subtraction. Check\n");
    out("         with: x86_64-w64-mingw32-objdump -p kernel32.dll | grep\n");
    out("         'Ordinal Base'. If that reads 1 again, the test has\n");
    out("         stopped testing the thing it was written for.\n");
    out("      Note the two ordinal-ish numbers in the export directory are\n");
    out("      NOT the same kind: AddressOfNameOrdinals holds an index that\n");
    out("      is ALREADY biased, and an import thunk holds a true ordinal\n");
    out("      that is not. find_export_ex uses one directly and subtracts\n");
    out("      from the other, deliberately.\n");
    out("      K32CurrentTeb has no implementation in kernel32 at all - its\n");
    out("      export entry is the string 'ntdll.NtCurrentTeb'. A non-NULL\n");
    out("      TEB proves the string was parsed and resolved in a second\n");
    out("      DLL; an unfollowed forwarder would return a pointer into\n");
    out("      kernel32's .edata and calling it would not come back.\n");
    out("      Forwarder chains are depth-capped at 4 - the format does not\n");
    out("      forbid a cycle and this runs on the kernel stack.\n");
    out("\n");

    out("[ ] BOOT - two REAL BUGS this found, both pre-existing, both the\n");
    out("      kind that only appear when something is finally run.\n");
    out("      1. NtWriteFile truncated its own Length. Length is a ULONG,\n");
    out("         and nt_stack_arg returns the whole 64-bit slot it sits\n");
    out("         in; the Win64 ABI only requires a caller to write the low\n");
    out("         four bytes of an eight-byte stack home. k32.exe's first\n");
    out("         47-byte write arrived as 0x901175080000002f. The low half\n");
    out("         is correct, so the right 47 bytes came out and the write\n");
    out("         then continued for the rest of the count - printing the\n");
    out("         image until it ran off the last mapped page and faulted\n");
    out("         in console_write. The symptom read as string corruption\n");
    out("         precisely because the first line was perfect.\n");
    out("      2. execve did not clear clear_child_tid. It is a user\n");
    out("         address in the image being REPLACED, so syscall_exit_\n");
    out("         process later wrote a zero through it into the new address\n");
    out("         space. This had been silently corrupting four bytes on\n");
    out("         every ELF exec for as long as fork has existed: two static\n");
    out("         musl binaries map the same region at the same address, so\n");
    out("         the write always landed on a real page. It only faulted\n");
    out("         once the new image was a PE at 0x140000000, where the\n");
    out("         inherited address is unmapped. user_ptr_ok cannot catch\n");
    out("         this - the address is a legal user address, just not this\n");
    out("         process's any more.\n");
    out("      Both are worth keeping in mind as a class: a stale user\n");
    out("      address surviving an address-space swap fails loudly only\n");
    out("      when the two images have different layouts.\n");
    out("\n");

    out("[ ] BOOT - legacy IRQ lines are SHARED now, and the handler\n");
    out("      signature changed to make that mean something. Two lines:\n");
    out("        irq: selftest passed\n");
    out("        legacy IRQ lines: none registered\n");
    out("      The second is correct on the default machine and is not an\n");
    out("      empty result to skip past: the selftest registers, drives a\n");
    out("      dispatch by hand, and unregisters, so an EMPTY table\n");
    out("      afterwards is part of what passed. A line listed there with\n");
    out("      a non-zero unclaimed count would mean some device is raising\n");
    out("      interrupts nobody owns.\n");
    out("      irq_handler_fn returns int now instead of void - non-zero\n");
    out("      for 'my device raised this'. That is not bookkeeping. INTx\n");
    out("      is LEVEL-triggered, so an interrupt no handler acknowledges\n");
    out("      re-asserts the moment the PIC is EOI'd and livelocks the\n");
    out("      machine; the count is the only way to see it happening.\n");
    out("      Genesis does NOT mask a line for producing unclaimed\n");
    out("      interrupts - Linux does after 99900 in 100000, which needs a\n");
    out("      rate over real time, and masking on a raw count would kill a\n");
    out("      line for a device that is merely slow to attach.\n");
    out("      What the selftest actually discriminates, since a weaker\n");
    out("      version would pass against the old code too:\n");
    out("        - the SECOND register on a line must succeed. The old\n");
    out("          table refused it outright, so this alone catches a\n");
    out("          revert.\n");
    out("        - BOTH handlers must run on one dispatch, and the one\n");
    out("          registered FIRST is the one that claims it - so a\n");
    out("          dispatch that stopped at the first non-zero return\n");
    out("          would never reach the second. That looks like a\n");
    out("          reasonable optimisation and is wrong: two devices on\n");
    out("          one wire can assert together.\n");
    out("        - unregistration matches on (handler, ctx), not handler\n");
    out("          alone. kernel/lkpi.c registers ONE trampoline function\n");
    out("          for every Linux driver and distinguishes them purely by\n");
    out("          ctx, so matching on the function would tear down\n");
    out("          somebody else's device.\n");
    out("      It calls irq_dispatch directly rather than provoking a\n");
    out("      device, and that is a real gap rather than a shortcut:\n");
    out("      nothing in this tree drives a shared legacy line, so a real\n");
    out("      two-driver delivery is not testable until item 12 brings a\n");
    out("      driver. The chain below irq_dispatch is what is covered.\n");
    out("      IRQF_SHARED in linux/interrupt.h is now honoured rather than\n");
    out("      accepted-and-ignored, and that needed BOTH halves: irq.c\n");
    out("      chaining, and lkpi.c allocating one trampoline record per\n");
    out("      registration instead of indexing its table by IRQ number -\n");
    out("      which was itself a second one-driver-per-line limit sitting\n");
    out("      above the first.\n");
    out("\n");

    out("[ ] BOOT - WDM has real IRPs, a real device stack, and real\n");
    out("      completion. All three were absent: the IRP was a STACK-\n");
    out("      ALLOCATED local in wdm.c with one embedded location, there\n");
    out("      was no IoCallDriver at all, and IoCompleteRequest was an\n");
    out("      empty function. A filter driver - the entire reason WDM has\n");
    out("      this shape - could not be written against it. Two lines:\n");
    out("        wdm_demo: matched 1234:1111  stack depth 2\n");
    out("        wdm: selftest passed - IRP went filter -> function ->\n");
    out("        completion\n");
    out("      'stack depth 2' is AddDevice doing what a real one does:\n");
    out("      IoCreateDevice, then IoAttachDeviceToDeviceStack on top of\n");
    out("      the PDO, keeping the returned lower-device pointer in its\n");
    out("      own extension. There is no downward pointer in\n");
    out("      DEVICE_OBJECT to keep it in - NT has none either, and a\n");
    out("      driver storing it itself is the actual convention.\n");
    out("      The selftest builds TWO drivers, and that is the point: one\n");
    out("      driver over one device behaves identically whether\n");
    out("      IoCallDriver walks a stack or calls MajorFunction[]\n");
    out("      directly, which is precisely why the old code's absence of a\n");
    out("      stack went unnoticed. It checks the PATH as a string,\n");
    out("      \"Ffc\" - filter dispatch, function dispatch, filter\n");
    out("      completion - so it can tell 'both ran' from 'both ran in\n");
    out("      the right order'. A bare \"f\" is what the old direct call\n");
    out("      produced; \"Ff\" without the c is a stack that layers but\n");
    out("      never completes back up.\n");
    out("      Direction matters and is NT's, not a free choice:\n");
    out("      CurrentLocation counts DOWN toward the hardware, so\n");
    out("      IoSkipCurrentIrpStackLocation and IoCopyCurrentIrpStack-\n");
    out("      LocationToNext mean what a driver author expects. A fresh\n");
    out("      IRP sits one ABOVE the top, which is why a caller fills in\n");
    out("      IoGetNextIrpStackLocation and not the current one.\n");
    out("      Two traps worth knowing before editing wdm.c:\n");
    out("        - the completion routine at location N belongs to the\n");
    out("          driver at N+1, so IoCompleteRequest hands it the device\n");
    out("          from ABOVE. Passing the one that just completed looks\n");
    out("          right and stays right until two filters are stacked.\n");
    out("        - IoCopyCurrentIrpStackLocationToNext must CLEAR the\n");
    out("          copied completion fields, or one routine is registered\n");
    out("          on two locations and runs twice for one request.\n");
    out("      One test in that file was written wrong first and the\n");
    out("      correction is worth keeping: calling IoCallDriver twice on a\n");
    out("      one-location IRP does NOT test forwarding past the bottom,\n");
    out("      because IoCompleteRequest walks CurrentLocation back to the\n");
    out("      top and the IRP is then legitimately re-sendable. The real\n");
    out("      case needs a driver holding the bottom location and\n");
    out("      forwarding without completing.\n");
    out("      Still synchronous end to end: nothing returns STATUS_PENDING\n");
    out("      and no IRP outlives the call that made it. IoAllocateIrp\n");
    out("      comes from a fixed 16-entry pool rather than kmalloc, which\n");
    out("      is deliberate on an I/O path - an allocator that fails under\n");
    out("      memory pressure exactly when the system is trying to write\n");
    out("      memory out is the classic deadlock.\n");
    out("\n");

    out("[ ] BOOT - a SECOND CPU is really executing. Run with -smp 2\n");
    out("      (and -smp 1, which is the regression half). Expect:\n");
    out("        acpi: MADT lists 2 CPUs, lapic at fee00000, apic ids 0 1\n");
    out("        smp: 2 cpus online\n");
    out("          cpu0  apic id 0  bsp  tlb shootdowns 0\n");
    out("          cpu1  apic id 1  ap   tlb shootdowns 1\n");
    out("        smp: selftest passed - 2 cpus, APs answered TLB\n");
    out("        shootdown IPIs\n");
    out("      Read the AP's non-zero shootdown count on the REPORT line,\n");
    out("      which prints before the selftest: that one came from a real\n");
    out("      unmap during boot, not from the test. smp_tlb_shootdown is\n");
    out("      called from inside paging.c's invlpg, so every unmap in the\n");
    out("      kernel is cross-CPU correct rather than only the ones\n");
    out("      somebody remembered.\n");
    out("      What SMP is not, yet: APs do not run user processes. sched.c\n");
    out("      still has one run queue and one current process. The APs\n");
    out("      reach C, install their own GDT/TSS/IDT/LAPIC, and idle in\n");
    out("      hlt answering IPIs. That is the mechanism, and calling it\n");
    out("      'the kernel is SMP' would be wrong.\n");
    out("      Three bugs found bringing this up, all worth knowing:\n");
    out("        1. The trampoline was put in .lowtext, which STARTS at\n");
    out("           KERNEL_LMA where the boot sector jumps.\n");
    out("           kernel_ap_trampoline.o sorts before kernel_boot64.o, so\n");
    out("           it silently took over the entry point and the machine\n");
    out("           booted to NOTHING - no output, no fault, QEMU just\n");
    out("           exited. The blob is copied to 0x8000 at runtime and\n");
    out("           every address in it is assembled as AP_BASE + (label -\n");
    out("           start), so it needs no link address at all and lives in\n");
    out("           .text now.\n");
    out("        2. smp_this_cpu read gs:0x10. GS is not a reliable 'which\n");
    out("           CPU am I' on the BSP: the per-CPU block sits in\n");
    out("           KERNEL_GS_BASE until the first swapgs at syscall entry,\n");
    out("           so during boot-time selftests GS_BASE is still 0 and\n");
    out("           gs:0x10 reads address 0x10. Matched on the LAPIC ID\n");
    out("           instead. Same trap syscall_set_user_gs_base already\n");
    out("           documents, reached from the other side.\n");
    out("        3. The AP answered ZERO IPIs while looking perfectly\n");
    out("           healthy. Every CPU has its OWN LAPIC with its own SVR\n");
    out("           and TPR; the BSP software-enabling its LAPIC does\n");
    out("           nothing for the AP, and an AP with a disabled LAPIC\n");
    out("           accepts no interrupts at all. Nothing faults - the\n");
    out("           sending side is entirely correct. See lapic_init_ap.\n");
    out("      Also expect idt_selftest to still pass. It nearly did not:\n");
    out("      it asserted that the first dynamic vector handed out is 254\n");
    out("      and that zero are bound afterwards, both of which stopped\n");
    out("      being true the moment SMP took a permanent vector for\n");
    out("      shootdown IPIs. It measures against a baseline now. A test\n");
    out("      that fails because working code was added is worse than no\n");
    out("      test, because the obvious reading is that the new code\n");
    out("      broke something.\n");
    out("\n");

    out("[ ] BOOT - EFER.NXE is set on EVERY cpu, not just the BSP.\n");
    out("      ROADMAP listed this under Owed as 'second CPU needs its own\n");
    out("      write'. EFER is a per-CPU MSR, so the BSP's write does\n");
    out("      nothing for an AP, and an AP without NXE treats the NX bit\n");
    out("      in a PTE as RESERVED - it would fault on the first kernel\n");
    out("      page it touched, since paging.c sets NX on data mappings.\n");
    out("      It is set in the trampoline, in the SAME wrmsr as EFER.LME,\n");
    out("      rather than later in C: that leaves no window in which the\n");
    out("      CPU is in long mode without it. The evidence is that an AP\n");
    out("      reaches smp_ap_entry and answers IPIs at all - every kernel\n");
    out("      page it touches on the way there is NX-mapped, so a missing\n");
    out("      NXE is not a subtle degradation here, it is an immediate\n");
    out("      fault.\n");
    out("\n");

    out("[ ] BOOT - REAL locks, and the two latent bugs they exposed.\n");
    out("      mtx/rwlock are real spin locks now (kernel/mtx.c), and the\n");
    out("      four things ROADMAP named are retrofitted onto them:\n");
    out("      kheap's absent locking, wdm.c's KeAcquireSpinLock and IRQL,\n");
    out("      the callout wheel, and UMA's free list, plus bus.c's\n");
    out("      registration pools. Run with -smp 2 and expect:\n");
    out("        lock: 9c40 contended increments across 2 cpus, none lost\n");
    out("        lock: selftest passed\n");
    out("        locks: selftest mtx  acquisitions 9c44  contended aba\n");
    out("      Read the CONTENDED count, not just the total. Both CPUs\n");
    out("      hammer one non-atomic counter through the lock; the total\n");
    out("      coming out exact is the result, but it could come out exact\n");
    out("      by luck if the two never overlapped. A non-zero contended\n");
    out("      count is what says they did. The selftest says so itself if\n");
    out("      contention was zero, rather than printing a green line that\n");
    out("      means less than it looks.\n");
    out("      On -smp 1 it prints that contention was NOT tested, and\n");
    out("      that is the honest state: with one CPU and interrupts off,\n");
    out("      an EMPTY lock implementation passes every single-threaded\n");
    out("      test that can be written. That is the whole reason this was\n");
    out("      not built before Part 10.\n");
    out("      The single-CPU half still earns its place - it catches the\n");
    out("      API mistakes a contention test cannot see. Chiefly: unlock\n");
    out("      must RESTORE the caller's interrupt state, not sti. Checked\n");
    out("      in both directions, and separately on the FAILED-trylock\n");
    out("      path, which is a different branch and the one that gets\n");
    out("      forgotten - a lock that leaks interrupts-disabled only on\n");
    out("      contention looks perfect until it is contended.\n");
    out("      TWO REAL BUGS, both pre-existing, both invisible until a\n");
    out("      real lock existed. Both were self-deadlocks that the old\n");
    out("      cli/sti lock tolerated because nested cli costs nothing:\n");
    out("        1. kh_calloc_locked called the PUBLIC kmalloc, which\n");
    out("           re-takes the heap lock - while kheap.c's own comment\n");
    out("           beside kmalloc claimed the lock \"is acquired exactly\n");
    out("           once, at the boundary\". kh_realloc_locked did the\n");
    out("           same. The invariant was written down and was not true.\n");
    out("        2. UMA held its lock across uz_init, and upstream's\n");
    out("           mb_zinit_pack allocates a cluster from a DIFFERENT\n");
    out("           zone inside it. One lock over all zones makes that an\n");
    out("           immediate re-entry. The rule now is that uz_init,\n");
    out("           uz_ctor and uz_dtor ALL run with the lock dropped -\n");
    out("           they are upstream's code and may allocate.\n");
    out("      Both were found by mtx_lock's recursive-acquire warning,\n");
    out("      which prints TWO stack frames on purpose: the immediate\n");
    out("      caller is nearly always the lock wrapper and says nothing,\n");
    out("      and the frame above it is the one that names the path.\n");
    out("      Worth keeping that warning non-fatal: it takes the lock\n");
    out("      anyway, on the reasoning that a wrong lock is more\n");
    out("      debuggable than a machine that hangs with no output.\n");
    out("      IRQL is REAL masking now, not a variable. KeRaiseIrql\n");
    out("      writes the LAPIC's task-priority register, so raising it\n");
    out("      genuinely stops vectors arriving. Two honest limits: it\n");
    out("      refuses to LOWER (NT faults there, and permissiveness would\n");
    out("      silently unmask interrupts a driver believed blocked), and\n");
    out("      it does NOT mask the timer or keyboard - those come through\n");
    out("      the 8259 via LINT0, which the TPR does not gate. A driver\n");
    out("      expecting DISPATCH_LEVEL to hold off the clock will be\n");
    out("      disappointed, and the reason is that Genesis's clock is not\n");
    out("      on the LAPIC yet.\n");
    out("\n");

    out("[ ] BOOT - UMA is FreeBSD's now. kernel/bsd/uma.c is DELETED.\n");
    out("      Part 3 vendored the mbuf consumer completely but left the\n");
    out("      allocator underneath it as 511 lines of Genesis's own slab\n");
    out("      code - so m_get()'s LIFECYCLE was FreeBSD's and the memory it\n");
    out("      lived in was not. kernel/bsd/vendor/uma_core.inc is now\n");
    out("      vendsrc/sys/vm/uma_core.c byte for byte, 6042 lines, the\n");
    out("      largest vendored file in this tree. md5sum it.\n");
    out("      THE ACCEPTANCE TEST IS THAT NOTHING CHANGED:\n");
    out("        mbuf: selftest passed\n");
    out("      That test was written against the shim and is not edited.\n");
    out("      It exercises m_get, m_gethdr, m_getcl, m_freem and free-list\n");
    out("      reuse; passing unedited against uma_core.c is what says the\n");
    out("      swap is real rather than a rename.\n");
    out("      One check WAS added, and it is the one the shim could not\n");
    out("      pass: zone_pack is a secondary zone over zone_mbuf's keg, so\n");
    out("      the two share ONE item pool. Allocate through m_getcl, free,\n");
    out("      then do 64 m_get/m_freem pairs and require zone_mbuf's item\n");
    out("      count not to climb. The shim's secondary zones kept DISJOINT\n");
    out("      pools - its own header said so - so every m_get there grew\n");
    out("      the zone. A real keg is the only way this passes.\n");
    out("      Two bugs found bringing it up, both in the glue:\n");
    out("        1. pmap_map returned 0. uma_startup1 used that as the base\n");
    out("           of the zone of zones and the first zone_ctor wrote\n");
    out("           through a near-null pointer. Genesis's direct map means\n");
    out("           the answer is phys_to_virt(start) - which is what\n");
    out("           amd64's own pmap_map does, and why it does not advance\n");
    out("           the *virt it is handed.\n");
    out("        2. PHYS_TO_VM_PAGE returned NULL for any page the glue had\n");
    out("           not allocated itself. FreeBSD has vm_page_array covering\n");
    out("           ALL physical memory, so that lookup is total; a partial\n");
    out("           one meant vsetzoneslab - called on every slab, including\n");
    out("           kmalloc-backed ones - wrote through NULL. The tokens are\n");
    out("           created on demand out of a hash table now, bounded by\n");
    out("           what UMA manages rather than by RAM size, because a real\n");
    out("           array would be a million entries against a 4MB kernel\n");
    out("           window.\n");
    out("      What is still shimmed, and it is a BETTER seam than before -\n");
    out("      a vm_page layer over allocators Genesis genuinely owns\n");
    out("      (pmm.c, paging.c, vmalloc.c), rather than a fake allocator\n");
    out("      standing in for a real one. FreeBSD draws the same line:\n");
    out("      uk_allocf/uk_freef are function pointers precisely so a\n");
    out("      keg's page source can be swapped. Named honestly:\n");
    out("        - ONE NUMA domain. True about the machine, not a deferral.\n");
    out("        - No SMR. UMA_ZONE_SMR PANICS rather than no-opping, so a\n");
    out("          future zone that needs it says so instead of corrupting.\n");
    out("        - No sysctl, ktr, ddb, asan, msan, memguard - compiled out\n");
    out("          the way a GENERIC-minus-debugging config does.\n");
    out("        - The reclaim task is never scheduled. Genesis has no\n");
    out("          memory pressure to reclaim under.\n");
    out("        - M_WAITOK does NOT block. Genesis's process.h and\n");
    out("          FreeBSD's both define `struct thread`, so waitq.c cannot\n");
    out("          be included in that translation unit. sys/malloc.h's note\n");
    out("          that M_WAITOK can return NULL therefore STAYS true - the\n");
    out("          plan expected to retire it and it is not retired.\n");
    out("        - Contiguous multi-page allocation retries until it sees an\n");
    out("          adjacent run. Fine at boot when memory is empty, and it\n");
    out("          can fail under fragmentation where a buddy allocator\n");
    out("          would not.\n");
    out("      Note the image is at 92%% of the size cap after this.\n");
    out("\n");

    out("[ ] BOOT - ULE is the LIVE scheduler policy, not a spare one.\n");
    out("      sched.h has said since it was written that ULE would go\n");
    out("      behind its four-function interface, and what ULE buys on a\n");
    out("      machine that runs processes on one CPU is the interactivity\n");
    out("      scoring - not the per-CPU run queues, which are still inert\n");
    out("      because Part 10 is mechanism only. Two lines:\n");
    out("        sched: ULE selftest passed\n");
    out("        sched: ULE picks - interactive 0, batch 0\n");
    out("      Both zero at report time is correct: only the boot process\n");
    out("      exists then, so the picker takes its single-candidate path.\n");
    out("      The score is upstream's sched_interact_score with upstream's\n");
    out("      constants, and LOW MEANS INTERACTIVE. That reads backwards\n");
    out("      and is checked as its own case, because a re-implementation\n");
    out("      that 'fixed' the direction would pass every other test here\n");
    out("      while inverting the entire policy.\n");
    out("      The selftest sets run_ticks/sleep_ticks directly rather than\n");
    out("      driving real processes into those ratios - deliberately.\n");
    out("      Driving them would test the timer, the blocker and the\n");
    out("      switcher at the same time as the policy, and a failure would\n");
    out("      not say which. It checks the extremes, the midpoint,\n");
    out("      MONOTONICITY (a discontinuous score passes the extremes),\n");
    out("      and the ordering the plan asked for - a frequently-blocking\n");
    out("      process outranking a CPU-bound one.\n");
    out("      Round-robin stays compiled in and sched_set_policy can swap\n");
    out("      back. That is not indecision: it is the only thing a ULE\n");
    out("      hang could be bisected against, which is what sched.h says\n");
    out("      the interface is FOR.\n");
    out("      Note ULE degenerates to round-robin when every process\n");
    out("      scores the same - the picker walks from AFTER the current\n");
    out("      one and keeps the lowest - so the existing behaviour is\n");
    out("      preserved rather than merely not broken.\n");
    out("\n");

    out("[ ] BOOT - a REAL TLB BUG, found by ULE's regression run and worth\n");
    out("      reading even though the fix is three lines.\n");
    out("      vmm_map_page_in and vmm_unmap_page_in only invalidated the\n");
    out("      TLB when `as == current_space`. That is right for a user\n");
    out("      space that is not loaded, and WRONG for the kernel half:\n");
    out("      every address space shares the kernel's PML4 slots - the\n");
    out("      same physical tables, not copies - so a mapping edited\n");
    out("      through vmm_kernel_space() is live in the loaded space too.\n");
    out("      kstack_alloc and kstack_free do exactly that, while a USER\n");
    out("      process is current. So:\n");
    out("        1. a kernel stack slot is freed, frames returned;\n");
    out("        2. reallocated to DIFFERENT frames - no invlpg;\n");
    out("        3. kstack_alloc zeroes it and thread_bootstrap_stack lays\n");
    out("           out the new thread - both writing through the STALE\n");
    out("           entry, into the OLD frames;\n");
    out("        4. the next CR3 load flushes the TLB, the thread is\n");
    out("           switched to, and reads frames nothing ever wrote.\n");
    out("      switch_context pops six zeroes and returns to address 0.\n");
    out("      The symptom is the least helpful one possible: RIP 0, CR2 0,\n");
    out("      a kernel stack full of zeroes, and NO BACKTRACE - because\n");
    out("      the frame chain is zeroed too, so Part 2's walker prints one\n");
    out("      line reading '#0 0'.\n");
    out("      It needed a vfork child to exit, be reaped, and the parent\n");
    out("      to vfork again reusing the same slot. That is ordinary shell\n");
    out("      behaviour - `sh -c` running one command and exiting - which\n");
    out("      is why it eventually showed up and why the INTERACTIVE shell\n");
    out("      regression never caught it. Worth remembering that the\n");
    out("      standard check here (boot to a prompt) exercises a strictly\n");
    out("      smaller path than a shell that runs a command and exits.\n");
    out("      The fix is needs_local_flush() in paging.c: flush if the\n");
    out("      space is loaded OR the address is in the kernel half.\n");
    out("\n");

    out("[ ] BOOT - the boot image has no cap to raise any more. Expect,\n");
    out("      from `python3 build.py image`:\n");
    out("        os.img: N bytes, kernel M sectors loaded at 0x100000,\n");
    out("        14587232 bytes of the 0x1000000 kernel window still free\n");
    out("        (7% used)\n");
    out("      TWO NUMBERS ARE THE CHECK. `loaded at 0x100000` says the\n");
    out("      image is above 1MB rather than at 0x7E00; and the free\n");
    out("      figure is DERIVED - build.py reads KERNEL_LMA out of\n");
    out("      linker.ld and KERNEL_MAP_SIZE out of kernel/include/paging.h\n");
    out("      rather than carrying a copy of either, so the three cannot\n");
    out("      drift.\n");
    out("      What it replaced: a MAX_IMG_SIZE constant raised by hand\n");
    out("      five times, each raise carrying a paragraph justifying it.\n");
    out("      It was a proxy for a real wall - the image loaded at 0x7E00\n");
    out("      and grew toward the 32-bit stack boot.asm sets at 0x90000,\n");
    out("      so 557568 bytes was the ceiling and the last raise landed\n");
    out("      within 512 bytes of it. INT 13h cannot write above 1MB, so\n");
    out("      boot.asm now enters UNREAL MODE (protected mode long enough\n");
    out("      to load ES from a 4GB-limit descriptor, then back, without\n");
    out("      touching ES again) and copies each chunk up out of a low\n");
    out("      bounce buffer.\n");
    out("      TWO THINGS FELL OUT OF THE MOVE, both worth remembering:\n");
    out("      (1) A20 had to be enabled BEFORE the copy rather than after.\n");
    out("      With the gate shut, every address with bit 20 set wraps into\n");
    out("      the megabyte below - the copy would appear to succeed while\n");
    out("      scribbling over the IVT, the BDA, the boot sector and the\n");
    out("      buffer it was reading from.\n");
    out("      (2) The sector's code grew past byte 446, into the MBR\n");
    out("      PARTITION TABLE area. Those bytes had been zero by accident\n");
    out("      for years because the tail padding covered them; with 464\n");
    out("      bytes of code the GDT landed inside the first entry, whose\n");
    out("      type byte read back as 0xCF - and Genesis's own partition\n");
    out("      scanner gave the boot disk a partition that was really a\n");
    out("      code fragment, shifting every volume number by one. The\n");
    out("      sector now pads to 446 explicitly and declares four zeroed\n");
    out("      entries, so NASM fails the build if it recurs.\n");
    out("\n");
    out("[ ] BOOT - the FreeBSD PROTOCOL LAYER, vendored, working end to\n");
    out("      end. Expect four separate checks, each printed on its own\n");
    out("      line because each fails independently and for a different\n");
    out("      reason:\n");
    out("        net: ARP check passed - 10.0.2.2 resolved: a frame we\n");
    out("        built was transmitted, answered, and received\n");
    out("        net: ICMP check passed - an echo reply came back through\n");
    out("        ip_input\n");
    out("        net: socket check passed - a UDP socket was created,\n");
    out("        connected and sent through\n");
    out("        net: UDP round trip passed - a datagram we sent was\n");
    out("        answered and delivered back up\n");
    out("      WHY FOUR AND NOT ONE. They test different layers, and a\n");
    out("      single verdict would hide which one broke. ARP proves the\n");
    out("      LINK layer both directions. ICMP proves ip_input, ip_output\n");
    out("      and the checksum. The socket check proves the inet domain\n");
    out("      registered, pffindproto found the UDP protosw, in_pcballoc\n");
    out("      got a PCB into the hash, and sosend turned a uio into mbufs.\n");
    out("      The round trip proves udp_input found that PCB again from an\n");
    out("      arriving datagram's 4-tuple and appended to the right\n");
    out("      socket's buffer.\n");
    out("      Every counter those checks read is a VENDORED one - arpstat\n");
    out("      from netinet/if_ether.c, icmpstat from ip_icmp.c, udpstat\n");
    out("      from udp_usrreq.c - so what is being checked is FreeBSD's own\n");
    out("      accounting of what happened, not a number the test keeps.\n");
    out("      WHAT IS VENDORED, which was the instruction. One Genesis .c\n");
    out("      file per upstream .c file, so the mapping is checkable with\n");
    out("      md5sum: net/if.c (5139 lines, WHOLE - this REPLACED the\n");
    out("      hand-written ifnet.c), if_ethersubr.c, if_dead.c,\n");
    out("      if_llatbl.c, radix.c, toeplitz.c, if_media.c; the ENTIRE\n");
    out("      net/route tree, thirteen files; netinet/ip_input.c,\n");
    out("      ip_output.c, ip_reass.c, ip_icmp.c, ip_id.c, ip_options.c,\n");
    out("      in.c, in_proto.c, in_pcb.c, in_mcast.c, in_rmx.c, in_fib.c,\n");
    out("      if_ether.c, igmp.c, udp_usrreq.c, raw_ip.c; kern/\n");
    out("      uipc_socket.c, uipc_sockbuf.c, uipc_domain.c, subr_hash.c,\n");
    out("      subr_unit.c; six libkern files. About 250 vendored headers,\n");
    out("      including sys/kernel.h, sys/sysctl.h, sys/cdefs.h,\n");
    out("      sys/time.h, sys/event.h, sys/eventhandler.h and net/vnet.h -\n");
    out("      each of which HAD BEEN a Genesis shim and each of which was\n");
    out("      replaced by the real file.\n");
    out("      WHAT IS HAND-WRITTEN IN THE PROTOCOL LAYER: nothing.\n");
    out("      kernel/bsd/arp.c and kernel/bsd/ifnet.c - the two files the\n");
    out("      previous pass wrote by hand - were DELETED.\n");
    out("      TWO MECHANISMS HAD TO BE BUILT FIRST, and both had been\n");
    out("      deliberately compiled out with a comment explaining why:\n");
    out("      (a) SYSINIT. The old comment said objcopy would flatten a\n");
    out("      linker set away. That is wrong: objcopy -O binary keeps every\n");
    out("      allocated section's CONTENTS and drops only the section\n");
    out("      table, and ld's __start_/__stop_ symbols are resolved at link\n");
    out("      time - which kernel/lib/ksyms_data.c had already been relying\n");
    out("      on. There are 53 initialisers now; finding them by hand is\n");
    out("      not a thing to do once, let alone per file added.\n");
    out("      (b) EVENTHANDLER. netinet/in.c does not CALL in_ifattach() -\n");
    out("      it subscribes it to ifnet_arrival_event. in_ifattach is what\n");
    out("      gives an interface its link-layer address table. With the\n");
    out("      event compiled out it never ran, ifp->if_inet stayed NULL,\n");
    out("      and the first thing to touch it page-faulted inside\n");
    out("      arp_add_ifa_lle during `ifconfig inet`. The backtrace pointed\n");
    out("      at ARP; the cause was a subscription dropped at compile time.\n");
    out("      Both are reported at boot now, because both failed silently.\n");
    out("      THREE REAL BUGS IN GENESIS'S OWN LOCKS were found by this,\n");
    out("      and none of them is a network bug:\n");
    out("      (1) The interrupt flag was saved IN THE LOCK - acquire\n");
    out("      stashed RFLAGS in lock->saved_flags, release restored it.\n");
    out("      That is correct only if locks are released in exactly reverse\n");
    out("      order. Take A (saves IF=1), take B (saves IF=0), drop A\n");
    out("      (restores IF=1 with B held), drop B (restores IF=0) - and\n");
    out("      interrupts are off forever. Non-LIFO release is what the\n");
    out("      network stack does constantly. The symptom was the timer\n");
    out("      stopping three ticks after the first ARP request, which\n");
    out("      presents as a hang in whatever happened to be running. It is\n");
    out("      a per-CPU nesting count now, which is what FreeBSD's\n");
    out("      spinlock_enter/spinlock_exit do and for this exact reason.\n");
    out("      (2) rw_try_rlock was mapped onto try-WRITE. It set readers to\n");
    out("      -1, the caller released as a reader, the count went to -2 and\n");
    out("      the lock was corrupt from that moment. Hung closing a UDP\n");
    out("      socket.\n");
    out("      (3) rw_try_upgrade returned unconditional success, so a\n");
    out("      caller believed it had exclusive access while still holding a\n");
    out("      read lock.\n");
    out("      All three are fixed in kernel/lib/mtx.c, which also grew the\n");
    out("      two tools that found them: a bounded-spin deadlock report and\n");
    out("      a dump of which locks a CPU still holds.\n");
    out("      STILL OPEN, and these are the honest edges:\n");
    out("      - TCP IS NOT HERE. netinet/tcp_*.c is not vendored.\n");
    out("      netinet/in_proto.c's protocol switch names a tcp_protosw\n");
    out("      whose pr_attach returns EPROTONOSUPPORT, so\n");
    out("      socket(AF_INET, SOCK_STREAM, 0) fails with the correct error\n");
    out("      rather than misbehaving. Every dependency TCP has below it\n");
    out("      now exists, which is why this is a next step and not a\n");
    out("      rewrite.\n");
    out("      - THERE IS NO socket(2). Sockets are kernel objects;\n");
    out("      kern/uipc_syscalls.c is not vendored, because bridging a\n");
    out("      socket to a file descriptor is VFS work rather than network\n");
    out("      work - it needs Genesis's fileobj/vfs layer to grow a fileops\n");
    out("      vector.\n");
    out("      - NO DHCP, so the address is still compiled in. It is APPLIED\n");
    out("      through the real SIOCAIFADDR ioctl now, exactly as\n");
    out("      ifconfig(8) issues it, and the default route through\n");
    out("      rib_add_default_route - so what is missing is something to\n");
    out("      learn the address FROM, not a way to set one.\n");
    out("      - NO LOOPBACK. net/if_clone.c is not vendored (it drags in\n");
    out("      netlink), and net/if_loop.c creates lo0 through the cloner.\n");
    out("      So 127.0.0.1 is not an address of this machine.\n");
    out("\n");
    out("[ ] BOOT - sysctl(9) is REAL, and the network stack's knobs are\n");
    out("      readable by name. Expect:\n");
    out("        sysctl: 170 static oids registered\n");
    out("          net.inet.ip.forwarding = 0\n");
    out("          net.inet.ip.ttl = 64\n");
    out("          net.inet.icmp.icmplim = 200\n");
    out("      Those values are read back THROUGH the tree - name lookup,\n");
    out("      then the OID's own handler - not printed from the variables.\n");
    out("      <sys/sysctl.h> is vendored, so every SYSCTL_INT and\n");
    out("      SYSCTL_PROC in the network stack is upstream's macro\n");
    out("      producing upstream's struct sysctl_oid. kern/kern_sysctl.c is\n");
    out("      NOT vendored and the reason is specific: two thirds of it is\n");
    out("      the __sysctl(2) system call - name translation for userland,\n");
    out("      ucred checks, capsicum rights, user-page wiring - and there\n");
    out("      is no sysctl(2) in Genesis's syscall table. What IS needed is\n");
    out("      everything below that seam, and upstream already draws the\n");
    out("      seam itself: sysctl_old_kernel/sysctl_new_kernel exist\n");
    out("      precisely so the tree can be driven from inside the kernel.\n");
    out("      Why it was built at all: the 236-line shim kept failing in\n");
    out("      one particular way - a vendored file would use a macro at an\n");
    out("      arity the shim did not have, and the error named the VENDORED\n");
    out("      file rather than the shim.\n");
    out("\n");
    out("[ ] BOOT - a REAL, UNMODIFIED FreeBSD NIC driver, driving real\n");
    out("      hardware. vendsrc/sys/dev/re/if_re.c - 4295 lines, not one of\n");
    out("      them changed - compiled, loaded from /boot/kernel at runtime,\n");
    out("      linked against the kernel's own symbol table, and attached.\n");
    out("      Expect:\n");
    out("        re: Chip rev. 0x74800000\n");
    out("        re: miibus: PHY at address 0, id 0:0\n");
    out("        re: Ethernet address 52:54:0:12:34:57\n");
    out("      Every one of those is read off the hardware. 0x74800000 is\n");
    out("      QEMU's RTL8139C+ chip revision; the MAC is the address QEMU\n");
    out("      assigned, read out of the chip's EEPROM by the driver's own\n");
    out("      code; and the PHY was found by probing the MII address space\n");
    out("      over bit-banged MDC/MDIO. A shim that returned zeros would\n");
    out("      show 0x00000000 and no PHY.\n");
    out("      Note WHICH driver. if_rl.c compiles and links too (2095 more\n");
    out("      lines, also unmodified) and it is not the one loaded, because\n");
    out("      it DECLINES this device: rl_probe returns ENXIO for revision\n");
    out("      0x20 with the comment 'let re(4) take care of this device'.\n");
    out("      That refusal is itself evidence - the real driver's real\n");
    out("      matching logic ran and made the right call.\n");
    out("      What had to be built for it: bus_space and bus_dma\n");
    out("      (kernel/bsd/busdma.c), ifnet and the if_t accessor API\n");
    out("      (kernel/bsd/ifnet.c), the MII framework including a\n");
    out("      HAND-WRITTEN miibus_if.h - upstream GENERATES that file from\n");
    out("      miibus_if.m and Genesis has no codegen step, so it is written\n");
    out("      out, and it only works at all because item 11's KOBJ\n");
    out("      generalization made a custom DEVMETHOD dispatchable.\n");
    out("      net/if_media.c is VENDORED WHOLE (kernel/bsd/ifmedia.c) - it\n");
    out("      is the one piece of the network stack that stands alone.\n");
    out("      FIVE bugs were found by getting this to run, all of them the\n");
    out("      honest error path working against a wrong number or a wrong\n");
    out("      assumption:\n");
    out("      (1) udelay spun on the timer TICK, which does not advance\n");
    out("      before sti - and modules load before sti. A driver calling\n");
    out("      DELAY() during attach hung the boot dead with no fault and no\n");
    out("      message. It spins on the TSC now, which runs regardless of\n");
    out("      interrupt state.\n");
    out("      (2) bus_alloc_resource could not allocate SYS_RES_IRQ at all -\n");
    out("      only memory and I/O BARs. re(4) printed its own 'couldn't\n");
    out("      allocate IRQ resources' and declined.\n");
    out("      (3) The bus_dma map pool was 32. A NIC creates one per\n");
    out("      DESCRIPTOR: re(4) wants 256 TX + 256 RX.\n");
    out("      (4) DRIVER_MODULE ignored its busname argument, so if_rl.c's\n");
    out("      three registrations (rl on pci, rl on cardbus, miibus on rl)\n");
    out("      all landed on the PCI devclass - three slots for one driver,\n");
    out("      two of them wrong.\n");
    out("      (5) kprintf had no field width, so a driver's '%08x' register\n");
    out("      dump printed the literal text '0x%08x'.\n");
    out("      NOT done, and it is the boundary this stops at: there is no\n");
    out("      network STACK. if_input counts a received packet and frees\n");
    out("      it, because there is nothing to hand it to (ROADMAP item 6).\n");
    out("      There are no PHY drivers either - upstream ships about forty -\n");
    out("      so link state is read straight out of the BMSR (clause 22,\n");
    out("      correct on any PHY) and autonegotiation is not driven.\n");
    out("      So: a working DRIVER against a missing stack, which is\n");
    out("      exactly the state this was aiming at.\n");
    out("\n");
    out("[ ] BOOT - Linux and FreeBSD driver source, compiled against\n");
    out("      Genesis's headers, loaded at runtime, reading real hardware.\n");
    out("      ROADMAP item 4's source-compat claim, made concrete. Expect:\n");
    out("        nb_rtl: mac 52:54:0:12:34:57  cmd 11  config1 c\n");
    out("        lkpi_ahci: HBA cap c0141f05 version 10000 - 6 ports, 32\n");
    out("        kld: 3 modules loaded\n");
    out("      src/kmod/nb_rtl.c is ordinary FreeBSD Newbus source -\n");
    out("      DEVMETHOD, device_t, bus_alloc_resource_any with PCIR_BAR(0),\n");
    out("      rman_get_start, device_printf, DRIVER_MODULE. src/kmod/\n");
    out("      lkpi_ahci.c is ordinary Linux source - struct pci_driver,\n");
    out("      PCI_DEVICE(), pr_info, kzalloc, ioremap, readl,\n");
    out("      module_pci_driver. Neither has a Genesis-specific line.\n");
    out("      The MAC is the discriminating part. 52:54:00:12:34:5x is\n");
    out("      QEMU's real assigned address, read a byte at a time out of\n");
    out("      the RTL8139's ID registers through bus_read_1. A resource\n");
    out("      pointing at nothing reads back all-ff or all-00. Same for\n");
    out("      AHCI: cap c0141f05 decodes to 6 ports and 32 command slots\n");
    out("      and version 10000 is AHCI 1.0 - plausible values, not zeros.\n");
    out("      The two exercise DIFFERENT halves on purpose. The AHCI BAR\n");
    out("      is MEMORY and goes through ioremap; the RTL8139 BAR is I/O\n");
    out("      PORTS and has no mapping at all. bus_read_1 dispatches on\n");
    out("      which kind the resource is, and that dispatch is untested by\n");
    out("      a driver that only ever sees one kind.\n");
    out("      Three defects had to be fixed before any of this could work,\n");
    out("      and they are the useful record:\n");
    out("      (1) The entry point. module_pci_driver(x) generated\n");
    out("      x_lkpi_module_init and DRIVER_MODULE(name,...) generated\n");
    out("      name_newbus_module_init, while kldload searched for three\n");
    out("      HARDCODED names that included neither - so a real driver\n");
    out("      relocated correctly and then failed KLD_ERR_NOENTRY with its\n");
    out("      entry point sitting in its own symbol table. Adding those two\n");
    out("      names would have broken again on the third, since the name\n");
    out("      depends on a macro argument. Both macros now emit a pointer\n");
    out("      into a .genesis_modinit SECTION, which is how both upstreams\n");
    out("      solve it (FreeBSD linker sets, Linux .initcall).\n");
    out("      (2) Nothing re-probed. A module loads long after\n");
    out("      bus_attach_children - it needs a filesystem to be read from -\n");
    out("      so its driver registered into a devclass whose devices had\n");
    out("      all been probed, matched nothing, and looked from the\n");
    out("      driver's side exactly like a successful load. bus_driver_added\n");
    out("      is upstream's BUS_DRIVER_ADDED.\n");
    out("      (3) kld_load called init through void(*)(void), discarding\n");
    out("      the int Linux's init_module returns - so a driver that failed\n");
    out("      init was recorded as loaded.\n");
    out("      THREE BUGS WERE INTRODUCED FIXING THOSE, all found by A/B\n");
    out("      against the boot log, all worth keeping:\n");
    out("      - A single 'has the attach pass run' flag was already set by\n");
    out("        bus_selftest.c, which attaches its own synthetic devclass\n");
    out("        early. So pci_generic - which matches everything and\n");
    out("        registers first - claimed all six functions before the\n");
    out("        three demo drivers existed. The flag is per-devclass now.\n");
    out("      - probe_best calls bind_driver before each candidate's probe,\n");
    out("        which FREES dev->softc. Calling it on a live device to see\n");
    out("        if a better driver exists destroys the attached one. The\n");
    out("        symptom was a log line claiming a driver had replaced one\n");
    out("        that was never on that device - the name read back was just\n");
    out("        the last candidate the trial loop had bound.\n");
    out("      - A replacement that failed to attach left the device with NO\n");
    out("        driver. test.sys sets no ID table, so wdm.c matches every\n");
    out("        function at BUS_PROBE_GENERIC; it took three devices from\n");
    out("        pci_generic and failed all three. Two fixes: the incumbent\n");
    out("        is restored on failure, and a WILDCARD match may no longer\n");
    out("        displace a working driver at all - taking a device away\n");
    out("        requires having actually recognised the hardware.\n");
    out("      NOT closed, and this is the honest measure. A real unmodified\n");
    out("      driver from vendsrc still does not build. if_rl.c - the REAL\n");
    out("      FreeBSD driver for the very chip nb_rtl.c probes - includes\n");
    out("      28 headers and Genesis answers 8. Check with:\n");
    out("        grep -oE '#include <[^>]+>' vendsrc/sys/dev/rl/if_rl.c\n");
    out("      The 20 missing ones cluster, which is the useful part:\n");
    out("        net/*  (8 headers)  - ifnet and the network stack, item 6\n");
    out("        dev/mii/* (3)       - the PHY framework, its own subsystem\n");
    out("                              and a generated KOBJ interface\n");
    out("        machine/bus.h, machine/resource.h, sys/rman.h - bus_space\n");
    out("                              and rman; genuinely fillable next\n");
    out("        sys/endian.h, sys/module.h, sys/socket.h, sys/sockio.h,\n");
    out("        dev/pci/pcireg.h    - small\n");
    out("      So: a driver WRITTEN in either idiom compiles and runs; a\n");
    out("      driver COPIED from either tree does not yet, and the distance\n");
    out("      to it is ifnet plus mii plus bus_space, not a long tail.\n");
    out("\n");
    out("[ ] BOOT - device interrupts through the IOAPIC, not the 8259\n");
    out("      pair. ROADMAP item 13's remaining half. Expect:\n");
    out("        ioapic: id 0, 24 entries, gsi 0-23\n");
    out("        ioapic: selftest passed - 8259 pair masked, timer on gsi 2\n");
    out("      Legacy lines keep vector 32+irq, so interrupt.c's dispatch,\n");
    out("      irq.c's tables and every driver's irq_register call are all\n");
    out("      unchanged - only the wire the interrupt travels on differs.\n");
    out("      What acknowledges it is NOT unchanged: an APIC-delivered\n");
    out("      interrupt takes lapic_eoi and a PIC-delivered one takes\n");
    out("      pic_send_eoi, so interrupt.c now calls irq_eoi and that one\n");
    out("      function answers 'which controller' for the whole tree.\n");
    out("      The discriminating check is that the 8259 masks read back\n");
    out("      ff/ff. Booting proves nothing on its own: if the PICs had\n");
    out("      been left live, every legacy interrupt would still arrive by\n");
    out("      the old path and the boot log would be identical whether or\n");
    out("      not a single redirection entry was right. With both masked,\n");
    out("      any interrupt that arrives came through the IOAPIC - so the\n");
    out("      tick advancing is real evidence and not a tautology.\n");
    out("      Expect this line too, and it is the trap:\n");
    out("        irq 0 -> gsi 2, vector 32  live  (overridden)\n");
    out("      An ISA IRQ number is not a GSI. The MADT's interrupt source\n");
    out("      overrides say so, and on QEMU - and essentially every machine\n");
    out("      with an IOAPIC - the timer comes out on GSI 2, not GSI 0.\n");
    out("      Confirmed by A/B: with the override table ignored, the entry\n");
    out("      reads 'irq 0 -> gsi 0' and looks perfectly correct, and the\n");
    out("      machine never ticks again.\n");
    out("      That A/B also fixed an ordering bug worth recording. The\n");
    out("      first run wedged at net_callout_selftest with NO output -\n");
    out("      every post-sti test waits on ticks, so a broken handover\n");
    out("      hangs them all before anything can say why. ioapic_selftest\n");
    out("      now runs first and is bounded by a spin count rather than by\n");
    out("      a second tick, which turns that silent hang into one line.\n");
    out("      Not done, and deliberately: no interrupt balancing. Every\n");
    out("      line is routed to the BSP and stays there. Choosing a\n");
    out("      destination per line is possible now (ioapic_route_irq takes\n");
    out("      an APIC ID) but deciding it dynamically is policy with no\n");
    out("      measured problem behind it.\n");
    out("\n");
    out("[ ] BOOT - a REAL loadable module, from /boot/kernel. ROADMAP\n");
    out("      item 4 names four directories a driver may come from and\n");
    out("      exactly one worked - the Windows path, via test.sys. The\n");
    out("      other three are real now. Three lines:\n");
    out("        hello_kmod: loaded from /boot/kernel\n");
    out("        hello_kmod: .data survived relocation (count 1)\n");
    out("        kld: 1 module loaded\n");
    out("      That is src/kmod/hello_kmod.c compiled with `gcc -c` to an\n");
    out("      ET_REL object - no program headers, no addresses, undefined\n");
    out("      symbols and relocation entries - loaded, placed, LINKED\n");
    out("      against the kernel's own symbol table, and called. Which is\n");
    out("      why item 11 listed a kernel symbol table as a prerequisite:\n");
    out("      without ksyms there is nothing to link against. Part 2's\n");
    out("      table gained ksym_resolve (name -> address) for it; the\n");
    out("      backtrace only ever needed the inverse.\n");
    out("      The SECOND line is the one that discriminates. A loader that\n");
    out("      placed only .text would still print the banner - the string\n");
    out("      is in .rodata reached by an R_X86_64_32S, and the counter is\n");
    out("      in .bss reached by a PC32. Check with:\n");
    out("        readelf -r src/kmod/hello_kmod.ko\n");
    out("      and expect R_X86_64_32S, R_X86_64_PC32 and R_X86_64_PLT32 -\n");
    out("      the three types kldload.c implements. Anything else is\n");
    out("      REFUSED rather than guessed at, because a wrong relocation\n");
    out("      is a jump to the wrong place.\n");
    out("      PLT32 is treated as PC32, which is correct: a statically\n");
    out("      linked kernel has no procedure linkage table, so a call\n");
    out("      through a 'PLT entry' is a direct call. The range check on\n");
    out("      that displacement is not decoration - 32 bits reaches only\n");
    out("      +/-2GB, and silently truncating gives a call into the middle\n");
    out("      of something else. It is also why the whole module is ONE\n");
    out("      allocation: sections scattered across the heap would work\n");
    out("      until two landed more than 2GB apart.\n");
    out("      One bug worth remembering: the scan found NOTHING on a\n");
    out("      volume that plainly had a module on it, because it tested\n");
    out("      for a lowercase '.ko' and the root is FAT - 8.3 names come\n");
    out("      back UPPER-cased. The loader was correct and was simply\n");
    out("      never handed anything.\n");
    out("      Also expect, and this is what Part 16 needs:\n");
    out("        hints: 3 compiled in\n");
    out("      A compiled-in device.hints in upstream's exact format, so a\n");
    out("      real hint line pastes in. Compiled in rather than read from\n");
    out("      a file because hints must be readable BEFORE the bus\n");
    out("      enumerates, and at that point in flk.c there is no\n");
    out("      filesystem - the same constraint FreeBSD has, solved the\n");
    out("      same way. Devices can be NAMED now too\n");
    out("      (device_set_devclass -> foo0, foo1), which is the identity\n");
    out("      BUS_PROBE_NOWILDCARD tests and which bus_add_child could not\n");
    out("      express: a devclass says whose drivers to TRY, not what a\n");
    out("      device IS.\n");
    out("\n");

    out("[ ] BOOT - the capability audit, and what it did NOT close.\n");
    out("      Part 17's method was mechanical rather than impressionistic:\n");
    out("      build a POSIX-ish checklist by category, cross-reference it\n");
    out("      against what syscall.c's table actually dispatches, and\n");
    out("      close what is missing. It found 65 of 107. It closed 23,\n");
    out("      taking it to 88 of 107, and every one of them is exercised:\n");
    out("        systest: 207 passed, 0 failed\n");
    out("      up from 182. The new section is 'Part 17 gap-fill'.\n");
    out("      Several of those checks are written to catch an\n");
    out("      implementation that is present but wrong, which is the\n");
    out("      failure mode a bare syscall-exists test cannot see:\n");
    out("        - pread must NOT move the descriptor offset. A\n");
    out("          lseek-then-read implementation passes every other\n");
    out("          pread test and fails only this one.\n");
    out("        - dup3 must REFUSE oldfd == newfd where dup2 accepts it.\n");
    out("          That difference is the entire reason dup3 exists, and a\n");
    out("          dup3 aliased to dup2 gets exactly this wrong.\n");
    out("        - setresuid must REFUSE three different ids rather than\n");
    out("          silently collapsing them - Genesis has one uid per\n");
    out("          process, and accepting a request it cannot represent\n");
    out("          would be a lie the caller cannot detect.\n");
    out("        - clock_getres must answer ONE TICK, not 1ns. ROADMAP\n");
    out("          already records tick resolution as Owed; reporting 1ns\n");
    out("          and then handing out tick-granular timestamps would be\n");
    out("          the same lie in the other direction.\n");
    out("      Where a syscall simply SUCCEEDS, it is because that is TRUE\n");
    out("      here, not convenient: fsync, fdatasync and msync all return\n");
    out("      0 because bcache is WRITE-THROUGH, so nothing is buffered\n");
    out("      for them to flush. If that cache ever becomes write-back,\n");
    out("      those three become real work.\n");
    out("      WHAT IS STILL MISSING, and it is one cluster: the fourteen\n");
    out("      file-NAMESPACE calls - mkdir, rmdir, unlink, rename,\n");
    out("      symlink, link, chmod, chown and friends. All blocked on the\n");
    out("      same thing, and it is not a syscall problem: fs_ops_t\n");
    out("      (kernel/include/fs.h) has lookup, read, write and iterate\n");
    out("      and NO mutation slots at all, so directory mutation is not\n");
    out("      expressible at the VFS layer and there is nothing for a\n");
    out("      mkdir to call. Closing it means implementing entry\n");
    out("      allocation, chain extension and chain freeing in fatfs.c,\n");
    out("      on the volume the kernel boots from. Recorded in ROADMAP\n");
    out("      with that description rather than half-answered - an\n");
    out("      -ENOSYS from a mkdir that could have worked is worse than\n");
    out("      not having one.\n");
    out("\n");

    out("[ ] BOOT - the review pass, and the five bugs it actually found.\n");
    out("      Part 18 read back every file this pass wrote, looking for\n");
    out("      the failure classes this codebase is prone to rather than\n");
    out("      reading for general correctness: fixed-pool exhaustion,\n");
    out("      off-by-one on the fixed arrays this tree prefers, dropped\n");
    out("      error returns, VA/PTE lifetime, and vendored-FreeBSD\n");
    out("      assumptions that disagree with the Genesis shim underneath.\n");
    out("      That last class is where four of the five came from, which\n");
    out("      is worth knowing for the next vendoring pass.\n");
    out("      FOUND AND FIXED:\n");
    out("      1. Kernel-half TLB invalidation was skipped whenever a user\n");
    out("         process was current - vmm_map_page_in only flushed when\n");
    out("         `as == current_space`, and kernel stacks are mapped\n");
    out("         through vmm_kernel_space(). See its own entry above; it\n");
    out("         is the most serious thing in this pass.\n");
    out("      2. kh_calloc_locked and kh_realloc_locked called the PUBLIC\n");
    out("         kmalloc, re-entering the heap lock, while kheap.c's own\n");
    out("         comment claimed the lock was taken exactly once.\n");
    out("      3. UMA held its zone lock across uz_init, and upstream's\n");
    out("         mb_zinit_pack allocates from a different zone inside it.\n");
    out("      4. genesis_kmem_malloc DISCARDED M_ZERO. keg_alloc_slab\n");
    out("         sets it for every keg that is not UMA_ZONE_MALLOC and\n");
    out("         then relies on the slab arriving zeroed - so a zone that\n");
    out("         asked for clean memory got whatever the heap last held.\n");
    out("         Silent by construction: a freshly booted heap is mostly\n");
    out("         zeroes, so it would work for a long time and then not.\n");
    out("      5. kldload.c's section-placement table was file-static, so\n");
    out("         a module whose init loaded another module would clobber\n");
    out("         the outer load's addresses. Nothing does that today;\n");
    out("         512 bytes of stack means it never can.\n");
    out("      Also corrected: uma_startup1 was handed 256KB of real\n");
    out("      kernel VA that is never used - genesis_pmap_map returns a\n");
    out("      direct-map address and never advances bootmem - which\n");
    out("      leaked the range and made a reader think it mattered.\n");
    out("      THE TEST MATRIX, all green:\n");
    out("        -smp 1 / 2 / 4, interactive boot: 6 matched, 8-9\n");
    out("          selftests, BusyBox, hello_kmod loaded, no faults\n");
    out("        extended machine (-device pci-bridge + vmxnet3 behind it\n");
    out("          + ich9-ahci): 9 matched, MSI selftest passed, a device\n");
    out("          enumerated on bus 1\n");
    out("        the full binary set, run and exited cleanly: systest\n");
    out("          (207 passed, 0 failed), /bin/hello, /bin/mhello,\n");
    out("          /bin/ls, k32.exe, hand.exe, hello.exe\n");
    out("      ONE THING TO WATCH, not a bug: the image is at 95%% of the\n");
    out("      size cap with about 21KB spare. Vendoring uma_core.c cost\n");
    out("      most of that. The next thing added may not fit.\n");
    out("\n");

    out("[ ] BOOT - the file NAMESPACE is writable: mkdir, rmdir, unlink,\n");
    out("      rename, and the three *at variants. This was the ONE gap\n");
    out("      Part 17's audit named and could not close - fs_ops_t had\n");
    out("      lookup, read, write and iterate and no mutation slots at\n");
    out("      all, so there was nothing for a mkdir to call. It has four\n");
    out("      now, and fatfs.c implements them. Run /bin/systest:\n");
    out("        file namespace (mkdir/unlink/rename)\n");
    out("        ...27 checks...\n");
    out("        systest: 234 passed, 0 failed\n");
    out("      up from 207. The syscall table is 95 of 107.\n");
    out("      THE CHECK THAT ACTUALLY MATTERS IS NOT systest. systest\n");
    out("      proves Genesis agrees with ITSELF, and a driver that wrote\n");
    out("      a self-consistent but non-standard layout would pass every\n");
    out("      one of those 27. The real verification is an INDEPENDENT\n");
    out("      FAT16 implementation reading what Genesis wrote. From the\n");
    out("      host, after running /bin/mkprobe:\n");
    out("        mdir -i build/disk.img ::/PROBE2\n");
    out("          .            <DIR>\n");
    out("          ..           <DIR>\n");
    out("          DEEP         <DIR>\n");
    out("        fsck.fat -n -v build/disk.img\n");
    out("          build/disk.img: 42 files, 281/16343 clusters\n");
    out("      No errors from fsck means BOTH FAT copies agree and no\n");
    out("      cluster was lost. That first part is why fat_volume_t now\n");
    out("      stores num_fats: it was read at mount and thrown away,\n");
    out("      because a reader only ever needs FAT #1. Updating one copy\n");
    out("      produces a volume this driver reads back perfectly and\n");
    out("      every other tool calls corrupt - the worst kind of wrong,\n");
    out("      since the machine that made the mess cannot see it.\n");
    out("      unlink was checked by ARITHMETIC rather than by absence: a\n");
    out("      12KB file on 2048-byte clusters is six of them, and the\n");
    out("      fsck cluster count fell by exactly six while two new\n");
    out("      directories added two. A chain walk that stopped early\n");
    out("      would leak, and one that ran on would free somebody else's.\n");
    out("      ORDERING, which is the whole of a filesystem writer's\n");
    out("      correctness and is written into fat.c at each site:\n");
    out("        - unlink writes the DIRECTORY ENTRY first, then frees the\n");
    out("          chain. The other order leaves a window where the entry\n");
    out("          names clusters already free, so a concurrent allocation\n");
    out("          hands them out and two files share blocks. This way\n");
    out("          leaks on failure, and fsck reclaims a leak. Losing\n");
    out("          space is recoverable; sharing it is not.\n");
    out("        - mkdir writes '.' and '..' BEFORE the parent's entry, so\n");
    out("          a failure leaves an unreferenced cluster rather than a\n");
    out("          directory no tool will walk.\n");
    out("        - rename across directories writes the new entry FIRST.\n");
    out("          A failure then leaves the file under BOTH names, which\n");
    out("          is confusing; the other order leaves it under NEITHER,\n");
    out("          which is data loss.\n");
    out("        - a deleted entry is 0xE5, never 0x00. Zero means 'never\n");
    out("          used' and ENDS the directory scan, so zeroing one in\n");
    out("          the middle hides every entry after it. The files stay\n");
    out("          on disk and become invisible - worse than losing them,\n");
    out("          because backups keep the invisible version.\n");
    out("      What is still NOT writable: file CONTENT. fatfs's .write is\n");
    out("      still NULL and fs_writable still answers no. That is a\n");
    out("      different gap - growing a file and allocating clusters as\n");
    out("      it goes - and closing the namespace did not close it.\n");
    out("      rename refuses to overwrite an existing name (-EEXIST)\n");
    out("      rather than replacing it as POSIX says. Doing that safely\n");
    out("      means unlinking the target only after the new entry is\n");
    out("      committed, and a half-done replace loses the file; the\n");
    out("      caller is told and can decide. Also -EXDEV across volumes,\n");
    out("      and -EBUSY on a mount point rather than an unmount.\n");
    out("\n");

    out("[ ] JUDGEMENT - the block cache is WRITE-THROUGH, and that is a\n");
    out("      decision with a cost. There is no sync(2), no unmount that\n");
    out("      flushes, no shutdown path and no flusher thread, so a\n");
    out("      write-back cache would lose every dirty block on reset - and\n");
    out("      the only writer today is a raw device write from ring 3,\n");
    out("      where the caller expects the byte to be on the platter when\n");
    out("      write() returns. The whole measured win is on the read side:\n");
    out("      boot to the shell went from 149 ATA commands to 43. Write-back\n");
    out("      later is a dirty bit in bcache.c plus a flush at the sync\n");
    out("      points; it is not a rewrite, and it is not free.\n");
    out("\n");

    out("[ ] BUILD - the ZFS reader must compile for BOTH targets.\n");
    out("      `python3 build.py kernel` and `sh tests/host/run.sh`. The\n");
    out("      host build links against a real libc, so it resolved realloc,\n");
    out("      asprintf, vsnprintf, strcat, bcmp and iscntrl silently while\n");
    out("      the kernel had none of them - an implicit declaration of a\n");
    out("      function returning a POINTER truncates it to int, which on a\n");
    out("      heap at 0xFFFFFFFF90000000 is a pointer to nothing. Only the\n");
    out("      freestanding build says so. Same class as SYSCALL_TRACE=1\n");
    out("      having to compile.\n");
    out("\n");

    out("[ ] BOOT - a ZFS pool as the ROOT volume.\n");
    out("      A pool mounts at D: and /mnt/d today, and systest's filesystem\n");
    out("      section runs identically on it and on the FAT root. What is\n");
    out("      unproven is `then flip root`: init read from a pool, execve\n");
    out("      from a dnode, and the ordering that implies at boot.\n");
    out("\n");

    out("[ ] JUDGEMENT - a big-endian pool is REFUSED, not read.\n");
    out("      The vendored reader byteswaps the uberblock and nothing\n");
    out("      below it, so a pool written on a big-endian machine fails at\n");
    out("      \"corrupted MOS\". That is upstream's limitation, kept rather\n");
    out("      than patched, and it is asserted as a refusal in\n");
    out("      tests/host/zfs_test.c so it cannot quietly become a misread.\n");
    out("      The cost is real: the only OpenZFS fixture with a populated\n");
    out("      directory (301 files) is one of the pools this cannot read.\n");
    out("\n");

    out("[ ] JUDGEMENT - vendored panic() spins, and vendored memory leaks.\n");
    out("      zfssubr.c's panic is print-then-loop-forever and is reachable\n");
    out("      on a damaged raidz reconstruction: in a boot loader that is\n");
    out("      correct, in a kernel it is a hang. Nothing overrides it,\n");
    out("      because overriding it means editing a vendored file. Also,\n");
    out("      the reader never frees a mounted pool - a loader exits, and\n");
    out("      this one does not. Both are prices of vendoring unmodified,\n");
    out("      written down rather than discovered.\n");
    out("\n");

    out("[ ] JUDGEMENT - name_for wrote past its own terminator.\n");
    out("      \\Device\\Harddisk<N>\\DR<N> was built by patching two indices\n");
    out("      into a literal. They were off by one, so it overwrote the\n");
    out("      separating backslash AND the NUL - registering a device whose\n");
    out("      name ran into stack garbage and was one namespace component\n");
    out("      instead of two. It registered SUCCESSFULLY. The lesson is not\n");
    out("      to count more carefully: a hand-counted offset into a literal\n");
    out("      cannot be checked and cannot fail loudly, so it is built by\n");
    out("      appending now.\n");
    out("\n");

    /* The HUMAN item for surprise removal is gone: it is a host test now
     * (tests/host/volume_test.c). It said "needs a removable device to pull"
     * and this machine has none - but a file-backed disk is exactly that, and
     * the test's own cleanup is the pull. It asserts what has to hold after
     * one: the volume goes depth first, BOTH names of each device go, the
     * mount comes down, and the device object survives to answer -ENODEV
     * rather than faulting.
     *
     * What is still HUMAN is the hardware half - a real USB stick, where the
     * removal arrives as an interrupt at an arbitrary point rather than as a
     * function call between two checks. */

    out("[ ] The spurious-wakeup cost of the shared readiness queue.\n");
    out("      Every waitq_wake_all in the kernel now also wakes every\n");
    out("      poller. That is correct and it is O(pollers) per pipe write.\n");
    out("      At MAX_PROCESSES 16 it is nothing; it is written down here\n");
    out("      because it is a real cost and not an obvious one.\n");
    out("      WIDER NOW: kern_synch.c's wakeup(9) goes through the same\n");
    out("      path, so every NIC and timer wakeup wakes every poller too.\n");
    out("      Still correct - a socket wakeup often IS a readiness change -\n");
    out("      and the frequency is no longer bounded by pipe traffic.\n");
    out("\n");

    out("[x] BOOT - kernel threads. Nothing in ring 3 can reach one, so the\n");
    out("      whole check is kthread_selftest() at boot. Expect exactly:\n");
    out("        kthread selftest: one refusal below is expected\n");
    out("        kthread: refusing 'kt-over' - KTHREAD_MAX (6) reached\n");
    out("        kthread: selftest passed\n");
    out("      The middle line is the cap test's POSITIVE half proving the\n");
    out("      refusal is a limit and not a blanket no; it is red, and it is\n");
    out("      supposed to be. Run under -smp 2 as well: switch_context now\n");
    out("      saves and restores RFLAGS, and the AP paths are the ones that\n");
    out("      exercise a switch under real contention. All three selftests\n");
    out("      have been run uniprocessor and -smp 2, with and without the\n");
    out("      data disk attached.\n");
    out("\n");

    out("[ ] JUDGEMENT - kernel threads are NOT preemptible.\n");
    out("      sched_tick sets the reschedule flag for one exactly as for a\n");
    out("      user process, and return_to_user never acts on it, because a\n");
    out("      kernel thread is always running in the kernel. So a kernel\n");
    out("      thread runs until it yields or blocks.\n");
    out("      This is a decision, not a gap: nothing in this kernel locks\n");
    out("      the structures a syscall mutates - the heap, the handle\n");
    out("      tables, the mount table - and preempting kernel code is what\n");
    out("      turns that into corruption. The cost is that a kernel thread\n");
    out("      which computes in a loop hangs the machine with no output.\n");
    out("      Written down because the failure names nothing.\n");
    out("\n");

    out("[x] A kernel thread with a REAL job: the taskqueue's servicing\n");
    out("      thread. Boot prints its pid and counters:\n");
    out("        taskqueue: taskq  thread pid N  enqueued .. ran .. \n");
    out("      'inline' in that line is the count of tasks that ran at their\n");
    out("      enqueue site because there was no thread yet. Non-zero is not\n");
    out("      a failure - boot enqueues before the thread exists - but a\n");
    out("      LARGE one means genesis_taskqueue_init is being called too\n");
    out("      late in flk.c.\n");
    out("\n");

    out("[x] BOOT - condvars and the taskqueue. Two more lines to expect:\n");
    out("        condvar: selftest passed\n");
    out("        taskqueue: selftest passed\n");
    out("      What each actually proves, because both are the kind of thing\n");
    out("      a completely broken implementation also passes:\n");
    out("        - the cv waiter is observed PROC_BLOCKED by a context that\n");
    out("          is itself running, and has NOT passed cv_wait until the\n");
    out("          signal. A cv_wait that returned immediately fails there.\n");
    out("        - an unsignalled cv_timedwait returns EWOULDBLOCK, not 0,\n");
    out("          so 'it was woken' and 'it gave up' stay distinguishable.\n");
    out("        - the task has NOT run when taskqueue_enqueue returns. That\n");
    out("          single check is the whole difference between what the\n");
    out("          taskqueue was and what it is.\n");
    out("        - a task that SLEEPS, drained to completion - which the\n");
    out("          inline version could not have survived, since it would\n");
    out("          have blocked whoever enqueued it.\n");
    out("\n");

    out("[x] The NETWORK STACK's tasks really are deferred now - the one\n");
    out("      class of caller whose timing the taskqueue change altered,\n");
    out("      and the evidence is an ORDERING in the boot log rather than\n");
    out("      a test of my own tasks:\n");
    out("        before:  re: link state changed to UP     (line 136)\n");
    out("                 miibus: link UP, media 23        (line 137)\n");
    out("        after:   miibus: link UP, media 23        (line 136)\n");
    out("                 Interrupts enabled               (line 164)\n");
    out("                 sched: ULE selftest passed       (line 199)\n");
    out("                 re: link state changed to UP     (line 201)\n");
    out("      That line is printed by do_link_state_change, which IS\n");
    out("      if_linktask's handler (if.inc:2075). It used to run inside\n");
    out("      the driver's attach, before sti; it now runs on the taskqueue\n");
    out("      thread sixty lines of boot later. Still unexercised:\n");
    out("      if_addmultitask, which needs a multicast join.\n");
    out("\n");

    out("[ ] UMA's periodic uma_timeout RUNS NOW, every 20 seconds, having\n");
    out("      never run before - taskqueue_enqueue_timeout used to decline\n");
    out("      and now schedules a real callout. It calls bucket_enable and\n");
    out("      zone_timeout on the taskqueue thread. Boots of two minutes\n");
    out("      have survived several firings, which is evidence and not\n");
    out("      proof: what nobody has watched is a LONG idle run, where the\n");
    out("      per-zone bucket resizing this drives actually accumulates.\n");
    out("\n");

    out("[x] fs_ops_t::unmount - ROADMAP item 7(a), the one item on that\n");
    out("      list that was a bug rather than a missing feature. A\n");
    out("      filesystem's per-volume slot was taken by a successful probe\n");
    out("      and never released, so a machine that had seen four pools\n");
    out("      stopped recognising the fifth - and it failed SILENTLY:\n");
    out("      fatfs_mount returned NULL, volume_probe found no filesystem,\n");
    out("      fs_root() stayed NULL. A disk that stops being mountable.\n");
    out("      Checked by tests/host/volume_test.c, seven insert/pull cycles\n");
    out("      against pools of four and eight. That check was MEASURED\n");
    out("      against the bug, not assumed to catch it: with fat_ops.unmount\n");
    out("      forced back to NULL it fails at cycle 4.\n");
    out("      What it does NOT do is make the pools bigger. Four ZFS pools\n");
    out("      at once is still the limit; four ever was the leak.\n");
    out("\n");

    out("[x] File CONTENT writing - ROADMAP item 7(b) and the oldest entry\n");
    out("      in the Owed list. fatfs's .write is fat_write_entry_at now.\n");
    out("      PART A's 'write(2) on a file' above is the ring-3 half; the\n");
    out("      growth half is on the host (tests/host/fat_write_test.c),\n");
    out("      because extending a file here needs O_CREAT to make a scratch\n");
    out("      file or O_TRUNC to shrink one back, and both are refused.\n");
    out("\n");

    out("[ ] The \"supreme\" privilege - root + NT AUTHORITY\\SYSTEM + NT\n");
    out("      SERVICE\\TrustedInstaller fused into one exemption from every\n");
    out("      ACL check. acl_access's root bypass (kernel/fs/acl.c) used to\n");
    out("      carry a comment saying that the day this kernel grew\n");
    out("      privileges as a real concept, that line would become a\n");
    out("      privilege test instead of a uid test - cred_is_supreme is\n");
    out("      that, and the bypass line now reads it instead of testing\n");
    out("      euid==0 directly. Exactly one uid holds it at a time, set by\n");
    out("      root through PR_GENESIS_GRANT_SUPREME (a prctl(2) op, not a\n");
    out("      new syscall number - this kernel's numbers are real Linux\n");
    out("      amd64 ones throughout and a fabricated one risks colliding\n");
    out("      with something Linux assigns later); PR_GENESIS_REVOKE_\n");
    out("      SUPREME and PR_GENESIS_QUERY_SUPREME are the other two.\n");
    out("      Checked at boot (acl_selftest, kernel/fs/acl_selftest.c): a\n");
    out("      credential with the flag clear stays denied by a 0000 ACL,\n");
    out("      the same credential with it set bypasses that same ACL, and\n");
    out("      root's existing bypass is unchanged by any of it. What that\n");
    out("      does NOT cover is the syscall gate itself - that a non-root\n");
    out("      caller's PR_GENESIS_GRANT_SUPREME is refused with -EPERM -\n");
    out("      which needs a ring-3 check here or in systest.c and does not\n");
    out("      have one yet.\n");
    out("\n");

    out("[x] O_CREAT, O_EXCL, O_TRUNC, O_APPEND, truncate and ftruncate all\n");
    out("      work. PART A's 'O_CREAT / O_TRUNC / O_APPEND / ftruncate'\n");
    out("      section is the check, and it cleans up after itself.\n");
    out("      The end-to-end proof is not in either suite, because neither\n");
    out("      can run a shell. By hand, from the prompt:\n");
    out("        # /bin/ls > /t.txt\n");
    out("        # /bin/ls /\n");
    out("        BIN  BOOT  ETC  LIB  SBIN  T.TXT  USR  WSR\n");
    out("      Spelled /bin/ls and not `echo` because the busybox applet\n");
    out("      links are not staged - see the next entry.\n");
    out("\n");

    out("[ ] The shell answers 'sh: echo: not found' for everything, and it\n");
    out("      is NOT the missing applet links it looks like.\n");
    out("      root/bin/busybox is a SHELL-ONLY build: 140KB, and strings\n");
    out("      finds no applet names in it at all - not even echo, which\n");
    out("      every ordinary ash carries as a builtin. There is nothing to\n");
    out("      link TO. Staging symlinks would fix nothing, and FAT16 has no\n");
    out("      symlinks to stage in any case.\n");
    out("      What is needed is a busybox rebuilt with applets enabled, and\n");
    out("      its source is not in this tree - a build input to acquire,\n");
    out("      not a line to change. Written this way round because 'the\n");
    out("      applet links are missing' sends you to tools/ and root/,\n");
    out("      which is the wrong place. It is the difference between a\n");
    out("      shell you can use and one you can start.\n");
    out("\n");

    out("[x] BOOT - the DISPATCHER OBJECTS, ROADMAP item 14's biggest gap.\n");
    out("      Expect: dispatch: selftest passed\n");
    out("      Events, semaphores and mutants, nameable under\n");
    out("      \\BaseNamedObjects, with waits that really block. What the\n");
    out("      check proves that a weaker one would not:\n");
    out("        - the second context opens the event BY NAME, never having\n");
    out("          been handed a pointer\n");
    out("        - the creator observes it in PROC_BLOCKED while itself\n");
    out("          running, and observes that it has NOT passed the wait\n");
    out("        - a semaphore release past its limit is refused AND the\n");
    out("          count is then shown not to have moved (a clamp passes the\n");
    out("          refusal on its own)\n");
    out("        - a notification event stays set and a synchronisation one\n");
    out("          auto-resets, which only a SECOND waiter can tell apart\n");
    out("        - waiting on a console is -EINVAL, asserted next to a wait\n");
    out("          that succeeds, so 'refused' is distinguishable from\n");
    out("          'nothing is waitable yet'\n");
    out("      The second context is a KERNEL THREAD and not a second\n");
    out("      process, because nothing in ring 3 can reach these yet.\n");
    out("\n");

    out("[x] The RING-3 half of the dispatcher objects: nine NT syscalls,\n");
    out("      with stubs and exports in src/ntdll. Run /bin/sync.exe from\n");
    out("      the shell; expect 'winsync: all checks passed'.\n");
    out("      It is a MinGW-built PE importing from ntdll.dll, so like\n");
    out("      hello.exe it knows no syscall number and no register\n");
    out("      convention - which is what makes it a check of the ABI and\n");
    out("      not of a hand-assembled agreement with itself.\n");
    out("      The 'NT call 0x... -> status 0x...' lines interleaved with\n");
    out("      its output are the kernel's own nt_trace, printed for every\n");
    out("      non-success status. Half those statuses are the POINT of the\n");
    out("      test, so those lines are expected and not failures.\n");
    out("\n");

    out("[ ] Item 14's check in the shape it was WRITTEN - a named event\n");
    out("      created by one PROCESS and opened by name from another - is\n");
    out("      still not possible: there is no NtCreateProcess, so a second\n");
    out("      NT process cannot be started.\n");
    out("      What exists between the two checks covers every mechanism:\n");
    out("      sync.exe does create-by-name, open-by-name and collide-on-\n");
    out("      second-create in one process, and dispatch_selftest does the\n");
    out("      blocking half with a kernel thread as the second context.\n");
    out("      Written down rather than called finished, because 'every\n");
    out("      mechanism is covered' is a weaker claim than the one the\n");
    out("      roadmap asked for and should not be allowed to pass for it.\n");
    out("\n");

    out("[ ] Only the three DISPATCHER types register into \\ObjectTypes.\n");
    out("      The device, file, pipe and console types do not, so a listing\n");
    out("      of \\ObjectTypes shows Event, Semaphore, Mutant and Type and\n");
    out("      describes less than it looks like it does. One line of\n");
    out("      ob_register_type per type, in each type's own file.\n");
    out("\n");

    out("[x] A pre-existing WEDGE in waitq_wait_until, found by the\n");
    out("      dispatcher check rather than reported by anything.\n");
    out("      When nothing else is runnable, schedule() returns at once and\n");
    out("      never touches the state - so a process leaving the loop ON\n");
    out("      ITS DEADLINE returned to ordinary code still marked\n");
    out("      PROC_BLOCKED, and the next schedule() refused to pick it, for\n");
    out("      good. A process wedged by a TIMEOUT, on an idle machine -\n");
    out("      which is the machine where 'nothing else was runnable' is\n");
    out("      most likely. poll(2) with a timeout takes that path.\n");
    out("      sys_nanosleep had already worked around it by hand, which is\n");
    out("      the tell: the workaround belonged in the one blocking loop.\n");
    out("      Why this is not a PART A check, which is where the standing\n");
    out("      instruction would otherwise put it: A WEDGED PROCESS CANNOT\n");
    out("      REPORT THAT IT IS WEDGED. Every ring-3 shape of this test\n");
    out("      ends the same way - the process times out, gets switched\n");
    out("      away from, and is never picked again - so the symptom is a\n");
    out("      hang and not a failed check, and a hang is the one result\n");
    out("      this program cannot print.\n");
    out("      The regression test is dispatch_selftest() at boot. It is\n");
    out("      what FOUND this: two of its checks time out on purpose and a\n");
    out("      later one needs the same context to run again, which is\n");
    out("      exactly the sequence that wedges. Before the fix the boot\n");
    out("      stopped there with no output; after it, the line reads\n");
    out("      'dispatch: selftest passed'.\n");
    out("      Recorded here rather than left as a boot line nobody would\n");
    out("      connect to poll(2).\n");
    out("\n");

    out("[x] BOOT - module .text is no longer W+X, the oldest open security\n");
    out("      remainder in the tree. Expect at boot:\n");
    out("        kld: W^X selftest passed (4 modules)\n");
    out("      Module images left the KERNEL HEAP to make this possible:\n");
    out("      protections are per page, and a heap allocation shares its\n");
    out("      edge pages with other objects, so sealing module text would\n");
    out("      have sealed somebody else's bytes. They have their own mapped\n");
    out("      window now, executable sections grouped first behind a\n");
    out("      page-aligned seam.\n");
    out("      The check reads the protections back out of the page tables.\n");
    out("      It CANNOT fail on a missing seal - an image starts\n");
    out("      non-executable, so a load that never seals faults on its\n");
    out("      first call, measured by disabling the seal. It exists for the\n");
    out("      silent error: a seal covering too much, leaving the data half\n");
    out("      unwritable.\n");
    out("\n");

    out("[x] BOOT - THE NIC GOT EXACTLY ONE INTERRUPT PER BOOT - fixed.\n");
    out("      The most interesting bug in this round, because nothing that\n");
    out("      changed was wrong. Expect at boot, all four:\n");
    out("        net: ARP check passed\n");
    out("        net: ICMP check passed\n");
    out("        net: socket check passed\n");
    out("        net: UDP round trip passed\n");
    out("        net: re  opackets 6  oerrors 0  ipackets 5  ierrors 0\n");
    out("      Three parts, all needed:\n");
    out("      (1) THE KERNEL IS NOT PREEMPTIVE - a kernel thread runs only\n");
    out("          when something calls schedule().\n");
    out("      (2) net_selftest's wait_for() SPUN WITHOUT YIELDING, so for\n");
    out("          the whole 200-tick wait no kernel thread could run.\n");
    out("      (3) if_re registers re_intr as a FILTER: it MASKS THE CHIP\n");
    out("          and hands receive to rl_inttask on taskqueue_fast, and\n");
    out("          re_int_task is the only thing that re-arms the mask.\n");
    out("      One interrupt, chip quiet, task on a queue whose thread the\n");
    out("      waiter is starving, reply sitting in the RX ring the whole\n");
    out("      time the counter reads zero.\n");
    out("      LATENT UNTIL THE TASKQUEUE BECAME REAL. taskqueue_enqueue used\n");
    out("      to call the task INLINE on the enqueuing CPU, so re_int_task\n");
    out("      ran inside the interrupt handler and re-armed the chip before\n");
    out("      the waiter got another turn. Deferring the work was correct\n");
    out("      and it turned a working spin into a deadlock.\n");
    out("      HOW IT WAS FOUND, because the method mattered more than the\n");
    out("      guess: a QEMU filter-dump pcap proved the reply was on the\n");
    out("      wire at +23ms, which moved the question from 'is the network\n");
    out("      up' to 'why does the driver not see it'; then a counter in the\n");
    out("      interrupt trampoline said ONE, which is not a number a race\n");
    out("      produces. Raising the wait to 2000 ticks changed nothing,\n");
    out("      which ruled out slowness and named it a deadlock.\n");
    out("      AND IT HAD BEEN PASSING ON LUCK - on where that one interrupt\n");
    out("      fell relative to the reply landing in the ring. A green line\n");
    out("      that depends on timing is worse than a red one, because it is\n");
    out("      the same green line either way.\n");
    out("      GENERAL SHAPE WORTH KEEPING: in a non-preemptive kernel, any\n");
    out("      loop waiting for work produced by a kernel thread must yield,\n");
    out("      or it cannot succeed however long it waits.\n");
    out("\n");

    out("[ ] JUDGEMENT - POLL(2) ON A SOCKET IS WOKEN NOW, AND THE FIX IS\n");
    out("      NOT COVERED BY A TEST. Said plainly because it is a change\n");
    out("      that works by argument rather than by measurement.\n");
    out("      Genesis's poll parks on ONE shared readiness queue that every\n");
    out("      waitq_wake_all in the kernel pokes - waitq.c argues for a\n");
    out("      single queue precisely so no wake site can be forgotten. The\n");
    out("      socket layer never calls waitq_wake_all: it wakes through\n");
    out("      selwakeuppri, which <sys/selinfo.h> defined as a no-op with\n");
    out("      the comment 'a socket layer built on this would block through\n");
    out("      waitq instead'. True until socket(2) existed; after it, a poll\n");
    out("      on a socket slept to its timeout with the data already in the\n");
    out("      receive buffer. Observed exactly that way: a blocking recvfrom\n");
    out("      returned the datagram while a five-second poll on the same\n");
    out("      descriptor reported nothing.\n");
    out("      selwakeuppri now pokes the readiness queue. It is the right\n");
    out("      hook rather than a convenient one - sowakeup funnels every\n");
    out("      socket-buffer readiness change through it, and it is compat\n");
    out("      code so nothing vendored is touched.\n");
    out("      WHY NO TEST: proving a wakeup needs data to arrive on a\n");
    out("      schedule, and this machine has no loopback interface, so its\n");
    out("      only UDP peer is QEMU's resolver - which answers only when the\n");
    out("      HOST has an upstream one. The boot check prints 'UDP reply not\n");
    out("      received' when it does not, and did so during this work. What\n");
    out("      would prove it is a loopback interface, and that is the thing\n");
    out("      to build before claiming this line.\n");
    out("\n");

    out("[x] BOOT - the systest socket check asserts the DESCRIPTOR LAYER,\n");
    out("      not the network, and that boundary was drawn the hard way.\n");
    out("      Three versions of it failed for three reasons that were all\n");
    out("      the test's fault:\n");
    out("        - a 2,000,000-iteration recvfrom spin, far too slow under\n");
    out("          emulation to finish inside the run\n");
    out("        - a second sendto immediately after the first, which fails\n");
    out("          with EHOSTUNREACH because arpresolve holds exactly ONE\n");
    out("          packet while a resolution is outstanding. net_selftest.c\n");
    out("          already retries its ping with the comment 'the first ping\n");
    out("          raced the ARP' - the same fact, learned twice\n");
    out("        - requiring a DNS reply, which depends on the host having a\n");
    out("          resolver and is outside the guest's control entirely\n");
    out("      What it checks now needs nothing outside the machine: the\n");
    out("      refusals, bind, getsockname proving a port was really CHOSEN,\n");
    out("      connect, write(2) through the object vtable, and POLLOUT on a\n");
    out("      connected socket - which is deterministic because an empty\n");
    out("      send buffer is always writable. The receive path stays owned\n");
    out("      by the boot check, which talks to 10.0.2.2 - answered by slirp\n");
    out("      itself rather than by the host, and therefore reliable.\n");
    out("\n");

    out("[x] BUILD - THE HOST SUITE HAD BEEN EXITING 1, and the reason is\n");
    out("      worth more than the fix. tests/host/check_zfs_boundary.py\n");
    out("      reported 294 failures on every run, ALL OF THEM FALSE: it\n");
    out("      matched includes on BASENAME against every file under\n");
    out("      kernel/zfs/, including kernel/zfs/compat/ - a vendored shim\n");
    out("      layer full of deliberately generic names (sys/param.h,\n");
    out("      sys/types.h, sys/queue.h) that kernel/bsd/compat/ also has. So\n");
    out("      every BSD file including <sys/param.h> was reported as\n");
    out("      reaching into ZFS when it resolves to the BSD header.\n");
    out("      THE SECOND CONSEQUENCE WAS WORSE THAN THE NOISE. run.sh has\n");
    out("      `set -e` and that check exits 1, so it silently GATED\n");
    out("      EVERYTHING AFTER IT. A new check appended below it never ran\n");
    out("      at all, and the suite looked green because the C tests print\n");
    out("      'all checks passed' earlier in the run. A check that always\n");
    out("      fails is a check nobody reads; one that always fails FIRST\n");
    out("      stops the others running.\n");
    out("      Basename matching cannot work here - colliding basenames\n");
    out("      across the two vendored trees is exactly why build.py hands\n");
    out("      out include paths per directory. The compat subtree is\n");
    out("      excluded and the path form is kept, and it was verified by\n");
    out("      making it FAIL on a real violation in both spellings\n");
    out("      (#include \"zfsimpl.h\" and #include <zfs/nvlist.h>).\n");
    out("\n");

    out("[x] BUILD - the staged tree is checked against what FAT16 can\n");
    out("      represent, before an image is built from it:\n");
    out("        tests/host/check_staged_tree.py\n");
    out("      ROADMAP item 10 asked for a systest that walks the /wsr mirror\n");
    out("      at run time. This is earlier and strictly better, and the\n");
    out("      reason is not speed: by the time the guest sees the volume,\n");
    out("      one of the two colliding names is simply GONE, so a walk of\n");
    out("      the mounted filesystem cannot tell a collision from a file\n");
    out("      nobody staged. The check has to run against the source tree.\n");
    out("      It uses fatfs.Fat16.name_to_11 - the stager's own encoder -\n");
    out("      rather than a reimplementation, so it cannot drift from the\n");
    out("      thing it checks. Verified by making it fail: the historical\n");
    out("      System32/system32 case and an over-long name are both caught\n");
    out("      by name. The walk asserts its own extent too (21 directories,\n");
    out("      43 entries), because a check that silently walked nothing\n");
    out("      reports exactly what a clean tree reports.\n");
    out("\n");

    out("[x] BUILD - tools/build_user.sh exits 0. It was exiting 1 on the\n");
    out("      rtl8139 module and the cause was not the script: if_rl.c and\n");
    out("      if_re.c both call device_get_sysctl_ctx and\n");
    out("      device_get_sysctl_tree in attach, neither existed, and `set -e`\n");
    out("      stopped src/kmod/build.sh at that file - before the two\n");
    out("      modules after it. So the failure was one missing pair of\n");
    out("      accessors wearing a build-system costume.\n");
    out("      Per-device sysctl contexts are real now: dev.<name>.<unit>,\n");
    out("      created on first use, and the same device gets the same pair\n");
    out("      every time - checked, because a driver that calls both\n");
    out("      accessors would otherwise build two trees.\n");
    out("      The trap that shaped it: sysctl_add_oid DOES NOT COPY THE\n");
    out("      NAME. The obvious implementation of the unit level - snprintf\n");
    out("      the number into a buffer - leaves the node holding a dangling\n");
    out("      name. Unit names come out of a static table of decimal\n");
    out("      strings instead.\n");
    out("\n");

    out("[x] BOOT - and the sysctl tree became the FIFTH thing kld_unload\n");
    out("      sweeps, which closes the hole the entry below used to name as\n");
    out("      an example. A knob added by a module names itself with a\n");
    out("      string literal in that module's .rodata and points oid_arg1 at\n");
    out("      the driver's softc. Two halves: the sweep, and bus.c calling\n");
    out("      device_sysctl_fini when it detaches a driver - which is what\n");
    out("      makes the sweep normally find nothing rather than normally\n");
    out("      refuse. Checked both directions with the ARG in range, not the\n");
    out("      handler, because the arg is the pointer that actually lives in\n");
    out("      the module.\n");
    out("\n");

    out("[x] BOOT - modules UNLOAD now. module_exit was accepted and never\n");
    out("      called; item 4's last remainder. Expect at boot:\n");
    out("        hello_kmod: exit called, 1 load this boot\n");
    out("        kld: unloaded HELLOKM.KO (0 drivers deregistered, ...)\n");
    out("        hello_kmod: loaded from /boot/kernel\n");
    out("        kld: unload selftest passed\n");
    out("      Three lines and then a fourth, because the test UNLOADS AND\n");
    out("      RELOADS: the module's exit runs, the image is unmapped, and\n");
    out("      it comes back. It must come back AT THE SAME ADDRESS, and\n");
    out("      that is the assertion a stub cannot pass - a kld_unload that\n");
    out("      cleared the table entry and forgot the page bitmap satisfies\n");
    out("      everything else here and puts the reload one image further up\n");
    out("      the window. The image allocator is a page bitmap now for that\n");
    out("      reason; it was a bump pointer, which was correct only while\n");
    out("      nothing was ever freed.\n");
    out("\n");

    out("[x] BOOT - and the leak was never the dangerous half. What kills\n");
    out("      the machine is a POINTER LEFT BEHIND: a module's driver_t,\n");
    out("      its interrupt handlers, its callout functions and its softc\n");
    out("      all live INSIDE the image that is about to be unmapped, and\n");
    out("      the fault comes later, in a timer tick or an interrupt, with\n");
    out("      nothing on the screen naming the module.\n");
    out("      So kld_unload ASKS - of the newbus driver tables, the IRQ\n");
    out("      handler pool, the callout wheel and the taskqueue - 'do you\n");
    out("      hold anything inside these bytes?', and REFUSES the unload if\n");
    out("      anything does. The question is an address range and not a\n");
    out("      list the module declares, because a declared list is one\n");
    out("      somebody forgets to add to and a missing entry is not a\n");
    out("      compile error.\n");
    out("      Honest limit, stated rather than implied: this is not a proof\n");
    out("      that no pointer survives. A module that stashed one somewhere\n");
    out("      none of those four walk passes every check and still faults.\n");
    out("      What holds is the useful half - a REFUSED unload is certainly\n");
    out("      unsafe, and an accepted one has been checked against\n");
    out("      everything this kernel can enumerate.\n");
    out("\n");

    out("[x] BOOT - the sweeps are checked in BOTH DIRECTIONS, which is the\n");
    out("      only thing that separates them from four functions returning\n");
    out("      zero - and zero-for-everything is exactly the failure that\n");
    out("      matters, because it turns the refusal into a rubber stamp.\n");
    out("      Each is asked of a range ONE OBJECT WIDE: zero before the\n");
    out("      registration, one after it, zero again after it is taken\n");
    out("      back. One object wide and not 'the kernel', so a handler some\n");
    out("      other subsystem registered cannot make the positive case pass\n");
    out("      by accident.\n");
    out("      And it is the CTX and the STRUCT that sit in range, not the\n");
    out("      function - both handler functions are kernel text either way.\n");
    out("      That is the quiet case: a struct callout is normally a field\n");
    out("      of the driver's softc, so the WHEEL'S OWN LINK runs through\n");
    out("      the module image, and a sweep that only looked at c_func\n");
    out("      would call it clean and walk the wheel into unmapped memory\n");
    out("      on the next tick - inside the timer interrupt, before any of\n");
    out("      the module's code ran.\n");
    out("\n");

    out("[x] BOOT - taking a driver off the bus, which is the part that\n");
    out("      cascaded. bus.c had no removal path: devclass_add_driver had\n");
    out("      no inverse and a driver_t pointer, once registered, was\n");
    out("      permanent. bus_unregister_range is that inverse, and the\n");
    out("      bus selftest covers its three different answers:\n");
    out("        - a driver WITH a detach method comes off cleanly, its\n");
    out("          softc freed and its device back to DS_NOTPRESENT\n");
    out("        - a detach that FORGETS a resource is reported (the count\n");
    out("          bus_unregister_range hands back is the only place that\n");
    out("          bug is visible) and the resource is reclaimed - checked\n");
    out("          by allocating the same bytes again afterwards, which a\n");
    out("          slot that had merely been counted would refuse\n");
    out("        - a driver with NO detach method, attached, is REFUSED,\n");
    out("          and refused WITHOUT CHANGING ANYTHING\n");
    out("      The first and third are a pair on purpose. A\n");
    out("      bus_can_unregister_range that returned -1 for everything\n");
    out("      would pass the refusal test on its own.\n");
    out("\n");

    out("[ ] JUDGEMENT - a refused unload can leave a module STOPPED BUT\n");
    out("      LOADED, and that is unavoidable rather than sloppy. The exit\n");
    out("      function is what RELEASES the registrations, so it has to run\n");
    out("      before the residual sweep can be meaningful - and it cannot\n");
    out("      be un-run if the sweep then finds something. The module stays\n");
    out("      mapped and in the table with its exit already called. It is a\n");
    out("      bug in that module, it is reported as one, and it is the safe\n");
    out("      end of the trade: the alternative is unmapping code something\n");
    out("      is about to call. Linux avoids it with refcounts checked\n");
    out("      BEFORE exit; that needs every registration to take one, which\n");
    out("      is a change to every registry rather than to the loader.\n");
    out("\n");

    out("[x] statfs(2) and fstatfs(2) are done, which clears the whole\n");
    out("      group the Owed list had waiting on the content path - and\n");
    out("      statfs never needed it. See PART A's 'statfs' section.\n");
    out("      The free count is NOT cached: it is an O(clusters) scan of\n");
    out("      the allocation table on every call. A value captured at mount\n");
    out("      is right once and confident forever after, and df reporting a\n");
    out("      full disk as empty is worse than df being slow.\n");
    out("\n");

    out("[ ] fstatfs(2) answers from the CWD, not from the descriptor. An\n");
    out("      open file object holds a node and a node carries no path, so\n");
    out("      there is no way from the descriptor to name its volume. Right\n");
    out("      whenever the descriptor and the working directory are on the\n");
    out("      same volume - which with one mounted volume is always, and\n");
    out("      wrong the moment a program fstatfs's a file on a second one.\n");
    out("      The fix is small: fs_node_t already carries its fs_volume_t,\n");
    out("      so it wants an fs_statfs_vol() taking that. Not done because\n");
    out("      nothing calls fstatfs yet, and a wrong answer nobody asks for\n");
    out("      is still a wrong answer waiting.\n");
    out("\n");

    out("[ ] ZFS still has NO write slot, and that is not the same gap. The\n");
    out("      vendored reader's vdev write callback returns EROFS - it is a\n");
    out("      different program, not a flag. See ROADMAP item 7.\n");
    out("\n");

    out("[ ] JUDGEMENT - a RETIRED slot can still be handed to a different\n");
    out("      medium, and then a stale handle reads somebody else's\n");
    out("      filesystem instead of getting -ENODEV. Both pools now prefer\n");
    out("      a never-used slot and fall back to a retired one only when\n");
    out("      there is none, so it takes more live volumes than the pool is\n");
    out("      deep before it can happen at all - but it is reachable, and\n");
    out("      it is the price of recovering the slots. The alternative is\n");
    out("      the leak, which is certain rather than merely possible.\n");
    out("      The real fix is a refcount on fs_volume_t, so a slot cannot be\n");
    out("      reused while a handle holds it and exhaustion is an honest\n");
    out("      -ENFILE. That is a change to every path that stores an\n");
    out("      fs_node_t, which is why it is written down rather than done.\n");
    out("\n");

    out("[ ] JUDGEMENT - taskqueue_drain from a non-thread context SPINS.\n");
    out("      A kernel thread waits on a condvar. Anything else cannot\n");
    out("      deschedule through sleep(9) at all - ksleep_can_block() asks\n");
    out("      'am I a kernel thread' when the question it wants is 'am I in\n");
    out("      an interrupt handler' - so it yields in a loop instead. It\n");
    out("      must: cv_wait there would `hlt`, and the thing being waited\n");
    out("      for is a kernel thread that needs the CPU to finish.\n");
    out("      The general fix is a per-context interrupt-depth count. It is\n");
    out("      NOT a global counter incremented in interrupt_dispatch: that\n");
    out("      function calls return_to_user, which schedules, so the count\n");
    out("      would be held across a context switch and read by the wrong\n");
    out("      thread - which reports 'in an interrupt' to a thread that is\n");
    out("      not in one, and refuses every block it makes.\n");
    out("\n");

    out("[x] The kernel did not finish booting with disk.img ATTACHED.\n");
    out("      FIXED. It stopped during kld_load_directories at:\n");
    out("        bus: DEVCLASS_MAX exceeded\n");
    out("      and never reached 'Interrupts enabled' - before sti, with no\n");
    out("      fault report, so every selftest after it was unreachable and\n");
    out("      neither systest nor this program could be run at all.\n");
    out("      It was not a NULL deref: devclass_find HALTS on exhaustion by\n");
    out("      design (bus.h calls it a build-time sizing bug, not a runtime\n");
    out("      condition). DEVCLASS_MAX was 8 and the boot needs 9.\n");
    out("      The part worth remembering is what was IN those eight: FOUR\n");
    out("      were the bus selftest's own - st_pass, st_child, st_inherit,\n");
    out("      st_nowild - and nothing frees a devclass. A test's devclass\n");
    out("      is indistinguishable from a driver's and just as permanent,\n");
    out("      so half the pool was spent before any real driver asked.\n");
    out("      Now 32, and bus_devclass_report prints the live count at boot\n");
    out("      ('bus devclasses: 9 of 32') so the headroom is a line in the\n");
    out("      log rather than something you learn by halting.\n");
    out("\n");
}

/* --- the mount table and volume objects ---------------------------------
 *
 * What surprise removal and drive letters actually produce, tested from ring
 * 3 rather than asserted in a comment.
 *
 * The control matters more than usual here. Every check below could pass by
 * the volume layer having done nothing at all - a /dev listing with no
 * volumes in it "passes" a test that only looks for absent things. So the
 * first check measures that there IS a volume, and everything after it is
 * conditional on that. A test that explains away its own zero is not a test. */

static void test_volumes(void) {
    int fd;
    i64 rc;
    char buf[4096];
    int  saw_volume = 0;
    int  saw_letter = 0;

    section("volumes and the mount table");

    /* A volume device exists and is named the way volume.c says it names
     * them. Through /dev, which is the merged \??\ + \Device\ view - so
     * this also checks the volume landed in a directory the merged view
     * searches, which is the half that a device registered by hand into only
     * one of them would fail. */
    fd = (int)sc4(SYS_openat, AT_FDCWD, "/dev", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        check(0, "open /dev to look for volumes");
        return;
    }
    rc = sc3(SYS_getdents64, fd, buf, sizeof(buf));
    sc1(SYS_close, fd);
    if (rc <= 0) {
        check(0, "/dev lists at least one entry");
        return;
    }
    {
        i64 off = 0;
        while (off < rc) {
            const char *name = buf + off + 19;
            unsigned short reclen =
                (unsigned short)((unsigned char)buf[off + 16] |
                                 ((unsigned char)buf[off + 17] << 8));
            if (reclen == 0) break;
            if (name[0] == 'H' && name[1] == 'a' && name[2] == 'r') {
                saw_volume = 1;              /* HarddiskVolume<N> */
            }
            if (name[1] == ':' && name[2] == '\0' &&
                name[0] >= 'C' && name[0] <= 'Z') {
                saw_letter = 1;
            }
            off += reclen;
        }
    }

    /* The control. Without it every check below reads as a pass on a system
     * with no storage at all. */
    check(saw_volume, "at least one \\Device\\HarddiskVolumeN is in /dev");
    if (!saw_volume) {
        return;
    }

    /* A: and B: are never assigned - the rule volume.c states. Asserted as a
     * NEGATIVE, so the positive above is what stops it passing vacuously: a
     * kernel that assigned no letters at all would pass this line and fail
     * the next. */
    check(saw_letter, "and a drive letter was assigned to something");
    {
        int a_or_b = 0;
        fd = (int)sc4(SYS_openat, AT_FDCWD, "/dev/A:", O_RDONLY, 0);
        if (fd >= 0) { a_or_b = 1; sc1(SYS_close, fd); }
        fd = (int)sc4(SYS_openat, AT_FDCWD, "/dev/B:", O_RDONLY, 0);
        if (fd >= 0) { a_or_b = 1; sc1(SYS_close, fd); }
        check(!a_or_b, "and neither A: nor B: was handed out");
    }

    /* The remainder parse: a path THROUGH a volume device resolves to a file.
     *
     * This is the whole of item 1 in one line. \Device\HarddiskVolumeN
     * resolves to the volume object, "\etc\motd" is handed to its parse op,
     * and what comes back is a readable file. Before the volume device
     * existed this was -ENOTDIR by construction. */
    fd = (int)sc4(SYS_openat, AT_FDCWD, "/dev/HarddiskVolume1/etc/motd",
                  O_RDONLY, 0);
    if (fd >= 0) {
        char c[8];
        i64 got = sc3(SYS_read, fd, c, sizeof(c));
        sc1(SYS_close, fd);
        check(got > 0, "a path through a volume device reads the file");
    } else {
        /* Not a failure on its own: the root volume may not be number 1 if a
         * disk was scanned before it. Reported rather than passed silently,
         * because a check that cannot run is not a check that passed. */
        check(0, "a path through a volume device reads the file "
                 "(volume 1 not present - check volume_report output)");
    }

    /* A character device has no parse op, so a remainder on one is -ENOTDIR -
     * and it comes from the NULL slot rather than from a call site deciding
     * it for every device. The positive case above is what makes this
     * meaningful: without it, a kernel that refused every remainder would
     * pass here too. */
    rc = sc4(SYS_openat, AT_FDCWD, "/dev/console/nonsense", O_RDONLY, 0);
    check(rc == -20 || rc == -2,
          "but a remainder on a character device is refused");
}

/* --- GPT --------------------------------------------------------------- */

/* CRC-32 as GPT specifies it, computed here independently of the kernel's.
 *
 * This is the check that cannot be made any other way. A CRC implementation
 * that is wrong in a CONSISTENT way validates every table it computes itself
 * and rejects every real one - and there is no way to notice that from inside
 * the kernel, because the kernel only ever compares its own output against
 * a disk written by a tool it cannot run. The known vector below is the
 * standard "123456789" check value, 0xCBF43926, which every conforming
 * CRC-32 agrees on. */
static unsigned int crc32_ref(const unsigned char *p, unsigned long n) {
    unsigned int crc = 0xFFFFFFFFu;
    unsigned long i;
    int k;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (unsigned int)(-(int)(crc & 1u)));
        }
    }
    return ~crc;
}

static void test_gpt_crc(void) {
    section("GPT checksum");

    check(crc32_ref((const unsigned char *)"123456789", 9) == 0xCBF43926u,
          "the reference CRC-32 matches the standard check value");

    /* The kernel's own part_crc32 cannot be called from ring 3, so what this
     * proves is only that the ALGORITHM in part.c - which is this same loop,
     * transcribed - agrees with the published vector. That is a PART B item
     * for the kernel side, and it is in the queue below. */
}

/* --- writing file content, from ring 3 -----------------------------------
 *
 * ROADMAP item 7(b) and the oldest entry in the Owed list: fatfs's .write was
 * NULL, so file CONTENT was read-only everywhere in this kernel. It is not
 * any more, and this is the whole path - open(O_RDWR), write(2), fileobj,
 * fs_write, fatfs_write, fat_write_entry_at, the block layer, the disk.
 *
 * SELF-RESTORING, and that is a constraint rather than a courtesy: this runs
 * against the real staged root, so a test that left different bytes in
 * /etc/motd would change what every later run of systest reads. The write is
 * therefore the SAME LENGTH as the original and the original is put back at
 * the end - which also makes the restore a second write, checked by the same
 * reader.
 *
 * It cannot test growth. Extending a file needs either O_CREAT (to make a
 * scratch file) or O_TRUNC (to shrink one back), and both are still refused
 * by sys_openat - see PART B. Growth is covered on the host instead, where an
 * image can be thrown away: tests/host/fat_write_test.c. */
static void test_file_write(void) {
    char original[64];
    char readback[64];
    i64  n;
    int  fd;
    int  i;

    section("write(2) on a file");

    fd = (int)sc4(SYS_openat, AT_FDCWD, "/etc/motd", O_RDWR, 0);
    check(fd >= 0, "a file opens O_RDWR now that the volume is writable");
    if (fd < 0) {
        return;
    }

    n = sc3(SYS_read, fd, original, sizeof(original));
    check(n > 0, "and reads");
    if (n <= 0) {
        sc1(SYS_close, fd);
        return;
    }

    /* Same length, different bytes. A different length would need a truncate
     * to undo. */
    for (i = 0; i < (int)n; i++) {
        readback[i] = (char)('A' + (i % 26));
    }

    check_eq(sc3(SYS_lseek, fd, 0, 0), 0, "seek back to the start");
    check_eq(sc3(SYS_write, fd, readback, (u64)n), n,
             "and write(2) reports every byte written");
    sc1(SYS_close, fd);

    /* Reopened, so the bytes come back off the volume rather than out of
     * whatever the descriptor was holding. */
    fd = (int)sc4(SYS_openat, AT_FDCWD, "/etc/motd", O_RDONLY, 0);
    check(fd >= 0, "the file reopens");
    if (fd >= 0) {
        char again[64];
        int same = 1;

        n = sc3(SYS_read, fd, again, sizeof(again));
        for (i = 0; i < (int)n; i++) {
            if (again[i] != (char)('A' + (i % 26))) {
                same = 0;
            }
        }
        check(same, "and reads back what was written - through the block "
                    "layer and onto the disk");
        sc1(SYS_close, fd);
    }

    /* Put it back, and check the restore rather than assuming it. A test that
     * corrupts the root and says nothing is worse than one that fails. */
    fd = (int)sc4(SYS_openat, AT_FDCWD, "/etc/motd", O_RDWR, 0);
    if (fd >= 0) {
        sc3(SYS_write, fd, original, (u64)n);
        sc1(SYS_close, fd);
    }
    fd = (int)sc4(SYS_openat, AT_FDCWD, "/etc/motd", O_RDONLY, 0);
    if (fd >= 0) {
        char again[64];
        int same = 1;

        sc3(SYS_read, fd, again, sizeof(again));
        for (i = 0; i < (int)n; i++) {
            if (again[i] != original[i]) {
                same = 0;
            }
        }
        check(same, "and the original content is restored - this test leaves "
                    "no trace on the staged root");
        sc1(SYS_close, fd);
    }
}

/* --- O_CREAT, O_TRUNC, O_APPEND and ftruncate ----------------------------
 *
 * The way IN to the write path. write(2) worked on a file that already
 * existed and nothing else: sys_openat refused all three flags with -EROFS,
 * so no program that opens a file for output could get started.
 *
 * Self-cleaning: the file this makes is unlinked at the end, and the unlink
 * is checked rather than assumed. A test that leaves a file on the staged
 * root changes what `ls /` prints for every run after it.
 */
static void test_create_truncate(void) {
    const char *path = "/vtest.txt";
    char buf[64];
    i64  n;
    int  fd;

    section("O_CREAT / O_TRUNC / O_APPEND / ftruncate");

    /* Clean up anything a previous aborted run left, so the -EEXIST check
     * below is testing O_EXCL and not the state of the disk. */
    sc1(SYS_unlink, path);

    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_CREAT | O_RDWR, 0644);
    check(fd >= 0, "O_CREAT makes a file that did not exist");
    if (fd < 0) {
        return;
    }
    check_eq(sc3(SYS_write, fd, "hello", 5), 5, "and it can be written");
    sc1(SYS_close, fd);

    /* O_EXCL is the half that has to REFUSE. Without it every O_CREAT is
     * silently create-or-open, and a lock file means nothing. */
    check_eq(sc4(SYS_openat, AT_FDCWD, path, O_CREAT | O_EXCL | O_RDWR, 0644),
             -EEXIST, "O_CREAT|O_EXCL on an existing file is -EEXIST");
    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_CREAT | O_RDWR, 0644);
    check(fd >= 0, "but plain O_CREAT on an existing file just opens it");

    /* And it did not empty the file - that is O_TRUNC's job, and an O_CREAT
     * that truncated would silently destroy data. */
    n = sc3(SYS_read, fd, buf, sizeof(buf));
    check(n == 5, "without truncating what was already in it");
    sc1(SYS_close, fd);

    /* --- O_APPEND ---------------------------------------------------- */
    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_WRONLY | O_APPEND, 0);
    check(fd >= 0, "O_APPEND opens");
    if (fd >= 0) {
        /* Deliberately seek to the START first. O_APPEND must override it:
         * the flag means "seek to the end before EVERY write", not "start at
         * the end", and an implementation that only positions at open passes
         * without this line. */
        sc3(SYS_lseek, fd, 0, 0);
        check_eq(sc3(SYS_write, fd, "world", 5), 5, "and writes");
        sc1(SYS_close, fd);
    }
    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd >= 0) {
        n = sc3(SYS_read, fd, buf, sizeof(buf));
        check(n == 10, "which APPENDED rather than overwrote");
        check(n == 10 && buf[0] == 'h' && buf[5] == 'w',
              "and landed after the existing content, not on top of it");
        sc1(SYS_close, fd);
    }

    /* --- ftruncate ----------------------------------------------------- */
    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_RDWR, 0);
    if (fd >= 0) {
        check_eq(sc2(SYS_ftruncate, fd, 4), 0, "ftruncate shortens a file");
        check_eq(sc2(SYS_ftruncate, fd, -1), -EINVAL,
                 "and a negative length is -EINVAL, not four billion");
        sc1(SYS_close, fd);
    }
    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd >= 0) {
        n = sc3(SYS_read, fd, buf, sizeof(buf));
        check(n == 4, "and the file really is shorter afterwards");
        /* ftruncate on a read-only descriptor is -EINVAL: the descriptor is
         * valid, the request is not one it can carry. */
        check_eq(sc2(SYS_ftruncate, fd, 0), -EINVAL,
                 "ftruncate through a read-only descriptor is refused");
        sc1(SYS_close, fd);
    }

    /* --- truncate(2) by path, and O_TRUNC ------------------------------- */
    check_eq(sc2(SYS_truncate, path, 2), 0, "truncate(2) works by path");
    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd >= 0) {
        check_eq(sc3(SYS_read, fd, buf, sizeof(buf)), 2, "and shortened it");
        sc1(SYS_close, fd);
    }

    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_WRONLY | O_TRUNC, 0);
    check(fd >= 0, "O_TRUNC opens");
    sc1(SYS_close, fd);
    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd >= 0) {
        check_eq(sc3(SYS_read, fd, buf, sizeof(buf)), 0,
                 "and emptied the file");
        sc1(SYS_close, fd);
    }
    /* O_TRUNC without write access is refused rather than silently ignored -
     * emptying a file opened read-only is the one reading nobody wants. */
    check_eq(sc4(SYS_openat, AT_FDCWD, path, O_RDONLY | O_TRUNC, 0), -EINVAL,
             "O_TRUNC without write access is refused, not ignored");

    /* --- access is still ENFORCED, now that writing is possible ---------
     *
     * This check exists because of what changed, not because of what did
     * not. While the volume was read-only, "a read-only descriptor cannot
     * write" was true for a reason that had nothing to do with descriptors -
     * nothing could write at all. Now that it can, the only thing standing
     * between an O_RDONLY handle and the file is the access check in
     * do_write, and an untested guard on a newly reachable path is the shape
     * most privilege bugs arrive in.
     *
     * Both directions, because a negative-only check cannot tell enforcement
     * from blanket refusal: the write that SHOULD work is asserted two lines
     * later. */
    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
    if (fd >= 0) {
        check_eq(sc3(SYS_write, fd, "no", 2), -EBADF,
                 "writing through a READ-ONLY descriptor is refused");
        sc1(SYS_close, fd);
    }
    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_WRONLY, 0);
    if (fd >= 0) {
        check_eq(sc3(SYS_write, fd, "yes", 3), 3,
                 "and through a write descriptor it works - the refusal is "
                 "the ACCESS MODE, not a read-only volume");
        check_eq(sc3(SYS_read, fd, buf, sizeof(buf)), -EBADF,
                 "while READING through a write-only descriptor is refused "
                 "in the same way");
        sc1(SYS_close, fd);
    }

    /* --- and leave nothing behind --------------------------------------- */
    check_eq(sc1(SYS_unlink, path), 0, "the test file is removed");
    check_eq(sc2(SYS_access, path, F_OK), -ENOENT,
             "and really is gone - this test leaves no trace");
}

/* --- statfs -------------------------------------------------------------
 *
 * The last entry from the group the Owed list said was waiting on the write
 * path, and the only one that never was: statfs needs FREE SPACE, which on
 * FAT means counting zero entries in the allocation table.
 *
 * The check that makes this real is the LAST one. Every field can be
 * plausible and constant - a statfs that returned a fixed table would pass
 * everything above it - so the test writes a file big enough to consume
 * clusters and requires the free count to have gone DOWN. That is the
 * difference between reading the allocation table and reciting the
 * superblock.
 */
static void test_statfs(void) {
    u64 st[15];              /* struct statfs is 120 bytes */
    u64 st2[15];
    const char *path = "/vstatfs.txt";
    static char block[8192];
    u64 free_before, free_after;
    int fd, i;

    section("statfs");

    check_eq(sc2(SYS_statfs, "/", st), 0, "statfs on the root succeeds");
    check_eq(st[0], 0x4D44, "and reports the FAT magic number");
    check(st[1] >= 512, "with a plausible block size");
    check(st[2] > 0, "and a non-zero block count");
    check(st[3] <= st[2], "free blocks do not exceed total blocks");
    check_eq(st[4], st[3],
             "f_bavail equals f_bfree - there is no reserved pool, and "
             "inventing one would be inventing a policy nothing implements");
    check_eq(st[8], 12, "f_namelen is 12 - 8.3 plus the dot");
    check_eq(st[5], 0,
             "f_files is 0 - FAT has no inode table, which is the honest "
             "answer and the one Linux's own vfat gives");

    check_eq(sc2(SYS_statfs, "/no/such/path", st), -ENOENT,
             "statfs on a missing path is -ENOENT");
    check_eq(sc2(SYS_statfs, "/etc/motd", st2), 0,
             "statfs on a FILE works - it describes the volume, not the file");
    check_eq(st2[2], st[2],
             "and gives the same volume, whichever path names it");

    /* --- the free count actually MOVES ---------------------------------- */
    free_before = st[3];

    sc1(SYS_unlink, path);
    fd = (int)sc4(SYS_openat, AT_FDCWD, path, O_CREAT | O_WRONLY, 0644);
    check(fd >= 0, "a file can be made to consume space with");
    if (fd >= 0) {
        for (i = 0; i < (int)sizeof(block); i++) {
            block[i] = (char)i;
        }
        check_eq(sc3(SYS_write, fd, block, sizeof(block)),
                 (i64)sizeof(block), "and filled");
        sc1(SYS_close, fd);
    }

    check_eq(sc2(SYS_statfs, "/", st2), 0, "statfs again");
    free_after = st2[3];
    check(free_after < free_before,
          "and the FREE COUNT WENT DOWN - the number is read off the "
          "allocation table, not recited from the superblock");
    if (free_after >= free_before) {
        out("        free was ");
        out_i64((i64)free_before);
        out(" and is now ");
        out_i64((i64)free_after);
        out("\n");
    }

    /* And back up again when the space is returned, which a counter that
     * only ever decreases would fail. */
    check_eq(sc1(SYS_unlink, path), 0, "the file is removed");
    check_eq(sc2(SYS_statfs, "/", st2), 0, "statfs once more");
    check_eq(st2[3], free_before,
             "and the free count is back exactly where it started");
}

/* --- main ---------------------------------------------------------------- */

void _start(void) {
    out("verification: work that compiles and has never run\n");

    test_fcntl();
    test_access();
    test_poll_basics();
    test_poll_pipe();
    test_poll_timeout();
    test_poll_blocking();
    test_nonblock();
    test_ppoll();
    test_wx();
    test_futex();
    test_clone_args();
    test_fs_vtable();
    test_block_device();
    test_cputime();
    test_volumes();
    test_gpt_crc();
    test_file_write();
    test_create_truncate();
    test_statfs();

    out("\nverification: ");
    out_i64(passes);
    out(" passed, ");
    out_i64(failures);
    out(" failed\n");

    print_manual_queue();

    sc1(SYS_exit_group, failures > 255 ? 255 : failures);
    __builtin_unreachable();
}
