#ifndef LINUX_ATOMIC_H
#define LINUX_ATOMIC_H

#include "linux/types.h"

/* <linux/atomic.h>.
 *
 * Real atomics via gcc's __atomic builtins, not a lock-and-increment. On a
 * kernel with a second CPU (kernel/arch/smp.c) the difference is the whole
 * point: a driver's refcount decremented non-atomically from two CPUs loses
 * decrements and frees an object that is still referenced, which presents
 * arbitrarily far from here.
 *
 * The memory ordering is __ATOMIC_SEQ_CST throughout - the strongest, and
 * what Linux's atomic_t operations guarantee for the value itself. The
 * _relaxed variants Linux also offers are deliberately not provided rather
 * than being defined as the strong ones under a name that promises less.
 */

typedef struct { volatile int counter; } atomic_t;
typedef struct { volatile long counter; } atomic64_t;

#define ATOMIC_INIT(i) { (i) }

static inline int atomic_read(const atomic_t *v) {
    return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST);
}
static inline void atomic_set(atomic_t *v, int i) {
    __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST);
}
static inline void atomic_add(int i, atomic_t *v) {
    __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST);
}
static inline void atomic_sub(int i, atomic_t *v) {
    __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST);
}
static inline void atomic_inc(atomic_t *v) { atomic_add(1, v); }
static inline void atomic_dec(atomic_t *v) { atomic_sub(1, v); }

static inline int atomic_add_return(int i, atomic_t *v) {
    return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}
static inline int atomic_sub_return(int i, atomic_t *v) {
    return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}
static inline int atomic_inc_return(atomic_t *v) {
    return atomic_add_return(1, v);
}
static inline int atomic_dec_return(atomic_t *v) {
    return atomic_sub_return(1, v);
}

/* Returns true when the result is zero. This is the refcount-drop idiom -
 * `if (atomic_dec_and_test(&x->ref)) free(x);` - and it has to be one
 * operation: decrementing and then reading back lets two CPUs both see zero
 * and both free. */
static inline int atomic_dec_and_test(atomic_t *v) {
    return atomic_sub_return(1, v) == 0;
}
static inline int atomic_inc_and_test(atomic_t *v) {
    return atomic_add_return(1, v) == 0;
}

static inline int atomic_cmpxchg(atomic_t *v, int old, int nw) {
    __atomic_compare_exchange_n(&v->counter, &old, nw, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}
static inline int atomic_xchg(atomic_t *v, int nw) {
    return __atomic_exchange_n(&v->counter, nw, __ATOMIC_SEQ_CST);
}

#endif
