#ifndef LINUX_MUTEX_H
#define LINUX_MUTEX_H

/* <linux/mutex.h>.
 *
 * A Linux mutex is a SLEEPING lock: a contending task blocks and the CPU goes
 * to somebody else. Genesis's mtx (kernel/lib/mtx.c) is a spinning lock.
 *
 * That difference is real and this header does not hide it. What it means in
 * practice: holding one of these across a long operation burns the other
 * CPU instead of yielding it, and taking one from interrupt context is
 * ALLOWED here where Linux would refuse. The first is a performance
 * difference on a two-CPU kernel with nothing much to schedule; the second is
 * a correctness difference in the permissive direction, so no driver breaks
 * because of it.
 *
 * It would become a genuine problem for a driver that sleeps while holding a
 * mutex - Linux permits that and this cannot, because a spinning waiter never
 * runs the scheduler. Nothing in this tree does it yet. Written down rather
 * than discovered.
 */

struct mutex {
    unsigned long opaque[8];
    int           initialised;
};

void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
int  mutex_trylock(struct mutex *m);
int  mutex_is_locked(struct mutex *m);

#define DEFINE_MUTEX(name) struct mutex name = { { 0 }, 0 }

/* mutex_lock_interruptible returns 0, or -EINTR if a signal arrived. Nothing
 * here can interrupt a spinning acquire, so it always succeeds and always
 * returns 0 - which is a valid outcome of the real function, so callers that
 * check are correct either way. */
#define mutex_lock_interruptible(m) (mutex_lock(m), 0)

#endif
