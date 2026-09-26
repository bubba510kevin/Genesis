/* Kernel library routines the network stack calls, VENDORED.
 *
 *   libkern/jenkins_hash.c        - Bob Jenkins's lookup3, whole. The hash
 *                                   ip_reass.c buckets fragments with and
 *                                   ether_gen_addr uses to derive a MAC.
 *   libkern/arc4random_uniform.c  - a uniform random number below a bound,
 *                                   whole. The modulo-bias rejection loop is
 *                                   the entire point of the file and is the
 *                                   thing a hand-written version gets wrong.
 *   kern/subr_hash.c              - hashinit/hashdestroy/phashinit, whole.
 *                                   Every protocol hash table in the stack is
 *                                   sized and allocated by these.
 *
 * All three are self-contained: no locks, no per-CPU state, no VM. They were
 * left undefined until the IP layer needed them, which is why they arrive
 * together rather than with the subsystem that first wanted one.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/hash.h>
#include <sys/libkern.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "kheap.h"
#include "kprintf.h"

/* --- randomness ---------------------------------------------------------
 *
 * arc4random() is the entropy source everything else here is built on, and
 * this machine does not have one worth the name: no RDRAND check, no
 * interrupt-timing pool, no /dev/random. What follows is a
 * counter-and-mix generator seeded from the TSC.
 *
 * That is NOT cryptographic randomness and it must not be used as if it
 * were. It is used for exactly two things in this tree, both of which need
 * unpredictability rather than secrecy:
 *
 *   - IP identifier selection (ip_id.c), where a predictable sequence lets a
 *     remote observer count this host's outbound packets and mount an idle
 *     scan.
 *   - hash table perturbation, where it only has to differ between boots.
 *
 * A TCP initial sequence number would be the case where this is not good
 * enough, and tcp_subr.c derives one through its own SipHash rather than
 * calling arc4random directly - so that hazard is not reached today. If
 * anything ever calls arc4random() for a key, this comment is the reason it
 * is wrong.
 */
static uint64_t arc4_state;

static uint64_t arc4_next(void) {
    uint64_t x;

    if (arc4_state == 0) {
        uint32 lo = 0, hi = 0;

        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        arc4_state = ((uint64_t)hi << 32) | lo;
        arc4_state |= 1;    /* splitmix64 degenerates from a zero state */
    }
    /* splitmix64: one multiply-xor-shift round. Chosen over a linear
     * congruential generator because the low bits of an LCG are famously
     * non-random, and every caller here takes a modulus. */
    arc4_state += 0x9E3779B97F4A7C15ULL;
    x = arc4_state;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return (x ^ (x >> 31));
}

uint32_t arc4random(void) {
    return ((uint32_t)(arc4_next() >> 32));
}

void arc4rand(void *ptr, u_int len, int reseed) {
    uint8_t *p = (uint8_t *)ptr;
    u_int i;

    (void)reseed;
    for (i = 0; i < len; i++) {
        p[i] = (uint8_t)(arc4_next() >> 24);
    }
}

#include "vendor/arc4random_uniform.inc"
#include "vendor/jenkins_hash.inc"

/* subr_hash.c's hashinit_flags() calls malloc() with M_WAITOK and asserts
 * the count is a power of two. Both hold here: sys/malloc.h maps malloc onto
 * kmalloc, and every caller in the network stack passes a power of two. */
#include "vendor/subr_hash.inc"

/* --- address printing and parsing, vendored ------------------------------
 *
 * libkern/inet_ntop.c, inet_ntoa.c, inet_aton.c and inet_pton.c, whole.
 *
 * net/if_llatbl.c logs an unresolved address with inet_ntop(), and
 * netinet/in.c reports one with inet_ntoa_r(). Small files individually,
 * worth vendoring together because getting the DOTTED-QUAD boundary cases
 * right by hand (a partial quad, a leading zero) is exactly the kind of
 * thing that is silently wrong.
 *
 * inet_aton.c is NOT among them: it parses with strtoul(), and there is no
 * strtoul here. Nothing in the vendored tree calls it - it is a userland
 * entry point that happens to live in libkern - so the honest move is to
 * leave it out rather than write a strtoul for it. */
#include "vendor/inet_ntop.inc"
#include "vendor/inet_ntoa.inc"
#include "vendor/inet_pton.inc"

/* libkern/qsort.c, whole. netinet/in_mcast.c sorts a source filter list with
 * it. Vendored rather than hand-written for the usual reason: this is the
 * three-way-partition quicksort with the median-of-three pivot and the
 * insertion-sort cutoff, and a naive version is quadratic on the sorted input
 * a filter list usually is. */
#include "vendor/qsort.inc"

/* --- random() ------------------------------------------------------------
 *
 * The old BSD pseudo-random generator, returning a non-negative long below
 * 2^31. IGMP uses it to jitter report timers, which is the whole reason it
 * exists in a network stack: if every host on a segment answered a query at
 * the same instant, the router would see a burst instead of a spread.
 *
 * Backed by the same generator as arc4random above, and the same warning
 * applies with more force: this is NOT random in any sense a security
 * property could rest on. For timer jitter that is fine - it only has to
 * differ between hosts, and the TSC seed does that. */
u_long random(void) {
    return ((u_long)(arc4_next() >> 33));
}
