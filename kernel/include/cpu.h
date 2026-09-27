#ifndef CPU_H
#define CPU_H

#include "typesk.h"

/* Does this CPU implement the CPUID instruction at all? Everything else here
 * depends on it. Anything from a Pentium onward does; the check is cheap and
 * is the only way to ask without risking an invalid-opcode fault. */
int cpu_has_cpuid(void);

/* Vendor string, e.g. "GenuineIntel" or "AuthenticAMD".
 * `out` must have room for 13 bytes (12 characters plus a terminator). */
void cpu_vendor(char *out);

/* Does this CPU support 64-bit long mode?
 * CPUID leaf 0x80000001, EDX bit 29. Requires checking the maximum extended
 * leaf first, because on a CPU without extended leaves, 0x80000001 returns
 * whatever the highest *basic* leaf returns rather than failing - which is a
 * fine way to conclude a Pentium supports long mode. */
int cpu_has_long_mode(void);

/* True if the CPU is already executing in long mode (EFER.LMA). Reads an MSR,
 * so only valid once cpu_has_long_mode() has said yes. */
int cpu_in_long_mode(void);

/* Does this CPU implement no-execute pages? CPUID leaf 0x80000001, EDX bit
 * 20, and subject to the same maximum-extended-leaf check as long mode above.
 *
 * This has to be asked rather than assumed, because bit 63 of a page table
 * entry is not "ignored when unsupported" - it is a RESERVED bit, and a
 * reserved bit set in a present entry faults on every access to that page
 * with a cause (error bit 3) that looks nothing like a permissions problem.
 * Setting NX without EFER.NXE therefore does not weaken enforcement, it
 * breaks the mapping. */
int cpu_has_nx(void);

/* Prints a one-line CPU summary via screen.c. Useful as a boot banner and as
 * a go/no-go check before attempting the long-mode transition. */
void cpu_report(uint8 color);

/* Enable the x87 FPU and SSE.
 *
 * SSE2 is architecturally baseline on x86-64, so every compiler emits it for
 * ordinary work - memcpy, memset, strlen, struct copies. But the instructions
 * raise #UD until CR4.OSFXSR says the OS is prepared to manage the register
 * state. The kernel builds with -mno-sse and does not need this; user code
 * does, and you do not get to recompile it.
 *
 * Must run before entering ring 3. */
void fpu_init(void);
void fpu_init_ap(void);   /* CR0/CR4 on an AP; see cpu.c */

/* --- per-thread FPU state ------------------------------------------------
 *
 * The x87, MMX and SSE registers are as much a thread's context as RIP is,
 * and nothing saved them across a context switch. That was correct while one
 * process existed and became wrong the moment two did - one thread's XMM
 * registers, rounding mode and exception mask silently became another's.
 *
 * FXSAVE_SIZE bytes, 16-byte aligned, or FXSAVE/FXRSTOR raise #GP. The
 * alignment is a property of where the caller puts the buffer, so it is
 * declared on the field in thread_t rather than checked here.
 *
 * XSAVE would be the modern answer and handles AVX state that FXSAVE drops
 * on the floor. It also needs XCR0 management and a variable-size area, and
 * FXSAVE covers everything a CPU without OSXSAVE enabled can put in those
 * registers - which is everything Genesis lets user code reach today. */
#define FXSAVE_SIZE 512

/* Fill an area with a clean starting state: x87 initialised, MXCSR at the
 * architectural default of 0x1F80 with every SSE exception masked.
 *
 * Zeroing the area instead would be a subtle disaster - MXCSR of zero UNMASKS
 * every SSE exception, so the first inexact result in a fresh process raises
 * #XF. The template is captured from the CPU itself at fpu_init, so it also
 * carries a valid MXCSR_MASK and cannot fail FXRSTOR's reserved-bit check. */
void fpu_thread_init(void *area);

void fpu_save(void *area);
void fpu_restore(const void *area);

#endif
