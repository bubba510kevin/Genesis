#ifndef _SYS__MUTEX_H_
#define _SYS__MUTEX_H_
/* Upstream splits struct mtx out so a header can embed one without the whole
 * locking API. Genesis's compat sys/mutex.h already defines it; redirected
 * rather than repeated, the same as sys/_lock.h and sys/_rwlock.h. */
#include <sys/mutex.h>
#endif
