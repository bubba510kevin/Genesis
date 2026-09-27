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

    /* CREATE_SUSPENDED is refused rather than ignored: a program that asks
     * for it is about to fill something in before ResumeThread, and a thread
     * that is already running would race it. There is no ResumeThread. */
    if (flags & CREATE_SUSPENDED) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return NULL_PTR;
    }
    if (start == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL_PTR;
    }

    /* STACK_SIZE_PARAM_IS_A_RESERVATION changes what the number means on
     * Windows (reserve vs commit). Here every stack is committed up front,
     * so both readings ask for the same thing. */
    st = NtCreateThreadEx(&h, THREAD_ALL_ACCESS, NULL_PTR, NtCurrentProcess(),
                          (PVOID)start, parameter, 0, 0, stack_size, 0,
                          NULL_PTR);
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

DWORD WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    LARGE_INTEGER timeout;
    NTSTATUS st;

    if (milliseconds == INFINITE) {
        st = NtWaitForSingleObject(handle, 0, NULL_PTR);
    } else {
        /* Negative is RELATIVE, in 100ns units. */
        timeout.QuadPart = -(long long)milliseconds * 10000LL;
        st = NtWaitForSingleObject(handle, 0, &timeout);
    }
    if (st == STATUS_SUCCESS) {
        return WAIT_OBJECT_0;
    }
    if (st == STATUS_TIMEOUT) {
        return WAIT_TIMEOUT;
    }
    k32_set_error_from_status(st);
    return WAIT_FAILED;
}
