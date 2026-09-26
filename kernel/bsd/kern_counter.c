/* counter(9)'s rate limiter, VENDORED.
 *
 * kernel/bsd/vendor/counter_rate.inc is lines 118-224 of
 * vendsrc/sys/kern/subr_counter.c, unmodified: the struct counter_rate
 * definition and counter_rate_alloc / counter_rate_free / counter_rate_get /
 * counter_ratecheck.
 *
 * The rest of subr_counter.c was left behind on purpose. It is the per-CPU
 * counter zone - counter_u64_alloc walking UMA's per-CPU allocator, the
 * sysctl handlers that sum a counter across CPUs - and <sys/counter.h> in
 * kernel/bsd/compat already stands in for that with a plain uint64 per
 * counter. Taking the rate limiter and leaving the allocator is the same
 * seam the whole compat tree draws.
 *
 * Why the rate limiter is worth vendoring rather than approximating: ICMP
 * uses it to cap error replies (icmplim, 200/s by default), and that is a
 * security control. A machine that answers every malformed packet with an
 * ICMP error is an amplifier aimed at whatever source address the packets
 * claim. The exact algorithm - including the cr_lock dance that makes the
 * rollover safe when two CPUs hit it at once, and the -1 return that says
 * "over limit, and you have already been told" - is the part that is easy to
 * get subtly wrong by hand.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/counter.h>
#include <sys/sysctl.h>
#include <machine/atomic.h>

/* M_COUNTER_RATE is a malloc type upstream declares with
 * MALLOC_DEFINE(M_COUNTER_RATE, ...) in subr_counter.c, outside the range
 * vendored here. sys/malloc.h in kernel/bsd/compat maps every malloc type
 * onto kmalloc, so this only has to exist and be distinct. */
static struct malloc_type genesis_m_counter_rate[1] = { { "counter_rate" } };
#define M_COUNTER_RATE  genesis_m_counter_rate

#include "vendor/counter_rate.inc"
