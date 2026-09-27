#include "bkl.h"
#include "kprintf.h"
#include "ksmp.h"
#include "process.h"
#include "sched.h"
#include "timer.h"
#include "typesk.h"
#include "wdm.h"
#include "kthread.h"

/* The NT kernel's multiprocessor interface, for WDM drivers.
 *
 * What a driver asks the kernel about processors, and the four ways it runs
 * code on them:
 *
 *   counting      KeQueryActiveProcessorCount(Ex), KeQueryActiveProcessors,
 *                 KeQueryMaximumProcessorCount(Ex), the group queries,
 *                 KeGetCurrentProcessorNumber(Ex), KeNumberProcessors
 *   everywhere    KeIpiGenericCall - one function on every CPU at once, at
 *                 IPI level, returning the calling CPU's result
 *   everywhere,   KeGenericCallDpc with KeSignalCallDpcSynchronize and
 *   in lockstep   KeSignalCallDpcDone - a DPC on every CPU with a barrier
 *   one, later    KeInitializeDpc / KeSetTargetProcessorDpc(Ex) /
 *                 KeInsertQueueDpc / KeRemoveQueueDpc / KeFlushQueuedDpcs
 *   here, now     KeSetSystemAffinityThread(Ex) /
 *                 KeRevertToUserAffinityThread(Ex) - move the calling thread
 *
 * plus the locks an SMP driver needs beside KeAcquireSpinLock (wdm.c):
 * the DPC-level pair, the raise-to-DPC and try variants, and in-stack queued
 * spin locks.
 *
 * --- how each is built on this kernel ---------------------------------------
 *
 * One group, up to SMP_MAX_CPUS processors: every "Ex" query answers for
 * group 0 and ALL_PROCESSOR_GROUPS alike.
 *
 * KeIpiGenericCall is smp_call_all: the worker runs in each CPU's IPI
 * handler, concurrently, without the big kernel lock - which is NT's own
 * contract for it (IPI_LEVEL: no locks but spin locks, no waiting).
 *
 * A DPC runs WITH the big kernel lock, on its target CPU. That is stronger
 * than NT's DISPATCH_LEVEL promise and it is deliberate: a DPC routine
 * completes IRPs and touches driver-shared state through kernel services
 * that are serialised by that lock here, not by spin locks. Each CPU has a
 * queue; KeInsertQueueDpc appends to the target's and kicks it, and the
 * queue is drained wherever that CPU holds the lock with nothing on its
 * stack that a DPC could disturb - the tail of an interrupt, the return to
 * ring 3, the idle loop (see wdm_dpc_drain).
 *
 * KeGenericCallDpc cannot use those queues: its barrier needs every CPU in
 * the routine at the same time, and the big kernel lock would admit them one
 * at a time - a deadlock at the first KeSignalCallDpcSynchronize. So it runs
 * the routine the way KeIpiGenericCall does, concurrently and lock-free. That
 * is exactly as much as its documented contract allows a routine to do.
 */

#define ALL_PROCESSOR_GROUPS 0xFFFFu

/* KeNumberProcessors: an exported DATA symbol, so a driver's import slot
 * holds this variable's ADDRESS and the driver reads through it. Set when
 * the scheduler starts on every CPU. */
int8 KeNumberProcessors = 1;

static uint32 active_count(void) {
    uint64 m = smp_online_mask();
    uint32 n = 0;

    while (m != 0) {
        n += (uint32)(m & 1);
        m >>= 1;
    }
    return n;
}

/* --- counting -------------------------------------------------------------- */

uint32 WDM_ABI KeGetCurrentProcessorNumber(void) {
    return (uint32)smp_cpu_index();
}

uint32 WDM_ABI KeGetCurrentProcessorNumberEx(PPROCESSOR_NUMBER ProcNumber) {
    uint32 n = (uint32)smp_cpu_index();

    if (ProcNumber != NULL) {
        ProcNumber->Group    = 0;
        ProcNumber->Number   = (uint8)n;
        ProcNumber->Reserved = 0;
    }
    return n;
}

uint32 WDM_ABI KeQueryActiveProcessorCount(PKAFFINITY ActiveProcessors) {
    if (ActiveProcessors != NULL) {
        *ActiveProcessors = smp_online_mask();
    }
    return active_count();
}

uint32 WDM_ABI KeQueryActiveProcessorCountEx(uint16 GroupNumber) {
    if (GroupNumber != 0 && GroupNumber != ALL_PROCESSOR_GROUPS) {
        return 0;
    }
    return active_count();
}

KAFFINITY WDM_ABI KeQueryActiveProcessors(void) {
    return smp_online_mask();
}

uint32 WDM_ABI KeQueryMaximumProcessorCount(void) {
    return (uint32)smp_cpu_count();
}

uint32 WDM_ABI KeQueryMaximumProcessorCountEx(uint16 GroupNumber) {
    if (GroupNumber != 0 && GroupNumber != ALL_PROCESSOR_GROUPS) {
        return 0;
    }
    return (uint32)smp_cpu_count();
}

uint16 WDM_ABI KeQueryActiveGroupCount(void) {
    return 1;
}

uint16 WDM_ABI KeQueryMaximumGroupCount(void) {
    return 1;
}

/* Group-relative number <-> system-wide index. One group, so they are the
 * same number; the calls exist so a driver written for groups works. */
uint32 WDM_ABI KeGetProcessorIndexFromNumber(PPROCESSOR_NUMBER ProcNumber) {
    if (ProcNumber == NULL || ProcNumber->Group != 0 ||
        ProcNumber->Number >= (uint8)smp_cpu_count()) {
        return 0xFFFFFFFFu;                    /* INVALID_PROCESSOR_INDEX */
    }
    return ProcNumber->Number;
}

NTSTATUS WDM_ABI KeGetProcessorNumberFromIndex(uint32 ProcIndex,
                                               PPROCESSOR_NUMBER ProcNumber) {
    if (ProcNumber == NULL || ProcIndex >= (uint32)smp_cpu_count()) {
        return (NTSTATUS)0xC000000Du;          /* STATUS_INVALID_PARAMETER */
    }
    ProcNumber->Group    = 0;
    ProcNumber->Number   = (uint8)ProcIndex;
    ProcNumber->Reserved = 0;
    return 0;
}

/* --- everywhere: KeIpiGenericCall -------------------------------------------- */

struct ipi_call {
    PKIPI_BROADCAST_WORKER worker;
    uint64                 context;
    int                    caller;
    uint64                 caller_result;
};

static void ipi_call_trampoline(void *arg) {
    struct ipi_call *c = arg;
    uint64 r = c->worker(c->context);

    if (smp_cpu_index() == c->caller) {
        c->caller_result = r;
    }
}

uint64 WDM_ABI KeIpiGenericCall(PKIPI_BROADCAST_WORKER BroadcastFunction,
                                uint64 Context) {
    struct ipi_call c;

    if (BroadcastFunction == NULL) {
        return 0;
    }
    c.worker        = BroadcastFunction;
    c.context       = Context;
    c.caller        = smp_cpu_index();
    c.caller_result = 0;
    smp_call_all(ipi_call_trampoline, &c);
    return c.caller_result;
}

/* --- everywhere, in lockstep: KeGenericCallDpc --------------------------------
 *
 * SystemArgument1 is the "done" counter and SystemArgument2 the barrier, both
 * pointers into this call's own frame - NT passes opaque values there too,
 * and a routine only ever hands them back to the two Signal calls. */
struct generic_dpc_sync {
    volatile int32 arrived;       /* barrier: CPUs that reached it this round */
    volatile int32 generation;    /* bumped as each barrier round releases    */
    volatile int32 done;
    int32          cpus;
};

struct generic_dpc_call {
    PKDEFERRED_ROUTINE       routine;
    void                    *context;
    struct generic_dpc_sync *sync;
};

static void generic_dpc_trampoline(void *arg) {
    struct generic_dpc_call *g = arg;
    KDPC dpc;

    dpc.TargetInfoAsUlong = 0;
    dpc.DpcListEntry      = NULL;
    dpc.ProcessorHistory  = 0;
    dpc.DeferredRoutine   = g->routine;
    dpc.DeferredContext   = g->context;
    dpc.SystemArgument1   = (void *)&g->sync->done;
    dpc.SystemArgument2   = (void *)g->sync;
    dpc.DpcData           = NULL;
    g->routine(&dpc, g->context, dpc.SystemArgument1, dpc.SystemArgument2);
}

void WDM_ABI KeGenericCallDpc(PKDEFERRED_ROUTINE Routine, void *Context) {
    struct generic_dpc_sync sync;
    struct generic_dpc_call g;

    if (Routine == NULL) {
        return;
    }
    sync.arrived    = 0;
    sync.generation = 0;
    sync.done       = 0;
    sync.cpus       = (int32)active_count();
    g.routine = Routine;
    g.context = Context;
    g.sync    = &sync;
    smp_call_all(generic_dpc_trampoline, &g);
}

/* A barrier every CPU in the KeGenericCallDpc passes together. Returns TRUE
 * on exactly one of them - the last to arrive - which is how a routine that
 * needs one CPU to do the shared part picks it. */
uint8 WDM_ABI KeSignalCallDpcSynchronize(void *SystemArgument2) {
    struct generic_dpc_sync *s = SystemArgument2;
    int32 gen, n;

    if (s == NULL) {
        return 0;
    }
    gen = s->generation;
    n = __atomic_add_fetch(&s->arrived, 1, __ATOMIC_ACQ_REL);
    if (n == s->cpus) {
        s->arrived = 0;
        __atomic_add_fetch(&s->generation, 1, __ATOMIC_RELEASE);
        return 1;
    }
    while (__atomic_load_n(&s->generation, __ATOMIC_ACQUIRE) == gen) {
        __asm__ volatile ("pause");
    }
    return 0;
}

void WDM_ABI KeSignalCallDpcDone(void *SystemArgument1) {
    if (SystemArgument1 != NULL) {
        __atomic_add_fetch((volatile int32 *)SystemArgument1, 1, __ATOMIC_RELEASE);
    }
}

/* --- one, later: DPC objects --------------------------------------------------
 *
 * TargetInfo's Number field holds the target CPU + 1 (0: "whichever CPU
 * queues it", NT's default). DpcData is non-NULL exactly while the DPC is
 * queued, which is what makes a second KeInsertQueueDpc return FALSE. The
 * queue is singly linked through DpcListEntry. */
static PKDPC dpc_head[SMP_MAX_CPUS];
static PKDPC dpc_tail[SMP_MAX_CPUS];
static volatile uint32 dpc_lock;
static volatile int dpc_pending[SMP_MAX_CPUS];
static uint64 dpc_ran[SMP_MAX_CPUS];

static uint64 dpc_lock_acquire(void) {
    uint64 f;

    __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(f) : : "memory");
    while (__atomic_exchange_n(&dpc_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        __asm__ volatile ("pause");
    }
    return f;
}

static void dpc_lock_release(uint64 f) {
    __atomic_store_n(&dpc_lock, 0, __ATOMIC_RELEASE);
    __asm__ volatile ("pushq %0\n\tpopfq" : : "r"(f) : "memory", "cc");
}

void WDM_ABI KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
                             void *DeferredContext) {
    if (Dpc == NULL) {
        return;
    }
    Dpc->TargetInfoAsUlong = 0;
    Dpc->Type              = 0x13;              /* DpcObject */
    Dpc->Importance        = 1;                 /* MediumImportance */
    Dpc->Number            = 0;
    Dpc->DpcListEntry      = NULL;
    Dpc->ProcessorHistory  = 0;
    Dpc->DeferredRoutine   = DeferredRoutine;
    Dpc->DeferredContext   = DeferredContext;
    Dpc->SystemArgument1   = NULL;
    Dpc->SystemArgument2   = NULL;
    Dpc->DpcData           = NULL;
}

void WDM_ABI KeInitializeThreadedDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
                                     void *DeferredContext) {
    /* A threaded DPC runs at PASSIVE_LEVEL in a thread on NT; here every DPC
     * already runs with the big kernel lock in a context that may take it,
     * which satisfies both kinds. */
    KeInitializeDpc(Dpc, DeferredRoutine, DeferredContext);
}

void WDM_ABI KeSetTargetProcessorDpc(PKDPC Dpc, int8 Number) {
    if (Dpc != NULL && Number >= 0 && Number < smp_cpu_count()) {
        Dpc->Number = (uint16)(Number + 1);
    }
}

NTSTATUS WDM_ABI KeSetTargetProcessorDpcEx(PKDPC Dpc, PPROCESSOR_NUMBER ProcNumber) {
    if (Dpc == NULL || ProcNumber == NULL || ProcNumber->Group != 0 ||
        ProcNumber->Number >= (uint8)smp_cpu_count()) {
        return (NTSTATUS)0xC000000Du;
    }
    Dpc->Number = (uint16)(ProcNumber->Number + 1);
    return 0;
}

void WDM_ABI KeSetImportanceDpc(PKDPC Dpc, int Importance) {
    if (Dpc != NULL) {
        Dpc->Importance = (uint8)Importance;
    }
}

uint8 WDM_ABI KeInsertQueueDpc(PKDPC Dpc, void *SystemArgument1,
                               void *SystemArgument2) {
    int target;
    uint64 f;

    if (Dpc == NULL) {
        return 0;
    }
    target = Dpc->Number != 0 ? (int)Dpc->Number - 1 : smp_cpu_index();
    if (target < 0 || target >= smp_cpu_count() || !smp_cpu(target)->online) {
        target = smp_cpu_index();
    }
    f = dpc_lock_acquire();
    if (Dpc->DpcData != NULL) {
        dpc_lock_release(f);
        return 0;                               /* already queued */
    }
    Dpc->SystemArgument1 = SystemArgument1;
    Dpc->SystemArgument2 = SystemArgument2;
    Dpc->DpcData         = (void *)&dpc_head[target];
    Dpc->DpcListEntry    = NULL;
    if (dpc_tail[target] != NULL) {
        dpc_tail[target]->DpcListEntry = (void *)Dpc;
    } else {
        dpc_head[target] = Dpc;
    }
    dpc_tail[target] = Dpc;
    dpc_pending[target] = 1;
    dpc_lock_release(f);

    /* The target drains its queue the next time it holds the big kernel
     * lock at a safe point. Another CPU is made to get there now; this one
     * gets there at the end of the interrupt or syscall it is in. */
    if (target != smp_cpu_index()) {
        smp_kick(target);
    } else {
        smp_this_cpu()->resched = 1;
    }
    return 1;
}

uint8 WDM_ABI KeRemoveQueueDpc(PKDPC Dpc) {
    int cpu;
    uint64 f;

    if (Dpc == NULL) {
        return 0;
    }
    f = dpc_lock_acquire();
    if (Dpc->DpcData == NULL) {
        dpc_lock_release(f);
        return 0;
    }
    for (cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        PKDPC prev = NULL, d = dpc_head[cpu];

        while (d != NULL && d != Dpc) {
            prev = d;
            d = (PKDPC)d->DpcListEntry;
        }
        if (d == Dpc) {
            if (prev == NULL) {
                dpc_head[cpu] = (PKDPC)d->DpcListEntry;
            } else {
                prev->DpcListEntry = d->DpcListEntry;
            }
            if (dpc_tail[cpu] == d) {
                dpc_tail[cpu] = prev;
            }
            break;
        }
    }
    Dpc->DpcData = NULL;
    Dpc->DpcListEntry = NULL;
    dpc_lock_release(f);
    return 1;
}

int wdm_dpc_pending(void) {
    return dpc_pending[smp_cpu_index()];
}

/* Run everything queued for this CPU. Called with the big kernel lock held,
 * at a point where nothing on this CPU's stack is in the middle of anything
 * a DPC could touch. Each DPC is unlinked (and DpcData cleared) BEFORE its
 * routine runs, so the routine may requeue itself - the periodic idiom. */
void wdm_dpc_drain(void) {
    int cpu = smp_cpu_index();

    while (dpc_pending[cpu]) {
        PKDPC d;
        uint64 f = dpc_lock_acquire();

        d = dpc_head[cpu];
        if (d == NULL) {
            dpc_pending[cpu] = 0;
            dpc_lock_release(f);
            break;
        }
        dpc_head[cpu] = (PKDPC)d->DpcListEntry;
        if (dpc_head[cpu] == NULL) {
            dpc_tail[cpu] = NULL;
        }
        d->DpcListEntry = NULL;
        d->DpcData      = NULL;
        d->ProcessorHistory |= 1ULL << cpu;
        dpc_lock_release(f);

        dpc_ran[cpu]++;
        if (d->DeferredRoutine != NULL) {
            d->DeferredRoutine(d, d->DeferredContext, d->SystemArgument1,
                               d->SystemArgument2);
        }
    }
}

/* Wait until every DPC queued before the call has run, on every CPU. */
void WDM_ABI KeFlushQueuedDpcs(void) {
    int cpu;
    uint32 spins;

    wdm_dpc_drain();
    for (cpu = 0; cpu < smp_cpu_count(); cpu++) {
        if (cpu == smp_cpu_index() || !dpc_pending[cpu]) {
            continue;
        }
        smp_kick(cpu);
        /* The target drains only with the big kernel lock, which this CPU
         * holds - so hand it over while waiting. */
        for (spins = 0; spins < 100000 && dpc_pending[cpu]; spins++) {
            int depth = bkl_release_all();

            __asm__ volatile ("pause");
            bkl_restore(depth);
        }
    }
}

uint64 wdm_dpc_count(int cpu) {
    return (cpu >= 0 && cpu < SMP_MAX_CPUS) ? dpc_ran[cpu] : 0;
}

/* --- here, now: moving the calling thread -------------------------------------
 *
 * NT's system affinity is a TEMPORARY override of the thread's own mask,
 * undone by the matching revert. The user mask is saved in the per-thread
 * slot below (indexed by the process table) and restored on revert. */
static uint64 saved_user_affinity[MAX_PROCESSES];
static uint8  has_saved_affinity[MAX_PROCESSES];

KAFFINITY WDM_ABI KeSetSystemAffinityThreadEx(KAFFINITY Affinity) {
    process_t *me = proc_current();
    int slot;
    KAFFINITY previous;

    if (me == NULL) {
        return 0;
    }
    slot = proc_index(me);
    previous = me->affinity;
    if (!has_saved_affinity[slot]) {
        saved_user_affinity[slot] = me->affinity;
        has_saved_affinity[slot] = 1;
    }
    if (sched_set_affinity(me, Affinity) == 0) {
        sched_migrate_self();
    }
    return previous;
}

void WDM_ABI KeSetSystemAffinityThread(KAFFINITY Affinity) {
    (void)KeSetSystemAffinityThreadEx(Affinity);
}

void WDM_ABI KeRevertToUserAffinityThreadEx(KAFFINITY Affinity) {
    process_t *me = proc_current();

    if (me == NULL) {
        return;
    }
    (void)sched_set_affinity(me, Affinity != 0 ? Affinity : ~0ULL);
    has_saved_affinity[proc_index(me)] = 0;
    sched_migrate_self();
}

void WDM_ABI KeRevertToUserAffinityThread(void) {
    process_t *me = proc_current();
    int slot;

    if (me == NULL) {
        return;
    }
    slot = proc_index(me);
    KeRevertToUserAffinityThreadEx(has_saved_affinity[slot]
                                   ? saved_user_affinity[slot] : ~0ULL);
}

void WDM_ABI KeSetSystemGroupAffinityThread(PGROUP_AFFINITY Affinity,
                                            PGROUP_AFFINITY PreviousAffinity) {
    process_t *me = proc_current();

    if (PreviousAffinity != NULL && me != NULL) {
        PreviousAffinity->Mask  = me->affinity;
        PreviousAffinity->Group = 0;
        PreviousAffinity->Reserved[0] = 0;
        PreviousAffinity->Reserved[1] = 0;
        PreviousAffinity->Reserved[2] = 0;
    }
    if (Affinity != NULL && Affinity->Group == 0) {
        KeSetSystemAffinityThread(Affinity->Mask);
    }
}

void WDM_ABI KeRevertToUserGroupAffinityThread(PGROUP_AFFINITY PreviousAffinity) {
    KeRevertToUserAffinityThreadEx(PreviousAffinity != NULL
                                   ? PreviousAffinity->Mask : 0);
}

/* --- the other spin-lock entry points ------------------------------------------ */

void WDM_ABI KeInitializeSpinLock(PKSPIN_LOCK SpinLock) {
    if (SpinLock != NULL) {
        SpinLock->locked = 0;
    }
}

/* Caller is already at DISPATCH_LEVEL (inside a DPC, or holding another
 * lock): take the lock without touching the IRQL. */
void WDM_ABI KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK SpinLock) {
    for (;;) {
        if (__atomic_exchange_n(&SpinLock->locked, 1u, __ATOMIC_ACQUIRE) == 0) {
            return;
        }
        while (__atomic_load_n(&SpinLock->locked, __ATOMIC_RELAXED)) {
            __asm__ volatile ("pause");
        }
    }
}

void WDM_ABI KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK SpinLock) {
    __atomic_store_n(&SpinLock->locked, 0u, __ATOMIC_RELEASE);
}

uint8 WDM_ABI KeTryToAcquireSpinLockAtDpcLevel(PKSPIN_LOCK SpinLock) {
    return __atomic_exchange_n(&SpinLock->locked, 1u, __ATOMIC_ACQUIRE) == 0;
}

KIRQL WDM_ABI KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK SpinLock) {
    KIRQL old;

    KeAcquireSpinLock(SpinLock, &old);
    return old;
}

uint8 WDM_ABI KeTestSpinLock(PKSPIN_LOCK SpinLock) {
    /* TRUE if the lock looks FREE - the hint a driver spins on before
     * trying again. */
    return __atomic_load_n(&SpinLock->locked, __ATOMIC_RELAXED) == 0;
}

/* In-stack queued spin locks: the lock handle lives in the caller's frame
 * and carries the IRQL to restore. NT queues waiters in order on the
 * handles themselves; the lock word here is the same KSPIN_LOCK every other
 * entry point uses, so a queued acquirer and a plain one exclude each other
 * correctly - which is the property a driver mixing the two relies on. */
void WDM_ABI KeAcquireInStackQueuedSpinLock(PKSPIN_LOCK SpinLock,
                                            PKLOCK_QUEUE_HANDLE LockHandle) {
    KIRQL old;

    KeAcquireSpinLock(SpinLock, &old);
    LockHandle->LockQueue.Next = NULL;
    LockHandle->LockQueue.Lock = SpinLock;
    LockHandle->OldIrql = old;
}

void WDM_ABI KeReleaseInStackQueuedSpinLock(PKLOCK_QUEUE_HANDLE LockHandle) {
    KeReleaseSpinLock(LockHandle->LockQueue.Lock, LockHandle->OldIrql);
}

void WDM_ABI KeAcquireInStackQueuedSpinLockAtDpcLevel(PKSPIN_LOCK SpinLock,
                                                      PKLOCK_QUEUE_HANDLE LockHandle) {
    KeAcquireSpinLockAtDpcLevel(SpinLock);
    LockHandle->LockQueue.Next = NULL;
    LockHandle->LockQueue.Lock = SpinLock;
    LockHandle->OldIrql = DISPATCH_LEVEL;
}

void WDM_ABI KeReleaseInStackQueuedSpinLockFromDpcLevel(PKLOCK_QUEUE_HANDLE LockHandle) {
    KeReleaseSpinLockFromDpcLevel(LockHandle->LockQueue.Lock);
}

/* --- interlocked lists ------------------------------------------------------------
 *
 * The ExInterlocked* list routines: a doubly linked LIST_ENTRY guarded by a
 * caller-supplied spin lock, which is how drivers share a work list between
 * an ISR/DPC on one CPU and a dispatch routine on another. */
PLIST_ENTRY WDM_ABI ExInterlockedInsertHeadList(PLIST_ENTRY ListHead,
                                                PLIST_ENTRY ListEntry,
                                                PKSPIN_LOCK Lock) {
    KIRQL old;
    PLIST_ENTRY first;

    KeAcquireSpinLock(Lock, &old);
    first = ListHead->Flink;
    ListEntry->Flink = first;
    ListEntry->Blink = ListHead;
    first->Blink = ListEntry;
    ListHead->Flink = ListEntry;
    KeReleaseSpinLock(Lock, old);
    return first == ListHead ? NULL : first;
}

PLIST_ENTRY WDM_ABI ExInterlockedInsertTailList(PLIST_ENTRY ListHead,
                                                PLIST_ENTRY ListEntry,
                                                PKSPIN_LOCK Lock) {
    KIRQL old;
    PLIST_ENTRY last;

    KeAcquireSpinLock(Lock, &old);
    last = ListHead->Blink;
    ListEntry->Flink = ListHead;
    ListEntry->Blink = last;
    last->Flink = ListEntry;
    ListHead->Blink = ListEntry;
    KeReleaseSpinLock(Lock, old);
    return last == ListHead ? NULL : last;
}

PLIST_ENTRY WDM_ABI ExInterlockedRemoveHeadList(PLIST_ENTRY ListHead,
                                                PKSPIN_LOCK Lock) {
    KIRQL old;
    PLIST_ENTRY first;

    KeAcquireSpinLock(Lock, &old);
    first = ListHead->Flink;
    if (first == ListHead) {
        first = NULL;
    } else {
        ListHead->Flink = first->Flink;
        first->Flink->Blink = ListHead;
    }
    KeReleaseSpinLock(Lock, old);
    return first;
}

/* --- time and stalls ---------------------------------------------------------------- */

void WDM_ABI KeStallExecutionProcessor(uint32 MicroSeconds) {
    uint64 hz = timer_tsc_hz();
    uint64 start = timer_tsc();
    uint64 want = hz != 0 ? ((uint64)MicroSeconds * hz) / 1000000ULL + 1
                          : (uint64)MicroSeconds * 5000ULL;

    while (timer_tsc() - start < want) {
        __asm__ volatile ("pause");
    }
}

int64 WDM_ABI KeQueryPerformanceCounter(int64 *PerformanceFrequency) {
    uint64 hz = timer_tsc_hz();

    if (hz == 0) {
        /* Before calibration: the tick, in its own unit. */
        if (PerformanceFrequency != NULL) {
            *PerformanceFrequency = (int64)timer_hz();
        }
        return (int64)timer_ticks_now();
    }
    if (PerformanceFrequency != NULL) {
        *PerformanceFrequency = (int64)hz;
    }
    return (int64)timer_tsc();
}

void WDM_ABI KeQueryTickCount(int64 *CurrentCount) {
    if (CurrentCount != NULL) {
        *CurrentCount = (int64)timer_ticks_now();
    }
}

uint32 WDM_ABI KeQueryTimeIncrement(void) {
    /* 100ns units per tick: 100000 at 100Hz. */
    return (uint32)(10000000u / timer_hz());
}

void wdm_smp_started(void) {
    KeNumberProcessors = (int8)active_count();
    smp_set_deferred_hook(wdm_dpc_pending, wdm_dpc_drain);
}

/* --- the selftest ---------------------------------------------------------------
 *
 * Each group of calls against the machine it is running on: counts that
 * agree with the scheduler's, an IPI worker that really ran once on every
 * CPU, a DPC barrier that really held every CPU together, a targeted DPC that
 * ran on its target and nowhere else, and a thread that really moved when
 * its system affinity said so. */
static volatile uint32 ipi_hits[SMP_MAX_CPUS];

static uint64 WDM_ABI ipi_worker(uint64 context) {
    uint32 me = KeGetCurrentProcessorNumber();

    __atomic_add_fetch(&ipi_hits[me], 1, __ATOMIC_RELAXED);
    return context + me;
}

static volatile uint32 gdpc_ran, gdpc_winners, gdpc_after_barrier_min;
static volatile uint32 gdpc_arrived_before;

static void WDM_ABI gdpc_routine(PKDPC Dpc, void *Context, void *Arg1, void *Arg2) {
    (void)Dpc; (void)Context;
    __atomic_add_fetch(&gdpc_arrived_before, 1, __ATOMIC_ACQ_REL);
    if (KeSignalCallDpcSynchronize(Arg2)) {
        __atomic_add_fetch(&gdpc_winners, 1, __ATOMIC_RELAXED);
    }
    /* Past the barrier, EVERY CPU must already have arrived - that is what
     * a barrier is. The smallest count any CPU sees here is the check. */
    {
        uint32 seen = __atomic_load_n(&gdpc_arrived_before, __ATOMIC_ACQUIRE);
        uint32 cur = gdpc_after_barrier_min;

        while (seen < cur &&
               !__atomic_compare_exchange_n(&gdpc_after_barrier_min, (uint32 *)&cur,
                                            seen, 0, __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
        }
    }
    __atomic_add_fetch(&gdpc_ran, 1, __ATOMIC_RELAXED);
    KeSignalCallDpcDone(Arg1);
}

static volatile int dpc_ran_on = -1;
static volatile uint32 dpc_runs;

static void WDM_ABI target_dpc(PKDPC Dpc, void *Context, void *Arg1, void *Arg2) {
    (void)Dpc; (void)Arg2;
    dpc_ran_on = (int)KeGetCurrentProcessorNumber();
    dpc_runs++;
    *(volatile uint32 *)Context = (uint32)(uint64)Arg1;
}

static volatile int      aff_done;
static volatile uint32   aff_landed[SMP_MAX_CPUS];

static void affinity_thread(void *arg) {
    int n = smp_cpu_count(), k;

    (void)arg;
    for (k = n - 1; k >= 0; k--) {
        KAFFINITY prev = KeSetSystemAffinityThreadEx(1ULL << k);

        aff_landed[k] = KeGetCurrentProcessorNumber() + 1;
        KeRevertToUserAffinityThreadEx(prev);
    }
    aff_done = 1;
}

int wdm_smp_selftest(void) {
    int failures = 0;
    int n = smp_cpu_count();
    int i;
    KAFFINITY mask = 0;

    /* Counting. */
    if (KeQueryActiveProcessorCount(&mask) != (uint32)n ||
        mask != smp_online_mask() || KeQueryActiveProcessors() != mask ||
        KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS) != (uint32)n ||
        KeNumberProcessors != (int8)n) {
        kprintf_c(0x0C, "wdm smp: processor counts disagree with the scheduler\n");
        failures++;
    }
    {
        PROCESSOR_NUMBER pn;
        uint32 idx = KeGetCurrentProcessorNumberEx(&pn);

        if (idx != (uint32)smp_cpu_index() || pn.Group != 0 || pn.Number != idx ||
            KeGetProcessorIndexFromNumber(&pn) != idx) {
            kprintf_c(0x0C, "wdm smp: KeGetCurrentProcessorNumberEx is wrong\n");
            failures++;
        }
    }

    /* KeIpiGenericCall: once on every CPU, caller's value returned. */
    for (i = 0; i < SMP_MAX_CPUS; i++) {
        ipi_hits[i] = 0;
    }
    {
        uint64 r = KeIpiGenericCall(ipi_worker, 1000);

        if (r != 1000 + (uint64)smp_cpu_index()) {
            kprintf_c(0x0C, "wdm smp: KeIpiGenericCall returned %lx\n", r);
            failures++;
        }
        for (i = 0; i < n; i++) {
            if (ipi_hits[i] != 1) {
                kprintf_c(0x0C, "wdm smp: IPI worker ran %d times on cpu%d\n",
                          (int)ipi_hits[i], i);
                failures++;
            }
        }
    }

    /* KeGenericCallDpc: every CPU, one barrier, exactly one winner. */
    gdpc_ran = 0;
    gdpc_winners = 0;
    gdpc_arrived_before = 0;
    gdpc_after_barrier_min = 0xFFFFFFFFu;
    KeGenericCallDpc(gdpc_routine, NULL);
    if (gdpc_ran != (uint32)n || gdpc_winners != 1 ||
        gdpc_after_barrier_min != (uint32)n) {
        kprintf_c(0x0C, "wdm smp: KeGenericCallDpc ran %d, %d barrier winners, "
                        "min arrivals past the barrier %d (want %d, 1, %d)\n",
                  (int)gdpc_ran, (int)gdpc_winners, (int)gdpc_after_barrier_min,
                  n, n);
        failures++;
    }

    /* A DPC targeted at the last CPU. */
    {
        KDPC dpc;
        volatile uint32 got = 0;
        int target = n - 1;

        dpc_ran_on = -1;
        dpc_runs = 0;
        KeInitializeDpc(&dpc, target_dpc, (void *)&got);
        KeSetTargetProcessorDpc(&dpc, (int8)target);
        if (!KeInsertQueueDpc(&dpc, (void *)0x5A5A, NULL)) {
            kprintf_c(0x0C, "wdm smp: KeInsertQueueDpc refused a fresh DPC\n");
            failures++;
        }
        if (target != smp_cpu_index() && KeInsertQueueDpc(&dpc, NULL, NULL)) {
            kprintf_c(0x0C, "wdm smp: a queued DPC was queued twice\n");
            failures++;
        }
        KeFlushQueuedDpcs();
        if (dpc_runs != 1 || dpc_ran_on != target || got != 0x5A5A ||
            dpc.DpcData != NULL) {
            kprintf_c(0x0C, "wdm smp: targeted DPC ran %d times on cpu%d "
                            "(want once on cpu%d)\n",
                      (int)dpc_runs, dpc_ran_on, target);
            failures++;
        }
        if (KeRemoveQueueDpc(&dpc)) {
            kprintf_c(0x0C, "wdm smp: removed a DPC that had already run\n");
            failures++;
        }
    }

    /* System affinity moves a thread: a kernel thread visits every CPU. */
    if (n > 1) {
        uint64 start = timer_ticks_now();

        aff_done = 0;
        for (i = 0; i < SMP_MAX_CPUS; i++) {
            aff_landed[i] = 0;
        }
        if (kthread_create(affinity_thread, NULL, "wdm-affinity") == NULL) {
            kprintf_c(0x0C, "wdm smp: could not start the affinity thread\n");
            failures++;
        } else {
            /* Yield as well as wait: one of the CPUs it must visit is this
             * one, and it can only land here if this thread gives way. */
            while (!aff_done && timer_ticks_now() - start < 500) {
                schedule();
                if (!aff_done) {
                    bkl_wait_for_interrupt();
                }
            }
            for (i = 0; i < n; i++) {
                if (aff_landed[i] != (uint32)i + 1) {
                    kprintf_c(0x0C, "wdm smp: affinity to cpu%d ran on cpu%d\n",
                              i, (int)aff_landed[i] - 1);
                    failures++;
                }
            }
        }
    }

    /* In-stack queued lock and the DPC-level pair exclude each other. */
    {
        KSPIN_LOCK l;
        KLOCK_QUEUE_HANDLE h;

        KeInitializeSpinLock(&l);
        KeAcquireInStackQueuedSpinLock(&l, &h);
        if (KeTryToAcquireSpinLockAtDpcLevel(&l) || KeTestSpinLock(&l)) {
            kprintf_c(0x0C, "wdm smp: a held queued lock looked free\n");
            failures++;
        }
        KeReleaseInStackQueuedSpinLock(&h);
        if (!KeTryToAcquireSpinLockAtDpcLevel(&l)) {
            kprintf_c(0x0C, "wdm smp: a released lock could not be taken\n");
            failures++;
        }
        KeReleaseSpinLockFromDpcLevel(&l);
    }

    if (failures == 0) {
        kprintf("wdm smp: selftest passed - %d cpus: IPI call, DPC barrier, "
                "targeted DPC, affinity migration\n", n);
    } else {
        kprintf_c(0x0C, "wdm smp: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
