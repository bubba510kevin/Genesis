/* Thread-local storage: TlsAlloc and FlsAlloc.
 *
 * TLS slots are Windows' own arrangement: 64 in the TEB at 0x1480, and 1024
 * more in an array the TEB points at from 0x1780, allocated for a thread the
 * first time it stores into one. The index pool is process-wide and lives
 * here, under a lock (threads of one process run concurrently). TlsFree
 * asks the kernel to clear that slot in EVERY thread (ThreadZeroTlsCell), so
 * the index can be handed out again reading NULL everywhere - the guarantee
 * TlsAlloc makes and a program relies on.
 *
 * FLS - slots with a destructor per index - is ntdll's (RtlFls*); these are
 * the Win32 shapes over it. */

#include "kernel32.h"

#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu
#define FLS_OUT_OF_INDEXES 0xFFFFFFFFu
#define TLS_TOTAL          (TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS)

static SRWLOCK tls_lock;
static QWORD   tls_used[TLS_TOTAL / 64];

static PVOID *teb_slots(void) {
    return (PVOID *)((BYTE *)NtCurrentTeb() + TEB_TLS_SLOTS_OFFSET);
}

static PVOID **teb_expansion(void) {
    return (PVOID **)((BYTE *)NtCurrentTeb() + TEB_TLS_EXPANSION_OFFSET);
}

DWORD WINAPI TlsAlloc(void) {
    DWORD i;

    RtlAcquireSRWLockExclusive(&tls_lock);
    for (i = 0; i < TLS_TOTAL; i++) {
        if (!(tls_used[i / 64] & (1ULL << (i % 64)))) {
            tls_used[i / 64] |= 1ULL << (i % 64);
            RtlReleaseSRWLockExclusive(&tls_lock);
            /* Fresh: this thread's copy must read NULL too, and a slot freed
             * and reallocated has been cleared everywhere by TlsFree. */
            if (i < TLS_MINIMUM_AVAILABLE) {
                teb_slots()[i] = NULL_PTR;
            } else if (*teb_expansion() != NULL_PTR) {
                (*teb_expansion())[i - TLS_MINIMUM_AVAILABLE] = NULL_PTR;
            }
            return i;
        }
    }
    RtlReleaseSRWLockExclusive(&tls_lock);
    SetLastError(ERROR_NO_MORE_ITEMS);
    return TLS_OUT_OF_INDEXES;
}

static BOOL tls_index_live(DWORD i) {
    return i < TLS_TOTAL && (tls_used[i / 64] & (1ULL << (i % 64))) != 0;
}

BOOL WINAPI TlsFree(DWORD index) {
    NTSTATUS st;

    RtlAcquireSRWLockExclusive(&tls_lock);
    if (!tls_index_live(index)) {
        RtlReleaseSRWLockExclusive(&tls_lock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    st = NtSetInformationThread(NtCurrentThread(), ThreadZeroTlsCell, &index,
                                sizeof(index));
    tls_used[index / 64] &= ~(1ULL << (index % 64));
    RtlReleaseSRWLockExclusive(&tls_lock);
    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return FALSE;
    }
    return TRUE;
}

LPVOID WINAPI TlsGetValue(DWORD index) {
    PVOID v;

    if (index < TLS_MINIMUM_AVAILABLE) {
        v = teb_slots()[index];
    } else if (index < TLS_TOTAL) {
        PVOID *exp = *teb_expansion();

        v = exp != NULL_PTR ? exp[index - TLS_MINIMUM_AVAILABLE] : NULL_PTR;
    } else {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL_PTR;
    }
    /* Documented: a successful TlsGetValue clears the last error, so a
     * caller can tell a stored NULL from a failure. */
    SetLastError(ERROR_SUCCESS);
    return v;
}

BOOL WINAPI TlsSetValue(DWORD index, LPVOID value) {
    if (index < TLS_MINIMUM_AVAILABLE) {
        teb_slots()[index] = value;
        return TRUE;
    }
    if (index >= TLS_TOTAL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (*teb_expansion() == NULL_PTR) {
        PVOID *exp = (PVOID *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        TLS_EXPANSION_SLOTS * sizeof(PVOID));

        if (exp == NULL_PTR) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        *teb_expansion() = exp;
    }
    (*teb_expansion())[index - TLS_MINIMUM_AVAILABLE] = value;
    return TRUE;
}

/* --- FLS ------------------------------------------------------------------- */

DWORD WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION callback) {
    DWORD index = 0;
    NTSTATUS st = RtlFlsAlloc(callback, &index);

    if (!NT_SUCCESS(st)) {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FLS_OUT_OF_INDEXES;
    }
    return index;
}

BOOL WINAPI FlsFree(DWORD index) {
    NTSTATUS st = RtlFlsFree(index);

    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return FALSE;
    }
    return TRUE;
}

PVOID WINAPI FlsGetValue(DWORD index) {
    PVOID v = NULL_PTR;
    NTSTATUS st = RtlFlsGetValue(index, &v);

    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return NULL_PTR;
    }
    SetLastError(ERROR_SUCCESS);
    return v;
}

BOOL WINAPI FlsSetValue(DWORD index, PVOID value) {
    NTSTATUS st = RtlFlsSetValue(index, value);

    if (!NT_SUCCESS(st)) {
        k32_set_error_from_status(st);
        return FALSE;
    }
    return TRUE;
}
