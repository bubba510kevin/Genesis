/* elfmix - a Linux program that loads Windows DLLs (ROADMAP item 19, stage 2).
 *
 * An ordinary static musl program. Through libgnt it loads mixdll.dll, a
 * MinGW-built DLL that imports kernel32, and calls into it. What it proves:
 *
 *   - before the first load an NT call from this process is refused
 *     (STATUS_INVALID_SYSTEM_SERVICE: no NT environment); after it, the
 *     same call is served (STATUS_INVALID_HANDLE for a bad handle);
 *   - the DLL and its dependencies load, each entry point runs once, and a
 *     second load returns the same base without running it again;
 *   - exports resolve, including kernel32's forwarders into ntdll;
 *   - calls cross the ABI both ways: through GNT_WINAPI (ms_abi) pointers
 *     with integer, many-argument and floating-point signatures, through
 *     the System V adapters, and back from the DLL into a GNT_WINAPI
 *     callback here;
 *   - the NT environment is real: GetCurrentProcessId is getpid(),
 *     GetCurrentThreadId is gettid(), the last error is per thread, and
 *     WriteFile(GetStdHandle(STD_OUTPUT_HANDLE)) writes to this program's
 *     own stdout;
 *   - a thread created AFTER the load (pthread_create, i.e. clone) gets a
 *     TEB of its own automatically.
 *
 * The last line is a tally in systest's shape for tools/guest_run.py.
 */

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "../libgnt/gnt.h"

static int passes, failures;

static void check(int cond, const char *what) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (cond) passes++; else failures++;
}

static long raw_nt(long nr, long a) {
    long r;

    __asm__ volatile ("syscall" : "=a"(r) : "a"(nr), "D"(a)
                      : "rcx", "r11", "memory");
    return r;
}

#define NT_CLOSE                       (0x4E540000L | 0x04)
#define STATUS_INVALID_HANDLE          0xC0000008L
#define STATUS_INVALID_SYSTEM_SERVICE  0xC000001CL

typedef int         (GNT_WINAPI *fn_v_t)(void);
typedef int         (GNT_WINAPI *fn_ii_t)(int, int);
typedef long long   (GNT_WINAPI *fn_sum10_t)(long long, long long, long long,
                                             long long, long long, long long,
                                             long long, long long, long long,
                                             long long);
typedef double      (GNT_WINAPI *fn_di_t)(double, int);
typedef double      (GNT_WINAPI *fn_dd_t)(double, double);
typedef unsigned    (GNT_WINAPI *fn_u_t)(void);
typedef unsigned    (GNT_WINAPI *fn_uu_t)(unsigned);
typedef void        (GNT_WINAPI *fn_setu_t)(unsigned);
typedef int         (GNT_WINAPI *fn_say_t)(const char *);
typedef int         (GNT_WINAPI *cb_t)(int);
typedef int         (GNT_WINAPI *fn_cb_t)(cb_t, int);

/* The same functions through the System V adapters: plain prototypes. */
typedef int         (*sv_ii_t)(int, int);
typedef long long   (*sv_sum10_t)(long long, long long, long long, long long,
                                  long long, long long, long long, long long,
                                  long long, long long);
typedef double      (*sv_dd_t)(double, double);
typedef unsigned    (*sv_u_t)(void);

static GNT_WINAPI int triple(int v) {
    return v * 3;
}

static void *dll;
static fn_u_t tid_fn;
static fn_setu_t set_err_fn;
static fn_u_t get_err_fn;

struct thread_result {
    unsigned nt_tid;
    long     linux_tid;
    unsigned err_seen_first;
    unsigned err_after_set;
};

/* A thread that exists BEFORE the first DLL is loaded: nt_attach has to
 * give it a TEB of its own, and it picks the new GS base up on its way back
 * to ring 3 (it is spinning in a sched_yield loop meanwhile). */
static volatile int early_go, early_done;
static struct {
    long     linux_tid;
    unsigned nt_tid;
    unsigned err;
} early;

static void *early_main(void *arg) {
    (void)arg;
    early.linux_tid = syscall(SYS_gettid);
    while (!early_go) {
        sched_yield();
    }
    early.nt_tid = tid_fn();
    set_err_fn(99);
    early.err = get_err_fn();
    early_done = 1;
    return 0;
}

static void *thread_main(void *arg) {
    struct thread_result *r = arg;

    r->linux_tid = syscall(SYS_gettid);
    r->nt_tid = tid_fn();
    r->err_seen_first = get_err_fn();    /* a fresh TEB: 0 */
    set_err_fn(4242);
    r->err_after_set = get_err_fn();
    return 0;
}

int main(void) {
    void *again, *k32, *ntdll;
    char msg[64];
    int n;

    pthread_t early_thread;
    int early_ok;

    printf("elfmix: Windows DLLs in a Linux process\n");

    early_ok = pthread_create(&early_thread, 0, early_main, 0) == 0;

    check(raw_nt(NT_CLOSE, 12345) == STATUS_INVALID_SYSTEM_SERVICE,
          "before any DLL: an NT call is refused (no NT environment)");

    dll = gnt_pe_open("mixdll.dll");
    check(dll != 0, "gnt_pe_open(\"mixdll.dll\") loads it");
    if (dll == 0) {
        printf("  (gnt_pe_errno %d)\n", gnt_pe_errno());
        printf("elfmix: %d passed, %d failed\n", passes, failures + 1);
        return 1;
    }
    check(raw_nt(NT_CLOSE, 12345) == STATUS_INVALID_HANDLE,
          "after: the same NT call is served (STATUS_INVALID_HANDLE)");

    check(((fn_v_t)gnt_pe_sym(dll, "attached"))() == 1,
          "its entry point ran once, with DLL_PROCESS_ATTACH");
    again = gnt_pe_open("/wsr/System32/mixdll.dll");
    check(again == dll, "loading it again returns the same base");
    check(((fn_v_t)gnt_pe_sym(dll, "attached"))() == 1,
          "and does not run the entry point again");

    k32 = gnt_pe_open("kernel32.dll");
    ntdll = gnt_pe_open("ntdll.dll");
    check(k32 != 0 && ntdll != 0 && k32 != dll && ntdll != k32,
          "kernel32 and ntdll came along (already loaded, distinct)");
    check(gnt_pe_sym(dll, "no_such_export") == 0 && gnt_pe_errno() == -2,
          "a missing export is 0, -ENOENT");
    check(gnt_pe_open("nosuch.dll") == 0 && gnt_pe_errno() == -8,
          "a missing DLL is 0, -ENOEXEC");
    check(gnt_pe_sym(k32, "RtlCaptureContext") != 0 &&
          gnt_pe_sym(k32, "RtlCaptureContext") == gnt_pe_sym(ntdll, "RtlCaptureContext"),
          "kernel32's forwarder to ntdll resolves to ntdll's function");

    /* --- ms_abi calls ------------------------------------------------------ */
    check(((fn_ii_t)gnt_pe_sym(dll, "add2"))(40, 2) == 42, "add2(40, 2) = 42");
    check(((fn_sum10_t)gnt_pe_sym(dll, "sum10"))(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 385,
          "ten arguments, six on the stack: sum10 = 385");
    check(((fn_di_t)gnt_pe_sym(dll, "scale"))(2.5, 4) == 10.0,
          "a double and an int: scale(2.5, 4) = 10");

    /* --- System V adapters ------------------------------------------------- */
    check(((sv_ii_t)gnt_pe_sym_sysv(dll, "add2"))(40, 2) == 42,
          "through an adapter: add2 = 42");
    check(((sv_sum10_t)gnt_pe_sym_sysv(dll, "sum10"))(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 385,
          "through an adapter: sum10 = 385");
    check(((sv_dd_t)gnt_pe_sym_sysv(dll, "hypot2"))(3.0, 4.0) == 25.0,
          "through an adapter, leading doubles: hypot2(3, 4) = 25");
    check(((sv_u_t)gnt_pe_sym_sysv(k32, "GetCurrentProcessId"))() == (unsigned)getpid(),
          "through an adapter, straight into kernel32: GetCurrentProcessId");

    /* --- the NT environment ------------------------------------------------ */
    tid_fn = (fn_u_t)gnt_pe_sym(dll, "tid");
    set_err_fn = (fn_setu_t)gnt_pe_sym(dll, "set_last_error");
    get_err_fn = (fn_u_t)gnt_pe_sym(dll, "last_error");
    check(((fn_u_t)gnt_pe_sym(dll, "pid"))() == (unsigned)getpid(),
          "GetCurrentProcessId() == getpid()");
    check(tid_fn() == (unsigned)syscall(SYS_gettid),
          "GetCurrentThreadId() == gettid()");
    check(((fn_uu_t)gnt_pe_sym(dll, "last_error_roundtrip"))(1234) == 1234,
          "SetLastError / GetLastError round-trip through the TEB");

    fflush(stdout);
    n = ((fn_say_t)gnt_pe_sym(dll, "say"))("  (a line written by the DLL through WriteFile)\n");
    check(n == 48, "WriteFile(GetStdHandle(STD_OUTPUT_HANDLE)) writes to our stdout");

    check(((fn_cb_t)gnt_pe_sym(dll, "call_back"))(triple, 5) == 16,
          "a callback from the DLL back into this program: 5*3+1 = 16");

    /* --- a thread that was there before the load ---------------------------- */
    set_err_fn(7);
    early_go = 1;
    if (early_ok) {
        pthread_join(early_thread, 0);
    }
    check(early_ok && early_done && early.nt_tid == (unsigned)early.linux_tid &&
          early.linux_tid != getpid(),
          "a thread from BEFORE the load: GetCurrentThreadId() is its gettid()");
    check(early_ok && early.err == 99 && get_err_fn() == 7,
          "with a last error of its own");

    /* --- a thread created after the load ----------------------------------- */
    {
        pthread_t t;
        struct thread_result r;

        memset(&r, 0, sizeof(r));
        set_err_fn(7);
        if (pthread_create(&t, 0, thread_main, &r) == 0) {
            pthread_join(t, 0);
            check(r.nt_tid == (unsigned)r.linux_tid && r.linux_tid != getpid(),
                  "a new thread: GetCurrentThreadId() is its own gettid()");
            check(r.err_seen_first == 0 && r.err_after_set == 4242,
                  "and its own last error (a TEB of its own)");
            check(get_err_fn() == 7, "while the main thread's is untouched");
        } else {
            check(0, "pthread_create");
        }
    }

    snprintf(msg, sizeof(msg), "elfmix: %d passed, %d failed\n", passes, failures);
    fputs(msg, stdout);
    return failures == 0 ? 0 : 1;
}
