#ifndef BKL_H
#define BKL_H

#include "typesk.h"

/* The big kernel lock. See ksmp.h for the model and why it is this one.
 *
 * Ownership is PER CPU, not per thread, and that is what lets the lock
 * travel across a context switch: the CPU that holds it keeps holding it
 * while schedule() swaps threads, and whichever thread comes out of the
 * switch is the one that eventually releases it - on its way back to ring 3,
 * or by idling.
 *
 * The rules, every one of which has a single enforcement point:
 *   syscall entry          acquire           syscall_dispatch
 *   syscall / new-thread   release           syscall_return (asm)
 *     exit to ring 3
 *   interrupt from ring 3  acquire; release  interrupt_dispatch
 *   interrupt while idle   acquire; release  interrupt_dispatch
 *   interrupt in kernel    nothing (held)    interrupt_dispatch
 *   idle                   release; halt;    sched_idle_loop
 *                          reacquire
 *   any in-kernel wait     release; halt;    bkl_wait_for_interrupt
 *     for an interrupt     reacquire
 *
 * Acquisition spins with interrupts OFF and services the IPI mailboxes while
 * it spins (smp_poll_mailboxes), because the holder may be waiting for this
 * CPU's answer to a TLB shootdown and would otherwise deadlock against it.
 *
 * A ticket lock: first come, first served, so a CPU hammering syscalls
 * cannot starve another out of the kernel. */

void bkl_init(void);

/* Take the lock for this CPU if it does not already hold it (depth 0 -> 1),
 * or nest (depth + 1). Must be called with interrupts off. */
void bkl_acquire(void);

/* Undo one bkl_acquire; the lock is free when the depth reaches 0. */
void bkl_release(void);

/* Release completely, returning the depth so bkl_restore can put it back -
 * for code that must halt and let other CPUs into the kernel. */
int  bkl_release_all(void);
void bkl_restore(int depth);

/* Non-zero if this CPU holds the lock. */
int  bkl_held(void);

/* The release at the exit to ring 3: drop the lock completely. Called from
 * syscall_return's assembly and at the end of interrupt_dispatch. */
void bkl_exit_to_user(void);

/* Wait for the next interrupt with the lock released and interrupts
 * enabled, then take the lock back and return with interrupts in the state
 * they were in. The replacement for every `sti; hlt; cli` in a kernel wait
 * loop: holding the lock across a halt would stop every other CPU at the
 * kernel's door until this one's interrupt came. */
void bkl_wait_for_interrupt(void);

/* How many times any CPU had to wait for the lock, and the holder's CPU. */
uint64 bkl_contended(void);
int    bkl_owner(void);

#endif
