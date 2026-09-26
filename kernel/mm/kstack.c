#include "kstack.h"
#include "paging.h"
#include "pmm.h"
#include "typesk.h"
#include "vmalloc.h"

/* Slot layout, lowest address first:
 *
 *   base ---> [ guard hole, KSTACK_STRIDE - KSTACK_SIZE, UNMAPPED ]
 *             [ stack, KSTACK_SIZE, mapped RW kernel-only          ]
 *   top  ---> (one past the end; RSP starts here and grows down)
 *
 * Putting the guard BELOW the stack rather than above is the whole point:
 * a stack grows down, so overflow runs off the bottom. */

static int slot_mapped[KSTACK_SLOTS];
static uint64 region_base = KSTACK_BASE;

void kstack_init(void) {
    uint64 allocated = kvm_alloc_range((uint64)KSTACK_SLOTS * KSTACK_STRIDE,
                                       KSTACK_STRIDE);
    if (allocated != 0) {
        region_base = allocated;
    }
}

static uint64 slot_start(int slot) {
    return region_base + (uint64)slot * KSTACK_STRIDE;
}

uint64 kstack_base_of(int slot) {
    if (slot < 0 || slot >= KSTACK_SLOTS) {
        return 0;
    }
    return slot_start(slot) + (KSTACK_STRIDE - KSTACK_SIZE);
}

uint64 kstack_alloc(int slot) {
    uint64 base, top, page;

    if (slot < 0 || slot >= KSTACK_SLOTS || slot_mapped[slot]) {
        return 0;
    }

    base = kstack_base_of(slot);
    top  = slot_start(slot) + KSTACK_STRIDE;

    for (page = base; page < top; page += PMM_PAGE_SIZE) {
        /* No PAGE_USER: ring 3 must never be able to read a kernel stack.
         * It holds saved registers and syscall arguments from every thread
         * that has run on it. */
        if (vmm_alloc_page_in(vmm_kernel_space(), page,
                              PAGE_PRESENT | PAGE_RW) == 0) {
            /* Unwind what was mapped. A half-mapped stack is worse than none:
             * it works until a call chain goes deep enough to reach the hole. */
            uint64 undo;
            for (undo = base; undo < page; undo += PMM_PAGE_SIZE) {
                vmm_unmap_page_in(vmm_kernel_space(), undo, VMM_FREE_FRAME);
            }
            return 0;
        }
    }

    /* Zero it. A stack handed to a new thread with the previous occupant's
     * data still in it leaks whatever that thread had on its stack, and makes
     * uninitialised-local bugs behave differently depending on who ran
     * before. */
    {
        uint8 *p = (uint8 *)base;
        uint64 i;
        for (i = 0; i < KSTACK_SIZE; i++) {
            p[i] = 0;
        }
    }

    slot_mapped[slot] = 1;
    return top;
}

void kstack_free(int slot) {
    uint64 base, top, page;

    if (slot < 0 || slot >= KSTACK_SLOTS || !slot_mapped[slot]) {
        return;
    }
    base = kstack_base_of(slot);
    top  = slot_start(slot) + KSTACK_STRIDE;

    for (page = base; page < top; page += PMM_PAGE_SIZE) {
        vmm_unmap_page_in(vmm_kernel_space(), page, VMM_FREE_FRAME);
    }
    slot_mapped[slot] = 0;
}

int kstack_is_guard(uint64 addr) {
    uint64 offset, within;

    if (addr < region_base ||
        addr >= region_base + (uint64)KSTACK_SLOTS * KSTACK_STRIDE) {
        return 0;
    }
    offset = addr - region_base;
    within = offset % KSTACK_STRIDE;
    return within < (KSTACK_STRIDE - KSTACK_SIZE);
}
