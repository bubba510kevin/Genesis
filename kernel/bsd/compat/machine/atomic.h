#ifndef GENESIS_NET_COMPAT_MACHINE_ATOMIC_H
#define GENESIS_NET_COMPAT_MACHINE_ATOMIC_H

/* Genesis shim, not vendored. FreeBSD's amd64 <machine/atomic.h> is hand-
 * written asm with a separate UP variant selected at build time; these are
 * GCC's __atomic builtins instead, which emit the identical `lock`-prefixed
 * instructions on x86-64 and cost nothing to get right.
 *
 * These are REAL atomics even though Genesis is uniprocessor today. That is
 * deliberate and is not the same call as wdm.c's honest-uniprocessor spinlock
 * stub: an mbuf's embedded reference count is decremented from the same
 * routine that may be re-entered from an interrupt handler, so the read-
 * modify-write has to be indivisible against an IRQ landing mid-sequence even
 * with one CPU. Part 10 of the plan adds a second CPU and changes nothing
 * here. */

#include <sys/types.h>

static __inline void
atomic_add_int(volatile u_int *p, u_int v)
{
    __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

static __inline void
atomic_subtract_int(volatile u_int *p, u_int v)
{
    __atomic_fetch_sub(p, v, __ATOMIC_SEQ_CST);
}

/* Returns the value BEFORE the add - upstream's contract, and the reason
 * mb_free_ext() can use it to decide "was I the last reference". */
static __inline u_int
atomic_fetchadd_int(volatile u_int *p, u_int v)
{
    return (__atomic_fetch_add(p, v, __ATOMIC_SEQ_CST));
}

static __inline int
atomic_cmpset_int(volatile u_int *p, u_int expect, u_int src)
{
    return (__atomic_compare_exchange_n(p, &expect, src, 0,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0);
}

static __inline int
atomic_fcmpset_int(volatile u_int *p, u_int *expect, u_int src)
{
    return (__atomic_compare_exchange_n(p, expect, src, 0,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0);
}

static __inline u_int
atomic_load_int(volatile u_int *p)
{
    return (__atomic_load_n(p, __ATOMIC_SEQ_CST));
}

static __inline void
atomic_store_int(volatile u_int *p, u_int v)
{
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

#define atomic_load_acq_int(p)      atomic_load_int(p)
#define atomic_store_rel_int(p, v)  atomic_store_int(p, v)
#define atomic_add_acq_int(p, v)    atomic_add_int(p, v)
#define atomic_add_rel_int(p, v)    atomic_add_int(p, v)
#define atomic_subtract_rel_int(p, v) atomic_subtract_int(p, v)
#define atomic_cmpset_acq_int(p, e, s)  atomic_cmpset_int(p, e, s)
#define atomic_cmpset_rel_int(p, e, s)  atomic_cmpset_int(p, e, s)
#define atomic_fcmpset_acq_int(p, e, s) atomic_fcmpset_int(p, e, s)
#define atomic_fcmpset_rel_int(p, e, s) atomic_fcmpset_int(p, e, s)

#define atomic_thread_fence_acq()   __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define atomic_thread_fence_rel()   __atomic_thread_fence(__ATOMIC_RELEASE)
#define atomic_thread_fence_seq_cst() __atomic_thread_fence(__ATOMIC_SEQ_CST)


/* --- 64-bit and pointer-width forms (added for Part 12's UMA port) -------
 *
 * The mbuf port needed only the int forms. uma_core.c and uma_int.h use the
 * long/ptr/64 forms for byte counts and for the per-CPU cache bucket
 * pointers, where 32 bits would silently truncate on a machine with more
 * than 4GB under management.
 *
 * All of these are single instructions with a lock prefix - x86-64 gives
 * atomicity for free on aligned accesses of these widths, and the lock
 * prefix is what makes the read-modify-write ones indivisible. */

static __inline void
atomic_add_long(volatile u_long *p, u_long v)
{
	__asm __volatile("lock; addq %1,%0" : "+m"(*p) : "r"(v) : "cc", "memory");
}

static __inline void
atomic_subtract_long(volatile u_long *p, u_long v)
{
	__asm __volatile("lock; subq %1,%0" : "+m"(*p) : "r"(v) : "cc", "memory");
}

static __inline u_long
atomic_fetchadd_long(volatile u_long *p, u_long v)
{
	__asm __volatile("lock; xaddq %0,%1" : "+r"(v), "+m"(*p) : : "cc", "memory");
	return (v);
}

static __inline int
atomic_cmpset_long(volatile u_long *dst, u_long expect, u_long src)
{
	u_char res;

	__asm __volatile("lock; cmpxchgq %3,%1; sete %0"
	    : "=q"(res), "+m"(*dst), "+a"(expect) : "r"(src) : "memory", "cc");
	return (res);
}

static __inline int
atomic_fcmpset_long(volatile u_long *dst, u_long *expect, u_long src)
{
	u_char res;

	__asm __volatile("lock; cmpxchgq %3,%1; sete %0"
	    : "=q"(res), "+m"(*dst), "+a"(*expect) : "r"(src) : "memory", "cc");
	return (res);
}

static __inline u_long
atomic_load_long(volatile u_long *p)
{
	return (*p);
}

static __inline void
atomic_store_long(volatile u_long *p, u_long v)
{
	*p = v;
}

static __inline u_long
atomic_swap_long(volatile u_long *p, u_long v)
{
	__asm __volatile("xchgq %1,%0" : "+r"(v), "+m"(*p) : : "memory");
	return (v);
}

#define atomic_add_ptr(p, v)          atomic_add_long((volatile u_long *)(p), (u_long)(v))
#define atomic_subtract_ptr(p, v)     atomic_subtract_long((volatile u_long *)(p), (u_long)(v))
#define atomic_cmpset_ptr(p, e, s)    atomic_cmpset_long((volatile u_long *)(p), (u_long)(e), (u_long)(s))
#define atomic_fcmpset_ptr(p, e, s)   atomic_fcmpset_long((volatile u_long *)(p), (u_long *)(e), (u_long)(s))
#define atomic_load_ptr(p)            ((__typeof(*(p)))atomic_load_long((volatile u_long *)(p)))
#define atomic_store_ptr(p, v)        atomic_store_long((volatile u_long *)(p), (u_long)(v))
#define atomic_swap_ptr(p, v)         atomic_swap_long((volatile u_long *)(p), (u_long)(v))

#define atomic_add_64(p, v)           atomic_add_long((volatile u_long *)(p), (u_long)(v))
#define atomic_subtract_64(p, v)      atomic_subtract_long((volatile u_long *)(p), (u_long)(v))
#define atomic_load_64(p)             atomic_load_long((volatile u_long *)(p))
#define atomic_store_64(p, v)         atomic_store_long((volatile u_long *)(p), (u_long)(v))

#define atomic_thread_fence_acq_rel() __asm __volatile(" " : : : "memory")

#define atomic_load_acq_long(p)       (*(volatile u_long *)(p))
#define atomic_store_rel_long(p, v)   (*(volatile u_long *)(p) = (v))
#define atomic_load_acq_ptr(p)        (*(void * volatile *)(p))
#define atomic_store_rel_ptr(p, v)    (*(void * volatile *)(p) = (void *)(v))


/* 8- and 16-bit forms. UMA keeps some per-slab counters at these widths. */
static __inline uint8_t
atomic_load_8(volatile uint8_t *p)
{
	return (*p);
}

static __inline void
atomic_store_8(volatile uint8_t *p, uint8_t v)
{
	*p = v;
}

static __inline uint16_t
atomic_load_16(volatile uint16_t *p)
{
	return (*p);
}

static __inline void
atomic_store_16(volatile uint16_t *p, uint16_t v)
{
	*p = v;
}

static __inline uint32_t
atomic_load_32(volatile uint32_t *p)
{
	return (*p);
}

static __inline void
atomic_store_32(volatile uint32_t *p, uint32_t v)
{
	*p = v;
}

static __inline void
atomic_add_32(volatile uint32_t *p, uint32_t v)
{
	__asm __volatile("lock; addl %1,%0" : "+m"(*p) : "r"(v) : "cc", "memory");
}

static __inline void
atomic_subtract_32(volatile uint32_t *p, uint32_t v)
{
	__asm __volatile("lock; subl %1,%0" : "+m"(*p) : "r"(v) : "cc", "memory");
}

static __inline int
atomic_fcmpset_32(volatile uint32_t *dst, uint32_t *expect, uint32_t src)
{
	u_char res;

	__asm __volatile("lock; cmpxchgl %3,%1; sete %0"
	    : "=q"(res), "+m"(*dst), "+a"(*expect) : "r"(src) : "memory", "cc");
	return (res);
}

static __inline int
atomic_fcmpset_64(volatile uint64_t *dst, uint64_t *expect, uint64_t src)
{
	u_char res;

	__asm __volatile("lock; cmpxchgq %3,%1; sete %0"
	    : "=q"(res), "+m"(*dst), "+a"(*expect) : "r"(src) : "memory", "cc");
	return (res);
}

static __inline int
atomic_cmpset_64(volatile uint64_t *dst, uint64_t expect, uint64_t src)
{
	u_char res;

	__asm __volatile("lock; cmpxchgq %3,%1; sete %0"
	    : "=q"(res), "+m"(*dst), "+a"(expect) : "r"(src) : "memory", "cc");
	return (res);
}

static __inline uint64_t
atomic_fetchadd_64(volatile uint64_t *p, uint64_t v)
{
	__asm __volatile("lock; xaddq %0,%1" : "+r"(v), "+m"(*p) : : "cc", "memory");
	return (v);
}

static __inline uint32_t
atomic_fetchadd_32(volatile uint32_t *p, uint32_t v)
{
	__asm __volatile("lock; xaddl %0,%1" : "+r"(v), "+m"(*p) : : "cc", "memory");
	return (v);
}

/* The 32-bit acquire/release forms <sys/buf_ring.h> uses. Added when the
 * real net/if_var.h landed, which includes buf_ring for the multiqueue
 * transmit path.
 *
 * acq and rel are REAL barriers rather than aliases for the plain form: a
 * lock-free ring's whole correctness is that the consumer sees the producer's
 * data write before it sees the index update, and vice versa. On x86 the
 * hardware gives that ordering for ordinary loads and stores, so what is
 * needed is a COMPILER barrier - which is what __ATOMIC_ACQUIRE and
 * __ATOMIC_RELEASE emit here, and it is not nothing. */
#ifndef atomic_load_acq_32
static __inline uint32 atomic_load_acq_32(volatile uint32 *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static __inline void atomic_store_rel_32(volatile uint32 *p, uint32 v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static __inline int atomic_cmpset_32(volatile uint32 *p, uint32 old,
                                     uint32 nw) {
    return __atomic_compare_exchange_n(p, &old, nw, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static __inline int atomic_cmpset_acq_32(volatile uint32 *p, uint32 o,
                                         uint32 n) {
    return atomic_cmpset_32(p, o, n);
}
static __inline int atomic_cmpset_rel_32(volatile uint32 *p, uint32 o,
                                         uint32 n) {
    return atomic_cmpset_32(p, o, n);
}
/* atomic_load_32 / atomic_store_32 already exist earlier in this file. */
#endif

/* critical_enter/exit - hold off preemption on THIS CPU without disabling
 * interrupts. Genesis's scheduler only ever switches at a preemption point
 * (see sched_tick / return_to_user), so a counter that those points check
 * would be the faithful implementation; there is no such counter yet, and
 * every current caller is either a per-CPU cache fast path or a lock-free
 * ring that is not otherwise reached.
 *
 * Interrupts are NOT disabled here. That is a real difference from what a
 * caller may assume - it protects against preemption, not against an
 * interrupt handler on the same CPU touching the same per-CPU data. Written
 * down because the name suggests more than it does. */
#ifndef critical_enter
static __inline void critical_enter(void) { }
static __inline void critical_exit(void) { }
#endif

#endif /* GENESIS_NET_COMPAT_MACHINE_ATOMIC_H */
