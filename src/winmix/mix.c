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

/* --- stage 3: a Linux .so in this process --------------------------------
 *
 * src/somix/libmixa.so and its dependency libmixb.so - freestanding shared
 * objects built by the host gcc - loaded with LoadLibrary and called through
 * GetProcAddress, as any Windows program would. */

typedef int       (WINAPI *w_v_t)(void);
typedef int       (WINAPI *w_i_t)(int);
typedef int       (WINAPI *w_ii_t)(int, int);
typedef long long (WINAPI *w_14_t)(long long, long long, long long, long long,
                                   long long, long long, long long, long long,
                                   long long, long long, long long, long long,
                                   long long, long long);
typedef double    (WINAPI *w_dd_t)(double, double);
typedef const char *(WINAPI *w_name_t)(int);
typedef long long (WINAPI *w_str_t)(const char *);
typedef long long (WINAPI *w_l_t)(void);
typedef int       (WINAPI *w_si_t)(const char *);
typedef int       (WINAPI *w_fmt_t)(char *, int, int, int);

/* The raw System V symbol, for a signature the adapter cannot translate
 * (an int then a double) and for a callback the .so calls back into. */
typedef double (__attribute__((sysv_abi)) *s_id_t)(int, double);
typedef int    (__attribute__((sysv_abi)) *s_cb_t)(int);
typedef int    (WINAPI *w_callback_t)(s_cb_t, int);

static __attribute__((sysv_abi)) int sysv_triple(int v) {
    return v * 3;
}

/* Holds known values in every register Win64 says survives a call - RBX,
 * RBP, RSI, RDI, R12-R15, XMM6, XMM15 - across a call to `fn` (in RCX),
 * and returns 1 if they all did and fn returned 7. a_clobber, through the
 * adapter, zeroes RSI, RDI and XMM6-15 the way any System V function may:
 * only the adapter saving them makes this 1. */
int probe_adapter(FARPROC fn);
__asm__(
".text\n"
".globl probe_adapter\n"
"probe_adapter:\n"
"    pushq %rbx\n  pushq %rbp\n  pushq %rsi\n  pushq %rdi\n"
"    pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
"    subq $0x48, %rsp\n"
"    movdqu %xmm6, 0x20(%rsp)\n  movdqu %xmm15, 0x30(%rsp)\n"
"    movq %rcx, %r11\n"
"    movabsq $0x1111111111111111, %rbx\n"
"    movabsq $0x2222222222222222, %rbp\n"
"    movabsq $0x3333333333333333, %rsi\n"
"    movabsq $0x4444444444444444, %rdi\n"
"    movabsq $0x5555555555555555, %r12\n"
"    movabsq $0x6666666666666666, %r13\n"
"    movabsq $0x7777777777777777, %r14\n"
"    movabsq $0x8888888888888888, %r15\n"
"    movabsq $0x9999999999999999, %rax\n"
"    movq %rax, %xmm6\n  movq %rax, %xmm15\n"
"    call *%r11\n"
"    movl %eax, %r8d\n"
"    xorl %eax, %eax\n"
"    movabsq $0x1111111111111111, %rcx\n  cmpq %rcx, %rbx\n  jne 9f\n"
"    movabsq $0x2222222222222222, %rcx\n  cmpq %rcx, %rbp\n  jne 9f\n"
"    movabsq $0x3333333333333333, %rcx\n  cmpq %rcx, %rsi\n  jne 9f\n"
"    movabsq $0x4444444444444444, %rcx\n  cmpq %rcx, %rdi\n  jne 9f\n"
"    movabsq $0x5555555555555555, %rcx\n  cmpq %rcx, %r12\n  jne 9f\n"
"    movabsq $0x6666666666666666, %rcx\n  cmpq %rcx, %r13\n  jne 9f\n"
"    movabsq $0x7777777777777777, %rcx\n  cmpq %rcx, %r14\n  jne 9f\n"
"    movabsq $0x8888888888888888, %rcx\n  cmpq %rcx, %r15\n  jne 9f\n"
"    movabsq $0x9999999999999999, %rdx\n"
"    movq %xmm6, %rcx\n  cmpq %rdx, %rcx\n  jne 9f\n"
"    movq %xmm15, %rcx\n  cmpq %rdx, %rcx\n  jne 9f\n"
"    cmpl $7, %r8d\n  jne 9f\n"
"    movl $1, %eax\n"
"9:\n"
"    movdqu 0x20(%rsp), %xmm6\n  movdqu 0x30(%rsp), %xmm15\n"
"    addq $0x48, %rsp\n"
"    popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n"
"    popq %rdi\n  popq %rsi\n  popq %rbp\n  popq %rbx\n"
"    ret\n"
);

static int str_is(const char *a, const char *b) {
    if (a == NULL_PTR) {
        return 0;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void so_tests(void) {
    HMODULE so, so2, dep;
    FARPROC clob;

    say("-- a Linux .so in a Windows program (LoadLibrary)\r\n");

    so = LoadLibraryA("C:\\lib\\libmixa.so");
    check(so != NULL_PTR, "LoadLibraryA(\"C:\\lib\\libmixa.so\") loads it");
    if (so == NULL_PTR) {
        return;
    }
    so2 = LoadLibraryA("libmixa.so");
    check(so2 == so, "a bare \"libmixa.so\" is found in /lib: the same module");
    dep = LoadLibraryA("libmixb.so");
    check(dep != NULL_PTR && dep != so,
          "its DT_NEEDED, libmixb.so, came with it (already loaded)");
    check(GetProcAddress(so, "no_such_symbol") == NULL_PTR &&
          GetLastError() == ERROR_PROC_NOT_FOUND,
          "a missing symbol: NULL, ERROR_PROC_NOT_FOUND");
    check(LoadLibraryA("nosuch.so") == NULL_PTR && GetLastError() == ERROR_MOD_NOT_FOUND,
          "a missing .so: NULL, ERROR_MOD_NOT_FOUND");

    check(((w_v_t)GetProcAddress(so, "a_ctor"))() == 42,
          "its constructor (.init_array) ran");
    check(((w_ii_t)GetProcAddress(so, "a_add"))(40, 2) == 42,
          "a_add(40, 2) = 42 - through its PLT into libmixb (JUMP_SLOT)");
    check(((w_i_t)GetProcAddress(so, "a_call_fp"))(5) == 6,
          "a function pointer in its data (R_X86_64_64) calls libmixb");
    check(str_is(((w_name_t)GetProcAddress(so, "a_name"))(2), "two"),
          "its own pointer table (R_X86_64_RELATIVE): a_name(2) = \"two\"");
    check(((w_v_t)GetProcAddress(so, "a_bump"))() == 1 &&
          ((w_v_t)GetProcAddress(so, "a_bump"))() == 2 &&
          ((w_ii_t)GetProcAddress(dep, "b_add"))(1, 1) == 4,
          "libmixb's data through libmixa's GOT (GLOB_DAT): one variable");
    check(((w_14_t)GetProcAddress(so, "a_sum14"))(1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                                   11, 12, 13, 14) == 1015,
          "fourteen arguments through the adapter: a_sum14 = 1015");
    check(((w_dd_t)GetProcAddress(so, "a_hyp"))(3.0, 4.0) == 25.0,
          "leading doubles through the adapter: a_hyp(3, 4) = 25");
    check(((s_id_t)GenesisGetElfProcAddress(so, "a_mixed"))(4, 2.5) == 10.0,
          "an int then a double, raw System V: a_mixed(4, 2.5) = 10");
    check(((w_callback_t)GetProcAddress(so, "a_callback"))(sysv_triple, 5) == 30,
          "the .so calls back into this program: 5*3*2 = 30");
    clob = GetProcAddress(so, "a_clobber");
    check(clob != NULL_PTR && probe_adapter(clob),
          "the adapter preserves RSI, RDI, XMM6-15 (System V destroys them)");

    check(((w_l_t)GetProcAddress(so, "a_getpid"))() == (long long)GetCurrentProcessId(),
          "a raw Linux getpid(2) from inside the .so is this process");
    check(((w_str_t)GetProcAddress(so, "a_write"))("  (written by the .so with write(2))\r\n") == 38,
          "and its write(2) to fd 1 reaches the console");
}

/* --- a Linux .so that USES libc, in a Windows process (GNTlibc) ------------
 *
 * src/somix/libmixc.so is an ordinary musl-linked shared object: it calls
 * malloc, snprintf, strtol, the string functions and libm, and it reads
 * errno. Its DT_NEEDED is libc.so - GNTlibc, musl built as a Genesis shared
 * object. libc.so's constructor installs a thread pointer before any of this
 * runs, which is the whole point: none of these functions work in a PE
 * process without it. Each returns a value we check, so the test does not
 * depend on where fd 1 is wired. */

static void libc_tests(void) {
    HMODULE c;
    char buf[32];
    FARPROC f;

    say("-- a Linux .so that uses libc, in a Windows program (GNTlibc)\r\n");

    c = LoadLibraryA("C:\\lib\\libmixc.so");
    if (c == NULL_PTR) {
        /* Not a failure: libmixc.so (and libc.so) are built only when a
         * GNTlibc checkout was present at build time. Where it is absent,
         * skip these checks rather than fail the suite. */
        say("   (skipped: libmixc.so not present - GNTlibc not built)\r\n");
        return;
    }
    check(1, "LoadLibraryA(\"C:\\lib\\libmixc.so\"): libc.so came with it");

    check(((w_v_t)GetProcAddress(c, "c_ctor_val"))() == 1234,
          "its constructor called malloc, after libc.so's set up the TCB");
    check(((w_si_t)GetProcAddress(c, "c_dup_len"))("genesis") == 7,
          "malloc + memcpy + strlen: c_dup_len(\"genesis\") = 7");
    check(((w_str_t)GetProcAddress(c, "c_csv_sum"))("10,20,3,9") == 42,
          "strtol over a malloc'd copy: c_csv_sum(\"10,20,3,9\") = 42");
    f = GetProcAddress(c, "c_format");
    check(f != NULL_PTR && ((w_fmt_t)f)(buf, (int)sizeof buf, 40, 2) == 7 &&
          str_is(buf, "40+2=42"),
          "snprintf into our buffer: \"40+2=42\", length 7");
    check(((w_dd_t)GetProcAddress(c, "c_hypot"))(3.0, 4.0) == 5.0,
          "libm through the adapter: c_hypot(3, 4) = 5");
    check(((w_v_t)GetProcAddress(c, "c_errno_ok"))() == 1,
          "errno is in the thread pointer: a failed fopen sets ENOENT");
}

/* --- LoadLibrary for an ordinary DLL (ROADMAP item 14(e)) ------------------ */

static void dll_tests(void) {
    HMODULE dll, again, k32;

    say("-- LoadLibrary of a DLL at run time\r\n");

    k32 = LoadLibraryA("kernel32.dll");
    check(k32 != NULL_PTR, "LoadLibraryA(\"kernel32.dll\"): already loaded, its base");
    check(k32 != NULL_PTR && GetProcAddress(k32, "GetCurrentProcessId") != NULL_PTR &&
          ((w_l_t)GetProcAddress(k32, "GetCurrentProcessId"))() ==
              (long long)GetCurrentProcessId(),
          "GetProcAddress on it returns the real GetCurrentProcessId");
    check(k32 != NULL_PTR && GetProcAddress(k32, "K32CurrentTeb") != NULL_PTR,
          "a forwarder (kernel32 -> ntdll.NtCurrentTeb) resolves");
    check(k32 != NULL_PTR && GetProcAddress(k32, (LPCSTR)(ULONG_PTR)42) != NULL_PTR,
          "and an ordinal-only export: kernel32 #42");

    dll = LoadLibraryA("mixdll");
    check(dll != NULL_PTR, "LoadLibraryA(\"mixdll\"): .dll added, found in System32");
    if (dll == NULL_PTR) {
        return;
    }
    check(((w_v_t)GetProcAddress(dll, "attached"))() == 1,
          "its DllMain ran once with DLL_PROCESS_ATTACH");
    again = LoadLibraryW(L"C:\\wsr\\System32\\mixdll.dll");
    check(again == dll && ((w_v_t)GetProcAddress(dll, "attached"))() == 1,
          "LoadLibraryW by full path: the same module, DllMain not rerun");
    check(((w_ii_t)GetProcAddress(dll, "add2"))(40, 2) == 42, "and add2(40, 2) = 42");
    check(FreeLibrary(dll), "FreeLibrary succeeds (nothing is unloaded yet)");
}

void start(void) {
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("mix: PE and ELF code in one process\r\n");

    routing_tests();
    dll_tests();
    so_tests();
    libc_tests();

    say("mix: ");
    say_u((DWORD)passes);
    say(" passed, ");
    say_u((DWORD)failures);
    say(" failed\r\n");
    ExitProcess(failures == 0 ? 0 : 1);
}
