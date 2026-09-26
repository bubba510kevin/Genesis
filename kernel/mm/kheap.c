#include "kheap.h"
#include "klock.h"
#include "paging.h"
#include "pmm.h"
#include "typesk.h"
#include "vmalloc.h"

/* ===========================================================================
 * Design
 * ---------------------------------------------------------------------------
 * The PMM already hands out page frames and the VMM already maps them, so what
 * is missing is sub-page granularity: a place to put a 12-byte process struct
 * without burning a whole frame on it. That is this file.
 *
 * Layout of a block. Every byte between heap_start and heap_end belongs to
 * exactly one block, used or free, with no gaps - that "blocks tile the heap"
 * invariant is what makes the boundary tags usable for navigation:
 *
 *     +--------+-----------------------------+--------+
 *     | header |          payload            | footer |
 *     +--------+-----------------------------+--------+
 *      KH_HDR         size - KH_OVERHEAD        KH_FTR
 *     ^                ^
 *     block            what kmalloc() returns
 *
 * Header and footer both carry {magic, size}, where size is the *whole* block
 * including both tags. The duplicated size in the footer is the trick that
 * makes backward coalescing O(1): given a block, the bytes immediately below
 * it are the previous block's footer, and its size field says how far back
 * that block starts. Without it, merging with the previous block would mean
 * rescanning the heap from the beginning on every free.
 *
 * "Used" vs "free" is encoded in the magic value rather than a separate flag
 * field, so a corrupted or double-freed pointer fails the magic check for free
 * rather than needing a distinct validity check.
 *
 * Free blocks are additionally threaded onto a doubly-linked free list whose
 * next/prev pointers live *inside the payload area* of the free block itself.
 * A free block's payload is dead space by definition, so the list costs zero
 * extra memory. That is also why the minimum block size is three alignment
 * units: two of tags plus one for the two list pointers.
 *
 * Allocation policy is first-fit over the free list. Not the best policy going
 * - best-fit or segregated size classes fragment less - but it is the one
 * whose failure modes are easiest to reason about, and the roadmap's eventual
 * slab allocator is what should actually serve the hot fixed-size paths
 * (task_struct, inodes) later. This is the general-purpose fallback under it.
 *
 * Invariant maintained everywhere: no two adjacent blocks are both free.
 * Every path that creates a free block either coalesces or can prove its
 * neighbours are in use. kheap_check() verifies this.
 *
 * --- Portability ---------------------------------------------------------
 * Nothing here assumes a 32-bit target. Addresses and sizes are kh_uptr, the
 * tag layout is derived from sizeof(), and alignment is 2 * sizeof(void *) so
 * it becomes 16 bytes on 64-bit the way a hosted malloc's does. There is no
 * endianness dependency (magics are compared as integers, never as bytes), no
 * inline assembly, and no compiler extensions - plain C89-compatible source.
 * Every alignment assumption the code relies on is checked at compile time
 * below, so a target where one stops holding fails the build loudly.
 *
 * Not implemented on purpose:
 *   - The heap never shrinks. Returning frames means unmapping trailing pages,
 *     which is easy, but there is no pressure signal yet to decide when that
 *     is worth it. Add it alongside a real OOM/reclaim path.
 *   - No locking. See the note on kh_commit_page below.
 * ======================================================================== */

/* Alignment of returned pointers. Two pointer-widths is the conventional
 * malloc guarantee: enough for any scalar the target has, including 16-byte
 * SSE/long-double types on x86-64. */
#define KH_ALIGN     ((kh_size)(2u * sizeof(void *)))

/* Two magics rather than a magic plus a flag: a use-after-free or double
 * free then fails the "is this a live block" test directly. Stored and
 * compared as integers, so there is no endianness dependency. */
#define KH_MAGIC_USED   ((kh_size)0xA110C8EDu)
#define KH_MAGIC_FREE   ((kh_size)0xF2EEB10Cu)

typedef struct {
    kh_size magic;
    kh_size size;   /* total block size, header + payload + footer */
} kh_header_t;

typedef struct {
    kh_size magic;  /* mirrors the header */
    kh_size size;   /* mirrors the header */
} kh_footer_t;

/* Lives in the payload of a free block. Never valid for a used block. */
typedef struct kh_node {
    struct kh_node *next;
    struct kh_node *prev;
} kh_node_t;

#define KH_HDR       ((kh_size)sizeof(kh_header_t))
#define KH_FTR       ((kh_size)sizeof(kh_footer_t))
#define KH_OVERHEAD  (KH_HDR + KH_FTR)
#define KH_MIN_BLOCK (KH_OVERHEAD + (kh_size)sizeof(kh_node_t))

/* --- compile-time assertions ------------------------------------------------
 * A failing one of these produces "size of array is negative" at the line in
 * question. Cheaper than discovering the same thing as a page fault in QEMU
 * with no debugger attached. The negative-array-size idiom is used rather
 * than _Static_assert so this stays buildable on a pre-C11 compiler. */
#define KH_ASSERT(name, cond) typedef char kh_assert_##name[(cond) ? 1 : -1]

/* kh_uptr must actually be pointer-sized, or every cast below truncates. If
 * this fires, fix the #if in kheap.h for your target. */
KH_ASSERT(uptr_is_pointer_sized, sizeof(kh_uptr) == sizeof(void *));

/* Payload address is block + KH_HDR, and blocks are KH_ALIGN-aligned, so the
 * header must be a whole number of alignment units or every pointer we hand
 * back is misaligned. */
KH_ASSERT(header_is_aligned, (KH_HDR % KH_ALIGN) == 0);
KH_ASSERT(footer_is_aligned, (KH_FTR % KH_ALIGN) == 0);
KH_ASSERT(min_block_is_aligned, (KH_MIN_BLOCK % KH_ALIGN) == 0);

/* kh_align_up uses a power-of-two mask. */
KH_ASSERT(align_is_power_of_two, (KH_ALIGN & (KH_ALIGN - 1)) == 0);

/* The heap's base must itself be aligned, or no block in it is. */
KH_ASSERT(heap_start_is_aligned, (KHEAP_START % 16u) == 0);

/* Page-granular growth must not break block alignment. */
KH_ASSERT(page_size_is_aligned, (PMM_PAGE_SIZE % 16u) == 0);

/* The VMM API now takes virt_addr_t / phys_addr_t, so there is no longer a
 * narrowing at the kh_commit_page seam and no 4GB ceiling on where the heap
 * may live. Kept as a sanity check that the heap range does not wrap. */
KH_ASSERT(heap_does_not_wrap,
          ((kh_uptr)KHEAP_START + (kh_uptr)KHEAP_MAX_SIZE) > (kh_uptr)KHEAP_START);

/* All of these are explicitly initialised in kheap_init() rather than relying
 * on static zero-init. The flat binary drops .bss on the floor (objcopy -O
 * binary skips NOBITS) and flk.c has to zero it by hand at boot; not depending
 * on that ordering keeps the heap correct even if someone reorders init. */
static kh_uptr    heap_start;
static kh_uptr    heap_end;      /* one past the last committed byte */
static kh_uptr    heap_limit;    /* hard cap, heap_end never exceeds this */
static kh_node_t *free_list;
static kh_size    stat_payload;  /* payload bytes currently handed out */

/* --- tiny local mem helpers -------------------------------------------------
 * Kept static and prefixed so they don't collide when a real string.c with
 * memset/memcpy shows up later; at that point delete these and include it. */
static void kh_memset(void *dst, uint8 value, kh_size len) {
    uint8 *d = (uint8 *)dst;
    while (len--) {
        *d++ = value;
    }
}

static void kh_memcpy(void *dst, const void *src, kh_size len) {
    uint8 *d = (uint8 *)dst;
    const uint8 *s = (const uint8 *)src;
    while (len--) {
        *d++ = *s++;
    }
}

static kh_size kh_align_up(kh_size value, kh_size align) {
    return (value + align - 1u) & ~(align - 1u);
}

/* --- the one platform seam -------------------------------------------------
 * Every call the heap makes into the VMM goes through here. Two reasons to
 * funnel it:
 *   1. Everything the heap knows about the VMM is one call. paging.h is
 *      64-bit now so there is no cast left to isolate, but keeping the seam
 *      means the next interface change is still a one-line edit here.
 *   2. When the kernel gains interrupts-during-allocation or SMP, the lock
 *      goes around the callers of this, and this is the obvious place to
 *      document that. As of now the allocator has NO locking: an IRQ handler
 *      that calls kmalloc() while a kmalloc() is already in progress will
 *      corrupt the free list. Interrupts are enabled in flk() after the heap
 *      is set up, so this becomes real the moment a handler allocates.
 */
static int kh_commit_page(kh_uptr virt) {
    return vmm_alloc_page((virt_addr_t)virt, PAGE_PRESENT | PAGE_RW) != 0;
}

/* --- the lock -------------------------------------------------------------
 * The free list is a global mutable structure with no atomicity anywhere in
 * it: a coalesce unlinks two blocks and relinks one, and an interrupt landing
 * between those steps leaves a list that points into the middle of a block.
 * Nothing in an IRQ handler allocates TODAY, which is precisely why this is
 * worth closing now - the first handler that calls kmalloc will not announce
 * itself, and the symptom is a corrupted heap discovered somewhere unrelated.
 *
 * Disabling interrupts is the right primitive for a uniprocessor kernel with
 * no preemption: it is the only thing that can interleave. Saving and
 * restoring RFLAGS rather than blindly doing cli/sti matters because these
 * calls nest and because kmalloc is legal before flk() enables interrupts at
 * all - an unconditional sti there would turn them on far too early.
 *
 * When SMP arrives this becomes irq-save plus a spinlock. The shape does not
 * change; the body of these two functions does. */
/* Statically initialised rather than via kmtx_init, because kh_lock is called
 * by kheap_init itself - there is no earlier moment to run an initialiser
 * in. The field that matters is `owner`: zeroed .bss would make it 0, which
 * is a VALID APIC id, and the first acquire on the boot CPU would report a
 * spurious recursive-acquire. */
static mtx_t kh_mtx = { 0, 0, "kheap", 0xFFFFFFFFu, 0, 0 };

/* The comment above predicted this: "when SMP arrives this becomes irq-save
 * plus a spinlock, the shape does not change, the body of these two
 * functions does". This is that change, and the shape did not change.
 *
 * The returned flags value is now unused - kmtx_lock saves and restores the
 * interrupt state itself - but the signature is kept so that every call site
 * still reads as a matched lock/unlock pair, and so that a future variant
 * that does need to hand something back has somewhere to put it. */
static kh_uptr kh_lock(void) {
    kmtx_lock(&kh_mtx);
    return 0;
}

static void kh_unlock(kh_uptr flags) {
    (void)flags;
    kmtx_unlock(&kh_mtx);
}

/* --- block accessors ---------------------------------------------------- */

static void *kh_payload(kh_header_t *h) {
    return (void *)((kh_uptr)h + KH_HDR);
}

static kh_header_t *kh_header_from_payload(void *p) {
    return (kh_header_t *)((kh_uptr)p - KH_HDR);
}

static kh_footer_t *kh_footer(kh_header_t *h) {
    return (kh_footer_t *)((kh_uptr)h + h->size - KH_FTR);
}

/* Write both tags at once. The only way a block's size should ever change. */
static void kh_set_block(kh_header_t *h, kh_size size, kh_size magic) {
    kh_footer_t *f;
    h->magic = magic;
    h->size  = size;
    f = (kh_footer_t *)((kh_uptr)h + size - KH_FTR);
    f->magic = magic;
    f->size  = size;
}

/* Next block, or NULL if h is the last one in the heap. */
static kh_header_t *kh_next(kh_header_t *h) {
    kh_uptr base = (kh_uptr)h;
    /* Compare by subtraction, never by addition: base + size wraps when the
     * heap sits high in the address space, and a wrapped sum compares as
     * "comfortably in range". See the note on kfree's size check. */
    if (base >= heap_end || h->size >= heap_end - base) {
        return NULL;
    }
    return (kh_header_t *)(base + h->size);
}

/* Previous block, or NULL if h is the first one. Reads the previous block's
 * footer, which sits immediately below h. */
static kh_header_t *kh_prev(kh_header_t *h) {
    kh_footer_t *pf;
    if ((kh_uptr)h <= heap_start) {
        return NULL;
    }
    pf = (kh_footer_t *)((kh_uptr)h - KH_FTR);
    if (pf->size == 0 || pf->size > ((kh_uptr)h - heap_start)) {
        return NULL;  /* corrupt; treat as "no previous block" and let
                       * kheap_check() report it rather than following it */
    }
    return (kh_header_t *)((kh_uptr)h - pf->size);
}

/* --- free list ---------------------------------------------------------- */

static void kh_list_insert(kh_header_t *h) {
    kh_node_t *n = (kh_node_t *)kh_payload(h);
    n->prev = NULL;
    n->next = free_list;
    if (free_list) {
        free_list->prev = n;
    }
    free_list = n;
}

static void kh_list_remove(kh_header_t *h) {
    kh_node_t *n = (kh_node_t *)kh_payload(h);
    if (n->prev) {
        n->prev->next = n->next;
    } else {
        free_list = n->next;
    }
    if (n->next) {
        n->next->prev = n->prev;
    }
    n->next = NULL;
    n->prev = NULL;
}

/* --- growing ------------------------------------------------------------ */

/* Commit at least `need` more bytes of heap by mapping fresh frames at the
 * top. Returns 1 if anything was added. The new space becomes one free block,
 * merged with the previous block if that was also free. */
static int kh_grow(kh_size need) {
    kh_size want, committed;
    kh_uptr new_end, addr;
    kh_header_t *block;
    kh_header_t *prev;

    if (heap_end >= heap_limit) {
        return 0;
    }

    /* Round up to a page, and never grow by less than KHEAP_INITIAL - one
     * mapping call per page is fine, but a heap that grows a page at a time
     * under a burst of small allocations spends all its time in kh_grow. */
    want = kh_align_up(need, (kh_size)PMM_PAGE_SIZE);
    if (want < (kh_size)KHEAP_INITIAL) {
        want = (kh_size)KHEAP_INITIAL;
    }

    new_end = heap_end + want;
    if (new_end > heap_limit || new_end < heap_end /* wrap */) {
        new_end = heap_limit;
    }

    committed = 0;
    for (addr = heap_end; addr < new_end; addr += PMM_PAGE_SIZE) {
        if (!kh_commit_page(addr)) {
            break;  /* out of physical memory; keep whatever we did get */
        }
        committed += PMM_PAGE_SIZE;
    }

    /* Not enough room even for the tags plus a free-list node: not worth
     * tracking, and a sub-minimum block would break the tiling invariant.
     * The pages stay mapped and simply go unused until the next grow. */
    if (committed < KH_MIN_BLOCK) {
        return 0;
    }

    block = (kh_header_t *)heap_end;
    heap_end += committed;              /* set before kh_next/kh_prev use it */

    kh_set_block(block, committed, KH_MAGIC_FREE);

    prev = kh_prev(block);
    if (prev && prev->magic == KH_MAGIC_FREE) {
        kh_list_remove(prev);
        kh_set_block(prev, prev->size + block->size, KH_MAGIC_FREE);
        block = prev;
    }

    kh_list_insert(block);
    return 1;
}

/* --- core allocation ---------------------------------------------------- */

/* Convert a caller's byte count into a legal total block size. */
static kh_size kh_block_size_for(kh_size size) {
    kh_size total = kh_align_up(size, KH_ALIGN) + KH_OVERHEAD;
    if (total < KH_MIN_BLOCK) {
        total = KH_MIN_BLOCK;
    }
    return total;
}

static kh_header_t *kh_find_fit(kh_size total) {
    kh_node_t *n;
    for (n = free_list; n != NULL; n = n->next) {
        kh_header_t *h = kh_header_from_payload(n);
        if (h->size >= total) {
            return h;
        }
    }
    return NULL;
}

/* Carve `total` bytes off the front of block h, returning the remainder to
 * the free list if it's big enough to be a block in its own right. h must
 * already be off the free list. Marks h used. */
static void kh_split_and_use(kh_header_t *h, kh_size total) {
    kh_size remainder = h->size - total;

    if (remainder >= KH_MIN_BLOCK) {
        kh_header_t *tail;
        kh_set_block(h, total, KH_MAGIC_USED);
        tail = (kh_header_t *)((kh_uptr)h + total);
        kh_set_block(tail, remainder, KH_MAGIC_FREE);
        /* Whatever followed h was in use (no two adjacent free blocks), so
         * tail needs no coalescing - the invariant still holds. */
        kh_list_insert(tail);
    } else {
        /* Remainder too small to be its own block; hand it to the caller as
         * slack rather than leaking it. */
        kh_set_block(h, h->size, KH_MAGIC_USED);
    }
}

static void *kh_malloc_locked(kh_size size) {
    kh_size total;
    kh_header_t *h;

    if (size == 0) {
        return NULL;
    }
    /* Reject sizes that would overflow the rounding in kh_block_size_for.
     * (kh_size)-1 rather than a literal, so this scales with the target. */
    if (size > ((kh_size)-1) - KH_OVERHEAD - KH_ALIGN) {
        return NULL;
    }

    total = kh_block_size_for(size);

    h = kh_find_fit(total);
    if (h == NULL) {
        if (!kh_grow(total)) {
            return NULL;
        }
        h = kh_find_fit(total);
        if (h == NULL) {
            return NULL;
        }
    }

    kh_list_remove(h);
    kh_split_and_use(h, total);
    stat_payload += h->size - KH_OVERHEAD;
    return kh_payload(h);
}

static void *kh_malloc_a_locked(kh_size size) {
    kh_size total, worst_case;
    int pass;

    if (size == 0) {
        return NULL;
    }
    if (size > ((kh_size)-1) - KH_OVERHEAD - KH_ALIGN - PMM_PAGE_SIZE) {
        return NULL;
    }
    total = kh_block_size_for(size);
    /* Worst case we have to skip almost a full page and still leave a legal
     * free block behind in front of the aligned one. */
    worst_case = total + PMM_PAGE_SIZE + KH_MIN_BLOCK;

    for (pass = 0; pass < 2; pass++) {
        kh_node_t *n;
        for (n = free_list; n != NULL; n = n->next) {
            kh_header_t *h = kh_header_from_payload(n);
            kh_uptr block_start = (kh_uptr)h;
            kh_uptr payload     = block_start + KH_HDR;
            kh_uptr aligned     = kh_align_up(payload, (kh_size)PMM_PAGE_SIZE);
            kh_size front_size;

            if (aligned != payload) {
                /* The leftover in front has to be a legal block of its own. */
                while (aligned - KH_HDR - block_start < KH_MIN_BLOCK) {
                    aligned += PMM_PAGE_SIZE;
                }
            }

            if (aligned - KH_HDR + total > block_start + h->size) {
                continue;  /* doesn't fit once alignment padding is counted */
            }

            kh_list_remove(h);

            if (aligned == payload) {
                kh_split_and_use(h, total);
                stat_payload += h->size - KH_OVERHEAD;
                return kh_payload(h);
            }

            /* Split into [front free][aligned block], then trim the tail of
             * the aligned block the usual way. */
            front_size = (kh_size)((aligned - KH_HDR) - block_start);
            {
                kh_size rest = h->size - front_size;
                kh_header_t *b = (kh_header_t *)(aligned - KH_HDR);

                kh_set_block(h, front_size, KH_MAGIC_FREE);
                kh_list_insert(h);

                kh_set_block(b, rest, KH_MAGIC_FREE);
                kh_split_and_use(b, total);
                stat_payload += b->size - KH_OVERHEAD;
                return kh_payload(b);
            }
        }

        if (!kh_grow(worst_case)) {
            break;
        }
    }
    return NULL;
}

static void *kh_calloc_locked(kh_size count, kh_size size) {
    kh_size total;
    void *p;

    if (count == 0 || size == 0) {
        return NULL;
    }
    if (count > ((kh_size)-1) / size) {
        return NULL;  /* multiplication would overflow */
    }
    total = count * size;

    /* kh_malloc_locked, NOT the public kmalloc.
     *
     * This called kmalloc, which re-takes the heap lock - and the file's own
     * comment beside kmalloc claims the lock "is acquired exactly once, at
     * the boundary", which was not true here. It was harmless for as long as
     * the lock was a nested cli/save-flags: taking it twice on one CPU cost
     * nothing. Part 11 replaced it with a real spin lock and the second
     * acquire became an immediate self-deadlock - the boot hung with one
     * line of output.
     *
     * A latent bug rather than a new one: any future lock with the same
     * shape would have hit it, and the comment describing the invariant was
     * already there to be read. */
    p = kh_malloc_locked(total);
    if (p) {
        kh_memset(p, 0, total);
    }
    return p;
}

/* --- freeing ------------------------------------------------------------ */

/* Merge h with any free neighbours. h must be marked free and already off
 * the free list; neighbours that get absorbed are removed from it here.
 * Returns the header of the merged block. */
static kh_header_t *kh_coalesce(kh_header_t *h) {
    kh_header_t *next = kh_next(h);
    kh_header_t *prev;

    if (next && next->magic == KH_MAGIC_FREE) {
        kh_list_remove(next);
        kh_set_block(h, h->size + next->size, KH_MAGIC_FREE);
    }

    prev = kh_prev(h);
    if (prev && prev->magic == KH_MAGIC_FREE) {
        kh_list_remove(prev);
        kh_set_block(prev, prev->size + h->size, KH_MAGIC_FREE);
        h = prev;
    }

    return h;
}

static void kh_free_locked(void *ptr) {
    kh_header_t *h;
    kh_footer_t *f;

    if (ptr == NULL) {
        return;
    }

    /* Pointer has to be inside the heap and past the first header. */
    if ((kh_uptr)ptr < heap_start + KH_HDR || (kh_uptr)ptr >= heap_end) {
        return;
    }

    h = kh_header_from_payload(ptr);

    if (h->magic == KH_MAGIC_FREE) {
        return;  /* double free - refuse rather than corrupt the free list */
    }
    if (h->magic != KH_MAGIC_USED) {
        return;  /* not a block header at all, or overrun from below */
    }
    if (h->size < KH_MIN_BLOCK || h->size > heap_end - (kh_uptr)h) {
        return;  /* size is nonsense; walking the footer would go off-heap */
    }

    f = kh_footer(h);
    if (f->magic != h->magic || f->size != h->size) {
        return;  /* footer trashed - almost always a buffer overrun by the
                  * caller. Bailing keeps the rest of the heap walkable. */
    }

    stat_payload -= h->size - KH_OVERHEAD;

    kh_set_block(h, h->size, KH_MAGIC_FREE);
    h = kh_coalesce(h);
    kh_list_insert(h);
}

static void *kh_realloc_locked(void *ptr, kh_size size) {
    kh_header_t *h;
    kh_size old_payload, total;
    void *fresh;

    if (ptr == NULL) {
        return kh_malloc_locked(size);
    }
    if (size == 0) {
        kh_free_locked(ptr);
        return NULL;
    }
    if ((kh_uptr)ptr < heap_start + KH_HDR || (kh_uptr)ptr >= heap_end) {
        return NULL;
    }

    h = kh_header_from_payload(ptr);
    if (h->magic != KH_MAGIC_USED) {
        return NULL;
    }

    old_payload = h->size - KH_OVERHEAD;
    total = kh_block_size_for(size);

    /* Shrinking, or growing into slack we already had: reuse in place. */
    if (total <= h->size) {
        kh_size remainder = h->size - total;
        if (remainder >= KH_MIN_BLOCK) {
            kh_header_t *tail;
            kh_set_block(h, total, KH_MAGIC_USED);
            tail = (kh_header_t *)((kh_uptr)h + total);
            kh_set_block(tail, remainder, KH_MAGIC_FREE);
            stat_payload -= old_payload;
            stat_payload += h->size - KH_OVERHEAD;
            /* The block after the original h was in use, but tail is new, so
             * coalesce forward to keep the no-adjacent-free rule. */
            tail = kh_coalesce(tail);
            kh_list_insert(tail);
        }
        return ptr;
    }

    fresh = kh_malloc_locked(size);   /* not kmalloc - see kh_calloc_locked */
    if (fresh == NULL) {
        return NULL;  /* caller's original pointer is still valid */
    }
    kh_memcpy(fresh, ptr, old_payload < size ? old_payload : size);
    kh_free_locked(ptr);
    return fresh;
}

/* --- init, stats, debug ------------------------------------------------- */

static void kh_init_locked(void) {
    kh_uptr base = (kh_uptr)KHEAP_START;

    /* Ask the kernel VA allocator for this range instead of trusting
     * KHEAP_START not to collide with KSTACK_BASE/PE_DRIVER_BASE by comment
     * alone (ROADMAP item 11). KHEAP_START stays the fallback - kvm_init()
     * runs before this (see flk.c), but if it were ever skipped, or the
     * region exhausted, falling back to the hand-picked constant is still
     * correct, just no longer collision-checked. 32-bit builds keep using
     * the constant directly: vmalloc.h's region is a 64-bit kernel-half
     * address, meaningless under kh_uptr's 32-bit typedef branch. */
#if defined(__x86_64__) || defined(__aarch64__) || defined(__LP64__) || \
    defined(_LP64) || defined(_WIN64)
    {
        uint64 allocated = kvm_alloc_range((uint64)KHEAP_MAX_SIZE, PMM_PAGE_SIZE);
        if (allocated != 0) {
            base = (kh_uptr)allocated;
        }
    }
#endif

    /* Explicit, not relying on .bss being zeroed - see the note above. */
    heap_start   = base;
    heap_end     = base;
    heap_limit   = base + (kh_uptr)KHEAP_MAX_SIZE;
    free_list    = NULL;
    stat_payload = 0;

    kh_grow((kh_size)KHEAP_INITIAL);
}

static void kh_stats_locked(kh_size *out_used, kh_size *out_free, kh_size *out_committed) {
    if (out_used) {
        *out_used = stat_payload;
    }
    if (out_free) {
        kh_size bytes = 0;
        kh_node_t *n;
        for (n = free_list; n != NULL; n = n->next) {
            bytes += kh_header_from_payload(n)->size - KH_OVERHEAD;
        }
        *out_free = bytes;
    }
    if (out_committed) {
        *out_committed = (kh_size)(heap_end - heap_start);
    }
}

static int kh_check_locked(void) {
    kh_uptr addr;
    int prev_was_free = 0;

    for (addr = heap_start; addr < heap_end; ) {
        kh_header_t *h = (kh_header_t *)addr;
        kh_footer_t *f;
        int is_free;

        if (h->magic != KH_MAGIC_USED && h->magic != KH_MAGIC_FREE) {
            return KHEAP_ERR_MAGIC;
        }
        if (h->size < KH_MIN_BLOCK || (h->size & (KH_ALIGN - 1u)) != 0 ||
            h->size > heap_end - addr) {
            return KHEAP_ERR_SIZE;
        }

        f = kh_footer(h);
        if (f->magic != h->magic || f->size != h->size) {
            return KHEAP_ERR_FOOTER;
        }

        is_free = (h->magic == KH_MAGIC_FREE);
        if (is_free && prev_was_free) {
            return KHEAP_ERR_ADJACENT;
        }
        prev_was_free = is_free;

        addr += h->size;
    }

    if (addr != heap_end) {
        return KHEAP_ERR_SIZE;  /* blocks didn't tile the heap exactly */
    }

    /* Every entry on the free list must actually be a free block, and the
     * links must be consistent in both directions. */
    {
        kh_node_t *n, *last = NULL;
        for (n = free_list; n != NULL; n = n->next) {
            kh_header_t *h = kh_header_from_payload(n);
            if ((kh_uptr)h < heap_start || (kh_uptr)h >= heap_end) {
                return KHEAP_ERR_FREELIST;
            }
            if (h->magic != KH_MAGIC_FREE) {
                return KHEAP_ERR_FREELIST;
            }
            if (n->prev != last) {
                return KHEAP_ERR_FREELIST;
            }
            last = n;
        }
    }

    return 0;
}

/* --- public entry points --------------------------------------------------
 * Every one of these is the same shape: take the lock, call the unlocked
 * implementation, put it back. The split exists so the implementations can
 * call each other freely without thinking about nesting - krealloc uses
 * kmalloc and kfree, kcalloc uses kmalloc - while the lock is acquired
 * exactly once, at the boundary. */

void *kmalloc(kh_size size) {
    kh_uptr f = kh_lock();
    void *p = kh_malloc_locked(size);
    kh_unlock(f);
    return p;
}

void *kmalloc_a(kh_size size) {
    kh_uptr f = kh_lock();
    void *p = kh_malloc_a_locked(size);
    kh_unlock(f);
    return p;
}

void *kcalloc(kh_size count, kh_size size) {
    kh_uptr f = kh_lock();
    void *p = kh_calloc_locked(count, size);
    kh_unlock(f);
    return p;
}

void *krealloc(void *ptr, kh_size size) {
    kh_uptr f = kh_lock();
    void *p = kh_realloc_locked(ptr, size);
    kh_unlock(f);
    return p;
}

void kfree(void *ptr) {
    kh_uptr f = kh_lock();
    kh_free_locked(ptr);
    kh_unlock(f);
}

void kheap_init(void) {
    kh_uptr f = kh_lock();
    kh_init_locked();
    kh_unlock(f);
}

void kheap_stats(kh_size *out_used, kh_size *out_free, kh_size *out_committed) {
    kh_uptr f = kh_lock();
    kh_stats_locked(out_used, out_free, out_committed);
    kh_unlock(f);
}

int kheap_check(void) {
    kh_uptr f = kh_lock();
    int rc = kh_check_locked();
    kh_unlock(f);
    return rc;
}
