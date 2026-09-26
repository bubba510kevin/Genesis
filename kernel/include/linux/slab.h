#ifndef LINUX_SLAB_H
#define LINUX_SLAB_H

#include "linux/types.h"

/* <linux/slab.h> - kmalloc and friends.
 *
 * Backed by Genesis's kheap (kernel/mm/kheap.c), which has real locking since
 * Part 11 of the item 11 pass, so these are safe from more than one CPU.
 *
 * --- about the gfp_t flags ------------------------------------------------
 * GFP_KERNEL means "may sleep to get this memory" and GFP_ATOMIC means "must
 * not sleep, I am in an interrupt". Genesis's heap never sleeps: it either
 * has the memory or returns NULL, immediately, in both cases. So the flags
 * are accepted and only __GFP_ZERO is acted on.
 *
 * That is a real simplification and it is worth being precise about which
 * direction it errs in. A driver asking for GFP_KERNEL gets ATOMIC
 * behaviour - it may get NULL where Linux would have blocked and succeeded.
 * Driver code checks kmalloc for NULL anyway (Linux does not guarantee
 * success either), so this is safe in the direction that matters; it is not
 * safe to conclude the opposite, that a GFP_ATOMIC allocation here is somehow
 * more reliable than it looks.
 */

typedef unsigned int gfp_t;

#define __GFP_ZERO   0x8000u
#define GFP_KERNEL   0x0001u
#define GFP_ATOMIC   0x0002u
#define GFP_NOWAIT   0x0004u
#define GFP_DMA      0x0008u
#define GFP_DMA32    0x0010u
#define GFP_NOIO     0x0020u
#define GFP_NOFS     0x0040u
#define GFP_USER     0x0080u

/* --- why these are macros over linux_-prefixed symbols -------------------
 *
 * Genesis ALREADY has a kmalloc, a kcalloc, a krealloc and a kfree, declared
 * in kernel/include/kheap.h with different signatures - kmalloc takes no gfp
 * flags and kfree takes a non-const pointer. Two global functions cannot
 * share a name, so the implementations carry linux_ prefixes and the Linux
 * spelling is a macro on top.
 *
 * Driver source is unaffected: it writes kmalloc(n, GFP_KERNEL) and never
 * includes kheap.h. The macro form also means a one-argument kmalloc(n) -
 * Genesis's own spelling - is a compile error rather than silently binding
 * to the wrong allocator, which is the failure this arrangement is chosen to
 * make impossible.
 */
void *linux_kmalloc(unsigned long size, gfp_t flags);
void *linux_kzalloc(unsigned long size, gfp_t flags);
void *linux_kcalloc(unsigned long n, unsigned long size, gfp_t flags);
void *linux_krealloc(void *p, unsigned long size, gfp_t flags);
void  linux_kfree(const void *p);
void *linux_kmalloc_array(unsigned long n, unsigned long size, gfp_t flags);

#define kmalloc(sz, fl)          linux_kmalloc((sz), (fl))
#define kzalloc(sz, fl)          linux_kzalloc((sz), (fl))
#define kcalloc(n, sz, fl)       linux_kcalloc((n), (sz), (fl))
#define krealloc(p, sz, fl)      linux_krealloc((p), (sz), (fl))
#define kfree(p)                 linux_kfree(p)
#define kmalloc_array(n, sz, fl) linux_kmalloc_array((n), (sz), (fl))

/* vmalloc/vfree exist in driver source and mean "virtually contiguous, maybe
 * not physically". Genesis's heap is both, so these are kmalloc. Correct, and
 * strictly stronger than what the caller asked for - which is the safe
 * direction, since nothing can depend on memory NOT being contiguous. */
#define vmalloc(sz)  kmalloc((sz), GFP_KERNEL)
#define vzalloc(sz)  kzalloc((sz), GFP_KERNEL)
#define vfree(p)     kfree(p)

#endif
