#include "ntdll.h"

/* Thread-local storage, the parts that run in user mode.
 *
 * IMPLICIT TLS (a module's .tls section): the kernel has already done the
 * storage - every thread's copy exists and TEB+0x58 points at it before the
 * thread runs a single instruction (kernel/exec/ntproc.c). What only user
 * mode can do is call the modules' TLS CALLBACKS, which is how a C runtime
 * constructs and destroys per-thread state: DLL_PROCESS_ATTACH on the main
 * thread before the image's entry point, DLL_THREAD_ATTACH as each thread
 * starts (both from RtlUserThreadStart), DLL_THREAD_DETACH as it exits
 * (RtlExitUserThread), DLL_PROCESS_DETACH at ExitProcess
 * (LdrShutdownProcess). The list comes from the table the kernel published
 * in the PEB.
 *
 * FIBER-LOCAL STORAGE (FlsAlloc): like TlsAlloc's slots, but each index may
 * carry a destructor, called with the thread's value when the thread exits
 * and, for every thread, when the index is freed. A thread's values hang off
 * TEB+0x17C8 in a block that is also on a process-wide list, which is what
 * lets FlsFree reach every thread's value. */

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3

#define PEB_TLS_OFFSET   0x800
#define TLS_MAGIC        0x534C5447u
#define TEB_FLS_DATA     0x17C8

typedef void (*PIMAGE_TLS_CALLBACK)(PVOID DllHandle, DWORD Reason, PVOID Reserved);

typedef struct {
    QWORD module_base, start, end, zero_fill, index_addr, callbacks, block_offset;
} tls_module_t;

typedef struct {
    DWORD        magic;
    DWORD        count;
    QWORD        area_bytes;
    tls_module_t mod[8];
} tls_table_t;

static tls_table_t *tls_table(void) {
    PPEB peb = NtCurrentTeb()->ProcessEnvironmentBlock;
    tls_table_t *t;

    if (peb == NULL_PTR) {
        return (tls_table_t *)NULL_PTR;
    }
    t = (tls_table_t *)((BYTE *)peb + PEB_TLS_OFFSET);
    return (t->magic == TLS_MAGIC && t->count <= 8) ? t : (tls_table_t *)NULL_PTR;
}

void ldrp_run_tls_callbacks(DWORD reason) {
    tls_table_t *t = tls_table();
    DWORD i;

    if (t == NULL_PTR) {
        return;
    }
    for (i = 0; i < t->count; i++) {
        PIMAGE_TLS_CALLBACK *cb = (PIMAGE_TLS_CALLBACK *)t->mod[i].callbacks;

        if (cb == NULL_PTR) {
            continue;
        }
        while (*cb != NULL_PTR) {
            (*cb)((PVOID)t->mod[i].module_base, reason, NULL_PTR);
            cb++;
        }
    }
}

/* The attach side, run first thing in RtlUserThreadStart. The main thread
 * is the one whose thread id is the process id. */
void ldrp_thread_attach(void) {
    PTEB teb = NtCurrentTeb();

    ldrp_run_tls_callbacks(teb->ClientIdThread == teb->ClientIdProcess
                           ? DLL_PROCESS_ATTACH : DLL_THREAD_ATTACH);
}

static volatile LONG process_detached;

static void fls_thread_exit(void);

/* ExitProcess's side: the exiting thread's FLS destructors, then every
 * module's DLL_PROCESS_DETACH. Once, however many threads race to exit. */
void LdrShutdownProcess(void) {
    if (__atomic_exchange_n(&process_detached, 1, __ATOMIC_ACQ_REL) == 0) {
        fls_thread_exit();
        ldrp_run_tls_callbacks(DLL_PROCESS_DETACH);
    }
}

/* ===========================================================================
 * fiber-local storage
 * ======================================================================== */

#define FLS_SLOTS 128
typedef void (*PFLS_CALLBACK_FUNCTION)(PVOID Data);

typedef struct fls_block {
    struct fls_block *next, *prev;         /* the process-wide list */
    PVOID             slot[FLS_SLOTS];
} fls_block_t;

static RTL_SRWLOCK             fls_lock;
static fls_block_t            *fls_blocks;
static QWORD                   fls_used[FLS_SLOTS / 64];
static PFLS_CALLBACK_FUNCTION  fls_callbacks[FLS_SLOTS];

static fls_block_t **fls_slot_of_teb(void) {
    return (fls_block_t **)((BYTE *)NtCurrentTeb() + TEB_FLS_DATA);
}

NTSTATUS RtlFlsAlloc(PFLS_CALLBACK_FUNCTION callback, DWORD *index) {
    DWORD i;

    if (index == NULL_PTR) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlAcquireSRWLockExclusive(&fls_lock);
    for (i = 1; i < FLS_SLOTS; i++) {             /* 0 is never handed out */
        if (!(fls_used[i / 64] & (1ULL << (i % 64)))) {
            fls_used[i / 64] |= 1ULL << (i % 64);
            fls_callbacks[i] = callback;
            RtlReleaseSRWLockExclusive(&fls_lock);
            *index = i;
            return STATUS_SUCCESS;
        }
    }
    RtlReleaseSRWLockExclusive(&fls_lock);
    return STATUS_NO_MEMORY;
}

static int fls_valid(DWORD i) {
    return i > 0 && i < FLS_SLOTS && (fls_used[i / 64] & (1ULL << (i % 64)));
}

NTSTATUS RtlFlsFree(DWORD index) {
    fls_block_t *b;
    PFLS_CALLBACK_FUNCTION cb;

    RtlAcquireSRWLockExclusive(&fls_lock);
    if (!fls_valid(index)) {
        RtlReleaseSRWLockExclusive(&fls_lock);
        return STATUS_INVALID_PARAMETER;
    }
    cb = fls_callbacks[index];
    /* Every thread's value, destroyed and cleared: the index may be handed
     * out again at once, and must read NULL everywhere when it is. */
    for (b = fls_blocks; b != NULL_PTR; b = b->next) {
        PVOID v = b->slot[index];

        b->slot[index] = NULL_PTR;
        if (v != NULL_PTR && cb != (PFLS_CALLBACK_FUNCTION)NULL_PTR) {
            cb(v);
        }
    }
    fls_callbacks[index] = (PFLS_CALLBACK_FUNCTION)NULL_PTR;
    fls_used[index / 64] &= ~(1ULL << (index % 64));
    RtlReleaseSRWLockExclusive(&fls_lock);
    return STATUS_SUCCESS;
}

NTSTATUS RtlFlsGetValue(DWORD index, PVOID *value) {
    fls_block_t *b = *fls_slot_of_teb();

    if (value == NULL_PTR || !fls_valid(index)) {
        return STATUS_INVALID_PARAMETER;
    }
    *value = b != NULL_PTR ? b->slot[index] : NULL_PTR;
    return STATUS_SUCCESS;
}

NTSTATUS RtlFlsSetValue(DWORD index, PVOID value) {
    fls_block_t **pb = fls_slot_of_teb();

    if (!fls_valid(index)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (*pb == NULL_PTR) {
        fls_block_t *b = (fls_block_t *)RtlAllocateHeap(NULL_PTR, HEAP_ZERO_MEMORY,
                                                        sizeof(fls_block_t));

        if (b == NULL_PTR) {
            return STATUS_NO_MEMORY;
        }
        RtlAcquireSRWLockExclusive(&fls_lock);
        b->next = fls_blocks;
        b->prev = (fls_block_t *)NULL_PTR;
        if (fls_blocks != NULL_PTR) {
            fls_blocks->prev = b;
        }
        fls_blocks = b;
        RtlReleaseSRWLockExclusive(&fls_lock);
        *pb = b;
    }
    (*pb)->slot[index] = value;
    return STATUS_SUCCESS;
}

/* A thread is ending: run the destructor for each of its non-NULL values,
 * then take its block off the list and free it. */
static void fls_thread_exit(void) {
    fls_block_t **pb = fls_slot_of_teb();
    fls_block_t *b = *pb;
    DWORD i;

    if (b == NULL_PTR) {
        return;
    }
    for (i = 1; i < FLS_SLOTS; i++) {
        PVOID v = b->slot[i];
        PFLS_CALLBACK_FUNCTION cb;

        if (v == NULL_PTR) {
            continue;
        }
        RtlAcquireSRWLockShared(&fls_lock);
        cb = fls_valid(i) ? fls_callbacks[i] : (PFLS_CALLBACK_FUNCTION)NULL_PTR;
        RtlReleaseSRWLockShared(&fls_lock);
        b->slot[i] = NULL_PTR;
        if (cb != (PFLS_CALLBACK_FUNCTION)NULL_PTR) {
            cb(v);
        }
    }
    RtlAcquireSRWLockExclusive(&fls_lock);
    if (b->prev != NULL_PTR) {
        b->prev->next = b->next;
    } else {
        fls_blocks = b->next;
    }
    if (b->next != NULL_PTR) {
        b->next->prev = b->prev;
    }
    RtlReleaseSRWLockExclusive(&fls_lock);
    *pb = (fls_block_t *)NULL_PTR;
    RtlFreeHeap(NULL_PTR, 0, b);
}

/* The detach side, run by RtlExitUserThread before the thread is gone:
 * FLS destructors first (they may still use TLS), then THREAD_DETACH. */
void ldrp_thread_detach(void) {
    fls_thread_exit();
    ldrp_run_tls_callbacks(DLL_THREAD_DETACH);
}
