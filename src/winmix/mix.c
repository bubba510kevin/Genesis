/* mix.exe - one process, both kinds of code. ROADMAP item 19's ring-3 check.
 *
 * Imports kernel32.dll alone. Stage 1 is what makes the rest possible: the
 * kernel picks the system-call table per CALL, not per process. A call whose
 * number carries NT_SYSCALL_TAG (0x4E54 in bits 16-31, added by Genesis's
 * ntdll) is an NT call; anything else is a Linux call - even here, in a
 * process that was started from a PE image. That is what lets a Linux .so
 * loaded into a Windows program make the Linux calls it was compiled to make.
 *
 * What it proves, with raw `syscall` instructions standing in for that .so:
 *
 *   - Linux calls from a PE process reach the Linux table and behave as
 *     Linux calls: getpid agrees with GetCurrentProcessId, uname says
 *     Genesis, a file opened and read through openat/read has the same bytes
 *     kernel32's ReadFile sees, anonymous mmap/munmap work, and errors come
 *     back as negated errnos, not NTSTATUS;
 *   - the same small number means different calls with and without the tag
 *     (untagged 3 is close(2); tagged 4 is NtClose);
 *   - win32k's range (untagged 0x1000-0x1FFF) is reserved in a Windows
 *     process: STATUS_INVALID_SYSTEM_SERVICE, not a Linux call.
 *
 * Every check prints ok/FAIL; the last line is a tally in systest's shape
 * so tools/guest_run.py can wait for it.
 */

#include "../kernel32/kernel32.h"

static HANDLE out;
static int passes, failures;

static void say(const char *s) {
    DWORD n = 0, written = 0;

    while (s[n] != '\0') {
        n++;
    }
    WriteFile(out, s, n, &written, NULL_PTR);
}

static void say_u(DWORD v) {
    char buf[12];
    int i = 11;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    say(&buf[i]);
}

static void check(int cond, const char *what) {
    say(cond ? "  ok    " : "  FAIL  ");
    say(what);
    say("\r\n");
    if (cond) passes++; else failures++;
}

/* A raw Linux system call: number in RAX, arguments in RDI, RSI, RDX, R10,
 * R8, R9. No tag - which is the whole point. */
static long long lsys(long long n, long long a, long long b, long long c,
                      long long d, long long e, long long f) {
    long long r;
    register long long r10 __asm__("r10") = d;
    register long long r8  __asm__("r8")  = e;
    register long long r9  __asm__("r9")  = f;

    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c),
                        "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return r;
}

#define L_READ     0
#define L_CLOSE    3
#define L_MMAP     9
#define L_MUNMAP   11
#define L_GETPID   39
#define L_UNAME    63
#define L_OPENAT   257
#define AT_FDCWD   (-100)

#define NT_TAG                         0x4E540000LL
#define STATUS_INVALID_HANDLE          0xC0000008u
#define STATUS_INVALID_SYSTEM_SERVICE  0xC000001Cu

static int same_bytes(const char *a, const char *b, DWORD n) {
    DWORD i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int has_genesis(const char *s) {
    const char *w = "Genesis";
    int i, j;

    for (i = 0; s[i] != '\0'; i++) {
        for (j = 0; w[j] != '\0' && s[i + j] == w[j]; j++) {
        }
        if (w[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

static void routing_tests(void) {
    char uts[390];
    char lbuf[64], wbuf[64];
    long long fd, n, addr;
    DWORD got = 0;
    HANDLE f;
    volatile char *p;

    say("-- Linux calls from a PE process (per-call routing)\r\n");

    check(lsys(L_GETPID, 0, 0, 0, 0, 0, 0) == (long long)GetCurrentProcessId(),
          "getpid(2) agrees with GetCurrentProcessId");

    check(lsys(L_UNAME, (long long)uts, 0, 0, 0, 0, 0) == 0 &&
          has_genesis(uts + 195),
          "uname(2) answers, and its version field says Genesis");

    fd = lsys(L_OPENAT, AT_FDCWD, (long long)"/etc/motd", 0, 0, 0, 0);
    check(fd >= 0, "openat(2) opens /etc/motd");
    n = lsys(L_READ, fd, (long long)lbuf, sizeof(lbuf), 0, 0, 0);
    check(n > 0, "read(2) reads it");
    check(lsys(L_CLOSE, fd, 0, 0, 0, 0, 0) == 0, "close(2) closes it");

    f = CreateFileW(L"C:\\etc\\motd", GENERIC_READ, 0, NULL_PTR, OPEN_EXISTING,
                    0, NULL_PTR);
    ReadFile(f, wbuf, sizeof(wbuf), &got, NULL_PTR);
    CloseHandle(f);
    check(n > 0 && (long long)got == n && same_bytes(lbuf, wbuf, got),
          "the same bytes kernel32's ReadFile reads through C:\\etc\\motd");

    addr = lsys(L_MMAP, 0, 8192, 3 /* READ|WRITE */, 0x22 /* PRIVATE|ANON */,
                -1, 0);
    check(addr > 0 && (addr & 0xFFF) == 0, "anonymous mmap(2) returns a page");
    if (addr > 0) {
        p = (volatile char *)addr;
        p[0] = 'x';
        p[8191] = 'y';
        check(p[0] == 'x' && p[8191] == 'y', "and the mapping is usable");
        check(lsys(L_MUNMAP, addr, 8192, 0, 0, 0, 0) == 0, "munmap(2) unmaps it");
    }

    check(lsys(L_CLOSE, 12345, 0, 0, 0, 0, 0) == -9,
          "a Linux error is a negated errno (close of a bad fd: -EBADF)");

    say("-- the tag decides, not the process\r\n");
    check((unsigned)lsys(NT_TAG | 0x04, 12345, 0, 0, 0, 0, 0) == STATUS_INVALID_HANDLE,
          "tagged 4 is NtClose: STATUS_INVALID_HANDLE");
    check(lsys(L_CLOSE, 12345, 0, 0, 0, 0, 0) == -9,
          "untagged 3 is close(2), in the same process");
    check((unsigned)lsys(0x1000, 0, 0, 0, 0, 0, 0) == STATUS_INVALID_SYSTEM_SERVICE,
          "untagged 0x1000 (win32k's range) is reserved, not a Linux call");
    check((unsigned)lsys(0x1FFF, 0, 0, 0, 0, 0, 0) == STATUS_INVALID_SYSTEM_SERVICE,
          "and so is 0x1FFF");
}

void start(void) {
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("mix: PE and ELF code in one process\r\n");

    routing_tests();

    say("mix: ");
    say_u((DWORD)passes);
    say(" passed, ");
    say_u((DWORD)failures);
    say(" failed\r\n");
    ExitProcess(failures == 0 ? 0 : 1);
}
