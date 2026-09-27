#ifndef GENESIS_KLOCK_H
#define GENESIS_KLOCK_H

#include "typesk.h"

/* Real locking primitives - ROADMAP item 6's mtx/rwlock/sx, and item 11's
 * "the allocator has NO locking".
 *
 * These were deliberately not built until Part 10 brought up a second CPU.
 * The reason is not that a lock is hard to write; it is that a lock with no
 * contention is indistinguishable from a no-op, so building one earlier
 * would have produced code that compiled, ran, and proved nothing. Every
 * "honest uniprocessor stub" in this tree - wdm.c's KeAcquireSpinLock,
 * callout.c's recorded-but-unacquired lock_object, kheap's absent locking -
 * was true while there was one CPU and stopped being true the moment there
 * were two.
 *
 * --- why interrupt state is part of the lock -----------------------------
 *
 * A spin lock taken by both an interrupt handler and ordinary kernel code
 * deadlocks on ONE CPU without any second CPU involved: the handler
 * interrupts the holder and spins for a lock that cannot be released until
 * the handler returns. Disabling interrupts on this core while holding it is
 * what makes that impossible, which is why acquire returns the previous
 * interrupt state and release takes it back rather than unconditionally
 * re-enabling. Unconditionally re-enabling would turn interrupts on inside a
 * caller that had deliberately turned them off.
 */

/* Tagged `kmtx`, not `mtx`. FreeBSD's <sys/mutex.h> defines `struct mtx`,
 * and kernel/bsd/compat provides that header for the vendored allocator - so
 * a Genesis `struct mtx` would be a second definition of the same tag in any
 * translation unit that saw both, which kernel/bsd/uma_vendor.c does. The
 * typedef name is unchanged, so nothing outside this header notices. */
typedef struct kmtx {
    volatile uint32 locked;
    uint64          saved_flags;   /* valid only while held */
    const char     *name;
    /* Which CPU holds it, or 0xFFFFFFFF. Not used for correctness - it is
     * for the "recursed on itself" check, which is the failure mode a
     * spinlock turns into a hang rather than a fault. */
    volatile uint32 owner;
    uint64          acquisitions;
    uint64          contended;     /* times a spin actually spun */
} mtx_t;

void kmtx_init(mtx_t *m, const char *name);
void kmtx_lock(mtx_t *m);
void kmtx_unlock(mtx_t *m);

/* Returns 1 if the lock was taken, 0 if it was already held. Never spins. */
int  kmtx_trylock(mtx_t *m);

/* True if THIS CPU holds it. For assertions in code that must be called with
 * a lock already held. */
int  kmtx_owned(mtx_t *m);

/* --- reader/writer -------------------------------------------------------
 *
 * Many readers or one writer. Writers can starve under a continuous stream
 * of readers, and that is accepted here rather than solved: the fair
 * variants need a queue, the queue needs an allocator, and the allocator is
 * one of the things this lock protects. Every current user holds a read lock
 * for a handful of instructions.
 */
typedef struct krwlock {
    volatile int32 readers;     /* -1 = a writer holds it */
    uint64         saved_flags;
    const char    *name;
    /* The CPU holding it for WRITING, or 0xFFFFFFFF. Recorded because
     * FreeBSD asks "do I hold this for writing" (rw_wowned - in_pcb.h's
     * INP_WLOCKED, all through TCP), and a writer holds a spinlock with
     * interrupts off, so "this CPU" and "this thread" are the same answer. */
    volatile uint32 wcpu;
} rwlock_t;

void krw_init(rwlock_t *rw, const char *name);

/* Non-zero if the calling CPU holds `rw` for writing. */
int  krw_wowned(rwlock_t *rw);
void krw_rlock(rwlock_t *rw);
void krw_runlock(rwlock_t *rw);
void krw_wlock(rwlock_t *rw);
void krw_wunlock(rwlock_t *rw);

/* One attempt at the write lock: 1 if taken, 0 if somebody else holds it.
 *
 * Exists because a caller can genuinely need to NOT block. The vendored
 * netinet/in_pcb.c wants the pcbinfo lock while already holding an inpcb
 * lock; if it cannot have it immediately it drops both and retries in the
 * other order, which is how it avoids a lock-order reversal. A "try" that
 * always succeeded would turn that reversal into a deadlock. */
int  krw_trywlock(rwlock_t *rw);

/* One attempt at a READ lock, the shared/exclusive helpers that go with it,
 * and a release that works out for itself which way the caller holds it.
 *
 * All four exist because the vendored network stack uses all four, and the
 * first version of this shim mapped try-read onto try-WRITE - which corrupted
 * the reader count the moment the caller released it as a reader. */
int  krw_tryrlock(rwlock_t *rw);
void krw_unlock(rwlock_t *rw);
int  krw_tryupgrade(rwlock_t *rw);
void krw_downgrade(rwlock_t *rw);

/* Name every write lock this CPU still holds, and return how many.
 *
 * A write lock keeps interrupts disabled for as long as it is held, so a path
 * that forgets to release one stops the timer and hangs the machine somewhere
 * else entirely. This is what turns that into a printed lock name. */
int  krw_report_held(uint8 color);
int  kmtx_report_held(uint8 color);

/* Exercise mutual exclusion, interrupt-state save/restore, trylock, and the
 * reader/writer rules. Returns the number of failures.
 *
 * On a multi-CPU boot it also runs a REAL contention test - see the
 * implementation - which is the only part that can distinguish a working
 * lock from a no-op. */
int lock_selftest(void);

/* Lock statistics for the boot report. */
void lock_report(uint8 color);

#endif
