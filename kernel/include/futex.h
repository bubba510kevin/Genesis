#ifndef FUTEX_H
#define FUTEX_H

#include "typesk.h"

/* The fast userspace mutex, to the extent musl's locking needs one.
 *
 * --- What a futex is, in one paragraph ------------------------------------
 * A lock whose UNCONTENDED path never enters the kernel. Userspace owns a
 * word; taking and releasing a free lock is an atomic instruction on that
 * word and nothing else. The kernel is called only when a thread must
 * actually wait, and only two operations are needed for that: "sleep until
 * this word stops holding the value I just read" and "wake whoever is
 * sleeping on this word". Everything else pthreads offers - mutexes,
 * condition variables, semaphores, joins - is built in userspace out of those
 * two.
 *
 * That is why this file is small and why it is the only kernel support
 * threads need beyond clone(). It is also why the value check inside
 * futex_wait cannot be skipped: userspace tested the word, decided to wait,
 * and the state can change in the gap between those two. The kernel re-tests
 * under the same conditions the waker will wake under, which is what closes
 * the lost-wakeup race.
 *
 * --- What is deliberately absent ------------------------------------------
 * FUTEX_REQUEUE, FUTEX_WAKE_OP and the priority-inheritance operations. musl
 * does not use them for basic locking, and each is a distinct protocol rather
 * than a variation on these two - approximating one with WAIT and WAKE
 * produces something that works under low contention and deadlocks under
 * high, which is the worst possible testing profile.
 *
 * FUTEX_PRIVATE_FLAG is accepted and ignored, which is correct rather than
 * lazy: it is an optimisation hint saying the futex is not shared between
 * processes, and this implementation keys on the physical address, so it is
 * already correct for both cases. Honouring the hint would mean a second
 * code path that is faster and can disagree. */

/* Sleep until *uaddr stops being `expected`, or somebody wakes this address.
 *
 * `timeout_ticks` is an ABSOLUTE deadline in timer ticks, or 0 for none - the
 * same convention waitq_wait_until uses, and absolute for the reason given
 * there.
 *
 * Returns 0 on a normal wake, -EAGAIN if the word did not hold `expected` in
 * the first place (which is not an error - it is the caller being told the
 * contention is already over), -EINTR on a signal, -ETIMEDOUT on the
 * deadline, or -EFAULT if the address is unmapped or misaligned. */
int64 futex_wait(uint64 uaddr, uint32 expected, uint64 timeout_ticks);

/* Wake threads sleeping on `uaddr`. Returns the number reported woken, or a
 * negative errno. See the definition for why every waiter in the bucket is
 * woken regardless of `count`. */
int64 futex_wake(uint64 uaddr, int count);

/* futex_wake with the result discarded and a null address tolerated.
 *
 * This exists for exactly one caller: the exit path, clearing
 * clear_child_tid. It is a separate function rather than a call with the
 * result cast away because the exit path must not acquire an error to handle
 * - a thread that is already dying cannot do anything useful with -EFAULT,
 * and the version of this that checks would have to decide what, on a path
 * where the answer is always "nothing". */
void futex_wake_addr(uint64 uaddr);

#endif
