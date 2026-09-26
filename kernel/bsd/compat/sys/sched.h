#ifndef GENESIS_BSD_COMPAT_SYS_SCHED_H
#define GENESIS_BSD_COMPAT_SYS_SCHED_H
/* sched_bind/sched_unbind pin a thread to a CPU while UMA drains a per-CPU
 * cache. Genesis's scheduler does not run kernel threads on APs at all
 * (Part 10 is mechanism only), so there is nothing to pin and nothing that
 * could migrate. A no-op that is TRUE rather than convenient. */
#define sched_pin()        do { } while (0)
#define sched_unpin()      do { } while (0)
#define sched_bind(td, c)  do { } while (0)
#define sched_unbind(td)   do { } while (0)
#endif
