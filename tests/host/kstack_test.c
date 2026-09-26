/* Host tests for kstack.c.
 *
 * The address arithmetic and the guard-hole predicate are pure, and the
 * mapping goes through the same VMM the rest of the harness already drives -
 * so "is the guard page actually unmapped" is answerable here rather than by
 * overflowing a stack on real hardware and hoping the fault is legible.
 *
 * What is not covered: whether the CPU actually lands on these stacks. That
 * needs TSS.rsp0, a real ring transition, and a SYSCALL - none of which exist
 * off-target. */

#include <stdio.h>

#include "kstack.h"
#include "paging.h"
#include "pmm.h"
#include "typesk.h"

extern void kstack_test_prepare(void);   /* provided by vmm_test.c */

static int ks_failures;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL  %s\n", what);
        ks_failures++;
    } else {
        printf("  ok    %s\n", what);
    }
}

int kstack_run_tests(void) {
    uint64 top_a, top_b;

    printf("\nkstack: allocation and guard pages\n");
    kstack_test_prepare();

    top_a = kstack_alloc(0);
    check(top_a != 0, "slot 0 allocates");
    check(top_a == KSTACK_BASE + KSTACK_STRIDE,
          "and its top is the end of the slot, since the stack grows down");
    check(kstack_base_of(0) == top_a - KSTACK_SIZE,
          "the mapped region is exactly KSTACK_SIZE below the top");

    /* Every byte of the stack proper must be mapped. A stack that is mapped
     * for most of its length works until a call chain goes deep enough. */
    {
        uint64 page;
        int all = 1;
        for (page = kstack_base_of(0); page < top_a; page += PMM_PAGE_SIZE) {
            if (vmm_get_phys_in(vmm_kernel_space(), page) == 0) {
                all = 0;
            }
        }
        check(all, "every page of the stack is mapped");
    }

    /* And the hole below it must not be. This is the assertion the whole
     * design exists for. */
    {
        uint64 page;
        int none = 1;
        for (page = KSTACK_BASE; page < kstack_base_of(0); page += PMM_PAGE_SIZE) {
            if (vmm_get_phys_in(vmm_kernel_space(), page) != 0) {
                none = 0;
            }
        }
        check(none, "the guard hole below it is unmapped");
    }

    check(kstack_is_guard(kstack_base_of(0) - 8),
          "an address just below the stack is recognised as a guard hit");
    check(!kstack_is_guard(kstack_base_of(0)),
          "the first mapped byte is not");
    check(!kstack_is_guard(top_a - 8), "nor is the top of the stack");
    check(!kstack_is_guard(0xFFFFFFFF90000000ULL),
          "an unrelated kernel address is not a guard hit");

    top_b = kstack_alloc(1);
    check(top_b != 0 && top_b != top_a, "a second slot is distinct");
    check(top_b - top_a == KSTACK_STRIDE, "and one stride away");
    /* Two stacks separated only by their own size would let an overflow in
     * one land in the other - the corruption case the guard exists to stop. */
    check(kstack_is_guard(top_a + 8),
          "the space between two stacks is a guard hole, not the next stack");

    check(kstack_alloc(0) == 0, "allocating a live slot twice is refused");
    check(kstack_alloc(-1) == 0, "a negative slot is refused");
    check(kstack_alloc(KSTACK_SLOTS) == 0, "a slot past the end is refused");

    {
        uint64 before = pmm_used_frames();
        kstack_free(1);
        check(pmm_used_frames() < before, "freeing a slot returns its frames");
        check(vmm_get_phys_in(vmm_kernel_space(), kstack_base_of(1)) == 0,
              "and unmaps it");
        check(kstack_alloc(1) != 0, "the slot can be allocated again");
        kstack_free(1);
    }
    kstack_free(0);

    return ks_failures;
}
