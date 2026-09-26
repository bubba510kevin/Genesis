#ifndef _SYS__LOCK_H_
#define _SYS__LOCK_H_
/* Upstream splits the lock TYPES into sys/_lock.h so a header can embed one
 * without pulling in the whole locking API. Genesis's compat sys/lock.h
 * already defines struct lock_object, and two definitions of it is a
 * redefinition error rather than a harmless duplicate - so this redirects
 * rather than repeating. */
#include <sys/lock.h>
#endif
