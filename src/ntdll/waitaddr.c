#include "ntdll.h"

/* RtlWaitOnAddress / RtlWakeAddressSingle / RtlWakeAddressAll - Windows 8's
 * "sleep until this word changes", which SRW locks and condition variables
 * are built on (sync.c) and which kernel32 exports as WaitOnAddress.
 *
 * The kernel provides only a per-thread alert (NtWaitForAlertByThreadId /
 * NtAlertThreadByThreadId); the address matching is here, as it is in real
 * ntdll: a hash table of waiters, each a node on its own thread's stack,
 * keyed by address. A waiter queues itself, THEN compares the word, and only
 * then sleeps - so a waker that changes the word and wakes the address after
 * the compare finds the node, and one that does it before makes the compare
 * fail. Either way nothing is lost. An alert left over from a race (a waker
 * that found the node just as the compare failed) surfaces later as a
 * spurious wakeup, which every caller re-tests for - the API permits them. */

typedef struct waiter {
    const volatile void *addr;
    QWORD                tid;
    struct waiter       *next;
    volatile LONG        woken;
} waiter_t;

#define BUCKETS 64

static struct {
    volatile LONG lock;
    waiter_t     *head;
} bucket[BUCKETS];

static unsigned bucket_of(const volatile void *a) {
    SIZE_T v = (SIZE_T)a;

    return (unsigned)((v >> 3) ^ (v >> 9) ^ (v >> 15)) % BUCKETS;
}

static void bucket_lock(unsigned b) {
    while (__atomic_exchange_n(&bucket[b].lock, 1, __ATOMIC_ACQUIRE) != 0) {
        __asm__ volatile ("pause");
    }
}

static void bucket_unlock(unsigned b) {
    __atomic_store_n(&bucket[b].lock, 0, __ATOMIC_RELEASE);
}

/* Remove `w` if it is still queued; 1 if it was. */
static int unqueue(unsigned b, waiter_t *w) {
    waiter_t **pp = &bucket[b].head;

    while (*pp != NULL_PTR && *pp != w) {
        pp = &(*pp)->next;
    }
    if (*pp == w) {
        *pp = w->next;
        return 1;
    }
    return 0;
}

static int same(const volatile void *addr, const void *cmp, SIZE_T size) {
    switch (size) {
    case 1: return *(const volatile BYTE *)addr == *(const BYTE *)cmp;
    case 2: return *(const volatile WORD *)addr == *(const WORD *)cmp;
    case 4: return *(const volatile DWORD *)addr == *(const DWORD *)cmp;
    case 8: return *(const volatile QWORD *)addr == *(const QWORD *)cmp;
    default: return 0;
    }
}

NTSTATUS RtlWaitOnAddress(const volatile void *addr, PVOID compare, SIZE_T size,
                          LARGE_INTEGER *timeout) {
    waiter_t w;
    unsigned b = bucket_of(addr);
    LARGE_INTEGER deadline, *dl = NULL_PTR;

    if (size != 1 && size != 2 && size != 4 && size != 8) {
        return STATUS_INVALID_PARAMETER;
    }
    if (timeout != NULL_PTR) {
        /* One ABSOLUTE deadline for the whole wait, so a spurious wake does
         * not restart a relative timeout from the top. */
        if (timeout->QuadPart < 0) {
            NtQuerySystemTime(&deadline);
            deadline.QuadPart -= timeout->QuadPart;
        } else {
            deadline = *timeout;
        }
        dl = &deadline;
    }

    w.addr  = addr;
    w.tid   = NtCurrentTeb()->ClientIdThread;
    w.woken = 0;
    bucket_lock(b);
    w.next = bucket[b].head;
    bucket[b].head = &w;
    bucket_unlock(b);

    if (!same(addr, compare, size)) {
        bucket_lock(b);
        unqueue(b, &w);
        bucket_unlock(b);
        return STATUS_SUCCESS;           /* already different: no wait */
    }
    for (;;) {
        NTSTATUS st = NtWaitForAlertByThreadId((PVOID)addr, dl);

        if (w.woken) {
            return STATUS_SUCCESS;
        }
        if (st == STATUS_TIMEOUT) {
            int was;

            bucket_lock(b);
            was = unqueue(b, &w);
            bucket_unlock(b);
            /* Not queued any more: a waker took it at the last moment, and
             * its alert is on its way - that counts as woken. */
            return was ? STATUS_TIMEOUT : STATUS_SUCCESS;
        }
        /* A leftover alert from an earlier race: sleep again. */
    }
}

static void wake(PVOID addr, int all) {
    unsigned b = bucket_of(addr);
    QWORD tids[32];
    int n = 0, i;
    waiter_t **pp;

    do {
        n = 0;
        bucket_lock(b);
        pp = &bucket[b].head;
        while (*pp != NULL_PTR && n < 32) {
            waiter_t *w = *pp;

            if (w->addr == addr) {
                *pp = w->next;
                tids[n++] = w->tid;
                /* Set before the alert, read after the waiter's wait
                 * returns: it is how the waiter tells a real wake from a
                 * stray alert. The node lives on the waiter's stack and must
                 * not be touched after this store. */
                __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
                if (!all) {
                    break;
                }
            } else {
                pp = &w->next;
            }
        }
        bucket_unlock(b);
        for (i = 0; i < n; i++) {
            NtAlertThreadByThreadId((HANDLE)(SIZE_T)tids[i]);
        }
    } while (all && n == 32);
}

void RtlWakeAddressSingle(PVOID addr) {
    wake(addr, 0);
}

void RtlWakeAddressAll(PVOID addr) {
    wake(addr, 1);
}
