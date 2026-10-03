#ifndef NTVM_H
#define NTVM_H

#include "object.h"
#include "paging.h"
#include "typesk.h"

/* The NT virtual-memory model (ROADMAP 16(l), item 14(d)): what
 * NtAllocateVirtualMemory, NtFreeVirtualMemory, NtProtectVirtualMemory and
 * NtQueryVirtualMemory work on.
 *
 * NT gives a page one of three STATES - free, reserved (address space held,
 * any access faults), committed (backed, accessible per its protection) -
 * and every allocation remembers its base and the protection it was made
 * with. Linux's mmap has none of that; it is "mapped or not". So each NT
 * address space carries a list of RUNS, a run being pages that agree on
 * everything NtQueryVirtualMemory reports: allocation base and protection,
 * state, protection, guard. The list is sorted, adjacent agreeing runs are
 * merged after every change, and so one run is exactly one region as
 * MEMORY_BASIC_INFORMATION describes it.
 *
 * COMMIT IS EAGER. Committing allocates and zeroes the frames there and
 * then, rather than on first touch. NT commits lazily, and so should this
 * one day (it is how a program reserves a gigabyte and touches a page); but
 * every kernel path that reads or writes user memory assumes a mapped page
 * is present, and demand-zero would have to teach all of them to fault
 * first. Eager commit keeps that invariant and costs memory a program asked
 * for anyway. Reserving stays free: a reservation maps nothing.
 *
 * PAGE_NOACCESS and PAGE_GUARD keep the page's contents. The frame stays
 * mapped, PRESENT but without PAGE_USER, so ring 3 faults on it and the
 * kernel - and a later protection change - still find the data. A guard
 * page's first touch raises STATUS_GUARD_PAGE_VIOLATION and clears the
 * guard (ntvm_guard_fault), the way stacks probe their way down on NT.
 *
 * Addresses ntvm chooses come from [NTVM_BASE, NTVM_LIMIT), clear of every
 * fixed region the kernel places (TEBs, thread stacks, TLS, the mmap window,
 * images at 0x140000000 and DLLs above, ELF objects at ELFSO_BASE), on NT's
 * 64KB allocation granularity. A caller-chosen base may be anywhere in user
 * space that nothing occupies. */

#define NTVM_GRANULARITY 0x10000ULL
#define NTVM_BASE        0x0000000800000000ULL
#define NTVM_LIMIT       0x0000004000000000ULL
#define NTVM_USER_TOP    0x00007FFFFFFF0000ULL

/* MEMORY_BASIC_INFORMATION, x64, 48 bytes. */
typedef struct __attribute__((packed)) {
    uint64 base_address;
    uint64 allocation_base;
    uint32 allocation_protect;
    uint16 partition_id;
    uint16 pad0;
    uint64 region_size;
    uint32 state;
    uint32 protect;
    uint32 type;
    uint32 pad1;
} ntvm_mbi_t;

/* Each returns an NTSTATUS. In/out base and size as the NT calls have them:
 * rounded on the way out to what was actually done. */
uint32 ntvm_allocate(address_space_t *as, uint64 *base, uint64 *size,
                     uint32 type, uint32 protect);
uint32 ntvm_free(address_space_t *as, uint64 *base, uint64 *size,
                 uint32 type);
uint32 ntvm_protect(address_space_t *as, uint64 *base, uint64 *size,
                    uint32 new_protect, uint32 *old_protect);
/* `images` lists (base, size) pairs of loaded images, `nimages` of them, so
 * a query inside one reports MEM_IMAGE; anything else mapped but not made
 * through ntvm (TEBs, stacks, the mmap window) reports MEM_PRIVATE. */
uint32 ntvm_query(address_space_t *as, uint64 addr, ntvm_mbi_t *out,
                  const uint64 *images, int nimages);

/* A ring-3 fault on a present page the user cannot reach: 1 when it was a
 * guard page - the guard is now cleared, and the caller raises
 * STATUS_GUARD_PAGE_VIOLATION - and 0 otherwise. */
int ntvm_guard_fault(address_space_t *as, uint64 addr);

/* Map a view of `section` (section.h): `offset` 64KB aligned, *view_size 0
 * for the rest of the section, *base 0 to let ntvm place it (MEM_TOP_DOWN
 * in `type` from the top). The view's protection must be one the section
 * allows. NtUnmapViewOfSection takes any address inside the view. */
uint32 ntvm_map_view(address_space_t *as, object_t *section, uint64 *base,
                     uint64 offset, uint64 *view_size, uint32 protect,
                     uint32 type);
uint32 ntvm_unmap_view(address_space_t *as, uint64 addr);
/* FlushViewOfFile: write the range of a view of a file-backed section back
 * to its file. *size 0 for the rest of the view. */
uint32 ntvm_flush(address_space_t *as, uint64 *base, uint64 *size);

/* Drop the run list (the frames go with the address space itself). */
void ntvm_destroy(address_space_t *as);

#endif
