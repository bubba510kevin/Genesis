/* mixdll.dll - a Windows DLL for Linux programs to load (ROADMAP item 19).
 *
 * Built by MinGW and imports kernel32.dll, so loading it into a Linux
 * process brings kernel32 and ntdll along and exercises the NT environment
 * the kernel attaches: the TEB (thread id, last error, per thread), the PEB
 * (the process id, and the standard handles GetStdHandle returns). src/
 * elfmix/elfmix.c is the program that loads it; each export is one thing it
 * checks. */

#include "../kernel32/kernel32.h"

#define EXPORT __declspec(dllexport)

static int attach_count;

BOOL WINAPI DllMainCRTStartup(PVOID instance, DWORD reason, PVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == 1) {                   /* DLL_PROCESS_ATTACH */
        attach_count++;
    }
    return TRUE;
}

EXPORT int attached(void) {
    return attach_count;
}

EXPORT int add2(int a, int b) {
    return a + b;
}

/* Ten arguments: four in registers and six on the stack, Win64-style. */
EXPORT long long sum10(long long a, long long b, long long c, long long d,
                       long long e, long long f, long long g, long long h,
                       long long i, long long j) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h +
           9 * i + 10 * j;
}

/* A double and an int: arguments 1 and 2 in XMM0 and RDX under Win64, but
 * XMM0 and RDI under System V. */
EXPORT double scale(double x, int k) {
    return x * (double)k;
}

EXPORT double hypot2(double a, double b) {
    return a * a + b * b;
}

EXPORT DWORD pid(void) {
    return GetCurrentProcessId();
}

EXPORT DWORD tid(void) {
    return GetCurrentThreadId();
}

EXPORT DWORD last_error_roundtrip(DWORD v) {
    SetLastError(v);
    return GetLastError();
}

EXPORT DWORD last_error(void) {
    return GetLastError();
}

EXPORT void set_last_error(DWORD v) {
    SetLastError(v);
}

/* Through the standard handle GetStdHandle reads out of the process
 * parameters the kernel built at attach time. */
EXPORT int say(const char *s) {
    DWORD n = 0, written = 0;

    while (s[n] != '\0') {
        n++;
    }
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &written, NULL_PTR)) {
        return -1;
    }
    return (int)written;
}

/* A callback INTO the Linux program: it must be a Win64 function. */
EXPORT int call_back(int (WINAPI *cb)(int), int v) {
    return cb(v) + 1;
}
