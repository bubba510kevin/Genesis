#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "typesk.h"

/* What the ISR stubs in idt.c hand to the dispatcher. The order is the reverse
 * of the push sequence, because the stack grows down: the last register pushed
 * sits at the lowest address and so comes first here.
 *
 * In long mode SS and RSP are pushed on EVERY interrupt, not only on a
 * privilege change as in 32-bit. One frame layout, no special case. */
struct interrupt_frame {
    uint64 r15, r14, r13, r12, r11, r10, r9, r8;
    uint64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64 vector;
    uint64 error_code;
    uint64 rip, cs, rflags, rsp, ss;   /* pushed by the CPU */
};

void interrupt_dispatch(struct interrupt_frame *frame);

/* Spurious-interrupt and unbound-dynamic-vector counts, one line, the same
 * posture as pci_report/e820_report. Both are counted rather than printed as
 * they happen: an unbound vector firing in a loop would otherwise take the
 * machine down through the console rather than through the fault. A non-zero
 * unhandled count after a clean boot means a device was programmed with a
 * vector nobody bound - which is the interesting failure, and is invisible
 * without this. */
void interrupt_report(uint8 color);

#endif
