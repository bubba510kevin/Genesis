#ifndef _SYS_EPOCH_H_
#define _SYS_EPOCH_H_

/* <sys/epoch.h> - FreeBSD's epoch-based reclamation, compiled out.
 *
 * An epoch is a deferred-free mechanism: a reader enters, walks a structure
 * without locking it, and exits; a writer that unlinks something waits for
 * every reader that could still see it to leave before freeing. The network
 * stack runs its receive path inside the "net epoch" so an interface can be
 * torn down without locking every packet.
 *
 * Genesis has no epoch, and more to the point has nothing that needs one:
 * there is no interface list to walk lock-free and no concurrent teardown
 * (nothing detaches). So NET_EPOCH_ENTER/EXIT are no-ops and the tracker a
 * driver declares on its stack is an empty struct.
 *
 * This is NOT a lock being silently dropped. An epoch protects a reader from
 * a concurrent WRITER, and there are no writers - if_free is never called
 * while a receive path is running because nothing calls it at all. If detach
 * ever becomes real, this file becomes real with it, and every call site is
 * already in the right place.
 */

struct epoch_tracker {
    int et_unused;
};

/* Embedded BY VALUE in struct ifaddr and struct ifmultiaddr, so it must be a
 * complete type even though nothing ever defers a free through it. Upstream
 * this holds the callback linkage epoch_call uses; here it is one int so the
 * containing structs have the right shape and nothing more. */
struct epoch_context {
    int ec_unused;
};
typedef struct epoch_context *epoch_context_t;

/* The deferred-free callback type. Upstream's spelling: a FUNCTION type, not
 * a pointer to one, so a declaration reads `epoch_callback_t *cb`. */
typedef void epoch_callback_t(epoch_context_t);
typedef struct epoch_tracker *epoch_tracker_t;
struct epoch;
typedef struct epoch *epoch_t;

#define NET_EPOCH_ENTER(et)          do { (void)(et); } while (0)
#define NET_EPOCH_EXIT(et)           do { (void)(et); } while (0)
#define NET_EPOCH_ENTER_ET(et)       NET_EPOCH_ENTER(et)
#define NET_EPOCH_EXIT_ET(et)        NET_EPOCH_EXIT(et)
#define NET_EPOCH_ASSERT()           do { } while (0)
#define NET_EPOCH_WAIT()             do { } while (0)
#define NET_EPOCH_CALL(f, c)         do { (void)(f); (void)(c); } while (0)
#define epoch_enter_preempt(e, et)   do { (void)(et); } while (0)
#define epoch_exit_preempt(e, et)    do { (void)(et); } while (0)

/* epoch_alloc/epoch_free and the drain barrier.
 *
 * An epoch here is a no-op (see the top of this file for the argument: there
 * are no lock-free readers to protect, because every list this kernel walks
 * is walked under a real lock). So an allocated epoch is a token that only
 * has to be distinguishable from NULL, and draining callbacks - waiting for
 * every deferred free to run - is trivially complete, because a deferred free
 * here runs immediately.
 *
 * net/if.c allocates one per interface in if_attach and drains it in
 * if_detach. Both have to exist for that to compile; neither has to do
 * anything for it to be correct HERE, and that would stop being true the
 * moment epoch stopped being a no-op. */
#define EPOCH_PREEMPT           0x1
#define EPOCH_LOCKED            0x2

static __inline epoch_t
epoch_alloc(const char *name, int flags)
{
    (void)name;
    (void)flags;
    /* A non-NULL token. Not allocated: nothing dereferences it, and a real
     * allocation here would be a leak with no matching content. */
    return ((epoch_t)(uintptr_t)1);
}

static __inline void
epoch_free(epoch_t e)
{
    (void)e;
}

#define NET_EPOCH_DRAIN_CALLBACKS()   do { } while (0)
#define epoch_drain_callbacks(e)      do { (void)(e); } while (0)

#endif
