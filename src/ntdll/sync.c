#include "ntdll.h"

/* User-mode synchronisation for multithreaded Win32 programs, and the small
 * processor/time helpers beside it.
 *
 * Threads of one process run on several CPUs at once (see the kernel's
 * ksmp.h), so every primitive here is built on atomic instructions and is
 * correct under true parallelism, not just under interleaving.
 *
 * Every primitive BLOCKS IN THE KERNEL when it has to wait - after a short
 * spin, because the holder is usually on another CPU about to let go:
 *   critical sections  on an auto-reset event, created on first contention -
 *                      NT's own design (LockSemaphore)
 *   SRW locks and      on the lock/condition word itself, through
 *   condition vars     RtlWaitOnAddress (waitaddr.c) over the kernel's
 *                      NtWaitForAlertByThreadId - which is how Windows 8 and
 *                      later build them too */

static inline LONG atomic_inc(volatile LONG *p) {
    return __atomic_add_fetch(p, 1, __ATOMIC_ACQ_REL);
}

static inline LONG atomic_dec(volatile LONG *p) {
    return __atomic_sub_fetch(p, 1, __ATOMIC_ACQ_REL);
}

static inline int cas_long(volatile LONG *p, LONG expect, LONG desired) {
    return __atomic_compare_exchange_n(p, &expect, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline int cas_size(volatile SIZE_T *p, SIZE_T expect, SIZE_T desired) {
    return __atomic_compare_exchange_n(p, &expect, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static HANDLE my_tid(void) {
    return (HANDLE)(SIZE_T)NtCurrentTeb()->ClientIdThread;
}

/* The waiting ladder: a few dozen pauses (the holder is on another CPU and
 * about to let go), then yields (it is not running - let it), then a tick's
 * sleep at a time (nothing else wants this CPU, and spinning would only heat
 * it). `round` is how many times the caller has come back. */
static void backoff(DWORD round) {
    if (round < 32) {
        __asm__ volatile ("pause");
    } else if (round < 256) {
        NtYieldExecution();
    } else {
        LARGE_INTEGER one_ms;

        one_ms.QuadPart = -10000;
        NtDelayExecution(0, &one_ms);
    }
}

/* ===========================================================================
 * critical sections
 * ======================================================================== */

#define CS_DEBUG_NONE ((PVOID)(SIZE_T)-1)

NTSTATUS RtlInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION cs, DWORD spin,
                                        DWORD flags) {
    (void)flags;
    cs->DebugInfo      = CS_DEBUG_NONE;
    cs->LockCount      = -1;
    cs->RecursionCount = 0;
    cs->OwningThread   = 0;
    cs->LockSemaphore  = 0;
    cs->SpinCount      = spin & 0x00FFFFFFu;
    return STATUS_SUCCESS;
}

NTSTATUS RtlInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION cs,
                                                  DWORD spin) {
    return RtlInitializeCriticalSectionEx(cs, spin, 0);
}

NTSTATUS RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs) {
    return RtlInitializeCriticalSectionEx(cs, 0, 0);
}

DWORD RtlSetCriticalSectionSpinCount(PRTL_CRITICAL_SECTION cs, DWORD spin) {
    DWORD old = (DWORD)cs->SpinCount;

    cs->SpinCount = spin & 0x00FFFFFFu;
    return old;
}

/* The event a contended section blocks on, created on first need. Two
 * threads can race to create it; the loser closes its own and uses the
 * winner's. */
static HANDLE cs_event(PRTL_CRITICAL_SECTION cs) {
    HANDLE ev = cs->LockSemaphore, mine = 0;

    if (ev != 0) {
        return ev;
    }
    if (!NT_SUCCESS(NtCreateEvent(&mine, 0x1F0003u, NULL_PTR,
                                  SynchronizationEvent, 0))) {
        return 0;
    }
    if (__atomic_compare_exchange_n(&cs->LockSemaphore, &ev, mine, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return mine;
    }
    NtClose(mine);
    return ev;
}

BOOLEAN RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION cs) {
    HANDLE me = my_tid();

    if (cas_long(&cs->LockCount, -1, 0)) {
        cs->OwningThread   = me;
        cs->RecursionCount = 1;
        return 1;
    }
    if (cs->OwningThread == me) {
        atomic_inc(&cs->LockCount);
        cs->RecursionCount++;
        return 1;
    }
    return 0;
}

NTSTATUS RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs) {
    HANDLE me = my_tid();
    SIZE_T spin;

    /* Spinning first, if asked: a section held for a few instructions by a
     * thread on another CPU is cheaper to wait out than to sleep on. */
    for (spin = cs->SpinCount; spin > 0; spin--) {
        if (cs->LockCount == -1 && cas_long(&cs->LockCount, -1, 0)) {
            cs->OwningThread   = me;
            cs->RecursionCount = 1;
            return STATUS_SUCCESS;
        }
        if (cs->OwningThread == me) {
            break;
        }
        __asm__ volatile ("pause");
    }

    if (atomic_inc(&cs->LockCount) != 0) {
        /* Taken. By us (recursion - the count we just added is the
         * recursion's), or by another thread (we are now a counted waiter,
         * and the owner's Leave will signal the event exactly once for us). */
        if (cs->OwningThread == me) {
            cs->RecursionCount++;
            return STATUS_SUCCESS;
        }
        {
            HANDLE ev = cs_event(cs);

            if (ev != 0) {
                NtWaitForSingleObject(ev, 0, NULL_PTR);
            } else {
                /* No event could be made: wait it out the slow way. The
                 * owner's Leave still signals (or tries to); we just do not
                 * sleep on it. */
                DWORD round = 0;

                while (cs->OwningThread != 0) {
                    backoff(round++);
                }
            }
        }
    }
    cs->OwningThread   = me;
    cs->RecursionCount = 1;
    return STATUS_SUCCESS;
}

NTSTATUS RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs) {
    if (--cs->RecursionCount > 0) {
        atomic_dec(&cs->LockCount);
        return STATUS_SUCCESS;
    }
    cs->OwningThread = 0;
    if (atomic_dec(&cs->LockCount) >= 0) {
        /* Someone is waiting (or about to): hand the section to exactly one
         * of them. The event is auto-reset, so a waiter that has not reached
         * its wait yet will find it signalled. */
        HANDLE ev = cs_event(cs);

        if (ev != 0) {
            NtSetEvent(ev, NULL_PTR);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION cs) {
    if (cs->LockSemaphore != 0) {
        NtClose(cs->LockSemaphore);
        cs->LockSemaphore = 0;
    }
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    return STATUS_SUCCESS;
}

BOOLEAN RtlIsCriticalSectionLockedByThread(PRTL_CRITICAL_SECTION cs) {
    return cs->OwningThread == my_tid() && cs->RecursionCount > 0;
}

/* ===========================================================================
 * slim reader/writer locks
 *
 * One word: bit 0 is "held exclusively", bits 1-62 count shared holders,
 * bit 63 says someone is asleep on the word. A contended acquirer spins
 * briefly (the holder is probably on another CPU and about to let go), then
 * sets the waiters bit and sleeps IN THE KERNEL until the word changes
 * (RtlWaitOnAddress). A release that sees the bit wakes the sleepers.
 *
 * Writers are preferred: once a writer is waiting (bit 63 set), new readers
 * wait too, so a stream of readers cannot starve it.
 * ======================================================================== */

#define SRW_EXCLUSIVE ((SIZE_T)1)
#define SRW_READER    ((SIZE_T)2)
#define SRW_WAITERS   ((SIZE_T)1 << 63)
#define SRW_SPIN      64

void RtlWakeAddressAll(PVOID addr);
NTSTATUS RtlWaitOnAddress(const volatile void *addr, PVOID compare, SIZE_T size,
                          LARGE_INTEGER *timeout);
void RtlWakeAddressSingle(PVOID addr);

void RtlInitializeSRWLock(PRTL_SRWLOCK l) {
    l->Value = 0;
}

BOOLEAN RtlTryAcquireSRWLockExclusive(PRTL_SRWLOCK l) {
    SIZE_T v = l->Value;

    return (v & ~SRW_WAITERS) == 0 &&
           cas_size(&l->Value, v, v | SRW_EXCLUSIVE);
}

BOOLEAN RtlTryAcquireSRWLockShared(PRTL_SRWLOCK l) {
    SIZE_T v = l->Value;

    return !(v & (SRW_EXCLUSIVE | SRW_WAITERS)) &&
           cas_size(&l->Value, v, v + SRW_READER);
}

/* Sleep until the word is no longer `v | SRW_WAITERS`, having set the bit
 * so the holder's release knows to wake us. */
static void srw_sleep(PRTL_SRWLOCK l, SIZE_T v) {
    SIZE_T want = v | SRW_WAITERS;

    if (!(v & SRW_WAITERS) && !cas_size(&l->Value, v, want)) {
        return;                          /* changed under us: re-test */
    }
    RtlWaitOnAddress(&l->Value, &want, sizeof(want), NULL_PTR);
}

void RtlAcquireSRWLockExclusive(PRTL_SRWLOCK l) {
    DWORD round = 0;

    for (;;) {
        SIZE_T v = l->Value;

        if ((v & ~SRW_WAITERS) == 0) {
            if (cas_size(&l->Value, v, v | SRW_EXCLUSIVE)) {
                return;
            }
            continue;
        }
        if (round++ < SRW_SPIN) {
            __asm__ volatile ("pause");
            continue;
        }
        srw_sleep(l, v);
    }
}

void RtlAcquireSRWLockShared(PRTL_SRWLOCK l) {
    DWORD round = 0;

    for (;;) {
        SIZE_T v = l->Value;

        if (!(v & (SRW_EXCLUSIVE | SRW_WAITERS))) {
            if (cas_size(&l->Value, v, v + SRW_READER)) {
                return;
            }
            continue;
        }
        if (round++ < SRW_SPIN) {
            __asm__ volatile ("pause");
            continue;
        }
        srw_sleep(l, v);
    }
}

void RtlReleaseSRWLockExclusive(PRTL_SRWLOCK l) {
    SIZE_T old = __atomic_exchange_n(&l->Value, 0, __ATOMIC_RELEASE);

    if (old & SRW_WAITERS) {
        RtlWakeAddressAll((PVOID)&l->Value);
    }
}

void RtlReleaseSRWLockShared(PRTL_SRWLOCK l) {
    SIZE_T v = __atomic_sub_fetch(&l->Value, SRW_READER, __ATOMIC_RELEASE);

    /* The last reader out, with someone waiting: clear the bit (they will
     * set it again if they lose the next race) and wake them. */
    if (v == SRW_WAITERS && cas_size(&l->Value, SRW_WAITERS, 0)) {
        RtlWakeAddressAll((PVOID)&l->Value);
    }
}

/* ===========================================================================
 * condition variables
 *
 * A generation counter. A sleeper reads it before releasing the lock and
 * sleeps in the kernel until it moves (RtlWaitOnAddress), so a wake landing
 * between the release and the sleep changes the word the sleep compares
 * against and is not lost. Wake bumps it and wakes one sleeper; WakeAll
 * bumps it and wakes them all.
 * ======================================================================== */

void RtlInitializeConditionVariable(PRTL_CONDITION_VARIABLE cv) {
    cv->Value = 0;
}

void RtlWakeConditionVariable(PRTL_CONDITION_VARIABLE cv) {
    __atomic_add_fetch(&cv->Value, 1, __ATOMIC_ACQ_REL);
    RtlWakeAddressSingle((PVOID)&cv->Value);
}

void RtlWakeAllConditionVariable(PRTL_CONDITION_VARIABLE cv) {
    __atomic_add_fetch(&cv->Value, 1, __ATOMIC_ACQ_REL);
    RtlWakeAddressAll((PVOID)&cv->Value);
}

static NTSTATUS cv_wait(PRTL_CONDITION_VARIABLE cv, SIZE_T seen,
                        LARGE_INTEGER *timeout) {
    if (timeout != NULL_PTR && timeout->QuadPart == 0) {
        return STATUS_TIMEOUT;
    }
    return RtlWaitOnAddress(&cv->Value, &seen, sizeof(seen), timeout);
}

NTSTATUS RtlSleepConditionVariableCS(PRTL_CONDITION_VARIABLE cv,
                                     PRTL_CRITICAL_SECTION cs,
                                     LARGE_INTEGER *timeout) {
    SIZE_T seen = __atomic_load_n(&cv->Value, __ATOMIC_ACQUIRE);
    NTSTATUS st;

    RtlLeaveCriticalSection(cs);
    st = cv_wait(cv, seen, timeout);
    RtlEnterCriticalSection(cs);
    return st;
}

NTSTATUS RtlSleepConditionVariableSRW(PRTL_CONDITION_VARIABLE cv,
                                      PRTL_SRWLOCK l, LARGE_INTEGER *timeout,
                                      DWORD flags) {
    SIZE_T seen = __atomic_load_n(&cv->Value, __ATOMIC_ACQUIRE);
    NTSTATUS st;
    int shared = (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) != 0;

    if (shared) {
        RtlReleaseSRWLockShared(l);
    } else {
        RtlReleaseSRWLockExclusive(l);
    }
    st = cv_wait(cv, seen, timeout);
    if (shared) {
        RtlAcquireSRWLockShared(l);
    } else {
        RtlAcquireSRWLockExclusive(l);
    }
    return st;
}

/* ===========================================================================
 * interlocked singly linked lists
 *
 * A spin lock in the header's top bit guards push and pop - which makes
 * them linearisable and immune to the ABA problem a naive compare-and-swap
 * list has, at the cost of being lock-free only in name. The header is
 * opaque to callers except through QueryDepth and FirstEntry, which read it
 * the same way.
 * ======================================================================== */

#define SL_LOCK  (1ULL << 63)
#define SL_DEPTH 0xFFFFULL

static void sl_lock(PSLIST_HEADER h) {
    DWORD round = 0;

    for (;;) {
        QWORD v = h->DepthAndLock;

        if (!(v & SL_LOCK) &&
            __atomic_compare_exchange_n(&h->DepthAndLock, &v, v | SL_LOCK, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        backoff(round++);
    }
}

static void sl_unlock(PSLIST_HEADER h, QWORD depth) {
    __atomic_store_n(&h->DepthAndLock, depth & SL_DEPTH, __ATOMIC_RELEASE);
}

void RtlInitializeSListHead(PSLIST_HEADER h) {
    h->Next = NULL_PTR;
    h->DepthAndLock = 0;
}

PSLIST_ENTRY RtlInterlockedPushEntrySList(PSLIST_HEADER h, PSLIST_ENTRY e) {
    PSLIST_ENTRY old;
    QWORD depth;

    sl_lock(h);
    depth = h->DepthAndLock & SL_DEPTH;
    old = h->Next;
    e->Next = old;
    h->Next = e;
    sl_unlock(h, depth + 1);
    return old;
}

PSLIST_ENTRY RtlInterlockedPopEntrySList(PSLIST_HEADER h) {
    PSLIST_ENTRY e;
    QWORD depth;

    sl_lock(h);
    depth = h->DepthAndLock & SL_DEPTH;
    e = h->Next;
    if (e != NULL_PTR) {
        h->Next = e->Next;
        depth--;
    }
    sl_unlock(h, depth);
    return e;
}

PSLIST_ENTRY RtlInterlockedFlushSList(PSLIST_HEADER h) {
    PSLIST_ENTRY e;

    sl_lock(h);
    e = h->Next;
    h->Next = NULL_PTR;
    sl_unlock(h, 0);
    return e;
}

PSLIST_ENTRY RtlFirstEntrySList(const SLIST_HEADER *h) {
    return h->Next;
}

WORD RtlQueryDepthSList(PSLIST_HEADER h) {
    return (WORD)(h->DepthAndLock & SL_DEPTH);
}

/* ===========================================================================
 * processors and time
 * ======================================================================== */

DWORD RtlGetCurrentProcessorNumber(void) {
    return NtGetCurrentProcessorNumber();
}

void RtlGetCurrentProcessorNumberEx(PPROCESSOR_NUMBER pn) {
    NtGetCurrentProcessorNumberEx(pn);
}

BOOLEAN RtlQueryPerformanceCounter(LARGE_INTEGER *counter) {
    return NT_SUCCESS(NtQueryPerformanceCounter(counter, NULL_PTR));
}

BOOLEAN RtlQueryPerformanceFrequency(LARGE_INTEGER *frequency) {
    LARGE_INTEGER c;

    return NT_SUCCESS(NtQueryPerformanceCounter(&c, frequency));
}
