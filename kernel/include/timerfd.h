#ifndef TIMERFD_H
#define TIMERFD_H

#include "typesk.h"

struct object;

/* timerfd(2): a timer as a descriptor - readable once it has expired, and a
 * read returns how many times it has since the last read. See
 * kernel/fs/timerfd.c. Times are nanoseconds on the clock given at create
 * (CLOCK_REALTIME is converted to the monotonic clock when the timer is
 * set, so a later change of the wall clock does not move it). */

#define TFD_TIMER_ABSTIME  1u

int  timerfd_create(struct object **out, int clockid);
int  timerfd_is_timerfd(const struct object *obj);

/* Arm (value_ns != 0) or disarm (value_ns == 0). `value_ns` is relative,
 * or an absolute time on the timer's clock with TFD_TIMER_ABSTIME. The
 * previous setting, as timerfd_gettime reports it, comes back in old_*. */
int  timerfd_settime(struct object *obj, uint32 flags, uint64 value_ns,
                     uint64 interval_ns, uint64 *old_value_ns,
                     uint64 *old_interval_ns);
void timerfd_gettime(struct object *obj, uint64 *value_ns,
                     uint64 *interval_ns);

#endif
