/* mhello - an ordinary C program.
 *
 * The point of this file is what is NOT in it. No syscall numbers, no inline
 * assembly, no _start, no knowledge that Genesis exists. It is what you would
 * write on any Unix, compiled by a stock x86_64-linux-musl toolchain with no
 * flags Genesis chose, and if it runs then the claim "Genesis's ELF
 * personality IS the Linux x86-64 ABI" is true in the only way that counts.
 *
 * src/hello.c is the other half of the pair and is the opposite: raw syscall
 * instructions, -nostdlib, its own _start. That one proves the kernel's
 * syscall entry works. This one proves a libc's assumptions about the kernel
 * hold - which is a much larger surface, and includes a great many things
 * nobody wrote down: that brk grows, that the auxv is complete enough to find
 * PT_TLS, that writev reports a count rather than a status, that a failed
 * open returns the errno the caller expects.
 *
 * Each line below is chosen to exercise something specific:
 *
 *   stdio          buffered output over writev, and the isatty/ioctl probe
 *                  musl makes to decide whether stdout is line-buffered
 *   malloc         brk and mmap, through musl's allocator rather than a
 *                  bump pointer of ours
 *   fopen/fgets    open, read, close and the FILE layer over them
 *   time+gmtime    clock_gettime, and the calendar conversion coming out the
 *                  same as the kernel's own rtc_days_from_civil
 *   nanosleep      the timed-wakeup path in the scheduler
 *   argv/environ   the stack the kernel built, read the way the ABI says
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv)
{
    char *buf;
    FILE *f;
    struct timespec ts;
    time_t now;
    struct tm tm;
    int i;

    puts("mhello: a stock musl-linked ELF, running on Genesis");

    buf = malloc(256);
    if (!buf) {
        puts("malloc failed");
        return 1;
    }
    strcpy(buf, "malloc/strcpy/free work");
    puts(buf);
    free(buf);

    printf("argc=%d argv[0]=%s\n", argc, argv[0]);
    for (i = 0; environ[i] && i < 4; i++)
        printf("environ[%d]=%s\n", i, environ[i]);
    if (i == 0)
        puts("environ is empty");

    f = fopen("/etc/motd", "r");
    if (f) {
        char line[128];
        if (fgets(line, sizeof line, f))
            printf("/etc/motd via stdio: %s", line);
        fclose(f);
    } else {
        perror("/etc/motd");
    }

    /* time() goes through clock_gettime on this kernel - there is no vDSO to
     * shortcut it - so a plausible date here means the CMOS read at boot and
     * the tick-derived clock both work. A date in 1970 means the RTC could
     * not be read, which the kernel says at boot. */
    now = time(NULL);
    if (gmtime_r(&now, &tm)) {
        char when[64];
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", &tm);
        printf("time is %s (epoch %lld)\n", when, (long long)now);
    }

    /* Short enough not to be noticed, long enough to cross a 10ms tick and
     * actually reach the sleep path rather than returning immediately. */
    ts.tv_sec = 0;
    ts.tv_nsec = 30000000;
    if (nanosleep(&ts, NULL) == 0)
        puts("nanosleep returned cleanly");
    else
        perror("nanosleep");

    puts("mhello: done");
    return 0;
}
