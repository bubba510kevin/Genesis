#ifndef KSLEEP_H
#define KSLEEP_H

#include "typesk.h"

/* The blocking half of sleep(9), for kernel/bsd/kern_synch.c.
 *
 * --- why this is its own header, and this narrow ---------------------------
 * kern_synch.c is compiled against the vendored FreeBSD headers, and
 * <sys/proc.h> defines `struct thread`. So does kernel/include/process.h.
 * Including both in one translation unit is two definitions of one tag, and
 * the error names whichever header the compiler reached second - which is
 * why kern_synch.c could not simply call waitq_wait_until() and sched_block()
 * itself, and why the file's own header said a kernel thread abstraction was
 * "what would replace it" rather than doing the replacing.
 *
 * This is that abstraction, cut down to the three calls kern_synch.c needs
 * and depending on nothing but typesk.h. Everything it hides - the wait
 * queues, proc_current(), the interrupt discipline - lives in
 * kernel/proc/ksleep.c, on the Genesis side of the header split.
 *
 * --- what it does NOT do ---------------------------------------------------
 * It does not own the wait CHANNELS. The hash from a channel pointer to a
 * slot, and the generation counter that closes the lost-wakeup window,
 * remain in kern_synch.c where sleep(9)'s semantics belong. This header
 * takes a slot INDEX and a generation to compare against, and its only job
 * is to get the caller off the CPU until one of them changes.
 */

/* Wait-channel slots. Must match kern_synch.c's SLEEP_HASH_SIZE, which
 * #defines itself to this so the two cannot drift. */
#define KSLEEP_SLOTS 64

/* Non-zero if the caller is on something that can be descheduled: a kernel
 * thread, or a process inside a system call - anything but an interrupt
 * handler and a CPU's idle thread. It used to be kernel threads only, which
 * made a system call that slept in BSD code (a blocking TCP connect) halt
 * its CPU in place instead of yielding - and the thread that would have
 * completed the connect never got to run.
 *
 * This is the question that decides whether a sleep blocks or idles, and it
 * has to be asked at the sleep rather than answered once at boot: the same
 * tsleep() call in the same driver runs on a kernel thread in one path and
 * inside an interrupt handler in another, and an interrupt handler has
 * nothing to deschedule. Answering yes there would block the handler and
 * take the interrupt's wakeup with it. */
int ksleep_can_block(void);

/* Empty every channel queue. Called once, from kthread_init(). */
void ksleep_init(void);

/* Block until *gen differs from `seen`, `timo` ticks pass, or something else
 * wakes this thread. Returns non-zero if *gen changed.
 *
 * `timo` is RELATIVE, in timer ticks, and 0 means "no deadline of my own" -
 * the caller is expected to have imposed one already (kern_synch.c does; see
 * GSLEEP_MAX_TICKS there for why a sleep with no ceiling is not offered).
 *
 * Only legal when ksleep_can_block() is non-zero. Returns 0 immediately
 * otherwise rather than blocking a context that cannot be resumed, so a
 * caller that forgets the check gets a spin rather than a dead machine. */
int ksleep_wait(unsigned int slot, const volatile uint32 *gen, uint32 seen,
                uint32 timo);

/* Make every thread blocked on `slot` runnable. Safe from an interrupt
 * handler - it only marks processes ready and sets the reschedule flag. */
void ksleep_wake(unsigned int slot);

#endif
