#ifndef VMALLOC_H
#define VMALLOC_H

#include "typesk.h"

/* Kernel dynamic virtual-address range allocator.
 *
 * Before this file, every subsystem that needed a chunk of the kernel half
 * picked its own base address by hand: KHEAP_START (kheap.h), KSTACK_BASE
 * (kstack.h), PE_DRIVER_BASE (pe.h) are each a bare #define, verified not to
 * collide with the others by a COMMENT, not by code. Nothing stops a fourth
 * one from silently overlapping - this is ROADMAP item 11's own "A KERNEL
 * VIRTUAL-ADDRESS ALLOCATOR" gap.
 *
 * This does not replace vmm_map_page_in/paging.c - it is purely bookkeeping
 * one layer above it, tracking which VA RANGES are already spoken for so two
 * unrelated subsystems' base addresses can never collide again. A subsystem
 * still calls vmm_map_page_in/vmm_alloc_page_in itself to actually back its
 * range with frames.
 *
 * No dynamic memory: kheap_init() is one of this file's own callers, so this
 * cannot depend on kmalloc existing yet. A fixed-size sorted array of
 * allocated ranges, the same "fixed pool, leak shows up as an allocation
 * failure not fragmentation" convention paging.c's address_space_t pool and
 * bus.c's bus_dev_t pool both already use. */

/* Clear of KERNEL_VMA's 4MB image window (KERNEL_MAP_SIZE, paging.h) at the
 * bottom, and leaving headroom below the top of the kernel half at the top -
 * the direct map lives in its own PML4 slot (PHYSMAP_BASE, paging.h) and is
 * never in this region regardless. */
#define KVM_REGION_START 0xFFFFFFFF81000000ULL
#define KVM_REGION_END   0xFFFFFFFFFF000000ULL

#define KVM_MAX_RANGES 64

/* Call once, after paging_init() and before anything below it in flk.c's
 * boot order requests a range - concretely, before kheap_init(). */
void kvm_init(void);

/* Reserve `size` bytes (rounded up to a whole page) somewhere in
 * [KVM_REGION_START, KVM_REGION_END), aligned to `align` (a power of two,
 * at least PMM_PAGE_SIZE). First-fit over the gaps between already-
 * allocated ranges. Returns the base, or 0 if the region is exhausted,
 * fragmented past what first-fit can place, or the range table is full. */
uint64 kvm_alloc_range(uint64 size, uint64 align);

/* Give a range back - `base` must be exactly what kvm_alloc_range returned.
 * Only correct once nothing still maps inside it; this does not unmap
 * anything itself. No-op if `base` is not a range this allocator owns. */
void kvm_free_range(uint64 base);

#endif
