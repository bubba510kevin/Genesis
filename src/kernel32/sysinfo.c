/* The processors, and steering threads across them; priorities; time.
 *
 * Win32's shape over the NT calls in kernel/exec/nt_sys.c: a SYSTEM_INFO
 * instead of a SYSTEM_BASIC_INFORMATION, a BOOL and GetLastError instead of
 * an NTSTATUS, milliseconds and FILETIMEs instead of 100ns intervals. The
 * numbers themselves are the kernel's - the processor count is the number of
 * CPUs actually scheduling, and an affinity mask set here really keeps a
 * thread off the others. */

#include "kernel32.h"

/* err.c's - the DLL has no C library. */
void *memset(void *dst, int c, SIZE_T n);

static BOOL fail(NTSTATUS st) {
    k32_set_error_from_status(st);
    return FALSE;
}

/* --- the system ---------------------------------------------------------- */

void WINAPI GetSystemInfo(LPSYSTEM_INFO si) {
    SYSTEM_BASIC_INFORMATION b;
    SYSTEM_PROCESSOR_INFORMATION p;

    if (si == NULL_PTR) {
        return;
    }
    memset(si, 0, sizeof(*si));
    memset(&b, 0, sizeof(b));
    memset(&p, 0, sizeof(p));
    NtQuerySystemInformation(SystemBasicInformation, &b, sizeof(b), NULL_PTR);
    NtQuerySystemInformation(SystemProcessorInformation, &p, sizeof(p), NULL_PTR);
    si->wProcessorArchitecture      = PROCESSOR_ARCHITECTURE_AMD64;
    si->dwPageSize                  = b.PageSize;
    si->lpMinimumApplicationAddress = (LPVOID)b.MinimumUserModeAddress;
    si->lpMaximumApplicationAddress = (LPVOID)b.MaximumUserModeAddress;
    si->dwActiveProcessorMask       = (DWORD_PTR)b.ActiveProcessorsAffinityMask;
    si->dwNumberOfProcessors        = (DWORD)(unsigned char)b.NumberOfProcessors;
    si->dwProcessorType             = PROCESSOR_AMD_X8664;
    si->dwAllocationGranularity     = b.AllocationGranularity;
    si->wProcessorLevel             = p.ProcessorLevel;
    si->wProcessorRevision          = p.ProcessorRevision;
}

void WINAPI GetNativeSystemInfo(LPSYSTEM_INFO si) {
    /* A 64-bit process on a 64-bit system: the same answer. */
    GetSystemInfo(si);
}

static DWORD active_mask_count(KAFFINITY m) {
    DWORD n = 0;

    while (m != 0) {
        n += (DWORD)(m & 1);
        m >>= 1;
    }
    return n;
}

static KAFFINITY system_mask(void) {
    SYSTEM_BASIC_INFORMATION b;

    memset(&b, 0, sizeof(b));
    NtQuerySystemInformation(SystemBasicInformation, &b, sizeof(b), NULL_PTR);
    return b.ActiveProcessorsAffinityMask;
}

DWORD WINAPI GetActiveProcessorCount(WORD group) {
    if (group != 0 && group != ALL_PROCESSOR_GROUPS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return active_mask_count(system_mask());
}

DWORD WINAPI GetMaximumProcessorCount(WORD group) {
    SYSTEM_PROCESSOR_INFORMATION p;

    if (group != 0 && group != ALL_PROCESSOR_GROUPS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    memset(&p, 0, sizeof(p));
    NtQuerySystemInformation(SystemProcessorInformation, &p, sizeof(p), NULL_PTR);
    return p.MaximumProcessors;
}

WORD WINAPI GetActiveProcessorGroupCount(void)  { return 1; }
WORD WINAPI GetMaximumProcessorGroupCount(void) { return 1; }

DWORD WINAPI GetCurrentProcessorNumber(void) {
    return NtGetCurrentProcessorNumber();
}

void WINAPI GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER pn) {
    NtGetCurrentProcessorNumberEx(pn);
}

BOOL WINAPI GetLogicalProcessorInformation(
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer, PDWORD length) {
    DWORD got = 0;
    NTSTATUS st;

    if (length == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    st = NtQuerySystemInformation(SystemLogicalProcessorInformation, buffer,
                                  buffer != NULL_PTR ? *length : 0, &got);
    *length = got;
    if (st == STATUS_INFO_LENGTH_MISMATCH) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    return NT_SUCCESS(st) ? TRUE : fail(st);
}

BOOL WINAPI GetLogicalProcessorInformationEx(
        LOGICAL_PROCESSOR_RELATIONSHIP rel,
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer, PDWORD length) {
    DWORD got = 0, wanted = (DWORD)rel;
    NTSTATUS st;

    if (length == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    st = NtQuerySystemInformationEx(SystemLogicalProcessorAndGroupInformation,
                                    &wanted, sizeof(wanted), buffer,
                                    buffer != NULL_PTR ? *length : 0, &got);
    *length = got;
    if (st == STATUS_INFO_LENGTH_MISMATCH) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    return NT_SUCCESS(st) ? TRUE : fail(st);
}

/* --- affinity ------------------------------------------------------------------ */

BOOL WINAPI GetProcessAffinityMask(HANDLE process, PDWORD_PTR proc_mask,
                                   PDWORD_PTR sys_mask) {
    PROCESS_BASIC_INFORMATION pbi;
    NTSTATUS st;

    if (proc_mask == NULL_PTR || sys_mask == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    st = NtQueryInformationProcess(process, ProcessBasicInformation, &pbi,
                                   sizeof(pbi), NULL_PTR);
    if (!NT_SUCCESS(st)) {
        return fail(st);
    }
    *proc_mask = (DWORD_PTR)pbi.AffinityMask;
    *sys_mask  = (DWORD_PTR)system_mask();
    return TRUE;
}

BOOL WINAPI SetProcessAffinityMask(HANDLE process, DWORD_PTR mask) {
    KAFFINITY m = mask;
    NTSTATUS st = NtSetInformationProcess(process, ProcessAffinityMask, &m,
                                          sizeof(m));

    return NT_SUCCESS(st) ? TRUE : fail(st);
}

/* The previous mask, read through ThreadBasicInformation, which is how
 * Windows' own SetThreadAffinityMask answers it; 0 on failure. */
DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask) {
    THREAD_BASIC_INFORMATION tbi;
    KAFFINITY m = mask;
    NTSTATUS st;

    st = NtQueryInformationThread(thread, ThreadBasicInformation, &tbi,
                                  sizeof(tbi), NULL_PTR);
    if (!NT_SUCCESS(st)) {
        fail(st);
        return 0;
    }
    st = NtSetInformationThread(thread, ThreadAffinityMask, &m, sizeof(m));
    if (!NT_SUCCESS(st)) {
        fail(st);
        return 0;
    }
    return (DWORD_PTR)tbi.AffinityMask;
}

DWORD WINAPI SetThreadIdealProcessor(HANDLE thread, DWORD ideal) {
    NTSTATUS st = NtSetInformationThread(thread, ThreadIdealProcessor, &ideal,
                                         sizeof(ideal));

    /* The previous ideal processor comes back AS the status - a success
     * code that is a number. An error status has the top bit set. */
    if (!NT_SUCCESS(st)) {
        fail(st);
        return (DWORD)-1;
    }
    return (DWORD)st;
}

BOOL WINAPI GetThreadIdealProcessorEx(HANDLE thread, PPROCESSOR_NUMBER ideal) {
    NTSTATUS st;

    if (ideal == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    st = NtQueryInformationThread(thread, ThreadIdealProcessorEx, ideal,
                                  sizeof(*ideal), NULL_PTR);
    return NT_SUCCESS(st) ? TRUE : fail(st);
}

BOOL WINAPI SetThreadIdealProcessorEx(HANDLE thread, PPROCESSOR_NUMBER ideal,
                                      PPROCESSOR_NUMBER previous) {
    NTSTATUS st;

    if (ideal == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (previous != NULL_PTR && !GetThreadIdealProcessorEx(thread, previous)) {
        return FALSE;
    }
    st = NtSetInformationThread(thread, ThreadIdealProcessorEx, ideal,
                                sizeof(*ideal));
    return NT_SUCCESS(st) ? TRUE : fail(st);
}

BOOL WINAPI GetThreadGroupAffinity(HANDLE thread, PGROUP_AFFINITY ga) {
    NTSTATUS st;

    if (ga == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    st = NtQueryInformationThread(thread, ThreadGroupInformation, ga,
                                  sizeof(*ga), NULL_PTR);
    return NT_SUCCESS(st) ? TRUE : fail(st);
}

BOOL WINAPI SetThreadGroupAffinity(HANDLE thread, const GROUP_AFFINITY *ga,
                                   PGROUP_AFFINITY previous) {
    GROUP_AFFINITY copy;
    NTSTATUS st;

    if (ga == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (previous != NULL_PTR && !GetThreadGroupAffinity(thread, previous)) {
        return FALSE;
    }
    copy = *ga;
    st = NtSetInformationThread(thread, ThreadGroupInformation, &copy,
                                sizeof(copy));
    return NT_SUCCESS(st) ? TRUE : fail(st);
}

/* --- priorities ---------------------------------------------------------------- */

BOOL WINAPI SetThreadPriority(HANDLE thread, int priority) {
    LONG v = priority;
    NTSTATUS st = NtSetInformationThread(thread, ThreadBasePriority, &v,
                                         sizeof(v));

    return NT_SUCCESS(st) ? TRUE : fail(st);
}

int WINAPI GetThreadPriority(HANDLE thread) {
    LONG v = 0;
    NTSTATUS st = NtQueryInformationThread(thread, ThreadBasePriority, &v,
                                           sizeof(v), NULL_PTR);

    if (!NT_SUCCESS(st)) {
        fail(st);
        return THREAD_PRIORITY_ERROR_RETURN;
    }
    return (int)v;
}

/* PROCESS_PRIORITY_CLASS's one-byte class, 1..6, against the Win32 flags. */
static const DWORD class_flags[7] = {
    0, IDLE_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS, HIGH_PRIORITY_CLASS,
    REALTIME_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS,
    ABOVE_NORMAL_PRIORITY_CLASS
};

BOOL WINAPI SetPriorityClass(HANDLE process, DWORD cls) {
    BYTE pc[2];
    int i;
    NTSTATUS st;

    for (i = 1; i <= 6; i++) {
        if (class_flags[i] == cls) {
            break;
        }
    }
    if (i > 6) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pc[0] = 0;
    pc[1] = (BYTE)i;
    st = NtSetInformationProcess(process, ProcessPriorityClass, pc, sizeof(pc));
    return NT_SUCCESS(st) ? TRUE : fail(st);
}

DWORD WINAPI GetPriorityClass(HANDLE process) {
    BYTE pc[2] = {0, 0};
    NTSTATUS st = NtQueryInformationProcess(process, ProcessPriorityClass, pc,
                                            sizeof(pc), NULL_PTR);

    if (!NT_SUCCESS(st) || pc[1] == 0 || pc[1] > 6) {
        fail(st);
        return 0;
    }
    return class_flags[pc[1]];
}

/* --- times ---------------------------------------------------------------------- */

static void to_filetime(LPFILETIME ft, long long v) {
    if (ft != NULL_PTR) {
        ft->dwLowDateTime  = (DWORD)v;
        ft->dwHighDateTime = (DWORD)((unsigned long long)v >> 32);
    }
}

static BOOL times_out(KERNEL_USER_TIMES *t, LPFILETIME c, LPFILETIME e,
                      LPFILETIME k, LPFILETIME u) {
    to_filetime(c, t->CreateTime.QuadPart);
    to_filetime(e, t->ExitTime.QuadPart);
    to_filetime(k, t->KernelTime.QuadPart);
    to_filetime(u, t->UserTime.QuadPart);
    return TRUE;
}

BOOL WINAPI GetProcessTimes(HANDLE process, LPFILETIME creation,
                            LPFILETIME exit_time, LPFILETIME kernel,
                            LPFILETIME user) {
    KERNEL_USER_TIMES t;
    NTSTATUS st = NtQueryInformationProcess(process, ProcessTimes, &t,
                                            sizeof(t), NULL_PTR);

    return NT_SUCCESS(st) ? times_out(&t, creation, exit_time, kernel, user)
                          : fail(st);
}

BOOL WINAPI GetThreadTimes(HANDLE thread, LPFILETIME creation,
                           LPFILETIME exit_time, LPFILETIME kernel,
                           LPFILETIME user) {
    KERNEL_USER_TIMES t;
    NTSTATUS st = NtQueryInformationThread(thread, ThreadTimes, &t, sizeof(t),
                                           NULL_PTR);

    return NT_SUCCESS(st) ? times_out(&t, creation, exit_time, kernel, user)
                          : fail(st);
}

/* --- yielding and sleeping ------------------------------------------------------ */

BOOL WINAPI SwitchToThread(void) {
    return NtYieldExecution() != STATUS_NO_YIELD_PERFORMED;
}

DWORD WINAPI SleepEx(DWORD ms, BOOL alertable) {
    LARGE_INTEGER interval;

    if (ms == INFINITE) {
        /* Forever, a long sleep at a time - there is nothing to wake it. */
        for (;;) {
            interval.QuadPart = -36000000000LL;          /* an hour */
            NtDelayExecution((BOOLEAN)(alertable != 0), &interval);
        }
    }
    interval.QuadPart = -(long long)ms * 10000LL;
    NtDelayExecution((BOOLEAN)(alertable != 0), &interval);
    return 0;
}

void WINAPI Sleep(DWORD ms) {
    SleepEx(ms, FALSE);
}

/* --- clocks ---------------------------------------------------------------------- */

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *count) {
    NTSTATUS st;

    if (count == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    st = NtQueryPerformanceCounter(count, NULL_PTR);
    return NT_SUCCESS(st) ? TRUE : fail(st);
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *freq) {
    LARGE_INTEGER c;
    NTSTATUS st;

    if (freq == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    st = NtQueryPerformanceCounter(&c, freq);
    return NT_SUCCESS(st) ? TRUE : fail(st);
}

ULONGLONG WINAPI GetTickCount64(void) {
    LARGE_INTEGER c, f;

    if (!NT_SUCCESS(NtQueryPerformanceCounter(&c, &f)) || f.QuadPart <= 0) {
        return 0;
    }
    /* Split to keep the multiply from overflowing a long uptime. */
    return (ULONGLONG)(c.QuadPart / f.QuadPart) * 1000ULL +
           (ULONGLONG)((c.QuadPart % f.QuadPart) * 1000LL / f.QuadPart);
}

DWORD WINAPI GetTickCount(void) {
    return (DWORD)GetTickCount64();
}

void WINAPI GetSystemTimeAsFileTime(LPFILETIME ft) {
    LARGE_INTEGER t;

    t.QuadPart = 0;
    NtQuerySystemTime(&t);
    to_filetime(ft, t.QuadPart);
}

void WINAPI GetSystemTimePreciseAsFileTime(LPFILETIME ft) {
    GetSystemTimeAsFileTime(ft);
}

/* --- synchronisation wrappers ----------------------------------------------------
 *
 * Most of kernel32's synchronisation surface is FORWARDED to ntdll in the
 * export table (kernel32.def) - it is Rtl* with another name, as on Windows.
 * These are the few whose Win32 signature differs from the Rtl one: a BOOL
 * where ntdll returns an NTSTATUS or a one-byte BOOLEAN, or milliseconds
 * where ntdll takes an NT interval. */

BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs,
                                                  DWORD spin) {
    RtlInitializeCriticalSectionAndSpinCount(cs, spin);
    return TRUE;
}

BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin,
                                        DWORD flags) {
    RtlInitializeCriticalSectionEx(cs, spin, flags);
    return TRUE;
}

BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION cs) {
    return RtlTryEnterCriticalSection(cs) ? TRUE : FALSE;
}

static LARGE_INTEGER *ms_timeout(DWORD ms, LARGE_INTEGER *buf) {
    if (ms == INFINITE) {
        return NULL_PTR;
    }
    buf->QuadPart = -(long long)ms * 10000LL;
    return buf;
}

BOOL WINAPI SleepConditionVariableCS(PCONDITION_VARIABLE cv,
                                     LPCRITICAL_SECTION cs, DWORD ms) {
    LARGE_INTEGER t;
    NTSTATUS st = RtlSleepConditionVariableCS(cv, cs, ms_timeout(ms, &t));

    if (st == STATUS_TIMEOUT) {
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l,
                                      DWORD ms, DWORD flags) {
    LARGE_INTEGER t;
    NTSTATUS st = RtlSleepConditionVariableSRW(cv, l, ms_timeout(ms, &t), flags);

    if (st == STATUS_TIMEOUT) {
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}
