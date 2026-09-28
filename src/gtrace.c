/* gtrace CMD [ARGS...] - run CMD with the kernel's syscall trace on.
 *
 * Every Linux system call CMD makes - and everything it forks and execs -
 * is printed on the console as it enters ("[pid] name(args)") and as it
 * returns ("[pid] nr = result"). The trace is the kernel's, switched on for
 * this process by prctl(PR_GENESIS_TRACE, 1) and inherited from here on; see
 * the tracing block in kernel/proc/syscall.c.
 *
 * It is the tool for "the program went quiet": the last line printed is the
 * call it is sitting in. */
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#define PR_GENESIS_TRACE 0x47454e04

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: gtrace CMD [ARGS...]\n");
        return 2;
    }
    if (prctl(PR_GENESIS_TRACE, 1, 0, 0, 0) != 0) {
        perror("gtrace: prctl");
        return 1;
    }
    execv(argv[1], argv + 1);
    perror("gtrace: execv");
    return 127;
}
