/* Kernel timers - see ktimer.h. */

#include "ktimer.h"
#include "timer.h"

static ktimer_t *armed_list;

static void unlink_timer(ktimer_t *t) {
    ktimer_t **at;

    for (at = &armed_list; *at != NULL; at = &(*at)->next) {
        if (*at == t) {
            *at = t->next;
            break;
        }
    }
    t->next = NULL;
    t->armed = 0;
}

void ktimer_arm(ktimer_t *t, uint64 due) {
    if (t->armed) {
        unlink_timer(t);
    }
    t->due = due;
    t->armed = 1;
    t->next = armed_list;
    armed_list = t;
}

void ktimer_cancel(ktimer_t *t) {
    if (t->armed) {
        unlink_timer(t);
    }
}

void ktimer_tick(void) {
    uint64 now = timer_ticks_now();
    ktimer_t *t;

    /* Restart from the head after every fire: the callback may re-arm its
     * own timer (pushing it to the head) or cancel another, so the list it
     * leaves is not the one being walked. A timer already fired this tick is
     * passed over, so a callback that re-arms for a tick already reached
     * cannot keep this loop going - it fires on the next tick. */
again:
    for (t = armed_list; t != NULL; t = t->next) {
        if (t->due <= now && t->fired_at != now) {
            unlink_timer(t);
            t->fired_at = now;
            t->fire(t);
            goto again;
        }
    }
}

uint64 ktimer_ns_to_tick(uint64 ns) {
    uint64 hz = timer_hz();

    /* Split to stay inside 64 bits for any ns a uint64 can hold. */
    return (ns / 1000000000ULL) * hz +
           ((ns % 1000000000ULL) * hz + 999999999ULL) / 1000000000ULL;
}
