#ifndef KHEAP_H
#define KHEAP_H

#include "typesk.h"

/* Kernel heap: a first-fit, boundary-tagged free-list allocator sitting
 * on top of the VMM. kmalloc() hands out byte-granularity chunks; the
 * heap grows a page at a time via vmm_alloc_page() when it runs dry.
 *
 * --- Portability ---------------------------------------------------------
 * This file deliberately has no 32-bit assumptions baked into it. Sizes and
 * addresses use kh_uptr/kh_size (below), block layout is derived from
 * sizeof() rather than hardcoded byte counts, and alignment scales with the
 * pointer width. The intent is that when Phase 2 of the roadmap moves the
 * kernel to long mode, this file needs no edits - only typesk.h and the VMM
 * interface do. kheap.c carries compile-time assertions that fail the build
 * if any of that stops being true, rather than letting it fail at runtime.
 *
 * That move has happened: paging.h's vmm_alloc_page() takes virt_addr_t now,
 * so the narrowing this file used to guard against no longer exists and the
 * heap can live anywhere canonical. Every VMM call still goes through the one
 * kh_commit_page wrapper - that seam is where locking will go, not where a
 * cast lives.
 */

/* Pointer-sized unsigned integer. typesk.h has no uintptr_t; when you add
 * one there (you will need it kernel-wide for the 64-bit port), delete this
 * and typedef kh_uptr to it. */
#if defined(__x86_64__) || defined(__aarch64__) || defined(__LP64__) || \
    defined(_LP64) || defined(_WIN64)
typedef uint64 kh_uptr;
#else
typedef uint32 kh_uptr;
#endif

/* Sizes are pointer-width too, so an allocation can in principle span the
 * whole address space on 64-bit. On 32-bit this is uint32, which makes the
 * public API below source-compatible with the previous version - flk.c and
 * any existing callers need no changes. */
typedef kh_uptr kh_size;

/* Placement. These are the only target-policy constants in the heap; a
 * higher-half kernel overrides KHEAP_START from the build system rather
 * than editing this header.
 *
 *   - Above the 4MB identity map, so heap pages are real VMM mappings
 *     rather than aliases of identity-mapped low RAM.
 *   - 0x400000..0x7FFFFF is left alone because flk.c's VMM smoke test
 *     maps a page there.
 *   - KHEAP_MAX_SIZE is 4MB, exactly one x86 page table's worth of address
 *     space, so the whole heap fits in a single entry of paging.c's
 *     MAX_DYNAMIC_TABLES pool no matter how far it grows. On a target with
 *     a different page-table fanout this is just a cap, not a correctness
 *     requirement.
 */
#ifndef KHEAP_START
#if defined(__x86_64__)
#define KHEAP_START      0xFFFFFFFF90000000ULL  /* kernel half, clear of the 16MB linear map */
#else
#define KHEAP_START      0xD0000000u          /* 32-bit build: dir index 832 */
#endif
#endif
#ifndef KHEAP_INITIAL
#define KHEAP_INITIAL    0x00010000u  /* 64KB committed up front */
#endif
#ifndef KHEAP_MAX_SIZE
#define KHEAP_MAX_SIZE   0x03000000u  /* 48MB: see below */
/* Raised from 16MB when the vendored ZFS reader landed. Its zfs_init()
 * allocates one buffer of SPA_MAXBLOCKSIZE - 16MB, the largest block ZFS can
 * have - as a dnode cache, unconditionally and for the life of the kernel.
 * At a 16MB cap that allocation is the whole heap and fails, and the vendored
 * code does not check it, so the failure would arrive as a null dereference
 * inside a filesystem rather than as an out-of-memory report.
 *
 * kernel/zfs/zfs_glue.c checks the pointer anyway. This raise is what makes
 * the check pass rather than what makes it unnecessary: 48MB is address
 * space, not memory - the heap still commits a page at a time. */
#endif

/* Call once, after pmm_init() and paging_init(). Commits KHEAP_INITIAL
 * bytes. Safe to call even if that fails - the heap just starts empty
 * and kmalloc() will try to grow it on first use. */
void kheap_init(void);

/* Allocate `size` bytes, aligned to 2 * sizeof(void *): 8 bytes on 32-bit,
 * 16 on 64-bit, matching what a hosted malloc guarantees. Returns NULL on
 * out-of-memory or if size is 0. Contents are uninitialised. */
void *kmalloc(kh_size size);

/* Same, but the returned pointer is page-aligned (PMM_PAGE_SIZE). Use this
 * for page directories, page tables, and anything else the MMU wants
 * aligned. Costs up to a page of internal fragmentation. */
void *kmalloc_a(kh_size size);

/* Allocate count*size bytes, zeroed. Returns NULL on overflow or OOM. */
void *kcalloc(kh_size count, kh_size size);

/* Resize an allocation, preserving min(old, new) bytes of contents.
 * krealloc(NULL, n) == kmalloc(n); krealloc(p, 0) frees p and
 * returns NULL. Returns NULL (leaving `ptr` valid) if it can't grow. */
void *krealloc(void *ptr, kh_size size);

/* Free a pointer returned by kmalloc/kmalloc_a/kcalloc/krealloc.
 * kfree(NULL) is a no-op. Detects the obvious corruption cases
 * (double free, trashed header) and refuses rather than corrupting
 * the heap further. */
void kfree(void *ptr);

/* out_used:      payload bytes currently handed out
 * out_free:      payload bytes sitting in the free list
 * out_committed: total bytes of address space backed by real frames
 * Any pointer may be NULL. */
void kheap_stats(kh_size *out_used, kh_size *out_free, kh_size *out_committed);

/* Walks every block start-to-end and verifies the boundary tags tile
 * the heap exactly. Returns 0 if the heap is consistent, or a negative
 * KHEAP_ERR_* code identifying the first problem found. Cheap enough to
 * call from a debug build after every kfree while you're bringing this
 * up; O(number of blocks). */
#define KHEAP_ERR_MAGIC     -1  /* header magic isn't USED or FREE */
#define KHEAP_ERR_SIZE      -2  /* block size is 0, misaligned, or runs past heap_end */
#define KHEAP_ERR_FOOTER    -3  /* footer doesn't mirror the header */
#define KHEAP_ERR_ADJACENT  -4  /* two adjacent free blocks (missed coalesce) */
#define KHEAP_ERR_FREELIST  -5  /* free-list membership doesn't match block flags */
int kheap_check(void);

#endif
