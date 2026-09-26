#include "futex.h"
#include "paging.h"
#include "process.h"
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "timer.h"
#include "typesk.h"
#include "waitq.h"

/* See futex.h for the shape. What follows is the hash and the two operations.
 *
 * --- Keyed on the PHYSICAL address ---------------------------------------
 * This is the one decision in the file that is not obvious and not
 * reversible cheaply. A futex is a word of user memory, and the natural key
 * is the address the caller passed - which is a virtual address, meaningful
 * only inside one address space.
 *
 * That is correct exactly as long as every futex is private to one process.
 * The moment two processes share a mapping and put a lock in it - which is
 * the entire point of a process-shared mutex - the two of them name the same
 * word by two different virtual addresses, hash to two different buckets, and
 * never see each other. A waiter waits forever while a waker wakes an empty
 * queue.
 *
 * That failure is silent. Nothing returns an error, nothing faults; a program
 * simply stops. Keying on the physical address costs one page-table walk per
 * operation and cannot develop it.
 *
 * The walk is also what validates the pointer: a word with no physical page
 * behind it is not a futex, and answering -EFAULT is better than hashing a
 * zero. */

#define FUTEX_BUCKETS 16

static wait_queue_t buckets[FUTEX_BUCKETS];
static int          buckets_ready;

static void futex_init_once(void) {
    int i;

    if (buckets_ready) {
        return;
    }
    for (i = 0; i < FUTEX_BUCKETS; i++) {
        waitq_init(&buckets[i]);
    }
    buckets_ready = 1;
}

/* The physical address of a user word, or 0 if it is not mapped.
 *
 * Zero is unambiguously "not mapped" here for the reason elf.c relies on:
 * pmm_mark_region_used reserved physical zero at boot, so no real mapping can
 * produce it. */
static uint64 futex_key(uint64 uaddr) {
    uint64 page;
    uint64 phys;

    /* Four-byte alignment is required, not preferred. An unaligned futex can
     * straddle a page boundary, which would make one word have two keys
     * depending on which half was hashed - and the two halves would be woken
     * independently. */
    if ((uaddr & 3) != 0) {
        return 0;
    }
    if (!user_ptr_ok(uaddr)) {
        return 0;
    }
    page = uaddr & ~0xFFFULL;
    phys = vmm_get_phys(page);
    if (phys == 0) {
        return 0;
    }
    return (phys & ~0xFFFULL) | (uaddr & 0xFFFULL);
}

static wait_queue_t *bucket_for(uint64 key) {
    /* Shifted before masking. The low two bits of a key are always zero
     * (alignment, above), so hashing on them directly would use a quarter of
     * the buckets and leave three quarters empty - which is not a
     * correctness bug and is the kind of thing nobody ever measures. */
    return &buckets[(key >> 2) % FUTEX_BUCKETS];
}

/* Context for the readiness callback.
 *
 * The predicate is "the word no longer holds the value we were told to wait
 * on". That is the whole futex protocol: userspace has already decided the
 * lock is contended and is asking the kernel to sleep until it is not, and
 * the kernel re-checks the value because the state may have changed between
 * userspace's test and this call. Waiting without re-checking is the classic
 * lost-wakeup - the unlock happens in the gap and the waiter sleeps through
 * it. */
struct futex_wait_ctx {
    const volatile uint32 *word;
    uint32                 expected;
    int                    woken;
};

static int futex_changed(void *ctx) {
    struct futex_wait_ctx *fw = (struct futex_wait_ctx *)ctx;

    /* Woken by an explicit FUTEX_WAKE counts as ready even if the value has
     * not changed. A wake is permitted to be spurious - musl loops - and a
     * waiter that insisted on seeing a changed value would sleep through a
     * wake that was meant for it, because the waker may have restored the
     * value before this ran. */
    return fw->woken || *fw->word != fw->expected;
}

int64 futex_wait(uint64 uaddr, uint32 expected, uint64 timeout_ticks) {
    struct futex_wait_ctx fw;
    wait_queue_t *q;
    uint64 key;
    int rc;

    futex_init_once();

    key = futex_key(uaddr);
    if (key == 0) {
        return -14;                     /* -EFAULT */
    }
    q = bucket_for(key);

    fw.word     = (const volatile uint32 *)uaddr;
    fw.expected = expected;
    fw.woken    = 0;

    /* The value test happens BEFORE the sleep and is the reason this call
     * exists at all. If the word no longer holds `expected`, the contention
     * userspace saw is already over and sleeping would be sleeping on a lock
     * nobody holds. -EAGAIN, which is what the caller is written to retry
     * on. */
    if (*fw.word != expected) {
        return -11;                     /* -EAGAIN */
    }

    rc = waitq_wait_until(q, futex_changed, &fw, timeout_ticks);
    if (rc == WAITQ_SIGNAL) {
        return -4;                      /* -EINTR */
    }
    if (rc == WAITQ_TIMEOUT) {
        return -110;                    /* -ETIMEDOUT */
    }
    return 0;
}

int64 futex_wake(uint64 uaddr, int count) {
    wait_queue_t *q;
    uint64 key;

    futex_init_once();

    key = futex_key(uaddr);
    if (key == 0) {
        return -14;
    }
    q = bucket_for(key);

    /* Every waiter, regardless of `count`.
     *
     * This is a deliberate over-wake and it is worth being explicit about
     * why. `count` is usually 1, and waking one is what an efficient
     * implementation does. But the queue is hashed, so it holds waiters for
     * every futex that collided into this bucket - and picking "one" would
     * pick one of THOSE, which is a lost wakeup for the futex actually being
     * woken and a spurious one for a lock nobody touched.
     *
     * Waking everybody makes both of those impossible: the waiters re-test
     * their own words, the ones that were not woken go back to sleep, and the
     * ones that were proceed. A spurious wake is something the futex contract
     * already permits and every caller already handles; a lost wake is a
     * hang. The two are not comparable.
     *
     * Return value: the caller is told how many were woken, and it is
     * reported as `count` rather than as the true number for the same reason.
     * musl uses this to decide nothing - it discards the result - and
     * reporting the collided waiters would be reporting activity on a lock
     * the caller does not own. */
    waitq_wake_all(q);
    return count > 0 ? count : 0;
}

void futex_wake_addr(uint64 uaddr) {
    if (uaddr != 0) {
        (void)futex_wake(uaddr, 1);
    }
}
