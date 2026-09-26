#ifndef GENESIS_NET_COMPAT_SYS_COUNTER_H
#define GENESIS_NET_COMPAT_SYS_COUNTER_H

/* counter(9) - PER-CPU statistics counters.
 *
 * The point of the real thing is that incrementing a shared statistic from
 * every CPU at packet rate is a cache-line ping-pong, so each CPU gets its own
 * u64 and reads sum them.
 *
 * This was ONE u64 for a while, with a comment saying the per-CPU array was
 * removed because there was one CPU. Two things made that wrong:
 *
 *   - there are two CPUs now (Part 10), so two increments really can race and
 *     lose a count;
 *   - and, decisively, upstream code reaches INTO the array. netinet/ip_id.c
 *     does `CPU_FOREACH(i) arc4rand(zpcpu_get_cpu(V_ip_id, i), 8, 0)` to seed
 *     one value per CPU, and <sys/smp.h>'s zpcpu_get_cpu(base, cpu) is
 *     `&(base)[(cpu)]`. Against a single-u64 allocation that wrote 64 bytes
 *     into an 8-byte block and corrupted the heap - which showed up as a
 *     general protection fault inside kmalloc's free-list walk, four
 *     initialisers later, with nothing pointing at the cause.
 *
 * So a counter is a real array of MAXCPU u64s now. It is indexed by curcpu on
 * the increment path and summed on the read path, which is what upstream
 * does; what is still missing is the per-CPU-section padding that stops two
 * CPUs' counters sharing a cache line. That is a performance property, not a
 * correctness one, and it is named here rather than silently absent.
 */

#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/smp.h>

#include "kheap.h"

typedef uint64_t *counter_u64_t;

/* One slot per CPU this build can have. MAXCPU rather than the number
 * actually online, because zpcpu_get_cpu(c, i) is valid for any i up to
 * mp_maxid and mp_maxid is the compile-time maximum here. */
#define GENESIS_COUNTER_SLOTS   MAXCPU

static __inline counter_u64_t
counter_u64_alloc(int flags)
{
    uint64_t *c = (uint64_t *)kmalloc(sizeof(uint64_t) * GENESIS_COUNTER_SLOTS);
    int i;

    (void)flags;
    if (c != 0) {
        for (i = 0; i < GENESIS_COUNTER_SLOTS; i++) {
            c[i] = 0;
        }
    }
    return (c);
}

static __inline void
counter_u64_free(counter_u64_t c)
{
    kfree(c);
}

static __inline void
counter_u64_add(counter_u64_t c, int64_t v)
{
    if (c != 0) {
        /* This CPU's own slot: no atomic, which is the entire point. Reading
         * curcpu twice would be a bug if this were preemptible; it is not. */
        c[curcpu] += (uint64_t)v;
    }
}

/* Sum every CPU's slot. Not atomic against a concurrent increment, and
 * upstream's is not either - a statistic read while traffic flows is a
 * snapshot, not a transaction. */
static __inline uint64_t
counter_u64_fetch(counter_u64_t c)
{
    uint64_t total = 0;
    int i;

    if (c == 0) {
        return (0);
    }
    for (i = 0; i < GENESIS_COUNTER_SLOTS; i++) {
        total += c[i];
    }
    return (total);
}

static __inline void
counter_u64_zero(counter_u64_t c)
{
    int i;

    if (c != 0) {
        for (i = 0; i < GENESIS_COUNTER_SLOTS; i++) {
            c[i] = 0;
        }
    }
}

/* EARLY_COUNTER is upstream's placeholder for a counter allocated before the
 * counter zone exists - UMA points its own statistics at it during bootstrap
 * and swaps in a real one later. A single shared dummy is correct here for
 * the same reason: nothing reads the value during that window. */
extern uint64_t genesis_early_counter[GENESIS_COUNTER_SLOTS];
#define EARLY_COUNTER  (genesis_early_counter)

/* --- counter ARRAYS ------------------------------------------------------
 *
 * A protocol's whole statistics block is one of these: <net/vnet.h>'s
 * VNET_PCPUSTAT declares `counter_u64_t name[sizeof(struct ipstat) /
 * sizeof(uint64_t)]` and indexes it by field offset, so ipstat, icmpstat,
 * udpstat and tcpstat are all arrays of counters rather than a struct of
 * plain integers.
 *
 * Copied character for character from upstream's <sys/counter.h>: these are
 * pure loops over counter_u64_alloc/free, and the per-CPU part they exist to
 * hide is exactly what this file's counter_u64_t already stands in for. */
#define	COUNTER_ARRAY_ALLOC(a, n, wait)	do {			\
	for (int _i = 0; _i < (n); _i++)			\
		(a)[_i] = counter_u64_alloc(wait);		\
} while (0)

#define	COUNTER_ARRAY_FREE(a, n)	do {			\
	for (int _i = 0; _i < (n); _i++)			\
		counter_u64_free((a)[_i]);			\
} while (0)

#define	COUNTER_ARRAY_COPY(a, dstp, n)	do {			\
	for (int _i = 0; _i < (n); _i++)			\
		((uint64_t *)(dstp))[_i] = counter_u64_fetch((a)[_i]);\
} while (0)

#define	COUNTER_ARRAY_ZERO(a, n)	do {			\
	for (int _i = 0; _i < (n); _i++)			\
		counter_u64_zero((a)[_i]);			\
} while (0)

/* --- counter_rate --------------------------------------------------------
 *
 * A counter with a per-second cap on how often the caller may act. ICMP uses
 * one to limit error replies, which is a security property and not a
 * cosmetic one: without it a single spoofed packet stream turns this machine
 * into an amplifier pointed at whoever the source address names.
 *
 * Declared with upstream's signatures. The implementation IS vendored -
 * kernel/bsd/vendor/counter_rate.inc is kern/subr_counter.c lines 118-224
 * verbatim, which is the whole rate-checking section of that file. The rest
 * of subr_counter.c is the per-CPU zone machinery this header stands in for
 * and was left behind. */
struct counter_rate;   /* opaque, exactly as upstream - the definition is
                        * inside kernel/bsd/vendor/counter_rate.inc */

struct counter_rate *counter_rate_alloc(int flags, int period);
void counter_rate_free(struct counter_rate *);
int64_t counter_ratecheck(struct counter_rate *, int64_t);
uint64_t counter_rate_get(struct counter_rate *);

#endif /* GENESIS_NET_COMPAT_SYS_COUNTER_H */
