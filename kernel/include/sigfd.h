#ifndef SIGFD_H
#define SIGFD_H

#include "typesk.h"

struct object;
struct process;

/* signalfd(2) and pidfd_open(2) - kernel/proc/sigfd.c.
 *
 * A signalfd is a mask: reading it takes signals in that mask from the
 * READING thread's pending set (lowest first) as 128-byte signalfd_siginfo
 * records, which is how Linux defines it - the descriptor names no thread.
 * The caller blocks those signals so they stay pending instead of being
 * delivered.
 *
 * A pidfd names a process: readable once the whole process has ended (its
 * process object is signalled, proc_group_finished), and the target of
 * pidfd_send_signal and waitid(P_PIDFD). */

int  signalfd_create(struct object **out, uint64 mask);
int  signalfd_set_mask(struct object *obj, uint64 mask);   /* -EINVAL: not one */

int  pidfd_create(struct object **out, struct process *leader);
int  pidfd_pid(const struct object *obj);                   /* -1: not one */

#endif
