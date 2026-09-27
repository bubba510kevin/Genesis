/* smp.exe - the multiprocessor, from a Windows program that knows nothing
 * about Genesis.
 *
 * Imports kernel32.dll alone and calls it exactly as a Win32 program would:
 * GetSystemInfo, GetLogicalProcessorInformation(Ex), the affinity and ideal-
 * processor calls, priorities, the clocks, and the synchronisation
 * primitives - with threads really running on different CPUs at the same
 * time, which is what makes the synchronisation checks mean anything.
 *
 * The concurrency proof is the ping-pong: two threads pinned to two CPUs
 * hand a token back and forth by spinning on shared memory, never blocking
 * and never yielding. On one CPU each hand-off would wait for a timer
 * preemption, and the exchanges below would take minutes; finishing them in
 * a second or two is only possible if both threads run at once.
 *
 * Every check prints ok/FAIL; the last line is a tally in systest's shape
 * for tools/guest_run.py. */

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

static void say_u(unsigned long long v) {
    char buf[24];
    int i = 23;

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

static void section(const char *s) {
    say("\r\n");
    say(s);
    say("\r\n");
}

static DWORD popcount(DWORD_PTR m) {
    DWORD n = 0;

    while (m) {
        n += (DWORD)(m & 1);
        m >>= 1;
    }
    return n;
}

static long long qpc(void) {
    LARGE_INTEGER c;

    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

static long long qpf(void) {
    LARGE_INTEGER f;

    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

static DWORD ncpu;
static DWORD_PTR all_mask;

/* --- the system ------------------------------------------------------------ */

static void test_system(void) {
    SYSTEM_INFO si;
    DWORD len = 0;
    BOOL ok;

    section("smp: the system");
    GetSystemInfo(&si);
    ncpu = si.dwNumberOfProcessors;
    all_mask = si.dwActiveProcessorMask;
    check(ncpu >= 1, "GetSystemInfo reports at least one processor");
    say("        processors: ");
    say_u(ncpu);
    say("\r\n");
    check(popcount(all_mask) == ncpu, "and one mask bit per processor");
    check(si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64,
          "architecture is AMD64");
    check(si.dwPageSize == 4096 && si.dwAllocationGranularity == 0x10000,
          "page size 4K, allocation granularity 64K");
    check(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) == ncpu,
          "GetActiveProcessorCount agrees");
    check(GetMaximumProcessorCount(0) >= ncpu, "GetMaximumProcessorCount >= active");
    check(GetActiveProcessorGroupCount() == 1, "one processor group");
    check(GetCurrentProcessorNumber() < ncpu,
          "GetCurrentProcessorNumber names a real processor");

    ok = GetLogicalProcessorInformation(NULL_PTR, &len);
    check(!ok && GetLastError() == ERROR_INSUFFICIENT_BUFFER && len != 0,
          "GetLogicalProcessorInformation sizes its buffer");
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION lpi[16];
        DWORD i, cores = 0;
        DWORD_PTR seen = 0;

        len = sizeof(lpi);
        check(GetLogicalProcessorInformation(lpi, &len), "and fills it");
        for (i = 0; i < len / sizeof(lpi[0]); i++) {
            if (lpi[i].Relationship == RelationProcessorCore) {
                cores++;
                seen |= lpi[i].ProcessorMask;
            }
        }
        check(cores == ncpu && seen == all_mask,
              "one core entry per processor, covering the mask");
    }
    {
        BYTE buf[1024];
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e;
        DWORD off = 0, cores = 0, groups_active = 0;

        len = sizeof(buf);
        check(GetLogicalProcessorInformationEx(RelationAll,
                  (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len),
              "GetLogicalProcessorInformationEx(RelationAll)");
        while (off < len) {
            e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (e->Size == 0) {
                break;
            }
            if (e->Relationship == RelationProcessorCore) {
                cores++;
            } else if (e->Relationship == RelationGroup) {
                groups_active = e->Group.GroupInfo[0].ActiveProcessorCount;
            }
            off += e->Size;
        }
        check(cores == ncpu, "a processor record per core");
        check(groups_active == ncpu, "and the group reports every one active");
    }
}

/* --- affinity -------------------------------------------------------------------- */

static void test_affinity(void) {
    DWORD_PTR pm = 0, sm = 0, prev;
    DWORD k;
    int landed = 1;

    section("smp: affinity");
    check(GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm),
          "GetProcessAffinityMask");
    check(pm == all_mask && sm == all_mask,
          "the process may run on every processor");

    /* Move this thread across every CPU. Each move takes effect inside the
     * call: the very next instruction runs on the named CPU. */
    for (k = 0; k < ncpu; k++) {
        prev = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << k);
        if (prev == 0 || GetCurrentProcessorNumber() != k) {
            landed = 0;
        }
    }
    check(landed, "SetThreadAffinityMask moves the thread to each processor");
    prev = SetThreadAffinityMask(GetCurrentThread(), all_mask);
    check(prev == ((DWORD_PTR)1 << (ncpu - 1)),
          "and returns the previous mask");
    check(SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << 40) == 0 &&
          GetLastError() == ERROR_INVALID_PARAMETER,
          "a mask naming no real processor is refused");

    {
        GROUP_AFFINITY ga, old;

        ga.Mask = 1;
        ga.Group = 0;
        ga.Reserved[0] = ga.Reserved[1] = ga.Reserved[2] = 0;
        check(SetThreadGroupAffinity(GetCurrentThread(), &ga, &old),
              "SetThreadGroupAffinity");
        check(GetCurrentProcessorNumber() == 0 && old.Mask == all_mask,
              "lands on processor 0 and reports the old mask");
        ga.Mask = all_mask;
        SetThreadGroupAffinity(GetCurrentThread(), &ga, NULL_PTR);
        check(GetThreadGroupAffinity(GetCurrentThread(), &old) &&
              old.Mask == all_mask, "GetThreadGroupAffinity reads it back");
    }

    if (ncpu > 1) {
        check(SetProcessAffinityMask(GetCurrentProcess(), 2),
              "SetProcessAffinityMask to processor 1 alone");
        check(GetCurrentProcessorNumber() == 1, "moves the calling thread there");
        GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
        check(pm == 2, "and GetProcessAffinityMask says so");
        SetProcessAffinityMask(GetCurrentProcess(), all_mask);
    }

    {
        DWORD before = SetThreadIdealProcessor(GetCurrentThread(), ncpu - 1);
        DWORD again  = SetThreadIdealProcessor(GetCurrentThread(), 0);
        PROCESSOR_NUMBER pn;

        check(before != (DWORD)-1 && again == ncpu - 1,
              "SetThreadIdealProcessor returns the previous ideal");
        check(GetThreadIdealProcessorEx(GetCurrentThread(), &pn) &&
              pn.Group == 0 && pn.Number == 0,
              "GetThreadIdealProcessorEx reads it back");
    }
}

/* --- priorities and time ----------------------------------------------------------- */

static void test_priority_and_time(void) {
    long long f, t0, t1;
    ULONGLONG k0, k1;
    FILETIME c, e, k, u, now;

    section("smp: priorities and clocks");
    check(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) &&
          GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_ABOVE_NORMAL,
          "SetThreadPriority / GetThreadPriority");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    check(!SetThreadPriority(GetCurrentThread(), 99),
          "an out-of-range priority is refused");
    check(SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) &&
          GetPriorityClass(GetCurrentProcess()) == HIGH_PRIORITY_CLASS,
          "SetPriorityClass / GetPriorityClass");
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    f = qpf();
    check(f > 1000000, "QueryPerformanceFrequency is a real high-resolution rate");
    t0 = qpc();
    k0 = GetTickCount64();
    Sleep(50);
    t1 = qpc();
    k1 = GetTickCount64();
    check((t1 - t0) * 1000 / f >= 50, "Sleep(50) sleeps at least 50ms");
    check((t1 - t0) * 1000 / f < 1000, "and not wildly longer");
    check(k1 - k0 >= 40, "GetTickCount64 moves with it");
    GetSystemTimeAsFileTime(&now);
    check(now.dwHighDateTime > 0x01D00000u,
          "GetSystemTimeAsFileTime is a plausible date (after 2014)");
    check(GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u),
          "GetThreadTimes");
    check(GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) &&
          (u.dwLowDateTime | u.dwHighDateTime) != 0,
          "GetProcessTimes reports user time spent");
    SwitchToThread();
    check(1, "SwitchToThread returns");
}

/* --- threads on every CPU at once ---------------------------------------------------- */

#define MAXT 8

static volatile LONG arrived;
static volatile DWORD ran_on[MAXT];

static DWORD WINAPI pinned_worker(LPVOID p) {
    DWORD i = (DWORD)(SIZE_T)p;

    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << i);
    ran_on[i] = GetCurrentProcessorNumber() + 1;
    __atomic_add_fetch(&arrived, 1, __ATOMIC_ACQ_REL);
    return 0;
}

/* Ping-pong: thread A writes an odd number, thread B answers with the next
 * even one. Neither blocks or yields. */
#define PINGS 200000
static volatile LONG ball;

static DWORD WINAPI pong(LPVOID p) {
    LONG i;

    (void)p;
    SetThreadAffinityMask(GetCurrentThread(), 2);
    for (i = 0; i < PINGS; i++) {
        while (__atomic_load_n(&ball, __ATOMIC_ACQUIRE) != 2 * i + 1) {
            __asm__ volatile ("pause");
        }
        __atomic_store_n(&ball, 2 * i + 2, __ATOMIC_RELEASE);
    }
    return 0;
}

static void test_parallel(void) {
    HANDLE h[MAXT];
    DWORD i, n = ncpu < MAXT ? ncpu : MAXT;
    int distinct = 1;

    section("smp: threads on every processor");
    arrived = 0;
    for (i = 0; i < n; i++) {
        ran_on[i] = 0;
        h[i] = CreateThread(NULL_PTR, 0, pinned_worker, (LPVOID)(SIZE_T)i, 0,
                            NULL_PTR);
    }
    for (i = 0; i < n; i++) {
        WaitForSingleObject(h[i], INFINITE);
        CloseHandle(h[i]);
    }
    for (i = 0; i < n; i++) {
        if (ran_on[i] != i + 1) {
            distinct = 0;
        }
    }
    check(arrived == (LONG)n, "one thread per processor ran");
    check(distinct, "each on the processor it pinned itself to");

    if (ncpu > 1) {
        HANDLE t;
        long long t0, t1, f = qpf();
        LONG k;

        SetThreadAffinityMask(GetCurrentThread(), 1);
        ball = 0;
        t = CreateThread(NULL_PTR, 0, pong, NULL_PTR, 0, NULL_PTR);
        t0 = qpc();
        for (k = 0; k < PINGS; k++) {
            __atomic_store_n(&ball, 2 * k + 1, __ATOMIC_RELEASE);
            while (__atomic_load_n(&ball, __ATOMIC_ACQUIRE) != 2 * k + 2) {
                __asm__ volatile ("pause");
            }
        }
        t1 = qpc();
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        SetThreadAffinityMask(GetCurrentThread(), all_mask);
        say("        200000 cross-CPU round trips in ");
        say_u((unsigned long long)((t1 - t0) * 1000 / f));
        say("ms\r\n");
        check((t1 - t0) < f * 20,
              "two spinning threads exchange 200000 times - both running AT ONCE");
    }
}

/* --- synchronisation under contention -------------------------------------------------- */

#define ROUNDS 20000

static CRITICAL_SECTION cs;
static SRWLOCK srw;
static volatile long long shared_plain;      /* deliberately NOT atomic */

static DWORD WINAPI cs_worker(LPVOID p) {
    int i;

    (void)p;
    for (i = 0; i < ROUNDS; i++) {
        EnterCriticalSection(&cs);
        shared_plain++;
        LeaveCriticalSection(&cs);
    }
    return 0;
}

static DWORD WINAPI srw_worker(LPVOID p) {
    int i;

    (void)p;
    for (i = 0; i < ROUNDS; i++) {
        AcquireSRWLockExclusive(&srw);
        shared_plain++;
        ReleaseSRWLockExclusive(&srw);
    }
    return 0;
}

static void run_workers(LPTHREAD_START_ROUTINE fn, DWORD n) {
    HANDLE h[MAXT];
    DWORD i;

    for (i = 0; i < n; i++) {
        h[i] = CreateThread(NULL_PTR, 0, fn, NULL_PTR, 0, NULL_PTR);
    }
    for (i = 0; i < n; i++) {
        WaitForSingleObject(h[i], INFINITE);
        CloseHandle(h[i]);
    }
}

static CONDITION_VARIABLE cv;
static volatile int cv_flag;

static DWORD WINAPI cv_producer(LPVOID p) {
    (void)p;
    Sleep(30);
    EnterCriticalSection(&cs);
    cv_flag = 1;
    LeaveCriticalSection(&cs);
    WakeConditionVariable(&cv);
    return 0;
}

typedef struct node { SLIST_ENTRY e; int v; } node_t;
static SLIST_HEADER slist;
static node_t nodes[MAXT][100];

static DWORD WINAPI slist_worker(LPVOID p) {
    DWORD t = (DWORD)(SIZE_T)p;
    int i;

    for (i = 0; i < 100; i++) {
        nodes[t][i].v = (int)(t * 1000 + i);
        InterlockedPushEntrySList(&slist, &nodes[t][i].e);
    }
    return 0;
}

static DWORD WINAPI heap_worker(LPVOID p) {
    DWORD t = (DWORD)(SIZE_T)p;
    HANDLE heap = GetProcessHeap();
    int i;
    DWORD bad = 0;

    for (i = 0; i < 500; i++) {
        BYTE *b = (BYTE *)HeapAlloc(heap, 0, 48);
        int j;

        if (b == NULL_PTR) {
            bad++;
            continue;
        }
        for (j = 0; j < 48; j++) {
            b[j] = (BYTE)(t + 1);
        }
        for (j = 0; j < 48; j++) {
            if (b[j] != (BYTE)(t + 1)) {
                bad++;
            }
        }
        HeapFree(heap, 0, b);
    }
    return bad;
}

static void test_sync(void) {
    DWORD n = ncpu < MAXT ? ncpu : MAXT;

    if (n < 2) {
        n = 2;
    }
    section("smp: synchronisation under real contention");

    InitializeCriticalSection(&cs);
    shared_plain = 0;
    run_workers(cs_worker, n);
    check(shared_plain == (long long)n * ROUNDS,
          "a CRITICAL_SECTION loses no increments across processors");

    check(TryEnterCriticalSection(&cs), "TryEnterCriticalSection on a free section");
    check(TryEnterCriticalSection(&cs), "and again - it is recursive for the owner");
    LeaveCriticalSection(&cs);
    LeaveCriticalSection(&cs);

    InitializeSRWLock(&srw);
    shared_plain = 0;
    run_workers(srw_worker, n);
    check(shared_plain == (long long)n * ROUNDS,
          "an SRWLOCK (exclusive) loses no increments");
    AcquireSRWLockShared(&srw);
    check(TryAcquireSRWLockShared(&srw), "two shared holders at once");
    check(!TryAcquireSRWLockExclusive(&srw), "and no exclusive while they hold it");
    ReleaseSRWLockShared(&srw);
    ReleaseSRWLockShared(&srw);
    check(TryAcquireSRWLockExclusive(&srw), "exclusive once they let go");
    ReleaseSRWLockExclusive(&srw);

    InitializeConditionVariable(&cv);
    cv_flag = 0;
    {
        HANDLE p = CreateThread(NULL_PTR, 0, cv_producer, NULL_PTR, 0, NULL_PTR);
        BOOL woke = TRUE;

        EnterCriticalSection(&cs);
        while (!cv_flag && woke) {
            woke = SleepConditionVariableCS(&cv, &cs, 5000);
        }
        LeaveCriticalSection(&cs);
        check(cv_flag == 1 && woke, "SleepConditionVariableCS wakes on the signal");
        WaitForSingleObject(p, INFINITE);
        CloseHandle(p);
    }
    EnterCriticalSection(&cs);
    check(!SleepConditionVariableCS(&cv, &cs, 20) && GetLastError() == ERROR_TIMEOUT,
          "and times out with ERROR_TIMEOUT when nobody signals");
    LeaveCriticalSection(&cs);
    DeleteCriticalSection(&cs);

    InitializeSListHead(&slist);
    {
        HANDLE h[MAXT];
        DWORD i, popped = 0;

        for (i = 0; i < n; i++) {
            h[i] = CreateThread(NULL_PTR, 0, slist_worker, (LPVOID)(SIZE_T)i, 0,
                                NULL_PTR);
        }
        for (i = 0; i < n; i++) {
            WaitForSingleObject(h[i], INFINITE);
            CloseHandle(h[i]);
        }
        check(QueryDepthSList(&slist) == n * 100,
              "interlocked SList pushes from every thread all land");
        while (InterlockedPopEntrySList(&slist) != NULL_PTR) {
            popped++;
        }
        check(popped == n * 100 && QueryDepthSList(&slist) == 0,
              "and pop back out, every one");
    }

    {
        HANDLE h[MAXT];
        DWORD i, code, bad = 0;

        for (i = 0; i < n; i++) {
            h[i] = CreateThread(NULL_PTR, 0, heap_worker, (LPVOID)(SIZE_T)i, 0,
                                NULL_PTR);
        }
        for (i = 0; i < n; i++) {
            WaitForSingleObject(h[i], INFINITE);
            GetExitCodeThread(h[i], &code);
            bad += code;
            CloseHandle(h[i]);
        }
        check(bad == 0, "the process heap survives concurrent HeapAlloc/HeapFree");
    }
}

void start(void) {
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("smp: the multiprocessor, from Win32\r\n");

    test_system();
    test_affinity();
    test_priority_and_time();
    test_parallel();
    test_sync();

    say("\r\nsmp: ");
    say_u((unsigned long long)passes);
    say(" passed, ");
    say_u((unsigned long long)failures);
    say(" failed\r\n");
    ExitProcess((DWORD)failures);
}
