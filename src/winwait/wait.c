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
 *   - the Win32 event and semaphore wrappers behave as documented;
 *   - APCs: QueueUserAPC runs only at an ALERTABLE wait (SleepEx,
 *     WaitForSingleObjectEx, WaitForMultipleObjectsEx), on the target
 *     thread, in order, and the interrupted wait returns
 *     WAIT_IO_COMPLETION with every register the caller relied on intact -
 *     checked with an APC that deliberately destroys them.
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

/* --- APC helpers ----------------------------------------------------------- */

static volatile LONG apc_runs;
static volatile ULONG_PTR apc_order[8];
static volatile DWORD apc_tid;

static void WINAPI record_apc(ULONG_PTR data) {
    LONG i = __sync_fetch_and_add(&apc_runs, 1);

    if (i < 8) {
        apc_order[i] = data;
    }
    apc_tid = GetCurrentThreadId();
}

static void WINAPI chain_apc(ULONG_PTR data) {
    record_apc(data);
    if (data < 3) {
        /* Queued from inside an APC: runs before the wait returns. */
        QueueUserAPC(chain_apc, GetCurrentThread(), data + 1);
    }
}

static volatile int worker_ready;
static volatile DWORD worker_result;

static DWORD WINAPI alertable_sleeper(LPVOID param) {
    (void)param;
    worker_ready = 1;
    worker_result = SleepEx(INFINITE, 1);
    return 0;
}

static DWORD WINAPI alertable_waiter(LPVOID param) {
    worker_ready = 1;
    worker_result = WaitForSingleObjectEx((HANDLE)param, INFINITE, 1);
    return 0;
}

static DWORD WINAPI plain_waiter(LPVOID param) {
    worker_ready = 1;
    worker_result = WaitForSingleObject((HANDLE)param, INFINITE);
    SleepEx(0, 1);                         /* now let the APC in */
    return 0;
}

/* The register check. clobber_apc breaks the Win64 ABI on purpose: it
 * overwrites every callee-saved general register and XMM6/XMM15 and
 * returns. probe_sleep holds known values in exactly those registers
 * across an alertable SleepEx that runs it - so they survive only if the
 * kernel's CONTEXT captured them and NtContinue put every one back. Returns
 * 1 when all survived and SleepEx said WAIT_IO_COMPLETION. */
extern volatile LONG clobbered;
volatile LONG clobbered;
void WINAPI clobber_apc(ULONG_PTR data);
int probe_sleep(void);
__asm__(
".text\n"
".globl clobber_apc\n"
"clobber_apc:\n"
"    movabsq $0x0BADBADBADBADBAD, %rax\n"
"    movq %rax, %rbx\n  movq %rax, %rbp\n  movq %rax, %rsi\n"
"    movq %rax, %rdi\n  movq %rax, %r12\n  movq %rax, %r13\n"
"    movq %rax, %r14\n  movq %rax, %r15\n"
"    pcmpeqd %xmm6, %xmm6\n  pcmpeqd %xmm15, %xmm15\n"
"    lock incl clobbered(%rip)\n"
"    ret\n"
".globl probe_sleep\n"
"probe_sleep:\n"
"    pushq %rbx\n  pushq %rbp\n  pushq %rsi\n  pushq %rdi\n"
"    pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
"    subq $0x48, %rsp\n"
"    movdqu %xmm6, 0x20(%rsp)\n  movdqu %xmm15, 0x30(%rsp)\n"
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
"    movl $2000, %ecx\n  movl $1, %edx\n"
"    call SleepEx\n"
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
"    cmpl $0xC0, %r8d\n  jne 9f\n"
"    movl $1, %eax\n"
"9:\n"
"    movdqu 0x20(%rsp), %xmm6\n  movdqu 0x30(%rsp), %xmm15\n"
"    addq $0x48, %rsp\n"
"    popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n"
"    popq %rdi\n  popq %rsi\n  popq %rbp\n  popq %rbx\n"
"    ret\n"
);

static void apc_tests(void) {
    HANDLE t, e, pair[2];
    DWORD me = GetCurrentThreadId(), tid = 0;

    say("\r\nAPCs\r\n");
    apc_runs = 0;
    check(QueueUserAPC(record_apc, GetCurrentThread(), 11) != 0,
          "QueueUserAPC to this thread");
    Sleep(20);
    check(apc_runs == 0, "a NON-alertable Sleep does not run it");
    check(SleepEx(0, 1) == WAIT_IO_COMPLETION && apc_runs == 1 &&
          apc_order[0] == 11 && apc_tid == me,
          "SleepEx(0, TRUE) runs it, with its argument, and returns "
          "WAIT_IO_COMPLETION");
    check(SleepEx(0, 1) == 0, "and it ran once - the next SleepEx is plain");

    apc_runs = 0;
    QueueUserAPC(record_apc, GetCurrentThread(), 1);
    QueueUserAPC(record_apc, GetCurrentThread(), 2);
    QueueUserAPC(record_apc, GetCurrentThread(), 3);
    check(SleepEx(1000, 1) == WAIT_IO_COMPLETION && apc_runs == 3 &&
          apc_order[0] == 1 && apc_order[1] == 2 && apc_order[2] == 3,
          "three queued APCs all run in ONE alertable wait, first in first "
          "out");

    apc_runs = 0;
    QueueUserAPC(chain_apc, GetCurrentThread(), 1);
    check(SleepEx(0, 1) == WAIT_IO_COMPLETION && apc_runs == 3,
          "an APC that queues another: both run before the wait returns");

    clobbered = 0;
    QueueUserAPC(clobber_apc, GetCurrentThread(), 0);
    check(probe_sleep() && clobbered == 1,
          "an APC that destroys every callee-saved register and XMM6/15: "
          "the interrupted code sees none of it (NtContinue restores all)");

    /* Another thread, asleep. */
    apc_runs = 0;
    worker_ready = 0;
    worker_result = 0;
    t = CreateThread(NULL_PTR, 0, alertable_sleeper, NULL_PTR, 0, &tid);
    while (!worker_ready) {
        Sleep(1);
    }
    Sleep(20);
    check(QueueUserAPC(record_apc, t, 21) != 0,
          "QueueUserAPC to a thread in SleepEx(INFINITE, TRUE)");
    check(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 &&
          worker_result == WAIT_IO_COMPLETION && apc_runs == 1 &&
          apc_order[0] == 21 && apc_tid == tid,
          "wakes it: the APC ran ON THAT THREAD and SleepEx returned "
          "WAIT_IO_COMPLETION");
    CloseHandle(t);

    /* Another thread, in an alertable wait on an event. */
    e = CreateEventW(NULL_PTR, 0, 0, NULL_PTR);
    apc_runs = 0;
    worker_ready = 0;
    t = CreateThread(NULL_PTR, 0, alertable_waiter, e, 0, NULL_PTR);
    while (!worker_ready) {
        Sleep(1);
    }
    Sleep(20);
    QueueUserAPC(record_apc, t, 31);
    check(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 &&
          worker_result == WAIT_IO_COMPLETION && apc_runs == 1,
          "WaitForSingleObjectEx(..., TRUE) is ended by an APC");
    SetEvent(e);
    check(WaitForSingleObject(e, 0) == WAIT_OBJECT_0,
          "and took nothing - the event is still there to take");
    CloseHandle(t);

    /* Another thread, in a NON-alertable wait. */
    apc_runs = 0;
    worker_ready = 0;
    t = CreateThread(NULL_PTR, 0, plain_waiter, e, 0, NULL_PTR);
    while (!worker_ready) {
        Sleep(1);
    }
    Sleep(20);
    QueueUserAPC(record_apc, t, 41);
    Sleep(50);
    check(apc_runs == 0 && WaitForSingleObject(t, 0) == WAIT_TIMEOUT,
          "a NON-alertable WaitForSingleObject is not disturbed by an APC");
    SetEvent(e);
    check(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 &&
          worker_result == WAIT_OBJECT_0 && apc_runs == 1,
          "it wakes for its event, and the APC runs at its next alertable "
          "wait");
    check(!QueueUserAPC(record_apc, t, 51),
          "QueueUserAPC to a thread that has exited fails");
    CloseHandle(t);

    pair[0] = e;
    pair[1] = e;
    apc_runs = 0;
    QueueUserAPC(record_apc, GetCurrentThread(), 61);
    check(WaitForMultipleObjectsEx(2, pair, 0, 1000, 1) == WAIT_IO_COMPLETION &&
          apc_runs == 1,
          "WaitForMultipleObjectsEx(..., TRUE) runs a pending APC first");
    CloseHandle(e);
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
    check(r == WAIT_OBJECT_0,
          "once the mutex is released, wait-all gets both");
    check(WaitForSingleObject(sem, 0) == WAIT_TIMEOUT,
          "the semaphore's permit is taken");
    check(ReleaseMutex(mutex), "and the mutex is ours to release");
    ReleaseSemaphore(sem, 1, NULL_PTR);
    check(WaitForSingleObject(h[0], 5000) == WAIT_OBJECT_0 && holder_done,
          "and the holder got through its ReleaseMutex");
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

    apc_tests();

    say("\r\nwait: ");
    say_u((DWORD)passes);
    say(" passed, ");
    say_u((DWORD)failures);
    say(" failed\r\n");
    ExitProcess((DWORD)failures);
}
