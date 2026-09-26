#ifndef _SYS_RMLOCK_H_
#define _SYS_RMLOCK_H_

#include <sys/rwlock.h>
#include <sys/_rmlock.h>

/* rmlock - FreeBSD's READ-MOSTLY lock, adapted onto the ordinary rwlock.
 *
 * An rmlock is optimised for the case where readers vastly outnumber
 * writers: a reader takes a per-CPU tracker and no atomic at all, and the
 * writer pays for it by having to walk every CPU's tracker. The routing
 * table and the interface address lists use one.
 *
 * Mapping it onto rwlock keeps the SEMANTICS exactly - shared readers,
 * exclusive writer - and loses only the per-CPU optimisation. That is a
 * performance difference and not a correctness one, which is why this
 * adaptation is safe where the CK_/epoch ones needed an argument.
 *
 * The tracker a caller declares on its stack becomes an empty struct: with
 * a real rwlock underneath there is nothing per-CPU to track. */

/* The three structs come from <sys/_rmlock.h> (adapted, not vendored - see
 * the note there). They were defined here until a vendored header reached
 * that file first and produced two incompatible definitions of one lock. */

/* rmslock is the SLEEPABLE read-mostly lock, a separate type upstream because
 * its read side may block. Mapped onto the same rwlock for the same reason as
 * rmlock: nothing here sleeps while holding one. kern/subr_hash.c
 * static-asserts on its size, which is why the type has to exist even though
 * nothing takes one.
 */
#define rms_init(rms, name)   rw_init(&(rms)->rms_rw, name)
#define rms_destroy(rms)      rw_destroy(&(rms)->rms_rw)
#define rms_rlock(rms)        rw_rlock(&(rms)->rms_rw)
#define rms_runlock(rms)      rw_runlock(&(rms)->rms_rw)
#define rms_wlock(rms)        rw_wlock(&(rms)->rms_rw)
#define rms_wunlock(rms)      rw_wunlock(&(rms)->rms_rw)
#define rms_assert(rms, what) do { } while (0)

#define rm_init(rm, name)             rw_init(&(rm)->rm_rw, name)
#define rm_init_flags(rm, name, opts) rw_init(&(rm)->rm_rw, name)
#define rm_destroy(rm)                rw_destroy(&(rm)->rm_rw)
#define rm_rlock(rm, tracker)         do { (void)(tracker); rw_rlock(&(rm)->rm_rw); } while (0)
#define rm_runlock(rm, tracker)       do { (void)(tracker); rw_runlock(&(rm)->rm_rw); } while (0)
#define rm_wlock(rm)                  rw_wlock(&(rm)->rm_rw)
#define rm_wunlock(rm)                rw_wunlock(&(rm)->rm_rw)
#define rm_wowned(rm)                 rw_wowned(&(rm)->rm_rw)
#define rm_assert(rm, what)           do { } while (0)
#define RM_NOWITNESS  0x01
#define RM_RECURSE    0x02
#define RM_SLEEPABLE  0x04
#define RM_NEW        0x08
#define RA_LOCKED     1
#define RA_RLOCKED    2
#define RA_WLOCKED    3
#define RA_UNLOCKED   4

#endif
