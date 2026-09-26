#ifndef _SYS__RMLOCK_H_
#define _SYS__RMLOCK_H_

/* The rmlock STRUCTURES, adapted - see <sys/rmlock.h> for the argument.
 *
 * This file was the vendored upstream one for a while and had to be replaced:
 * it defines struct rmlock with a per-CPU tracker array, and <sys/rmlock.h>
 * here adapts rmlock onto the ordinary rwlock, so the two definitions
 * disagreed about layout. Which one a translation unit got depended on
 * whether some unrelated header (netlink's, as it turned out) reached
 * <sys/_rmlock.h> first.
 *
 * Upstream's split is kept: the STRUCTS live here so a header that only needs
 * the size of one can include this without the whole locking API, and
 * <sys/rmlock.h> supplies the operations.
 */

#include <sys/_rwlock.h>

/* Empty: with a real rwlock underneath there is nothing per-CPU to track.
 * The caller still declares one on its stack, which is why it must exist. */
struct rm_priotracker { int rmp_unused; };

struct rmlock  { struct rwlock rm_rw; };
struct rmslock { struct rwlock rms_rw; };

#endif /* _SYS__RMLOCK_H_ */
