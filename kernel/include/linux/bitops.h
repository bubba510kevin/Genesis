#ifndef LINUX_BITOPS_H
#define LINUX_BITOPS_H

#include "linux/types.h"

/* <linux/bitops.h>. Bit arrays as Linux driver source uses them.
 *
 * The word size is 64 here (BITS_PER_LONG), which matters: a driver that
 * declares its bitmap with DECLARE_BITMAP gets the right number of words
 * only if this agrees with the compiler's `long`. It does - this kernel is
 * LP64 - and saying so here is what makes it a fact rather than an accident.
 */

#define BITS_PER_LONG  64
#define BIT(n)         (1UL << (n))
#define BIT_ULL(n)     (1ULL << (n))
#define BIT_MASK(nr)   (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)   ((nr) / BITS_PER_LONG)
#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]

#define GENMASK(h, l) \
    (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))

/* Atomic, like Linux's - two CPUs setting different bits in the same word
 * must not lose either. The non-atomic __set_bit forms exist upstream for
 * where the caller already holds a lock; they are deliberately not provided
 * here rather than aliased onto the atomic ones under a name that says
 * otherwise. */
static inline void set_bit(unsigned int nr, volatile unsigned long *addr) {
    __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST);
}
static inline void clear_bit(unsigned int nr, volatile unsigned long *addr) {
    __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST);
}
static inline int test_bit(unsigned int nr, const volatile unsigned long *addr) {
    return (addr[BIT_WORD(nr)] >> (nr % BITS_PER_LONG)) & 1UL;
}
static inline int test_and_set_bit(unsigned int nr,
                                   volatile unsigned long *addr) {
    unsigned long old = __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr),
                                          __ATOMIC_SEQ_CST);
    return (old & BIT_MASK(nr)) != 0;
}
static inline int test_and_clear_bit(unsigned int nr,
                                     volatile unsigned long *addr) {
    unsigned long old = __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr),
                                           __ATOMIC_SEQ_CST);
    return (old & BIT_MASK(nr)) != 0;
}

/* ffs returns a ONE-based index and 0 for "no bits set"; __ffs returns a
 * ZERO-based index and is undefined for zero. Both spellings exist in driver
 * source and confusing them is an off-by-one in a register field. */
static inline int ffs(int x)  { return x ? __builtin_ffs(x) : 0; }
static inline unsigned long __ffs(unsigned long x) {
    return (unsigned long)__builtin_ctzl(x);
}
static inline int fls(int x)  { return x ? 32 - __builtin_clz((unsigned)x) : 0; }
static inline int hweight32(u32 x) { return __builtin_popcount(x); }
static inline int hweight64(u64 x) { return __builtin_popcountll(x); }

#endif
