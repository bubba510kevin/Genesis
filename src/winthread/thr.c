/* thr.exe - Win32 threads, from a Windows program that knows nothing about
 * Genesis. ROADMAP item 14(a)'s ring-3 check.
 *
 * Imports kernel32.dll alone, like k32.exe: CreateThread, WaitForSingleObject,
 * GetExitCodeThread and the rest, exactly as any Win32 program calls them.
 * What it proves is the thing a single-threaded process cannot show:
 *
 *   - a thread really runs, concurrently, with its own stack;
 *   - it has its OWN TEB - GetCurrentThreadId and GetLastError are per
 *     thread, which they cannot be while every thread reads one GS:0;
 *   - a start routine that RETURNS ends the thread with that value
 *     (RtlUserThreadStart), and ExitThread does the same from deeper down;
 *   - its handle is waitable, is unsignalled while it runs (WAIT_TIMEOUT,
 *     STILL_ACTIVE) and signalled after, with the exit code intact;
 *   - more threads can be created over a process's life than there are
 *     slots at once - dead ones are reclaimed, TEB and stack included;
 *   - ExitProcess ends the process even with a thread still running.
 *
 * Every check prints ok/FAIL, and the last line is a tally in systest's
 * shape so tools/guest_run.py can wait for it.
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

/* --- the workers -------------------------------------------------------- */

#define WORKERS 4
#define ROUNDS  2000

static volatile LONG counter;
static volatile DWORD seen_tid[WORKERS];
static volatile int   own_error_ok[WORKERS];
static volatile int   stack_ok[WORKERS];

static DWORD WINAPI worker(LPVOID param) {
    int i = (int)(SIZE_T)param;
    int r;
    char probe;                              /* an address on THIS stack */
    /* Through kernel32's forwarder rather than ntdll's own export, so this
     * program keeps importing kernel32 alone. */
    PTEB teb = (PTEB)K32CurrentTeb();

    seen_tid[i] = GetCurrentThreadId();
    stack_ok[i] = ((SIZE_T)&probe < (SIZE_T)teb->StackBase &&
                   (SIZE_T)&probe >= (SIZE_T)teb->StackLimit);

    /* Set, then do enough work that the scheduler will have run the other
     * workers in between, then read back. One shared TEB would hand back
     * whichever worker wrote last. */
    SetLastError(1000 + (DWORD)i);
    for (r = 0; r < ROUNDS; r++) {
        __sync_fetch_and_add(&counter, 1);
    }
    own_error_ok[i] = (GetLastError() == 1000 + (DWORD)i);

    return 100 + (DWORD)i;                   /* RtlUserThreadStart's job */
}

static void deeper_exit(void) {
    ExitThread(55);                          /* never returns */
}

static DWORD WINAPI exiter(LPVOID param) {
    (void)param;
    deeper_exit();
    return 1;                                /* not reached */
}

static volatile int go;

static DWORD WINAPI waiter(LPVOID param) {
    (void)param;
    while (!go) {
    }
    return 7;
}

static DWORD WINAPI quick(LPVOID param) {
    return (DWORD)(SIZE_T)param;
}

static HANDLE mutex;
static volatile int holding;

static DWORD WINAPI holder(LPVOID param) {
    /* Twice, so its death has a recursion depth to throw away. */
    WaitForSingleObject(mutex, INFINITE);
    WaitForSingleObject(mutex, INFINITE);
    holding = 1;
    while (!go) {
    }
    ExitThread((DWORD)(SIZE_T)param);        /* still holding it */
    return 0;                                /* not reached */
}

static volatile int ran;

static DWORD WINAPI runs(LPVOID param) {
    ran = 1;
    return (DWORD)(SIZE_T)param;
}

static volatile LONG spins;
static volatile int stop;

static DWORD WINAPI spinner(LPVOID param) {
    (void)param;
    while (!stop) {
        __sync_fetch_and_add(&spins, 1);
    }
    return 0;
}

static volatile int before_self, after_self;

static DWORD WINAPI self_suspender(LPVOID param) {
    (void)param;
    before_self = 1;
    SuspendThread(GetCurrentThread());
    after_self = 1;
    return 0;
}

/* Has `spins` stopped moving? Sampled across a real sleep, so a spinner
 * on another CPU has had time to move it if it could. */
static int frozen(void) {
    LONG a = spins;

    Sleep(60);
    return spins == a;
}

static volatile int wrote_after;

/* Blocks on `mutex` (held by the main thread), then would record that it
 * got past - which a terminated thread must never do. */
static DWORD WINAPI blocked_on_mutex(LPVOID param) {
    (void)param;
    WaitForSingleObject(mutex, INFINITE);
    wrote_after = 1;
    return 0;
}

static DWORD WINAPI forever(LPVOID param) {
    (void)param;
    for (;;) {
        __asm__ volatile ("" ::: "memory");  /* a loop, not UB to delete */
    }
    return 0;                                /* not reached */
}

void start(void) {
    HANDLE h[WORKERS];
    DWORD tid_reported[WORKERS];
    DWORD main_tid, code;
    int i, j, ok;

    out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("thr: Win32 threads\r\n");

    main_tid = GetCurrentThreadId();
    SetLastError(7);

    /* --- four workers, concurrently -------------------------------------- */
    ok = 1;
    for (i = 0; i < WORKERS; i++) {
        h[i] = CreateThread(NULL_PTR, 0, worker, (LPVOID)(SIZE_T)i, 0,
                            &tid_reported[i]);
        if (h[i] == NULL_PTR) {
            ok = 0;
        }
    }
    check(ok, "CreateThread x4 returns four handles");
    if (!ok) {
        say("thr: cannot continue without threads\r\n");
        ExitProcess(1);
    }

    ok = 1;
    for (i = 0; i < WORKERS; i++) {
        if (WaitForSingleObject(h[i], INFINITE) != WAIT_OBJECT_0) {
            ok = 0;
        }
    }
    check(ok, "WaitForSingleObject returns WAIT_OBJECT_0 for each");

    check(counter == WORKERS * ROUNDS,
          "the shared counter saw every increment from every thread");

    ok = 1;
    for (i = 0; i < WORKERS; i++) {
        if (!GetExitCodeThread(h[i], &code) || code != 100 + (DWORD)i) {
            ok = 0;
        }
    }
    check(ok, "each exit code is its start routine's RETURN value");

    ok = 1;
    for (i = 0; i < WORKERS; i++) {
        if (seen_tid[i] == 0 || seen_tid[i] == main_tid ||
            seen_tid[i] != tid_reported[i] ||
            GetThreadId(h[i]) != tid_reported[i]) {
            ok = 0;
        }
        for (j = 0; j < i; j++) {
            if (seen_tid[j] == seen_tid[i]) {
                ok = 0;
            }
        }
    }
    check(ok, "every thread has its own id - from its own TEB - matching "
              "what CreateThread and GetThreadId report");

    ok = 1;
    for (i = 0; i < WORKERS; i++) {
        if (!own_error_ok[i]) {
            ok = 0;
        }
    }
    check(ok, "GetLastError is per thread: each read back its own value");
    check(GetLastError() == 7,
          "and the main thread's is untouched by all four");

    ok = 1;
    for (i = 0; i < WORKERS; i++) {
        if (!stack_ok[i]) {
            ok = 0;
        }
    }
    check(ok, "each thread runs on its own stack, inside its TEB's bounds");

    ok = 1;
    for (i = 0; i < WORKERS; i++) {
        if (!CloseHandle(h[i])) {
            ok = 0;
        }
    }
    check(ok, "the handles close");

    /* --- ExitThread from inside a call ----------------------------------- */
    h[0] = CreateThread(NULL_PTR, 0, exiter, NULL_PTR, 0, NULL_PTR);
    check(h[0] != NULL_PTR &&
          WaitForSingleObject(h[0], INFINITE) == WAIT_OBJECT_0 &&
          GetExitCodeThread(h[0], &code) && code == 55,
          "ExitThread(55) from a nested call ends the thread with 55");
    CloseHandle(h[0]);

    /* --- a running thread's handle is not signalled ---------------------- */
    h[0] = CreateThread(NULL_PTR, 0, waiter, NULL_PTR, 0, NULL_PTR);
    check(h[0] != NULL_PTR &&
          WaitForSingleObject(h[0], 0) == WAIT_TIMEOUT,
          "a zero-timeout wait on a RUNNING thread is WAIT_TIMEOUT");
    check(GetExitCodeThread(h[0], &code) && code == STILL_ACTIVE,
          "and its exit code reads STILL_ACTIVE");
    go = 1;
    check(WaitForSingleObject(h[0], INFINITE) == WAIT_OBJECT_0 &&
          GetExitCodeThread(h[0], &code) && code == 7,
          "released, it finishes, and the wait sees it");
    CloseHandle(h[0]);

    /* --- a mutex whose owner dies holding it ----------------------------- */
    mutex = CreateMutexW(NULL_PTR, 0, NULL_PTR);
    check(mutex != NULL_PTR, "CreateMutexW");
    go = 0;
    holding = 0;
    h[0] = CreateThread(NULL_PTR, 0, holder, (LPVOID)(SIZE_T)9, 0, NULL_PTR);
    while (h[0] != NULL_PTR && !holding) {
    }
    check(WaitForSingleObject(mutex, 0) == WAIT_TIMEOUT,
          "while another thread holds it, a zero-timeout wait times out");
    check(!ReleaseMutex(mutex) && GetLastError() == ERROR_NOT_OWNER,
          "and ReleaseMutex by a non-owner fails with ERROR_NOT_OWNER");
    /* Released while this thread is BLOCKED on it - the case that hangs if
     * nobody abandons it. */
    go = 1;
    check(WaitForSingleObject(mutex, 5000) == WAIT_ABANDONED,
          "its owner exits holding it: a blocked waiter gets WAIT_ABANDONED");
    check(WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0,
          "the new owner's recursive take is ordinary - abandonment is "
          "reported once");
    check(ReleaseMutex(mutex) && ReleaseMutex(mutex) && !ReleaseMutex(mutex),
          "released twice, not four times: the dead owner's depth went with "
          "it");
    WaitForSingleObject(h[0], INFINITE);
    CloseHandle(h[0]);
    CloseHandle(mutex);

    /* --- more threads than slots, over time ------------------------------ */
    ok = 1;
    for (i = 0; i < 40; i++) {
        HANDLE q = CreateThread(NULL_PTR, 0, quick, (LPVOID)(SIZE_T)i, 0,
                                NULL_PTR);

        if (q == NULL_PTR ||
            WaitForSingleObject(q, INFINITE) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(q, &code) || code != (DWORD)i) {
            ok = 0;
            say("  (failed at thread ");
            say_u((DWORD)i);
            say(")\r\n");
            if (q != NULL_PTR) {
                CloseHandle(q);
            }
            break;
        }
        CloseHandle(q);
    }
    check(ok, "forty threads created and joined one after another - more "
              "than the process table holds at once, so dead ones are "
              "really reclaimed");

    /* --- suspension ------------------------------------------------------ */
    ran = 0;
    h[0] = CreateThread(NULL_PTR, 0, runs, (LPVOID)(SIZE_T)31,
                        CREATE_SUSPENDED, NULL_PTR);
    check(h[0] != NULL_PTR, "CreateThread with CREATE_SUSPENDED");
    Sleep(60);
    check(!ran && WaitForSingleObject(h[0], 0) == WAIT_TIMEOUT,
          "and it does not run - not one instruction - until resumed");
    check(ResumeThread(h[0]) == 1,
          "ResumeThread reports the count it found: 1");
    check(WaitForSingleObject(h[0], 5000) == WAIT_OBJECT_0 && ran &&
          GetExitCodeThread(h[0], &code) && code == 31,
          "and then it runs to completion");
    check(ResumeThread(h[0]) == (DWORD)-1,
          "resuming a thread that has exited fails");
    CloseHandle(h[0]);

    spins = 0;
    stop = 0;
    h[0] = CreateThread(NULL_PTR, 0, spinner, NULL_PTR, 0, NULL_PTR);
    while (h[0] != NULL_PTR && spins == 0) {
    }
    check(ResumeThread(h[0]) == 0,
          "ResumeThread on a running thread is a no-op that reports 0");
    check(SuspendThread(h[0]) == 0 && frozen(),
          "SuspendThread stops a thread spinning in ring 3");
    check(SuspendThread(h[0]) == 1 && ResumeThread(h[0]) == 2 && frozen(),
          "the count is counted: suspended twice, resumed once, still "
          "stopped");
    ok = 1;
    for (i = 2; i <= 127; i++) {
        if (SuspendThread(h[0]) != (DWORD)(i - 1)) {
            ok = 0;
        }
    }
    check(ok && SuspendThread(h[0]) == (DWORD)-1 &&
          GetLastError() == ERROR_SIGNAL_REFUSED,
          "up to 127, and the 128th is refused with ERROR_SIGNAL_REFUSED");
    for (i = 127; i > 1; i--) {
        (void)ResumeThread(h[0]);
    }
    check(ResumeThread(h[0]) == 1 && !frozen(),
          "the last ResumeThread lets it run again");
    stop = 1;
    check(WaitForSingleObject(h[0], 5000) == WAIT_OBJECT_0,
          "and it finishes normally");
    CloseHandle(h[0]);

    before_self = after_self = 0;
    h[0] = CreateThread(NULL_PTR, 0, self_suspender, NULL_PTR, 0, NULL_PTR);
    while (h[0] != NULL_PTR && !before_self) {
    }
    Sleep(60);
    check(!after_self && WaitForSingleObject(h[0], 0) == WAIT_TIMEOUT,
          "SuspendThread(GetCurrentThread()) stops the caller in the call");
    check(ResumeThread(h[0]) == 1 &&
          WaitForSingleObject(h[0], 5000) == WAIT_OBJECT_0 && after_self,
          "and another thread's ResumeThread lets it continue");
    CloseHandle(h[0]);

    /* --- TerminateThread ------------------------------------------------- */
    spins = 0;
    stop = 0;
    h[0] = CreateThread(NULL_PTR, 0, spinner, NULL_PTR, 0, NULL_PTR);
    while (h[0] != NULL_PTR && spins == 0) {
    }
    check(TerminateThread(h[0], 0x12345678u) &&
          WaitForSingleObject(h[0], 5000) == WAIT_OBJECT_0,
          "TerminateThread ends a thread spinning in ring 3, and its handle "
          "is signalled");
    check(GetExitCodeThread(h[0], &code) && code == 0x12345678u,
          "with the full 32-bit exit code it was given");
    check(frozen(), "and it really has stopped");
    check(!TerminateThread(h[0], 1) && GetLastError() == ERROR_ACCESS_DENIED,
          "terminating it again fails - it has already exited");
    stop = 1;
    CloseHandle(h[0]);

    mutex = CreateMutexW(NULL_PTR, 1, NULL_PTR);     /* held by this thread */
    wrote_after = 0;
    h[0] = CreateThread(NULL_PTR, 0, blocked_on_mutex, NULL_PTR, 0, NULL_PTR);
    Sleep(60);
    check(h[0] != NULL_PTR && WaitForSingleObject(h[0], 0) == WAIT_TIMEOUT,
          "a thread blocked in WaitForSingleObject on a held mutex");
    check(TerminateThread(h[0], 3) &&
          WaitForSingleObject(h[0], 5000) == WAIT_OBJECT_0,
          "is terminated out of the wait");
    ReleaseMutex(mutex);
    Sleep(60);
    check(!wrote_after,
          "and releasing the mutex afterwards does not wake it - it is "
          "gone, not interrupted");
    CloseHandle(h[0]);

    holding = 0;
    go = 0;
    h[0] = CreateThread(NULL_PTR, 0, holder, NULL_PTR, 0, NULL_PTR);
    while (h[0] != NULL_PTR && !holding) {
    }
    check(TerminateThread(h[0], 4) &&
          WaitForSingleObject(mutex, 5000) == WAIT_ABANDONED,
          "a thread terminated while holding a mutex abandons it");
    ReleaseMutex(mutex);
    ReleaseMutex(mutex);
    CloseHandle(h[0]);
    CloseHandle(mutex);

    ran = 0;
    h[0] = CreateThread(NULL_PTR, 0, runs, NULL_PTR, CREATE_SUSPENDED,
                        NULL_PTR);
    check(h[0] != NULL_PTR && TerminateThread(h[0], 5) &&
          WaitForSingleObject(h[0], 5000) == WAIT_OBJECT_0 &&
          GetExitCodeThread(h[0], &code) && code == 5 && !ran,
          "a thread terminated before it was ever resumed never runs");
    CloseHandle(h[0]);

    stop = 0;                   /* spinners that only stop when killed */
    ok = 1;
    for (i = 0; i < 20; i++) {
        HANDLE q = CreateThread(NULL_PTR, 0, spinner, NULL_PTR, 0, NULL_PTR);

        if (q == NULL_PTR || !TerminateThread(q, 6) ||
            WaitForSingleObject(q, 5000) != WAIT_OBJECT_0) {
            ok = 0;
        }
        if (q != NULL_PTR) {
            CloseHandle(q);
        }
    }
    check(ok, "twenty threads started and terminated in turn - terminated "
              "threads are reclaimed like exited ones");
    stop = 1;

    h[1] = CreateThread(NULL_PTR, 0, runs, NULL_PTR, CREATE_SUSPENDED,
                        NULL_PTR);
    check(h[1] != NULL_PTR, "a thread left suspended for ExitProcess");

    /* --- ExitProcess with a thread still running ------------------------- */
    h[0] = CreateThread(NULL_PTR, 0, forever, NULL_PTR, 0, NULL_PTR);
    check(h[0] != NULL_PTR, "a thread that never ends is started");
    say("  (ExitProcess now, with it still running - the shell prompt "
        "coming back is the check)\r\n");

    say("\r\nthr: ");
    say_u((DWORD)passes);
    say(" passed, ");
    say_u((DWORD)failures);
    say(" failed\r\n");
    ExitProcess((DWORD)failures);
}
