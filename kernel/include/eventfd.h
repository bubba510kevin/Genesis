#ifndef EVENTFD_H
#define EVENTFD_H

#include "typesk.h"

struct object;

/* eventfd(2)'s counter object - see kernel/fs/eventfd.c for what it is for
 * and why the two read modes are not the same facility.
 *
 * `semaphore` selects EFD_SEMAPHORE: a read takes 1 and decrements, rather
 * than taking the whole counter and zeroing it. Returns 0 and stores the
 * object in *out, or a negative errno. */
int eventfd_create(struct object **out, uint64 initval, int semaphore);

/* Non-zero if this object is an eventfd. fstat needs it for the same reason
 * pipe_is_pipe exists: a libc works out what it is holding from st_mode. */
int eventfd_is_eventfd(const struct object *obj);

#endif
