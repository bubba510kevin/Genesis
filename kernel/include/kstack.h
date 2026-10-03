#ifndef KSTACK_H
#define KSTACK_H

#include "typesk.h"

/* Per-thread kernel stacks.
 *
 * --- Why per thread and not per process ---
 * A kernel stack holds the state of a blocked syscall. Two threads of one
 * process can sit in read() at the same time, and nothing about sharing an
 * address space lets them share the stack that describes where each one
 * stopped. Linux, FreeBSD and Windows all landed here for that reason; the
 * sizes they picked differ, the arrangement does not.
 *
 * --- Where they live ---
 * The kernel half, always. A kernel stack inside a process's user space
 * disappears the instant CR3 changes - including during the context switch
 * doing the changing, which faults on the next instruction with no output.
 * The upper half is shared by every address space (vmm_space_create copies
 * the kernel PML4 entries), so a stack mapped here is reachable no matter
 * which process is running.
 *
 * --- Guard pages ---
 * Each slot is a stride wide and only the top KSTACK_SIZE is mapped; the
 * rest is a hole. Overflow then faults on an unmapped page with a readable
 * CR2 instead of quietly writing into whatever is below - which, for a
 * kernel compiled -mno-red-zone with recursive page-table walks in it, is
 * the difference between a diagnosable fault and memory corruption that
 * surfaces somewhere unrelated. */

#define KSTACK_SIZE     0x4000ULL   /* 16KB, matching Linux's x86-64 choice */
#define KSTACK_STRIDE   0x8000ULL   /* 32KB: the stack plus its guard hole  */
#define KSTACK_SLOTS    256  /* one per process slot - MAX_PROCESSES; process.c asserts it */

/* Above the kernel heap (0xFFFFFFFF90000000) and inside the same PDPT entry
 * as the rest of the kernel half. Fallback only now - see kstack_init()
 * below; kept as the literal value kstack_is_guard/slot_start use if
 * kstack_init() was never called or the kernel VA allocator (vmalloc.h,
 * ROADMAP item 11) had nothing left to give. */
#define KSTACK_BASE     0xFFFFFFFFA0000000ULL

/* Reserve this slot table's VA range from the kernel VA allocator
 * (vmalloc.h) instead of trusting KSTACK_BASE not to collide with
 * KHEAP_START/PE_DRIVER_BASE by comment alone. Call once, after kvm_init()
 * and before the first kstack_alloc(). Safe to skip - KSTACK_BASE remains
 * correct, just no longer collision-checked against the other two. */
void kstack_init(void);

/* Map a stack for `slot` and return its TOP - the value RSP should hold, and
 * what gdt_set_kernel_stack and the syscall entry both want. Returns 0 if the
 * slot is out of range or memory is exhausted.
 *
 * The stack grows DOWN from the returned address, so the top is one past the
 * highest mapped byte. */
uint64 kstack_alloc(int slot);

/* Unmap and free a slot's pages. The guard hole was never mapped, so there is
 * nothing to release there. */
void kstack_free(int slot);

/* Lowest mapped address of a slot - one page above its guard hole. Useful for
 * a fault handler deciding whether a CR2 landed in a guard page rather than
 * being an ordinary bad pointer. */
uint64 kstack_base_of(int slot);

/* Non-zero if `addr` is inside some slot's guard hole. This is what turns
 * "page fault at a strange address" into "thread N overflowed its kernel
 * stack" in the exception handler. */
int kstack_is_guard(uint64 addr);

#endif
