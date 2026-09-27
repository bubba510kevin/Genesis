/* Genesis: the callout(9) timer wheel, behind FreeBSD's real API.
 *
 * --- what is vendored and what is not ------------------------------------
 * kernel/bsd/compat/sys/callout.h and sys/_callout.h are byte-for-byte
 * vendsrc/sys/sys/. That means `struct callout` has upstream's exact layout,
 * callout_reset()/callout_stop()/callout_drain()/callout_pending() are
 * upstream's exact macros, and real FreeBSD driver source using them
 * compiles against this unmodified - the same source-compat posture
 * kernel/newbus_compat.c and kernel/include/sys/bus.h already established.
 *
 * The ENGINE - vendsrc/sys/kern/kern_timeout.c - is not vendored, and this
 * is the one place in this pass where that was the wrong-looking call, so
 * the reason is worth stating precisely. kern_timeout.c is 1546 lines built
 * on machinery that does not exist here yet:
 *
 *   - per-CPU callout state (`struct callout_cpu cc_cpu[MAXCPU]`), reached
 *     through PCPU_GET - that is Part 10 of this plan.
 *   - a softclock KERNEL THREAD per CPU, woken by the hardclock, which is
 *     what runs any callout not marked C_DIRECT_EXEC. That needs kthread(9)
 *     and sleepqueue(9).
 *   - `struct mtx cc_lock` around every operation - Part 11.
 *   - callout migration between CPUs on reschedule, which is most of the
 *     file's complexity and is meaningless on one CPU.
 *
 * Vendoring it would mean vendoring the scheduler and the lock manager
 * first. So: vendor the interface, implement the wheel. When Parts 10 and 11
 * land, kern_timeout.c becomes vendorable and this file is what it replaces
 * - which is why every function below matches upstream's signature and
 * return-value contract exactly rather than a convenient subset.
 *
 * --- the wheel -----------------------------------------------------------
 * Upstream's algorithm, at this kernel's resolution. Deadlines are absolute
 * sbintime_t (see kernel/bsd/compat/sys/time.h). A callout hashes into a
 * bucket by a fixed shift of its deadline, and callout_process() sweeps
 * every bucket between the last one it swept and the one the current time
 * falls in.
 *
 * Two consequences of hashing rather than sorting, both upstream's and both
 * deliberate:
 *   - insertion and removal are O(1), which is what makes it usable from a
 *     driver's fast path.
 *   - a deadline more than one wheel revolution out hashes into a bucket
 *     that will be swept early. That is why the sweep re-checks c_time
 *     against now and leaves anything not yet due where it is, rather than
 *     trusting the bucket. Get that check wrong and long timeouts fire
 *     early, which is the classic bug in a hand-rolled wheel.
 */

/* <sys/callout.h> puts the whole API - callout_reset, callout_stop,
 * callout_pending, callout_drain - behind #ifdef _KERNEL. Same reason as
 * kernel/bsd/mbuf.c: upstream sets it per-object on the command line, and it
 * is set here instead so it is scoped to this translation unit. */
#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/lock.h>
#include <sys/callout.h>

#include "timer.h"
#include "klock.h"
#include "bkl.h"
#include "bsd.h"

/* kernel/bsd/kern_time.c - the backing for the VENDORED <sys/time.h>. */
void genesis_time_init(void);
void genesis_time_tick(void);

/* 512 buckets at 1/128 s each covers four seconds per revolution. Both are
 * powers of two so the index is a shift and a mask.
 *
 * CALLOUT_BUCKET_SHIFT is chosen against SBT_1S (1 << 32): a shift of 25
 * makes one bucket 1 << 25 sbt, which is 1/128 s - just under the 10ms tick,
 * so a 100Hz sweep visits one or two buckets per tick rather than hundreds.
 * Raising the tick rate needs no change here; lowering the bucket span below
 * the tick would only mean sweeping more empty buckets. */
#define CALLOUT_BUCKET_SHIFT 25
#define CALLOUT_BUCKETS      512
#define CALLOUT_BUCKET_MASK  (CALLOUT_BUCKETS - 1)

static struct callout_list wheel[CALLOUT_BUCKETS];

/* Bucket index the last sweep reached, so a sweep knows where to resume.
 * Held as a full bucket NUMBER, not an index, so that a sweep can tell "one
 * bucket has passed" from "a whole revolution has passed" - the latter has
 * to scan everything rather than nothing. */
static uint64 last_bucket;

static int callout_ready;

/* Set from timer.h's real programmed rate at init. Declared extern by
 * sys/time.h because <sys/callout.h>'s callout_reset() macro multiplies by
 * tick_sbt directly. */
int hz;
sbintime_t tick_sbt;

/* Uptime as an sbintime_t.
 *
 * Resolution is one tick - timer.c counts ticks, not cycles - so this
 * advances in 10ms steps at 100Hz. That is a real limit on how precisely a
 * callout can fire and it is worth being explicit about: callout_reset(c, 1,
 * ...) means "at or after the next tick", not "10ms from this instant".
 *
 * Computed as ticks * tick_sbt rather than by converting nanoseconds,
 * because tick_sbt is the exact same number callout_reset() multiplies the
 * caller's tick count by. Converting through ns would round differently in
 * the two places and a callout could land one tick short of its own
 * deadline, which reads as a timer that occasionally does not fire. */
/* sbinuptime() itself now comes from the VENDORED <sys/time.h>, which
 * defines it as an inline over binuptime(). kernel/bsd/kern_time.c implements
 * binuptime() against this same tick counter, so the value is unchanged - but
 * there is one definition of it in the tree instead of two, and every
 * vendored file that asks for the time gets the same answer this one does. */

/* Interrupts off across list manipulation.
 *
 * Not a lock and not pretending to be one. The race this closes is real and
 * present on one CPU: callout_reset() and callout_stop() run in normal
 * kernel context, callout_process() runs inside the timer interrupt, and
 * both touch the same LIST. Part 11 replaces these two helpers with a real
 * mutex; until then this is sufficient and, unlike a no-op stub, correct for
 * the machine it runs on. */
/* The wheel's lock.
 *
 * This was intr_disable/intr_restore, and sys/lock.h's shim said in so many
 * words that a real mutex was Part 11's job and that disabling interrupts
 * was sufficient "on one CPU with callouts run from a tick handler". Part 10
 * removed that premise: a second CPU can call callout_reset while the first
 * is inside callout_process walking the same bucket list.
 *
 * Interrupt-disable is still part of it and always was - the tick handler
 * runs in interrupt context on this same CPU, so the lock must exclude it
 * too - which is exactly what kmtx_lock does on top of the spin. */
static mtx_t callout_mtx = { 0, 0, "callout", 0xFFFFFFFFu, 0, 0 };

static uint64 intr_disable(void) {
    kmtx_lock(&callout_mtx);
    return 0;
}

static void intr_restore(uint64 flags) {
    (void)flags;
    kmtx_unlock(&callout_mtx);
}

static uint64 bucket_of(sbintime_t when) {
    return (uint64)when >> CALLOUT_BUCKET_SHIFT;
}

/* Unlink a pending callout. Caller holds interrupts off. */
static void callout_unlink(struct callout *c) {
    LIST_REMOVE(c, c_links.le);
    c->c_iflags &= ~CALLOUT_PENDING;
}

void net_callout_init(void) {
    int i;

    for (i = 0; i < CALLOUT_BUCKETS; i++) {
        LIST_INIT(&wheel[i]);
    }

    /* timer_hz() rather than TIMER_HZ: the PIT divisor is an integer, so the
     * rate actually programmed for 100Hz is 99.998Hz, and timer.h's own
     * comment says anything converting ticks to time must use the real
     * number. A callout wheel is exactly that. */
    hz = (int)timer_hz();
    if (hz <= 0) {
        hz = TIMER_HZ;
    }
    tick_sbt = SBT_1S / hz;

    /* Before sbinuptime(), which is now an inline over binuptime(): hz and
     * tick_sbt were set just above and genesis_time_init() derives the
     * bintime forms of them that <sys/time.h> publishes. */
    genesis_time_init();

    last_bucket = bucket_of(sbinuptime());
    callout_ready = 1;

    /* Microseconds computed as tick_sbt * 1000000 / SBT_1S rather than
     * tick_sbt / SBT_1US: SBT_1US is SBT_1S/1000000 truncated to 4294 from
     * 4294.967, so dividing by it reports a 10000us tick as 10002. Scaling
     * up first keeps the printed number equal to the real one, which
     * matters because this line is the boot-time evidence for the check in
     * src/verif.c. */
    kprintf("callout: wheel up - %d buckets, %d Hz, tick %d us, "
            "wheel spans %d ms\n",
            CALLOUT_BUCKETS, hz,
            (int)(tick_sbt * 1000000 / SBT_1S),
            (int)(((sbintime_t)CALLOUT_BUCKETS << CALLOUT_BUCKET_SHIFT)
                  * 1000 / SBT_1S));
}

void callout_init(struct callout *c, int mpsafe) {
    (void)mpsafe;   /* CALLOUT_MPSAFE is deprecated upstream too */

    bzero(c, sizeof(*c));
    c->c_cpu = 0;
}

void _callout_init_lock(struct callout *c, struct lock_object *lock,
                        int flags) {
    bzero(c, sizeof(*c));
    c->c_lock = lock;

    /* Upstream stores exactly these two caller-visible bits out of `flags`
     * and derives the rest. Recorded rather than acted on: nothing acquires
     * c_lock here - see kernel/bsd/compat/sys/lock.h for why, and for when
     * that changes. */
    c->c_iflags = 0;
    c->c_flags = (short)(flags & (CALLOUT_RETURNUNLOCKED | CALLOUT_SHAREDLOCK));
}

void callout_when(sbintime_t sbt, sbintime_t precision, int flags,
                  sbintime_t *sbt_res, sbintime_t *prec_res) {
    sbintime_t to_sbt;
    sbintime_t to_pr;

    if ((flags & (C_ABSOLUTE | C_PRECALC)) != 0) {
        to_sbt = sbt;
    } else {
        /* C_HARDCLOCK means "the caller thinks in ticks", so a request
         * shorter than one tick is rounded up to one - a zero-tick timeout
         * would otherwise be already due and fire inside the same sweep it
         * was scheduled from. */
        if ((flags & C_HARDCLOCK) != 0 && sbt < tick_sbt) {
            sbt = tick_sbt;
        }
        to_sbt = sbinuptime() + sbt;
    }

    /* C_PREL(x) packs a "precision is 1/2^x of the interval" hint into the
     * flags. Honoured as upstream computes it even though nothing here
     * coalesces on it yet, so a driver passing the hint gets the same
     * numbers it would on FreeBSD. */
    to_pr = precision;
    if (C_PRELGET(flags) >= 0) {
        to_pr = sbt >> C_PRELGET(flags);
    }

    if (sbt_res != 0) {
        *sbt_res = to_sbt;
    }
    if (prec_res != 0) {
        *prec_res = to_pr;
    }
}

int callout_reset_sbt_on(struct callout *c, sbintime_t sbt, sbintime_t prec,
                         void (*func)(void *), void *arg, int cpu, int flags) {
    sbintime_t to_sbt;
    sbintime_t to_pr;
    uint64 saved;
    int cancelled = 0;

    (void)cpu;      /* one CPU; upstream would migrate here */

    if (callout_ready == 0) {
        return 0;
    }

    callout_when(sbt, prec, flags, &to_sbt, &to_pr);

    saved = intr_disable();

    /* Rescheduling an already-pending callout cancels the old deadline and
     * reports that it did - that return value is upstream's contract and is
     * what a caller uses to know it did not race with the handler. */
    if ((c->c_iflags & CALLOUT_PENDING) != 0) {
        callout_unlink(c);
        cancelled = 1;
    }

    c->c_func = func;
    c->c_arg = arg;
    c->c_time = to_sbt;
    c->c_precision = to_pr;
    c->c_iflags |= CALLOUT_PENDING;
    c->c_flags |= CALLOUT_ACTIVE;

    LIST_INSERT_HEAD(&wheel[bucket_of(to_sbt) & CALLOUT_BUCKET_MASK],
                     c, c_links.le);

    intr_restore(saved);
    return cancelled;
}

int callout_schedule_on(struct callout *c, int to_ticks, int cpu) {
    return callout_reset_sbt_on(c, tick_sbt * to_ticks, 0, c->c_func,
                                c->c_arg, cpu, C_HARDCLOCK);
}

int callout_schedule(struct callout *c, int to_ticks) {
    return callout_schedule_on(c, to_ticks, -1);
}

int _callout_stop_safe(struct callout *c, int flags) {
    uint64 saved;
    int was_pending;

    /* CS_DRAIN means "and wait until it is not running". Upstream needs that
     * because a callout runs on a softclock thread that can be mid-handler
     * on another CPU. Here every callout runs to completion inside
     * callout_process(), in the timer interrupt, so by the time any other
     * code observes the callout it is not running - unless the caller IS the
     * handler, in which case waiting would deadlock and upstream returns
     * without waiting too. Nothing to wait for either way. */
    (void)flags;

    saved = intr_disable();
    was_pending = (c->c_iflags & CALLOUT_PENDING) != 0;
    if (was_pending) {
        callout_unlink(c);
    }
    c->c_flags &= ~CALLOUT_ACTIVE;
    intr_restore(saved);

    return was_pending;
}

/* Sweep the wheel and run whatever is due.
 *
 * Called from the timer interrupt. Handlers therefore run with interrupts
 * disabled and must be short - the same rule as any FreeBSD callout marked
 * C_DIRECT_EXEC, which is what all of them effectively are here until a
 * softclock thread exists to defer the rest to.
 */
void callout_process(sbintime_t now) {
    struct callout_list expired;
    struct callout *c;
    struct callout *tmp;
    uint64 first;
    uint64 nowb;
    uint64 sweep;
    uint64 b;

    if (callout_ready == 0) {
        return;
    }

    LIST_INIT(&expired);

    nowb = bucket_of(now);
    first = last_bucket;

    /* A whole revolution or more has elapsed - the tick was blocked for four
     * seconds, or this is the first sweep after a long stall. Scanning every
     * bucket once is both correct and bounded; scanning the elapsed count
     * would be a multi-million iteration loop for the same result. */
    sweep = nowb - first;
    if (sweep >= CALLOUT_BUCKETS) {
        first = nowb - (CALLOUT_BUCKETS - 1);
    }

    for (b = first; b <= nowb; b++) {
        struct callout_list *head = &wheel[b & CALLOUT_BUCKET_MASK];

        LIST_FOREACH_SAFE(c, head, c_links.le, tmp) {
            /* The bucket only says the deadline HASHES here, not that it has
             * arrived - a callout more than one revolution out shares this
             * bucket with one that is due now. See the file header. */
            if (c->c_time > now) {
                continue;
            }
            LIST_REMOVE(c, c_links.le);
            c->c_iflags &= ~CALLOUT_PENDING;
            LIST_INSERT_HEAD(&expired, c, c_links.le);
        }
    }

    last_bucket = nowb;

    /* Run them off the wheel, not on it: a handler is entitled to call
     * callout_reset() on its own callout, and re-inserting into a bucket
     * this loop is still walking would either lose it or spin. */
    while (!LIST_EMPTY(&expired)) {
        c = LIST_FIRST(&expired);
        LIST_REMOVE(c, c_links.le);
        c->c_flags &= ~CALLOUT_ACTIVE;
        if (c->c_func != 0) {
            c->c_func(c->c_arg);
        }
    }
}

/* Genesis: what timer.c's tick handler calls. Separate from
 * callout_process() so that the sbintime conversion lives here, next to
 * sbinuptime(), rather than in the timer driver. */
void net_callout_tick(void) {
    /* time_second and time_uptime are GLOBALS that vendored code reads
     * directly rather than through a call (ip_reass.c ages its fragment
     * queues off time_uptime, tcp stamps connections off time_second), so
     * something has to advance them. This is the tick that does. */
    genesis_time_tick();
    callout_process(sbinuptime());
}

/* ------------------------------------------------------------------------
 * Selftest. See src/verif.c.
 */

static int fired_count;
static int fired_order[4];
static int fired_next;
static struct callout test_a;
static struct callout test_b;
static struct callout test_never;
static struct callout test_periodic;
static int periodic_count;

static void test_handler(void *arg) {
    int which = (int)(uintptr)arg;

    fired_count++;
    if (fired_next < 4) {
        fired_order[fired_next++] = which;
    }
}

static void test_periodic_handler(void *arg) {
    (void)arg;
    periodic_count++;
    if (periodic_count < 3) {
        callout_reset(&test_periodic, 1, test_periodic_handler, 0);
    }
}

int net_callout_selftest(void) {
    uint64 start;
    int failures = 0;

    fired_count = 0;
    fired_next = 0;
    periodic_count = 0;

    callout_init(&test_a, 1);
    callout_init(&test_b, 1);
    callout_init(&test_never, 1);
    callout_init(&test_periodic, 1);

    /* b is scheduled first but due later, so firing order proves the wheel
     * is ordering by deadline rather than by insertion. */
    callout_reset(&test_b, 5, test_handler, (void *)(uintptr)2);
    callout_reset(&test_a, 2, test_handler, (void *)(uintptr)1);

    /* Far enough out that it must not fire during this test, and far enough
     * that it lands well past one wheel revolution - which is the case that
     * catches a sweep trusting the bucket instead of re-checking c_time. */
    callout_reset(&test_never, hz * 600, test_handler, (void *)(uintptr)3);

    if (!callout_pending(&test_a) || !callout_pending(&test_b)) {
        kprintf_c(0x0C, "callout selftest: reset did not set PENDING\n");
        failures++;
    }

    /* Spin on the tick with interrupts on: net_callout_tick runs from the
     * timer interrupt, so this waits for real ticks rather than driving the
     * wheel by hand - which is the point, since driving it by hand would
     * test the sweep without testing the wiring into timer.c.
     *
     * With the big kernel lock RELEASED across each halt. The tick is the
     * PIT's, delivered to the BSP only, and this code may be running on an
     * AP: holding the lock through a bare hlt left the BSP spinning for it
     * with interrupts off - so no tick ever came, and the wait never ended.
     * A boot hang, found once dhclient's thread made the boot context
     * migrate. */
    start = timer_ticks_now();
    while (timer_ticks_now() - start < 10) {
        bkl_wait_for_interrupt();
    }

    if (fired_count != 2) {
        kprintf_c(0x0C, "callout selftest: %d fired, expected 2\n",
                  fired_count);
        failures++;
    }
    if (fired_next >= 2 && (fired_order[0] != 1 || fired_order[1] != 2)) {
        kprintf_c(0x0C, "callout selftest: fired out of deadline order\n");
        failures++;
    }
    if (callout_pending(&test_a) || callout_pending(&test_b)) {
        kprintf_c(0x0C, "callout selftest: PENDING still set after firing\n");
        failures++;
    }
    if (!callout_pending(&test_never)) {
        kprintf_c(0x0C, "callout selftest: long timeout fired early\n");
        failures++;
    }

    /* callout_stop on a pending callout reports that it cancelled one; on an
     * already-fired one it reports that there was nothing to cancel. Both
     * halves matter - a stop that always returns 1 is indistinguishable from
     * a working one until a caller relies on the answer. */
    if (callout_stop(&test_never) != 1) {
        kprintf_c(0x0C, "callout selftest: stop of a pending callout "
                        "returned 0\n");
        failures++;
    }
    if (callout_stop(&test_a) != 0) {
        kprintf_c(0x0C, "callout selftest: stop of a fired callout "
                        "returned 1\n");
        failures++;
    }

    /* A handler rescheduling its own callout - the periodic-timer idiom, and
     * the case that breaks if callout_process runs handlers while still
     * walking the bucket they re-insert into. */
    callout_reset(&test_periodic, 1, test_periodic_handler, 0);
    start = timer_ticks_now();
    while (timer_ticks_now() - start < 10) {
        bkl_wait_for_interrupt();
    }
    if (periodic_count != 3) {
        kprintf_c(0x0C, "callout selftest: periodic ran %d times, "
                        "expected 3\n", periodic_count);
        failures++;
    }

    if (failures == 0) {
        kprintf("callout: selftest passed\n");
    } else {
        kprintf_c(0x0C, "callout: selftest FAILED (%d)\n", failures);
    }
    return failures;
}

/* The taskqueue used to be here: taskqueue_fast, and a taskqueue_drain that
 * returned immediately because a task ran inline at its enqueue site and so
 * had always finished by the time anyone could ask.
 *
 * That comment ended "written down here so that whoever makes taskqueues
 * asynchronous knows this is one of the places that has to change with it".
 * They are asynchronous now, and this is that change: both moved to
 * kernel/bsd/kern_taskqueue.c, where drain is a real wait on a real condvar.
 *
 * What stayed here is the callout wheel, which the deadline form of a task
 * still delegates to - a timer plus a thread context is what
 * taskqueue_enqueue_timeout is, and the timer half is this file's. */

/* --- module unload: is any armed callout inside these bytes? --------------
 *
 * Asked by kldload.c before it unmaps a module image; see irq.c's twin for
 * why the argument is checked as well as the function.
 *
 * Three distinct pointers can point into a module here and all three are
 * fatal in a different way. c_func is called from the timer; c_arg is read by
 * it; and the struct callout ITSELF is usually a field of the driver's softc
 * or a static in the module, so the wheel's own LIST link would be walking
 * through unmapped memory on the next tick - which faults inside the timer
 * interrupt, before any of the callout's own code has run. That last one is
 * the reason this counts the callout's address too.
 *
 * Only ARMED callouts are on the wheel, which is exactly the set that
 * matters: a callout the module initialised and never reset, or one it
 * cancelled on the way out, is not linked anywhere and cannot be reached. */
int callout_count_in_range(uint64_t base, uint64_t size) {
    int i;
    int n = 0;

    if (size == 0) {
        return 0;
    }
    for (i = 0; i < CALLOUT_BUCKETS; i++) {
        struct callout *c;

        LIST_FOREACH(c, &wheel[i], c_links.le) {
            uint64_t self = (uint64_t)c;
            uint64_t fn   = (uint64_t)c->c_func;
            uint64_t arg  = (uint64_t)c->c_arg;

            if ((self >= base && self < base + size) ||
                (fn   >= base && fn   < base + size) ||
                (arg != 0 && arg >= base && arg < base + size)) {
                n++;
            }
        }
    }
    return n;
}
