/* tls.exe - thread-local storage, from a Windows program.
 *
 * Implicit TLS the way a C runtime sets it up: a .tls section with a
 * template, an IMAGE_TLS_DIRECTORY named _tls_used (the linker points the
 * image's TLS data directory at it), _tls_index for the loader to fill, and
 * a TLS callback in .CRT$XLB - exactly the shape of MinGW's own tlssup.c,
 * written out here because this program links no CRT. Then TlsAlloc and
 * FlsAlloc through kernel32.
 *
 * Imports kernel32.dll alone. The last line is a tally for guest_run.py. */

#include "../kernel32/kernel32.h"

static HANDLE out;
static int passes, failures;

static void say(const char *s) {
    DWORD n = 0, w = 0;

    while (s[n] != '\0') {
        n++;
    }
    WriteFile(out, s, n, &w, NULL_PTR);
}

static void say_u(unsigned long long v) {
    char b[24];
    int i = 23;

    b[i] = '\0';
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    say(&b[i]);
}

static void check(int c, const char *what) {
    say(c ? "  ok    " : "  FAIL  ");
    say(what);
    say("\r\n");
    if (c) passes++; else failures++;
}

/* --- implicit TLS: the CRT's tlssup.c, by hand ------------------------------ */

typedef void (*PIMAGE_TLS_CALLBACK)(PVOID DllHandle, DWORD Reason, PVOID Reserved);

typedef struct {
    ULONGLONG StartAddressOfRawData;
    ULONGLONG EndAddressOfRawData;
    ULONGLONG AddressOfIndex;
    ULONGLONG AddressOfCallBacks;
    DWORD     SizeOfZeroFill;
    DWORD     Characteristics;
} IMAGE_TLS_DIRECTORY64;

DWORD _tls_index = 0xFFFFFFFFu;         /* the loader writes the real one */

__attribute__((section(".tls"), used)) char _tls_start = 0;
__attribute__((section(".tls$BBB"), used)) int  tls_counter = 42;
__attribute__((section(".tls$BBB"), used)) char tls_name[16] = "template";
__attribute__((section(".tls$ZZZ"), used)) char _tls_end = 0;

static volatile LONG process_attach, thread_attach, thread_detach;

static void tls_callback(PVOID h, DWORD reason, PVOID reserved) {
    (void)h;
    (void)reserved;
    if (reason == 1) {
        __atomic_add_fetch(&process_attach, 1, __ATOMIC_ACQ_REL);
    } else if (reason == 2) {
        __atomic_add_fetch(&thread_attach, 1, __ATOMIC_ACQ_REL);
    } else if (reason == 3) {
        __atomic_add_fetch(&thread_detach, 1, __ATOMIC_ACQ_REL);
    }
}

__attribute__((section(".CRT$XLA"), used)) PIMAGE_TLS_CALLBACK __xl_a = 0;
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK __xl_b = tls_callback;
__attribute__((section(".CRT$XLZ"), used)) PIMAGE_TLS_CALLBACK __xl_z = 0;

__attribute__((section(".rdata$T"), used))
const IMAGE_TLS_DIRECTORY64 _tls_used = {
    (ULONGLONG)&_tls_start, (ULONGLONG)&_tls_end,
    (ULONGLONG)&_tls_index, (ULONGLONG)(&__xl_a + 1),
    0, 0
};

/* What a compiler emits for a __declspec(thread) access: this thread's
 * block from gs:[0x58][_tls_index], plus the variable's offset in .tls. */
static void *tls_ptr(const void *var) {
    BYTE *teb = (BYTE *)K32CurrentTeb();
    void **arr = *(void ***)(teb + 0x58);

    return (BYTE *)arr[_tls_index] + ((const BYTE *)var - (const BYTE *)&_tls_start);
}

#define TLS_VAR(v) (*(__typeof__(v) *)tls_ptr(&(v)))

static volatile int thread_saw, thread_set;

static DWORD WINAPI implicit_worker(LPVOID p) {
    (void)p;
    thread_saw = TLS_VAR(tls_counter);
    TLS_VAR(tls_counter) = 7;
    thread_set = TLS_VAR(tls_counter);
    return 0;
}

/* --- dynamic TLS ---------------------------------------------------------------- */

static DWORD dyn_index, exp_index;
static volatile int stage;
static volatile LPVOID worker_saw_after_free;

static DWORD WINAPI dynamic_worker(LPVOID p) {
    (void)p;
    TlsSetValue(dyn_index, (LPVOID)0x1111);
    TlsSetValue(exp_index, (LPVOID)0x2222);
    stage = 1;
    while (stage != 2) {
        SwitchToThread();
    }
    /* The main thread freed dyn_index while this thread still had a value
     * in it; the value must be gone. */
    worker_saw_after_free = TlsGetValue(dyn_index);
    stage = 3;
    return 0;
}

static volatile LONG fls_destroyed;
static volatile PVOID fls_destroyed_value;

static void fls_dtor(PVOID v) {
    fls_destroyed_value = v;
    __atomic_add_fetch(&fls_destroyed, 1, __ATOMIC_ACQ_REL);
}

static DWORD fls_index;

static DWORD WINAPI fls_worker(LPVOID p) {
    (void)p;
    FlsSetValue(fls_index, (PVOID)0xABC);
    return 0;
}

static void join(HANDLE h) {
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}

void start(void) {
    DWORD idx[80];
    int i, distinct = 1;
    HANDLE h;

    out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("tls: thread-local storage, from Win32\r\n\r\nimplicit TLS (.tls section)\r\n");

    check(_tls_index == 0, "the loader wrote _tls_index (the executable's is 0)");
    check(process_attach == 1,
          "the TLS callback ran DLL_PROCESS_ATTACH once, before the entry point");
    check(TLS_VAR(tls_counter) == 42, "the main thread's copy starts as the template");
    check(TLS_VAR(tls_name)[0] == 't' && TLS_VAR(tls_name)[7] == 'e',
          "all of it, not just the first word");
    TLS_VAR(tls_counter) = 100;
    check(tls_counter == 42, "and it is a COPY - the template is untouched");

    h = CreateThread(NULL_PTR, 0, implicit_worker, NULL_PTR, 0, NULL_PTR);
    join(h);
    check(thread_saw == 42, "a new thread gets a fresh copy of the template");
    check(thread_set == 7, "writes to its own copy");
    check(TLS_VAR(tls_counter) == 100, "without touching the main thread's");
    check(thread_attach == 1, "DLL_THREAD_ATTACH ran as it started");
    check(thread_detach == 1, "and DLL_THREAD_DETACH as it exited");

    say("\r\ndynamic TLS (TlsAlloc)\r\n");
    for (i = 0; i < 80; i++) {
        idx[i] = TlsAlloc();
        if (idx[i] == TLS_OUT_OF_INDEXES) {
            distinct = 0;
        }
    }
    for (i = 1; i < 80 && distinct; i++) {
        int j;
        for (j = 0; j < i; j++) {
            if (idx[i] == idx[j]) {
                distinct = 0;
            }
        }
    }
    check(distinct, "80 TlsAlloc indices, all distinct (past the 64 in the TEB)");
    SetLastError(1234);
    check(TlsGetValue(idx[5]) == NULL_PTR && GetLastError() == ERROR_SUCCESS,
          "a fresh slot reads NULL, and GetLastError is cleared");
    check(TlsSetValue(idx[70], (LPVOID)0x70) && TlsGetValue(idx[70]) == (LPVOID)0x70,
          "an expansion slot (index >= 64) stores and reads back");
    for (i = 0; i < 80; i++) {
        TlsFree(idx[i]);
    }
    check(!TlsFree(1500) && GetLastError() == ERROR_INVALID_PARAMETER,
          "TlsFree of an index that was never allocated fails");

    dyn_index = TlsAlloc();
    for (i = 0; i < 70; i++) {
        idx[i] = TlsAlloc();                /* push exp_index past 64 */
    }
    exp_index = TlsAlloc();
    check(exp_index >= 64, "an index in the expansion range");
    TlsSetValue(dyn_index, (LPVOID)0xAAAA);
    stage = 0;
    h = CreateThread(NULL_PTR, 0, dynamic_worker, NULL_PTR, 0, NULL_PTR);
    while (stage != 1) {
        SwitchToThread();
    }
    check(TlsGetValue(dyn_index) == (LPVOID)0xAAAA,
          "each thread has its own value in the same slot");
    check(TlsGetValue(exp_index) == NULL_PTR,
          "including in the expansion range");
    check(TlsFree(dyn_index), "TlsFree while another thread holds a value");
    stage = 2;
    while (stage != 3) {
        SwitchToThread();
    }
    check(worker_saw_after_free == NULL_PTR,
          "and that thread's value is cleared too - a reused index reads NULL");
    join(h);
    for (i = 0; i < 70; i++) {
        TlsFree(idx[i]);
    }
    TlsFree(exp_index);

    say("\r\nfiber-local storage (FlsAlloc)\r\n");
    fls_index = FlsAlloc(fls_dtor);
    check(fls_index != FLS_OUT_OF_INDEXES, "FlsAlloc with a destructor");
    h = CreateThread(NULL_PTR, 0, fls_worker, NULL_PTR, 0, NULL_PTR);
    join(h);
    check(fls_destroyed == 1 && fls_destroyed_value == (PVOID)0xABC,
          "the destructor ran with the thread's value when it exited");
    check(FlsSetValue(fls_index, (PVOID)0xDEF) &&
          FlsGetValue(fls_index) == (PVOID)0xDEF, "FlsSetValue / FlsGetValue");
    check(FlsFree(fls_index), "FlsFree");
    check(fls_destroyed == 2 && fls_destroyed_value == (PVOID)0xDEF,
          "which destroys the values still held");

    say("\r\ntls: ");
    say_u((unsigned long long)passes);
    say(" passed, ");
    say_u((unsigned long long)failures);
    say(" failed\r\n");
    ExitProcess((DWORD)failures);
}
