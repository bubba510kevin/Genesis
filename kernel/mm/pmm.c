#include "e820.h"
#include "pmm.h"

/* Bitmap physical memory manager. One bit per 4KB frame, 1 = used.
 *
 * The allocator itself is unchanged from the first version - a linear scan
 * over a bitmap, the same scheme early Linux used before the buddy allocator.
 * What changed is where the map of usable memory comes from: the firmware,
 * via E820, instead of a constant. */

static uint8  *bitmap;
static uint64  total_frames;
static uint64  used_frames;

/* One byte per frame, parallel to the bitmap.
 *
 * The bitmap answers "is this frame in use"; this answers "by how many". They
 * are kept separate rather than widening the bitmap because every allocation
 * path scans the bitmap and only the sharing paths touch this - eight frames
 * per byte is what makes that scan cheap, and it is not worth giving up to
 * store a number that is 1 for almost every frame in the system.
 *
 * refs[f] == 0 means the frame is free OR was never handed out by the
 * allocator (reserved regions, non-RAM holes). Those two are distinguished by
 * the bitmap, and pmm_free_frame refuses both. */
static uint8  *refs;
static uint64  refs_capacity;

#define REF_MAX 255

/* Where the last successful scan stopped. A linear search from frame 0 on
 * every allocation is O(n) in a bitmap that is mostly used at the bottom -
 * with the kernel, the bitmap and the low reservation all at low addresses,
 * every allocation re-walked the same few hundred used frames. Starting from
 * the last hit makes the common case O(1) and costs one wraparound in the
 * uncommon one. */
static uint64  scan_hint;

static inline void bitmap_set(uint64 frame) {
    bitmap[frame / 8] |= (uint8)(1u << (frame % 8));
}

static inline void bitmap_clear(uint64 frame) {
    bitmap[frame / 8] &= (uint8)~(1u << (frame % 8));
}

static inline int bitmap_test(uint64 frame) {
    return bitmap[frame / 8] & (1u << (frame % 8));
}

/* Release the frames wholly inside [base, base+length).
 *
 * Rounds INWARD, the opposite of pmm_mark_region_used: a frame is only handed
 * to the allocator if every byte of it is vouched for. E820 boundaries are
 * not always page aligned (0x9FC00 for the EBDA is the usual example), and a
 * frame that is half usable is not usable. */
static void mark_region_free(phys_addr_t base, phys_addr_t length) {
    uint64 first = (base + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64 last  = (base + length) / PMM_PAGE_SIZE;   /* exclusive */
    uint64 frame;

    if (last > total_frames) {
        last = total_frames;
    }
    for (frame = first; frame < last; frame++) {
        if (bitmap_test(frame)) {
            bitmap_clear(frame);
            used_frames--;
        }
    }
}

void pmm_init(const e820_map_t *map, uint8 *bitmap_start, uint64 bitmap_bytes,
              uint8 *refcounts, uint64 refcount_entries) {
    uint64 capacity_frames = bitmap_bytes * 8;
    phys_addr_t top = e820_highest_usable(map);
    uint64 i;

    bitmap        = bitmap_start;
    refs          = refcounts;
    refs_capacity = refcounts != NULL ? refcount_entries : 0;

    /* A refcount array smaller than the bitmap would leave the frames past
     * its end untracked, and an untracked frame that gets shared is freed
     * while still mapped. Managing less memory is survivable; that is not. */
    if (capacity_frames > refs_capacity) {
        capacity_frames = refs_capacity;
    }

    total_frames = top / PMM_PAGE_SIZE;
    if (total_frames > capacity_frames) {
        /* More RAM than the bitmap can describe. The excess is simply not
         * managed - correct, just wasteful. Widen the buffer in flk.c if this
         * ever matters. */
        total_frames = capacity_frames;
    }

    /* Everything unusable to start with, then release what E820 vouches for.
     * The reverse order - assume usable, subtract the holes - is what the
     * previous version effectively did, and it is wrong by default: it hands
     * out every address the firmware forgot to mention. */
    for (i = 0; i < bitmap_bytes; i++) {
        bitmap[i] = 0xFF;
    }
    used_frames = total_frames;
    scan_hint   = 0;

    for (i = 0; i < total_frames; i++) {
        refs[i] = 0;
    }

    for (i = 0; i < map->count; i++) {
        const e820_entry_t *e = &map->entries[i];

        if (e->type != E820_TYPE_USABLE || e->length == 0) {
            continue;
        }
        if (e->base + e->length < e->base) {
            continue;   /* firmware arithmetic wrapped; ignore it */
        }
        mark_region_free((phys_addr_t)e->base, (phys_addr_t)e->length);
    }
}

void pmm_mark_region_used(phys_addr_t phys_addr, phys_addr_t length) {
    uint64 start_frame = phys_addr / PMM_PAGE_SIZE;
    uint64 end_frame   = (phys_addr + length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64 frame;

    if (end_frame > total_frames) {
        end_frame = total_frames;
    }
    for (frame = start_frame; frame < end_frame; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            /* One reference, held by nobody in particular and never dropped.
             * Reserved memory should never be freed, and giving it a count
             * means an erroneous free costs a reference rather than handing
             * the kernel image back to the allocator. */
            refs[frame] = 1;
            used_frames++;
        }
    }
}

static phys_addr_t take_frame(uint64 frame) {
    bitmap_set(frame);
    refs[frame] = 1;
    used_frames++;
    scan_hint = frame + 1;
    /* The cast is load-bearing: without it this multiply is done in the width
     * of `frame` times an int and can wrap. */
    return (phys_addr_t)frame * PMM_PAGE_SIZE;
}

phys_addr_t pmm_alloc_frame(void) {
    uint64 frame;

    for (frame = scan_hint; frame < total_frames; frame++) {
        if (!bitmap_test(frame)) {
            return take_frame(frame);
        }
    }
    for (frame = 0; frame < scan_hint && frame < total_frames; frame++) {
        if (!bitmap_test(frame)) {
            return take_frame(frame);
        }
    }
    return 0;   /* out of memory */
}

phys_addr_t pmm_alloc_frame_below(phys_addr_t limit) {
    uint64 last = limit / PMM_PAGE_SIZE;
    uint64 frame;

    if (last > total_frames) {
        last = total_frames;
    }
    for (frame = 0; frame < last; frame++) {
        if (!bitmap_test(frame)) {
            return take_frame(frame);
        }
    }
    return 0;
}

void pmm_free_frame(phys_addr_t phys_addr) {
    uint64 frame = phys_addr / PMM_PAGE_SIZE;

    if (frame >= total_frames) {
        return;
    }
    if (!bitmap_test(frame)) {
        return;                  /* already free - a double free, ignored */
    }
    if (refs[frame] == 0) {
        return;                  /* never allocated; not ours to hand out */
    }
    if (refs[frame] == REF_MAX) {
        return;                  /* saturated: pinned forever, see pmm.h */
    }
    if (--refs[frame] > 0) {
        return;                  /* somebody else still maps it */
    }

    bitmap_clear(frame);
    used_frames--;
    if (frame < scan_hint) {
        scan_hint = frame;       /* reuse it before scanning past it again */
    }
}

void pmm_ref_frame(phys_addr_t phys_addr) {
    uint64 frame = phys_addr / PMM_PAGE_SIZE;

    if (frame >= total_frames || !bitmap_test(frame)) {
        return;
    }
    if (refs[frame] < REF_MAX) {
        refs[frame]++;
    }
}

uint32 pmm_frame_refs(phys_addr_t phys_addr) {
    uint64 frame = phys_addr / PMM_PAGE_SIZE;

    if (frame >= total_frames || !bitmap_test(frame)) {
        return 0;
    }
    return refs[frame];
}

uint64 pmm_total_frames(void) { return total_frames; }
uint64 pmm_used_frames(void)  { return used_frames;  }
uint64 pmm_free_frames(void)  { return total_frames - used_frames; }
