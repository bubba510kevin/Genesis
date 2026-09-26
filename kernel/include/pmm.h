#ifndef PMM_H
#define PMM_H

#include "e820.h"
#include "typesk.h"

#define PMM_PAGE_SIZE 4096

/* Bitmap physical memory manager: one bit per 4KB frame, 1 = used.
 *
 * The frame bitmap is sized by the caller and capped by pmm_init, so the
 * amount of memory this can manage is a property of the buffer you hand it
 * rather than a constant compiled in here. A machine with more RAM than the
 * bitmap can describe gets the low portion managed and the rest ignored -
 * wasteful, but never wrong. */

/* Builds the free map from the BIOS memory map.
 *
 * Everything starts marked used, and only ranges a type-1 E820 entry vouches
 * for are released. That ordering is the point: an address the firmware did
 * not describe is not RAM, and the previous "assume N megabytes are all RAM"
 * version handed out the VGA aperture as though it were memory.
 *
 * Partial frames at the edges of a usable range are left used - a frame is
 * only free if all 4096 bytes of it are.
 *
 * The caller still has to reserve what it knows about and E820 does not: the
 * kernel image, the bitmap, and anything the bootloader left in low memory.
 * Do that with pmm_mark_region_used() before the first allocation. */
void pmm_init(const e820_map_t *map, uint8 *bitmap_start, uint64 bitmap_bytes,
              uint8 *refcounts, uint64 refcount_entries);

/* Mark a physical region unusable. Rounds outward - a region covering any
 * part of a frame reserves the whole frame. */
void pmm_mark_region_used(phys_addr_t phys_addr, phys_addr_t length);

/* Returns the physical address of a free 4KB frame, or 0 if out of memory.
 * Frame 0 is always reserved (it falls inside the kernel's low-memory
 * reservation), so 0 is unambiguously a failure and not an address. */
phys_addr_t pmm_alloc_frame(void);

/* Same, but restricted to frames below `limit`.
 *
 * Needed exactly once, and only for a chicken-and-egg case: the page tables
 * that build the direct map cannot themselves be reached through the direct
 * map, so they have to come from the range the kernel window already covers.
 * Any other caller reaching for this is probably working around a mapping
 * that should exist. */
phys_addr_t pmm_alloc_frame_below(phys_addr_t limit);

/* Drop one reference to a frame. The frame returns to the allocator only when
 * the last reference goes.
 *
 * This used to be an unconditional free, on the assertion that the caller held
 * the only mapping - true for page tables and for anonymous user pages, and
 * false the moment copy-on-write fork lets two address spaces name the same
 * frame. Every existing caller keeps working unchanged: a frame handed out by
 * the allocator starts at one reference, so the first free is still the last
 * one unless somebody explicitly took another.
 *
 * Freeing a frame that has no references does nothing. That covers frames
 * reserved before the allocator ever saw them (the kernel image, firmware
 * regions), where the old behaviour would have released memory that is not
 * RAM into the free pool. */
void pmm_free_frame(phys_addr_t phys_addr);

/* Take one more reference to a frame. Used when a mapping is duplicated
 * rather than created - which today means exactly one caller, vmm_space_clone.
 *
 * Saturates at 255 rather than wrapping. A frame that reaches the cap can
 * never be freed again, which leaks it; wrapping would free a frame that is
 * still mapped somewhere, which corrupts whatever maps it. With sixteen
 * process slots the cap is unreachable in practice. */
void pmm_ref_frame(phys_addr_t phys_addr);

/* How many references a frame has. Zero for a frame that is free or was never
 * handed out by the allocator. The copy-on-write fault handler reads this to
 * decide between copying the page and simply making it writable again. */
uint32 pmm_frame_refs(phys_addr_t phys_addr);

/* Stats. Frame counts, not bytes. */
uint64 pmm_total_frames(void);
uint64 pmm_used_frames(void);
uint64 pmm_free_frames(void);

#endif
