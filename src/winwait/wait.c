/* wait.exe - waiting on several objects at once, from a Windows program that
 * knows nothing about Genesis. ROADMAP item 14(b)'s ring-3 check.
 *
 * Imports kernel32.dll alone. What it proves:
 *
 *   - WaitForMultipleObjects(wait-any) takes the LOWEST signalled index, and
 *     blocks until one is signalled;
 *   - wait-all is ALL OR NOTHING: while one object of the set is unavailable
 *     the others are left untouched for anybody else to take, and when the
 *     wait completes every object has been taken in one step;
 *   - duplicates in a wait-all, a non-waitable handle and a bad count are
 *     refused rather than hanging;
 *   - an abandoned mutex in the set reports WAIT_ABANDONED_0 + its index;
 *   - the Win32 event and semaphore wrappers behave as documented.
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

/* --- helpers run on other threads ---------------------------------------- */

static HANDLE ev[3];

static DWORD WINAPI set_later(LPVOID param) {
    Sleep(50);
    SetEvent(ev[(int)(SIZE_T)param]);
    return 0;
}

static HANDLE mutex, sem;
static volatile int holding, release_now, holder_done;

static DWORD WINAPI mutex_holder(LPVOID param) {
    (void)param;
    WaitForSingleObject(mutex, INFINITE);
    holding = 1;
    while (!release_now) {
        Sleep(5);
    }
    ReleaseMutex(mutex);
    holder_done = 1;
    return 0;
}

static DWORD WINAPI dies_holding(LPVOID param) {
    (void)param;
    WaitForSingleObject(mutex, INFINITE);
    return 0;                                /* without releasing it */
}

static DWORD WINAPI quick(LPVOID param) {
    Sleep((DWORD)(SIZE_T)param);
    return 0;
}

static volatile LONG released;

static DWORD WINAPI gate_waiter(LPVOID param) {
    WaitForSingleObject((HANDLE)param, INFINITE);
    __sync_fetch_and_add(&released, 1);
    return 0;
}

void start(void) {
    HANDLE h[4], two[2];
    DWORD r;
    LONG prev;
    int i, ok;

    out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("wait: WaitForMultipleObjects, events, semaphores\r\n");

    /* --- wait-any ---------------------------------------------------------- */
    for (i = 0; i < 3; i++) {
        ev[i] = CreateEventW(NULL_PTR, 0, 0, NULL_PTR);  /* auto-reset */
    }
    check(ev[0] != NULL_PTR && ev[1] != NULL_PTR && ev[2] != NULL_PTR,
          "CreateEventW x3 (auto-reset, unsignalled)");
    check(WaitForMultipleObjects(3, ev, 0, 0) == WAIT_TIMEOUT,
          "wait-any on three unsignalled events times out");
    SetEvent(ev[2]);
    check(WaitForMultipleObjects(3, ev, 0, 0) == WAIT_OBJECT_0 + 2,
          "one signalled: wait-any returns WAIT_OBJECT_0 + its index");
    check(WaitForMultipleObjects(3, ev, 0, 0) == WAIT_TIMEOUT,
          "and took it - the auto-reset event is clear again");
    SetEvent(ev[2]);
    SetEvent(ev[1]);
    check(WaitForMultipleObjects(3, ev, 0, 0) == WAIT_OBJECT_0 + 1,
          "two signalled: the LOWEST index wins");
    check(WaitForMultipleObjects(3, ev, 0, 0) == WAIT_OBJECT_0 + 2,
          "and the other is still there for the next wait");

    h[0] = CreateThread(NULL_PTR, 0, set_later, (LPVOID)(SIZE_T)1, 0,
                        NULL_PTR);
    check(WaitForMultipleObjects(3, ev, 0, 5000) == WAIT_OBJECT_0 + 1,
          "wait-any BLOCKS until another thread signals one");
    WaitForSingleObject(h[0], INFINITE);
    CloseHandle(h[0]);
    check(WaitForMultipleObjects(3, ev, 0, 30) == WAIT_TIMEOUT,
          "a 30ms wait-any with nothing signalled times out");

    /* --- wait-all ---------------------------------------------------------- */
    SetEvent(ev[0]);
    SetEvent(ev[1]);
    check(WaitForMultipleObjects(3, ev, 1, 0) == WAIT_TIMEOUT,
          "wait-all with two of three signalled times out");
    check(WaitForSingleObject(ev[0], 0) == WAIT_OBJECT_0 &&
          WaitForSingleObject(ev[1], 0) == WAIT_OBJECT_0,
          "and took NEITHER of the two - both were still signalled");
    SetEvent(ev[0]);
    SetEvent(ev[1]);
    h[0] = CreateThread(NULL_PTR, 0, set_later, (LPVOID)(SIZE_T)2, 0,
                        NULL_PTR);
    check(WaitForMultipleObjects(3, ev, 1, 5000) == WAIT_OBJECT_0,
          "wait-all blocks until the last one is signalled");
    check(WaitForMultipleObjects(3, ev, 0, 0) == WAIT_TIMEOUT,
          "and then took all three at once");
    WaitForSingleObject(h[0], INFINITE);
    CloseHandle(h[0]);

    /* All or nothing, against a competitor. The set is {semaphore of one,
     * mutex held by another thread}. While the mutex is held, the semaphore
     * must stay available to anybody else - a wait-all that took it early
     * would be holding one lock while waiting for another. */
    sem   = CreateSemaphoreW(NULL_PTR, 1, 1, NULL_PTR);
    mutex = CreateMutexW(NULL_PTR, 0, NULL_PTR);
    holding = release_now = holder_done = 0;
    h[0] = CreateThread(NULL_PTR, 0, mutex_holder, NULL_PTR, 0, NULL_PTR);
    while (!holding) {
        Sleep(1);
    }
    two[0] = sem;
    two[1] = mutex;
    check(WaitForMultipleObjects(2, two, 1, 50) == WAIT_TIMEOUT,
          "wait-all on {semaphore, mutex held elsewhere} times out");
    check(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0,
          "and left the semaphore's permit for somebody else");
    ReleaseSemaphore(sem, 1, NULL_PTR);
    release_now = 1;
    r = WaitForMultipleObjects(2, two, 1, 5000);
    check(r == WAIT_OBJECT_0 && holder_done,
          "once the mutex is released, wait-all gets both");
    check(WaitForSingleObject(sem, 0) == WAIT_TIMEOUT,
          "the semaphore's permit is taken");
    check(ReleaseMutex(mutex), "and the mutex is ours to release");
    ReleaseSemaphore(sem, 1, NULL_PTR);
    WaitForSingleObject(h[0], INFINITE);
    CloseHandle(h[0]);

    /* --- threads, abandonment, refusals ------------------------------------ */
    ok = 1;
    for (i = 0; i < 4; i++) {
        h[i] = CreateThread(NULL_PTR, 0, quick, (LPVOID)(SIZE_T)(i * 20), 0,
                            NULL_PTR);
        if (h[i] == NULL_PTR) {
            ok = 0;
        }
    }
    check(ok && WaitForMultipleObjects(4, h, 1, 5000) == WAIT_OBJECT_0,
          "wait-all on four thread handles returns when all have exited");
    for (i = 0; i < 4; i++) {
        CloseHandle(h[i]);
    }

    h[0] = CreateThread(NULL_PTR, 0, dies_holding, NULL_PTR, 0, NULL_PTR);
    WaitForSingleObject(h[0], INFINITE);
    CloseHandle(h[0]);
    two[0] = ev[0];
    two[1] = mutex;
    check(WaitForMultipleObjects(2, two, 0, 0) == WAIT_ABANDONED_0 + 1,
          "an abandoned mutex at index 1 is WAIT_ABANDONED_0 + 1");
    ReleaseMutex(mutex);

    two[0] = ev[0];
    two[1] = ev[0];
    check(WaitForMultipleObjects(2, two, 1, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "the same handle twice in a wait-all is refused");
    SetEvent(ev[0]);
    check(WaitForMultipleObjects(2, two, 0, 0) == WAIT_OBJECT_0,
          "but is fine in a wait-any");
    two[1] = out;
    check(WaitForMultipleObjects(2, two, 0, 0) == WAIT_FAILED,
          "a console handle in the set fails rather than hanging");
    check(WaitForMultipleObjects(0, ev, 0, 0) == WAIT_FAILED &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "a count of zero is ERROR_INVALID_PARAMETER");

    /* --- the wrappers themselves ------------------------------------------- */
    check(!ReleaseSemaphore(sem, 0, &prev),
          "ReleaseSemaphore by zero is refused");
    check(!ReleaseSemaphore(sem, 1, &prev) &&
          GetLastError() == ERROR_TOO_MANY_POSTS,
          "past the maximum is ERROR_TOO_MANY_POSTS");
    WaitForSingleObject(sem, 0);
    check(ReleaseSemaphore(sem, 1, &prev) && prev == 0,
          "ReleaseSemaphore reports the previous count");

    h[3] = CreateEventW(NULL_PTR, 1, 0, NULL_PTR);        /* manual reset */
    released = 0;
    for (i = 0; i < 3; i++) {
        h[i] = CreateThread(NULL_PTR, 0, gate_waiter, h[3], 0, NULL_PTR);
    }
    Sleep(50);
    check(released == 0, "three threads wait on a manual-reset event");
    SetEvent(h[3]);
    check(WaitForMultipleObjects(3, h, 1, 5000) == WAIT_OBJECT_0 &&
          released == 3,
          "one SetEvent releases all three");
    check(WaitForSingleObject(h[3], 0) == WAIT_OBJECT_0,
          "and a manual-reset event stays signalled");
    check(ResetEvent(h[3]) && WaitForSingleObject(h[3], 0) == WAIT_TIMEOUT,
          "until ResetEvent");
    for (i = 0; i < 4; i++) {
        CloseHandle(h[i]);
    }

    say("\r\nwait: ");
    say_u((DWORD)passes);
    say(" passed, ");
    say_u((DWORD)failures);
    say(" failed\r\n");
    ExitProcess((DWORD)failures);
}
