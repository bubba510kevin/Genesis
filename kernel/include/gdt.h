#ifndef GDT_H
#define GDT_H

#include "typesk.h"

#define GDT_KERNEL_CODE   0x08
#define GDT_KERNEL_DATA   0x10
#define GDT_USER_DATA     0x18   /* must precede user code: SYSRET requires it */
#define GDT_USER_CODE     0x20
#define GDT_TSS           0x28   /* 16-byte descriptor: occupies 0x28 and 0x30 */

/* Installs the GDT and TSS, reloads every segment register, and loads TR.
 * Replaces the minimal GDT the boot trampoline set up. */
void gdt_init(void);

/* Stack the CPU switches to on a ring 3 -> ring 0 transition. Set this on
 * every context switch, or the first syscall from a new task lands on the
 * previous task's kernel stack. */
void gdt_set_kernel_stack(uint64 rsp0);

/* Alternate stack for IST index 1..7. An IDT gate naming a matching index
 * switches to it unconditionally, even ring 0 to ring 0 - which is what makes
 * the double fault handler survive a broken kernel stack. */
void gdt_set_ist(int index, uint64 stack_top);

/* --- per-CPU (Part 10) ---------------------------------------------------
 * Every CPU gets its OWN GDT and TSS. The descriptors are identical, but the
 * TSS cannot be shared - two CPUs sharing one share rsp0 and all seven IST
 * stacks, so a double fault on either lands on the same emergency stack and
 * a ring transition on either loads the other's kernel stack. */
void gdt_init_ap(int cpu);
void gdt_set_ist_cpu(int cpu, int index, uint64 stack_top);

#endif
