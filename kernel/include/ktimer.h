#ifndef KTIMER_H
#define KTIMER_H

#include "typesk.h"

/* Kernel timers: a callback run from the timer interrupt once the tick
 * count reaches `due`.
 *
 * What timerfd needs and nothing before it did. Every timed wait in this
 * kernel so far was a WAITER with a deadline (waitq_wait_until, nanosleep):
 * the sleeper itself knows when to give up. A timer is the other way round -
 * an OBJECT that becomes ready at a time, with nobody necessarily waiting on
 * it, and a poll or epoll_wait on a set containing it has no deadline of its
 * own to wake at. So something has to run at that time and tell the
 * readiness queue: this.
 *
 * The callback runs in the IRQ 0 handler on the BSP, after timer_tick, with
 * the big kernel lock held (the handler takes it), so it may change object
 * state and call waitq_wake_all - as kbd_irq does - but must not block. An
 * unsorted list: there are a handful of armed timers, and the walk is
 * cheaper than keeping order. Resolution is one tick. */

typedef struct ktimer {
    uint64          due;            /* tick at which fire runs */
    void          (*fire)(struct ktimer *t);
    struct ktimer  *next;
    uint64          fired_at;       /* tick of the last fire (ktimer_tick) */
    int             armed;
} ktimer_t;

/* Arm (or re-arm) for tick `due`; a due tick already past fires at the next
 * tick. Safe to call from a fire callback on its own timer - a timer fires at
 * most once per tick, so re-arming it for `now` from its own callback waits
 * for the next tick instead of looping. */
void ktimer_arm(ktimer_t *t, uint64 due);
void ktimer_cancel(ktimer_t *t);

/* The tick's half. Called from the IRQ 0 path. */
void ktimer_tick(void);

/* Nanoseconds -> the first tick at or after them (rounded up, so a timer
 * never fires early). */
uint64 ktimer_ns_to_tick(uint64 ns);

#endif
