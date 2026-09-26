#ifndef GENESIS_BSD_COMPAT_SYS_LOCK_H
#define GENESIS_BSD_COMPAT_SYS_LOCK_H

/* Genesis shim, not vendored: just enough of FreeBSD's lock(9) to give
 * <sys/callout.h>'s `struct lock_object *c_lock` a type.
 *
 * A callout can be initialised with a lock (callout_init_mtx and friends),
 * and upstream's softclock then acquires that lock around the handler so the
 * handler and the code that scheduled it cannot race. Genesis has no mutexes
 * yet - Part 11 of the plan builds them - so kernel/bsd/callout.c accepts a
 * lock_object and records it, and does not acquire anything.
 *
 * That is an honest stub in the same sense as wdm.c's KeAcquireSpinLock: on
 * one CPU with callouts run from a tick handler, the race the lock exists to
 * close is between the handler and interrupt-disabled scheduling code, and
 * callout.c disables interrupts across the operations that need it. It stops
 * being sufficient the moment Part 10 brings up a second CPU, which is
 * exactly when Part 11 fills this in. Written down here so that is a planned
 * step rather than a discovery. */

/* --- lo_class: which KIND of lock this object fronts ---------------------
 *
 * Not upstream's spelling. FreeBSD reaches the right unlock through a
 * `struct lock_class *` and a class index packed into lo_flags, which is
 * machinery this shim does not have and does not need - there are three
 * lock types here, not eight.
 *
 * It exists because sleep(9) and condvars both have to DROP THE CALLER'S
 * LOCK and reacquire it, and both are handed a `struct lock_object *` with
 * no other clue what it is. kern_synch.c's _sleep() used to assume a mutex
 * for any non-NULL lock. That was invisible only because nothing in this
 * tree reached _sleep() directly - the vendored callers all go through the
 * mtx_sleep/sx_sleep/rm_sleep macros, each of which knows its own type - and
 * it would have called mtx_unlock on an sx the first time one did. A condvar
 * cannot dodge it that way: cv_wait(cvp, lock) is ONE entry point for all
 * three types.
 *
 * Every lock in this tree is classified, because every one of them is
 * initialised through genesis_mtx_init/genesis_sx_init/genesis_rw_init - the
 * MTX_SYSINIT/SX_SYSINIT/RW_SYSINIT macros call those too rather than
 * building a lock_object by hand. So LO_CLASS_NONE means "this lock never
 * went through its init", which is a bug in its own right; the sleep paths
 * treat it as a mutex (the behaviour before this field existed) and latch a
 * one-time complaint rather than either guessing silently or refusing to
 * sleep. */
#define LO_CLASS_NONE   0
#define LO_CLASS_MTX    1
#define LO_CLASS_SX     2
#define LO_CLASS_RW     3

typedef struct lock_object {
    const char *lo_name;
    unsigned    lo_flags;
    unsigned    lo_class;
} lock_object_t;

#define LO_INITIALIZED  0x00010000
#define LO_WITNESS      0x00020000
#define LO_RECURSABLE   0x00080000
#define LO_SLEEPABLE    0x00100000

/* The lock-assertion flags upstream's LOCK_CLASS machinery takes. Every
 * assertion here compiles away (there is no WITNESS), but the constants have
 * to exist because they appear as arguments. Upstream's values. */
#define	LA_MASKASSERT	0x000000ff
#define	LA_UNLOCKED	0x00000000
#define	LA_LOCKED	0x00000001
#define	LA_SLOCKED	0x00000002
#define	LA_XLOCKED	0x00000004
#define	LA_RECURSED	0x00000008
#define	LA_NOTRECURSED	0x00000010

#endif /* GENESIS_BSD_COMPAT_SYS_LOCK_H */
