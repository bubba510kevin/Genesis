#ifndef LINUX_SPINLOCK_H
#define LINUX_SPINLOCK_H

#include "linux/types.h"

/* <linux/spinlock.h>, over Genesis's real mtx (kernel/lib/mtx.c).
 *
 * These are genuinely real - a spin on an atomic exchange with interrupts
 * disabled across the critical section - because there is genuinely a second
 * CPU (kernel/arch/smp.c) for them to contend against. Before Part 10 of the
 * item 11 pass this header would have been a no-op stub, and ROADMAP item 11
 * exists to stop exactly that kind of placeholder outliving its excuse.
 *
 * The struct is declared here rather than being a typedef of mtx_t so that
 * driver source can embed a spinlock_t by value in its own softc - which is
 * what Linux driver source does - without this header dragging klock.h into
 * every driver translation unit. The size has to be right for that to work,
 * so it is checked at compile time rather than assumed.
 */

typedef struct spinlock {
    /* Opaque storage for a Genesis mtx_t. Sized generously and checked
     * against the real thing in kernel/driver/lkpi.c - a static assert
     * there fails the build if klock.h's mtx_t ever outgrows this, which is
     * the only way a mismatch could otherwise be discovered (silently
     * scribbling on whatever field follows the lock in a driver's softc). */
    unsigned long opaque[8];
    int           initialised;
} spinlock_t;

typedef spinlock_t raw_spinlock_t;

void spin_lock_init(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
int  spin_trylock(spinlock_t *lock);

/* The _irqsave form saves the interrupt flag into `flags` and disables
 * interrupts; _irqrestore puts it back. Linux makes these macros because
 * `flags` is written by name rather than by pointer, and that is not
 * something a function can do - so the macro spelling is required for source
 * compatibility, not a shortcut. */
unsigned long linux_spin_lock_irqsave(spinlock_t *lock);
void          linux_spin_unlock_irqrestore(spinlock_t *lock,
                                           unsigned long flags);

#define spin_lock_irqsave(lock, flags) \
    do { (flags) = linux_spin_lock_irqsave(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) \
    linux_spin_unlock_irqrestore((lock), (flags))

/* _bh is "disable software interrupts". Genesis has no softirq/bottom-half
 * layer, so there is nothing extra to disable and these are the plain forms.
 * Correct today; it would stop being correct the moment a bottom-half
 * mechanism exists, which is why it is written down here rather than left to
 * look deliberate. */
#define spin_lock_bh(l)   spin_lock(l)
#define spin_unlock_bh(l) spin_unlock(l)

#define DEFINE_SPINLOCK(name) spinlock_t name = { { 0 }, 0 }
#define __SPIN_LOCK_UNLOCKED(name) { { 0 }, 0 }

#endif
