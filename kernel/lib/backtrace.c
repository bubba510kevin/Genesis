#include "backtrace.h"
#include "kprintf.h"
#include "ksyms.h"
#include "paging.h"

#define BACKTRACE_MAX_FRAMES 32

/* Anything below the kernel half is not a return address this walk should
 * trust - either the chain has run into a leaf frame's leftover stack
 * garbage or it has wandered off the end of a real one. Stopping here
 * rather than dereferencing further is what keeps a bad frame chain from
 * turning "print a backtrace" into a second fault while already handling
 * the first one. */
#define BACKTRACE_MIN_ADDR KERNEL_VMA

static void print_frame(int index, uint64 addr) {
    uint64 offset;
    const char *name = ksym_lookup(addr, &offset);

    kprintf_c(0x0C, "  #%d %lx", index, addr);
    if (name != 0) {
        kprintf_c(0x0C, " %s+%lx", name, offset);
    }
    kprintf_c(0x0C, "\n");
}

void backtrace_print(uint64 rip, uint64 rbp, uint8 color) {
    int i;
    uint64 prev_rbp;

    (void)color;  /* frames are printed at a fixed color; see print_frame */

    print_frame(0, rip);

    prev_rbp = 0;
    for (i = 1; i < BACKTRACE_MAX_FRAMES; i++) {
        const uint64 *frame;
        uint64 saved_rbp, return_addr;

        /* Frame pointers only ever grow (the stack grows down, and each
         * caller's frame sits above its callee's) - a chain that doesn't
         * satisfy that is corrupt or cyclic, not a longer backtrace. */
        if (rbp == 0 || rbp <= prev_rbp || (rbp & 0x7) != 0 ||
            rbp < BACKTRACE_MIN_ADDR) {
            break;
        }

        frame = (const uint64 *)rbp;
        saved_rbp   = frame[0];
        return_addr = frame[1];

        if (return_addr < BACKTRACE_MIN_ADDR) {
            break;
        }

        print_frame(i, return_addr);

        prev_rbp = rbp;
        rbp = saved_rbp;
    }
}
