/* Threads, and waiting on them.
 *
 * Thin by design, like the rest of this DLL: the kernel owns what a thread
 * is (kernel/exec/nt.c, NtCreateThreadEx), ntdll owns how one starts and
 * ends (RtlUserThreadStart / RtlExitUserThread), and what is left here is
 * the Win32 shape - a DWORD thread id, STILL_ACTIVE, WAIT_OBJECT_0, and
 * milliseconds instead of 100ns intervals.
 */

#include "kernel32.h"

HANDLE WINAPI CreateThread(LPVOID security, SIZE_T stack_size,
                           LPTHREAD_START_ROUTINE start, LPVOID parameter,
                           DWORD flags, LPDWORD thread_id) {
    HANDLE h = NULL_PTR;
    NTSTATUS st;

    (void)security;                 /* no inheritable handles yet */

    if (start == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL_PTR;
    }

    /* STACK_SIZE_PARAM_IS_A_RESERVATION changes what the number means on
     * Windows (reserve vs commit). Here every stack is committed up front,
     * so both readings ask for the same thing. */
    /* CREATE_SUSPENDED is Win32's 0x4; the native flag is 0x1. */
    st = NtCreateThreadEx(&h, THREAD_ALL_ACCESS, NULL_PTR, NtCurrentProcess(),
                          (PVOID)start, parameter,
                          (flags & CREATE_SUSPENDED)
                              ? THREAD_CREATE_FLAGS_CREATE_SUSPENDED : 0,
                          0, stack_size, 0, NULL_PTR);
    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return NULL_PTR;
    }
    if (thread_id != NULL_PTR) {
        *thread_id = GetThreadId(h);
    }
    return h;
}

void WINAPI ExitThread(DWORD code) {
    RtlExitUserThread((NTSTATUS)code);
}

HANDLE WINAPI GetCurrentThread(void) {
    /* A pseudo-handle, like GetCurrentProcess's -1: -2 means "me" wherever
     * it is passed and is never closed. */
    return NtCurrentThread();
}

DWORD WINAPI GetCurrentThreadId(void) {
    /* A load from this thread's own TEB - which is the whole reason every
     * thread has to have one. With a single shared TEB every thread would
     * report the main thread's id. */
    return (DWORD)NtCurrentTeb()->ClientIdThread;
}

/* Both of these are ThreadBasicInformation, which is what they are built on
 * in Windows too. */
static BOOL k32_thread_info(HANDLE thread, THREAD_BASIC_INFORMATION *tbi) {
    NTSTATUS st = NtQueryInformationThread(thread, ThreadBasicInformation,
                                           tbi, sizeof(*tbi), NULL_PTR);

    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return 0;
    }
    return 1;
}

DWORD WINAPI GetThreadId(HANDLE thread) {
    THREAD_BASIC_INFORMATION tbi;

    if (!k32_thread_info(thread, &tbi)) {
        return 0;
    }
    return (DWORD)(SIZE_T)tbi.ClientId.UniqueThread;
}

BOOL WINAPI GetExitCodeThread(HANDLE thread, LPDWORD code) {
    THREAD_BASIC_INFORMATION tbi;

    if (code == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!k32_thread_info(thread, &tbi)) {
        return 0;
    }
    /* STILL_ACTIVE (259) while it runs - the documented, and documentedly
     * ambiguous, answer: a thread that really exits with 259 is
     * indistinguishable from a running one, which is why WaitForSingleObject
     * is the right way to ask "has it finished". */
    *code = tbi.ExitStatus;
    return 1;
}

/* Wherever the thread is, it stops for good, with `code` as its exit code.
 * Whatever it was halfway through changing stays half-changed - which is
 * why this is a last resort on Windows too. Mutexes it held are abandoned. */
BOOL WINAPI TerminateThread(HANDLE thread, DWORD code) {
    NTSTATUS st = NtTerminateThread(thread, (NTSTATUS)code);

    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return 0;
    }
    return 1;
}

DWORD WINAPI SuspendThread(HANDLE thread) {
    DWORD prev = 0;
    NTSTATUS st = NtSuspendThread(thread, &prev);

    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return (DWORD)-1;
    }
    return prev;
}

DWORD WINAPI ResumeThread(HANDLE thread) {
    DWORD prev = 0;
    NTSTATUS st = NtResumeThread(thread, &prev);

    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return (DWORD)-1;
    }
    return prev;
}

DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    return WaitForSingleObjectEx(handle, milliseconds, 0);
}

DWORD WINAPI WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds,
                                   BOOL alertable) {
    LARGE_INTEGER timeout;
    NTSTATUS st;

    if (milliseconds == INFINITE) {
        st = NtWaitForSingleObject(handle, alertable ? 1 : 0, NULL_PTR);
    } else {
        /* Negative is RELATIVE, in 100ns units. */
        timeout.QuadPart = -(long long)milliseconds * 10000LL;
        st = NtWaitForSingleObject(handle, alertable ? 1 : 0, &timeout);
    }
    if (st == STATUS_SUCCESS) {
        return WAIT_OBJECT_0;
    }
    if (st == STATUS_USER_APC) {
        return WAIT_IO_COMPLETION;          /* APCs ran; nothing was taken */
    }
    if (st == STATUS_ABANDONED_WAIT_0) {
        /* The caller owns the mutex now - the previous owner died with it. */
        return WAIT_ABANDONED;
    }
    if (st == STATUS_TIMEOUT) {
        return WAIT_TIMEOUT;
    }
    k32_set_error_from_status(st);
    return WAIT_FAILED;
}

/* Milliseconds to NT's relative 100ns timeout; NULL for INFINITE. */
static LARGE_INTEGER *k32_timeout(DWORD milliseconds, LARGE_INTEGER *t) {
    if (milliseconds == INFINITE) {
        return NULL_PTR;
    }
    t->QuadPart = -(long long)milliseconds * 10000LL;
    return t;
}

DWORD WINAPI WaitForMultipleObjects(DWORD count, const HANDLE *handles,
                                    BOOL wait_all, DWORD milliseconds) {
    return WaitForMultipleObjectsEx(count, handles, wait_all, milliseconds, 0);
}

DWORD WINAPI QueueUserAPC(PAPCFUNC fn, HANDLE thread, ULONG_PTR data) {
    /* PAPCFUNC takes one argument and an NT APC routine three; under the
     * Win64 convention the extra two are simply never read. */
    NTSTATUS st = NtQueueApcThread(thread, (PPS_APC_ROUTINE)(void *)fn,
                                   (PVOID)data, NULL_PTR, NULL_PTR);

    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return 0;
    }
    return 1;
}

DWORD WINAPI WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles,
                                      BOOL wait_all, DWORD milliseconds,
                                      BOOL alertable) {
    LARGE_INTEGER timeout;
    NTSTATUS st;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || handles == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    st = NtWaitForMultipleObjects(count, (HANDLE *)handles,
                                  wait_all ? WaitAll : WaitAny,
                                  alertable ? 1 : 0,
                                  k32_timeout(milliseconds, &timeout));
    if ((DWORD)st == STATUS_USER_APC) {
        return WAIT_IO_COMPLETION;
    }
    /* WAIT_OBJECT_0 + i and WAIT_ABANDONED_0 + i are the NT statuses
     * themselves, which is why Win32 chose those values. */
    if ((DWORD)st < count ||
        ((DWORD)st >= STATUS_ABANDONED_WAIT_0 &&
         (DWORD)st < STATUS_ABANDONED_WAIT_0 + count) ||
        (DWORD)st == STATUS_TIMEOUT) {
        return (DWORD)st;
    }
    k32_set_error_from_status(st);
    return WAIT_FAILED;
}

/* --- events and semaphores --------------------------------------------- */

HANDLE WINAPI CreateEventW(LPVOID security, BOOL manual_reset,
                           BOOL initial_state, LPCWSTR name) {
    HANDLE h = NULL_PTR;
    NTSTATUS st;

    (void)security;
    if (name != NULL_PTR) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return NULL_PTR;
    }
    st = NtCreateEvent(&h, EVENT_ALL_ACCESS, NULL_PTR,
                       manual_reset ? NotificationEvent : SynchronizationEvent,
                       initial_state ? 1 : 0);
    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return NULL_PTR;
    }
    SetLastError(ERROR_SUCCESS);
    return h;
}

HANDLE WINAPI CreateEventA(LPVOID security, BOOL manual_reset,
                           BOOL initial_state, LPCSTR name) {
    if (name != NULL_PTR) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return NULL_PTR;
    }
    return CreateEventW(security, manual_reset, initial_state, NULL_PTR);
}

BOOL WINAPI SetEvent(HANDLE event) {
    NTSTATUS st = NtSetEvent(event, NULL_PTR);

    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return 0;
    }
    return 1;
}

BOOL WINAPI ResetEvent(HANDLE event) {
    NTSTATUS st = NtResetEvent(event, NULL_PTR);

    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return 0;
    }
    return 1;
}

HANDLE WINAPI CreateSemaphoreW(LPVOID security, LONG initial, LONG maximum,
                               LPCWSTR name) {
    HANDLE h = NULL_PTR;
    NTSTATUS st;

    (void)security;
    if (name != NULL_PTR) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return NULL_PTR;
    }
    st = NtCreateSemaphore(&h, SEMAPHORE_ALL_ACCESS, NULL_PTR, initial,
                           maximum);
    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return NULL_PTR;
    }
    SetLastError(ERROR_SUCCESS);
    return h;
}

HANDLE WINAPI CreateSemaphoreA(LPVOID security, LONG initial, LONG maximum,
                               LPCSTR name) {
    if (name != NULL_PTR) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return NULL_PTR;
    }
    return CreateSemaphoreW(security, initial, maximum, NULL_PTR);
}

BOOL WINAPI ReleaseSemaphore(HANDLE semaphore, LONG count, LONG *previous) {
    NTSTATUS st = NtReleaseSemaphore(semaphore, count, previous);

    if (!NT_SUCCESS(st)) {
        /* Past the maximum: ERROR_TOO_MANY_POSTS on Windows. The kernel
         * answers INVALID_PARAMETER for that and for a count below one. */
        if (st == STATUS_INVALID_PARAMETER && count > 0) {
            SetLastError(ERROR_TOO_MANY_POSTS);
        } else {
            k32_set_error_from_status(st);
        }
        return 0;
    }
    return 1;
}

/* --- mutexes ----------------------------------------------------------- */

HANDLE WINAPI CreateMutexW(LPVOID security, BOOL initial_owner, LPCWSTR name) {
    HANDLE h = NULL_PTR;
    NTSTATUS st;

    (void)security;                 /* no inheritable handles yet */
    if (name != NULL_PTR) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return NULL_PTR;
    }
    st = NtCreateMutant(&h, MUTANT_ALL_ACCESS, NULL_PTR,
                        initial_owner ? 1 : 0);
    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return NULL_PTR;
    }
    SetLastError(ERROR_SUCCESS);    /* not ERROR_ALREADY_EXISTS: it is new */
    return h;
}

HANDLE WINAPI CreateMutexA(LPVOID security, BOOL initial_owner, LPCSTR name) {
    if (name != NULL_PTR) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return NULL_PTR;
    }
    return CreateMutexW(security, initial_owner, NULL_PTR);
}

BOOL WINAPI ReleaseMutex(HANDLE mutex) {
    NTSTATUS st = NtReleaseMutant(mutex, NULL_PTR);

    if (!NT_SUCCESS(st)) {
        /* STATUS_MUTANT_NOT_OWNED - the caller does not hold it. */
        k32_set_error_from_status(st);
        return 0;
    }
    return 1;
}
