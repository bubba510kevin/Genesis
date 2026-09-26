#ifndef GENESIS_NET_COMPAT_SYS_SYSTM_H
#define GENESIS_NET_COMPAT_SYS_SYSTM_H

/* Genesis shim, not vendored: the kernel-wide utilities every FreeBSD .c
 * file assumes, reduced to what the vendored mbuf tree actually calls. */

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/param.h>
#include <machine/atomic.h>
/* errno, because the VENDORED sys/mbuf.h uses ENOBUFS inside a _KERNEL-only
 * section and does not include it itself. That section only became visible
 * when a real driver was compiled with -D_KERNEL (which FreeBSD kernel builds
 * always pass and the vendored mbuf tree here did not need). mbuf.h is
 * md5-checked against upstream and must not be edited, and driver source
 * includes <sys/systm.h> before <sys/mbuf.h>, so this is the place it can be
 * fixed at all. */
#include <sys/errno.h>
/* And the three types a driver's softc embeds BY VALUE - a mutex, a callout
 * and a task. Real driver source does not include <sys/mutex.h>,
 * <sys/callout.h> or <sys/taskqueue.h> itself (if_rl.c does not), because on
 * FreeBSD they arrive transitively through this header's own chain. An
 * incomplete type here is not a missing declaration, it is a struct whose
 * SIZE is unknown, so the driver's softc will not compile at all.
 *
 * This is what this file's header comment already describes itself as: the
 * kernel-wide utilities every FreeBSD .c file assumes. */
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/callout.h>
#include <sys/taskqueue.h>

#include "kprintf.h"

/* --- INVARIANTS ----------------------------------------------------------
 * Deliberately NOT defined. With INVARIANTS off, upstream's KASSERT compiles
 * to nothing, sys/queue.h's QMD_ASSERT compiles to nothing, and the vendored
 * files' `__diagused` variables are the reason that annotation exists.
 *
 * The tradeoff is real and is worth stating rather than leaving implicit: a
 * corrupted mbuf gets caught later and less legibly than it would with
 * assertions on. It is off because MSIZE is 256 bytes and INVARIANTS also
 * turns on UMA's trash_ctor/trash_dtor red-zoning, which the shim in
 * kernel/bsd/uma.c does not implement - a half-on INVARIANTS build would
 * assert on its own missing machinery rather than on a real bug.
 *
 * KASSERT is still WIRED to a real panic below rather than being defined
 * away, so turning INVARIANTS on later is a one-line change here and not a
 * porting job. */

/* Real, not a no-op: prints and halts, with a symbol-name backtrace from
 * Part 2's ksyms/backtrace facility. Defined in kernel/bsd/mbuf.c.
 *
 * Note the format-string contract differs from upstream's: kprintf supports
 * %d %u %x %lx %s %% and nothing else (see kernel/include/kprintf.h). Every
 * panic() call site in the vendored fragments was checked against that set;
 * a %p or a width specifier in a newly vendored file will print literally
 * rather than crash, which is the failure mode to expect if one is added. */
void panic(const char *fmt, ...) __attribute__((noreturn));

#ifdef INVARIANTS
#define KASSERT(exp, msg)       do { if (__predict_false(!(exp))) panic msg; } while (0)
#define MPASS(exp)              KASSERT((exp), ("assertion failed: " #exp))
#else
#define KASSERT(exp, msg)       do { } while (0)
#define MPASS(exp)              do { } while (0)
#endif

#define CTASSERT(x)             _Static_assert((x), #x)

/* hashinit_flags()'s wait flags, upstream's values. They live in
 * <sys/systm.h> upstream too - kern/subr_hash.c uses them and includes
 * nothing else that would carry them. */
/* new_unrhdr()'s "do not take a lock at all" sentinel, from upstream's
 * <sys/systm.h>. Distinct from NULL, which means "use the allocator's own
 * lock". */
#define	UNR_NO_MTX	((void *)(uintptr_t)-1)

#define	HASH_NOWAIT	0x00000001
#define	HASH_WAITOK	0x00000002

/* --- variable placement annotations, copied from upstream's <sys/systm.h> --
 *
 * These are upstream's spelling verbatim, and they live here rather than in
 * <sys/cdefs.h> because that is where upstream keeps them - which is not a
 * detail: they were in the Genesis cdefs.h shim, and vendoring the real
 * cdefs.h took them away, because the real one never had them.
 *
 * They are real placements, not no-ops. .data.read_mostly and friends are
 * picked up by linker.ld's *(.data*), so a variable annotated this way ends
 * up grouped with its peers exactly as intended - the point being to keep a
 * frequently-read global off the same cache line as a frequently-written one.
 * That matters here now that there is a second CPU. */
#define	__read_mostly		__section(".data.read_mostly")
#define	__read_frequently	__section(".data.read_frequently")
#define	__exclusive_cache_line	__aligned(CACHE_LINE_SIZE) \
				    __section(".data.exclusive_cache_line")

/* __diagused marks a variable used only by assertions. With INVARIANTS off
 * (see above) every KASSERT vanishes and the variable becomes unused, which
 * is exactly what upstream's definition is for. Same two-branch shape as
 * upstream, keyed on the same macro. */
#ifdef INVARIANTS
#define	__diagused
#else
#define	__diagused	__unused
#endif

#define printf                  kprintf

/* Lowercase min/max: FreeBSD's <sys/libkern.h> spelling, which the vendored
 * chain routines use (m_pullup's "min(min(max(len, max_protohdr), space),
 * n->m_len)"). Distinct from sys/param.h's MIN/MAX only in name. */
/* min/max are NOT macros here. The vendored <sys/libkern.h> - upstream's
 * home for them - defines imax/imin/lmax/lmin/max/min as real inline
 * FUNCTIONS, and a macro of the same name turns each of those definitions
 * into a syntax error. They were macros here only while libkern.h was
 * absent.
 *
 * The behavioural difference is worth knowing: upstream's min/max are typed
 * (u_int), so a caller passing a long gets it truncated, where the macro
 * would have worked on any type. That is upstream's own hazard and vendored
 * code is written around it - which is precisely the argument for matching
 * upstream rather than being helpfully different. */

/* WITNESS is FreeBSD's lock-order verifier. There are no locks here yet
 * (Part 11 of the plan adds them), so its one use in the vendored tree -
 * mbuf.h's MBUF_CHECKSLEEP, warning about an M_WAITOK allocation while
 * holding a lock - has nothing to check. */
#define WARN_GIANTOK            1
#define WARN_SLEEPOK            2
#define WITNESS_WARN(flags, lock, fmt, ...)  do { } while (0)

/* SYSINIT used to expand to nothing here, with a comment saying Genesis has
 * no link sets and that objcopy would flatten one away anyway. The second
 * half of that was wrong: objcopy -O binary keeps every allocated section's
 * CONTENTS - it only drops the section table - and ld's __start_/__stop_
 * symbols are resolved at link time, so a link set survives perfectly well.
 * kernel/lib/ksyms_data.c already relied on exactly that property.
 *
 * It is real now. <sys/kernel.h> is VENDORED and brings upstream's own
 * SYSINIT with it; kernel/bsd/sysinit.c walks the set in subsystem order.
 * The reason to bother is the failure mode the old arrangement had: an
 * initialiser that is silently dropped and has to be re-discovered by hand
 * every time a new vendored file adds one. That cost real time twice - once
 * for uma_startup, once for ether_init/vnet_ether_init, where the missing
 * call looked like working hardware that received nothing. */

/* VM_LOW_MBUFS / VM_LOW_PAGES are the two arguments mb_reclaim() and UMA
 * pass to the vm_lowmem event. EVENTHANDLER itself is REAL now - see
 * <sys/eventhandler.h>, which is vendored, and kernel/bsd/kern_eventhandler.c,
 * which implements it. It was compiled away here, and netinet/in.c's
 * subscription to ifnet_arrival_event went with it. */
/* Upstream's values, from <vm/vm_pageout.h> and <sys/mbuf.h>: which shortage
 * the vm_lowmem / mbuf_lowmem event is reporting. A subscriber branches on
 * them, so they have to be the real bit values rather than 0 and 1. */
#define VM_LOW_KMEM             0x01
#define VM_LOW_PAGES            0x02
#define VM_LOW_MBUFS            0x04

/* --- the kernel environment, and log() ----------------------------------
 *
 * Upstream declares both of these here, in <sys/systm.h>. Implemented in
 * kernel/bsd/kern_env.c over kernel/driver/hints.c's compiled-in table -
 * which is exactly what FreeBSD's own static environment is.
 *
 * getenv_* is what every TUNABLE_*_FETCH in <sys/kernel.h> resolves to, so
 * without these a vendored file's tunables are read as "absent" and it keeps
 * its compiled-in defaults. That is usually the right answer; the point of
 * having them is that it is now a decision rather than an accident. */
char	*kern_getenv(const char *name);
void	freeenv(char *env);
int	testenv(const char *name);
int	getenv_int(const char *name, int *data);
int	getenv_uint(const char *name, unsigned int *data);
int	getenv_long(const char *name, long *data);
int	getenv_ulong(const char *name, unsigned long *data);
int	getenv_string(const char *name, char *data, int size);
int	getenv_int64(const char *name, int64_t *data);
int	getenv_uint64(const char *name, uint64_t *data);
int	getenv_quad(const char *name, quad_t *data);
int	getenv_bool(const char *name, bool *data);
bool	getenv_is_true(const char *name);
bool	getenv_is_false(const char *name);

struct mbuf;
void	log(int level, const char *fmt, ...) __printflike(2, 3);
void	log_console(struct mbuf *m);

/* --- crossing the user/kernel boundary ----------------------------------
 *
 * net/if.c's ioctl path reads an ifreq out of userland and writes one back.
 * Genesis has a real userspace and a real check for whether an address
 * belongs to it (user_ptr_ok, kernel/include/syscall.h), so these are real
 * validated copies rather than memcpy under another name - which matters,
 * because the argument being validated arrives from a user process.
 *
 * Implemented in kernel/bsd/kern_env.c. Return 0 on success and EFAULT
 * otherwise, upstream's contract. */
int copyin(const void *uaddr, void *kaddr, size_t len);
int copyout(const void *kaddr, void *uaddr, size_t len);
int copyinstr(const void *uaddr, void *kaddr, size_t len, size_t *done);

/* copyinptr and memcpy_data are upstream's CHERI-capability-aware variants -
 * on a machine where a pointer carries a capability, copying one has to
 * preserve the tag. amd64 is not such a machine, and upstream's own
 * <sys/systm.h> #defines them to the plain forms on it. Same here, same
 * spelling. */
#define	copyinptr		copyin
#define	memcpy_data		memcpy

/* Upstream's "this line cannot be reached" marker. With INVARIANTS off it is
 * the compiler builtin, which lets GCC drop the unreachable path rather than
 * emit a call to a panic that will not happen. */
#define	__assert_unreachable()	__builtin_unreachable()

/* Overwrite a pointer with a recognisable garbage value after freeing what it
 * pointed at, so a use-after-free faults at a memorable address instead of
 * following a stale but plausible pointer. Upstream only does it under
 * INVARIANTS; here it is unconditional, because this kernel has no other
 * use-after-free detection and the cost is one store. */
#define	DEBUG_POISON_POINTER(v)	((v) = (void *)0xdeadc0dedeadc0deULL)

/* Non-zero until the boot process reaches the point where sleeping is safe.
 * kern/uipc_socket.c tests it to decide whether it may block. Genesis has no
 * point after which a KERNEL context may block on a wait channel with a
 * scheduler behind it (see kernel/bsd/kern_synch.c), so this stays 1 and the
 * socket layer takes its non-blocking path - which is the honest answer and
 * not a stub. */
extern int cold;

/* --- sleep(9) ------------------------------------------------------------
 *
 * Implemented in kernel/bsd/kern_synch.c - read that file's header before
 * relying on any of these, because the mechanism differs from upstream in a
 * way that matters: there is no scheduler behind a kernel-context sleep here,
 * so a sleep IDLES the CPU rather than yielding it.
 *
 * The names below are upstream's, and each carries the lock it must drop
 * while sleeping. Macros rather than functions for the four typed variants,
 * because upstream's own <sys/systm.h> defines them that way - they funnel
 * into one implementation and differ only in which unlock to use. */
int genesis_tsleep(const void *chan, int pri, const char *wmesg, int timo);

/* The two entry points kernel/bsd/kern_condvar.c sleeps through.
 *
 * Genesis additions, not upstream: FreeBSD's condvars reach the sleep queues
 * directly, and here there is one blocking loop (gsleep, in kern_synch.c) and
 * everything that blocks goes through it. `timo` is in ticks; 0 means the
 * caller imposes no deadline of its own and the ten-second ceiling applies.
 * The lock is dropped across the sleep and - for the first of the two -
 * reacquired before returning, whichever of the three kinds it is. */
struct lock_object;
int genesis_lo_sleep(const void *chan, struct lock_object *lock, int timo);
int genesis_lo_sleep_unlock(const void *chan, struct lock_object *lock,
                            int timo);

/* --- the kernel-thread bridge -------------------------------------------
 *
 * Spawn a kernel thread; returns its pid, or 0 if none could be created.
 * genesis_kthread_id() returns the pid of the kernel thread the caller is
 * running on, or 0 if the caller is not on one.
 *
 * Declared here rather than by including kernel/include/kthread.h, for the
 * reason kernel/include/ksleep.h documents at length: that header reaches
 * kernel/include/process.h, whose `struct thread` collides with the one in
 * <sys/proc.h>, and every file in kernel/bsd/ sees the latter.
 *
 * Two functions rather than one because a taskqueue needs both: one to get a
 * servicing thread, and one to answer "am I that thread" - which is what
 * stops taskqueue_drain() from being called from inside a task and waiting
 * forever for itself. */
int genesis_kthread_spawn(void (*fn)(void *), void *arg, const char *name);
int genesis_kthread_id(void);
int genesis_mtx_sleep(const void *chan, struct mtx *mtx, int pri,
                      const char *wmesg, int timo);
/* genesis_sx_sleep and genesis_rw_sleep are declared in <sys/sx.h> and
 * <sys/rwlock.h> respectively, where their lock types are complete. Declaring
 * them here would give `struct sx` prototype scope and make every later use a
 * different, incompatible type. */
int genesis_pause(const char *wmesg, int timo);

void wakeup(const void *chan);
void wakeup_one(const void *chan);
void wakeup_any(const void *chan);

#define tsleep(chan, pri, wmesg, timo)   genesis_tsleep((chan), (pri), (wmesg), (timo))
#define msleep(chan, mtx, pri, wmesg, timo) \
        genesis_mtx_sleep((chan), (mtx), (pri), (wmesg), (timo))
#define mtx_sleep(chan, mtx, pri, wmesg, timo) \
        genesis_mtx_sleep((chan), (mtx), (pri), (wmesg), (timo))
/* pause() itself is declared further down in this file and defined in
 * kernel/bsd/kern_synch.c. Not a macro: there is a real prototype for it
 * already, and a function-like macro of the same name turns that into a
 * syntax error. */
#define pause_sbt(wmesg, sbt, pr, fl)    pause((wmesg), (int)(((sbt) * hz) >> 32))

struct thread;
void sched_prio(struct thread *td, u_char prio);

/* log() with an already-started va_list. Same file, same severity mapping. */
void vlog(int level, const char *fmt, va_list ap);

/* "May this thread block?" - upstream tests for a held spinlock or an
 * interrupt context. There is no per-thread lock accounting here, so this
 * answers yes; the one caller (net/if.c deciding between M_WAITOK and
 * M_NOWAIT) therefore always takes the sleeping path, which is the safe
 * direction because Genesis's kmalloc does not sleep either way. */
#define THREAD_CAN_SLEEP()      (1)

/* --- byte primitives -----------------------------------------------------
 * Genesis has no libc and no kernel-wide string.h; its convention is that a
 * file needing memset duplicates a small static one (kheap.c's kh_memset is
 * the precedent). These are that duplication for the mbuf tree, under the
 * names the vendored code calls them by.
 *
 * bcopy() takes (src, dst) - the OPPOSITE argument order from memcpy(). That
 * is upstream's signature, not a transcription error, and getting it
 * backwards here would silently corrupt every m_copydata() call rather than
 * fail to build. */

static __inline void *
memcpy(void *dst, const void *src, size_t len)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (len--) {
        *d++ = *s++;
    }
    return (dst);
}

static __inline void *
memmove(void *dst, const void *src, size_t len)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || len == 0) {
        return (dst);
    }
    /* Overlapping and moving forward: copy back-to-front so the source is
     * read before it is overwritten. m_copyback() and m_adj() both shuffle
     * data inside one buffer, so this case is reached in practice. */
    if (d > s && d < s + len) {
        d += len;
        s += len;
        while (len--) {
            *--d = *--s;
        }
        return (dst);
    }
    while (len--) {
        *d++ = *s++;
    }
    return (dst);
}

static __inline void *
memset(void *dst, int c, size_t len)
{
    unsigned char *d = (unsigned char *)dst;
    while (len--) {
        *d++ = (unsigned char)c;
    }
    return (dst);
}

static __inline int
memcmp(const void *a, const void *b, size_t len)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (len--) {
        if (*p != *q) {
            return ((int)*p - (int)*q);
        }
        p++;
        q++;
    }
    return (0);
}

static __inline void
bcopy(const void *src, void *dst, size_t len)
{
    (void)memmove(dst, src, len);
}

static __inline void
bzero(void *dst, size_t len)
{
    (void)memset(dst, 0, len);
}

static __inline int
bcmp(const void *a, const void *b, size_t len)
{
    return (memcmp(a, b, len));
}


/* --- added for Part 12's UMA port --------------------------------------- */

/* long-typed min/max. Upstream distinguishes these from min/max by width,
 * and using the int forms would truncate a byte count above 2GB. */
/* lmin/lmax/ulmin/ulmax now come from the VENDORED <sys/libkern.h>, which
 * upstream is where they live. They were defined here while that header was
 * absent; keeping both is a redefinition error, and the vendored one is the
 * one to keep. */
#include <sys/libkern.h>

/* fls/flsl/flsll/ffs/ffsl/ffsll all come from the VENDORED <sys/libkern.h>
 * included just above, which is upstream's home for them. They were defined
 * here while that header was absent. */

/* ratecheck() used to be #define'd to (1) here - "Genesis has no timeval
 * clock wired in, so every check passes; louder, not quieter". That was the
 * right call when the only caller was UMA's zone-full warning.
 *
 * It is the wrong call now. ip_input.c and ip_icmp.c rate-limit on received
 * packet contents, so "every check passes" means a remote sender chooses how
 * much this machine prints. There is a real implementation in
 * kernel/bsd/kern_time.c, declared by the vendored <sys/time.h>, and the
 * macro had to go for it to be reachable: a function-like macro of the same
 * name turns the declaration into a syntax error. */

/* Critical-section assertion, compiled out with the rest of the assertions. */
#define CRITICAL_ASSERT(td)   do { } while (0)

/* Which thread owns a mutex. Only used by an assertion in uma_int.h. */
#define mtx_owner(m)          ((struct thread *)0)

/* TSENTER/TSEXIT are boot-time tracing markers (the "TSLOG" facility),
 * compiled out. */
#define TSENTER()        do { } while (0)
#define TSEXIT()         do { } while (0)
#define TSENTER2(x)      do { } while (0)
#define TSEXIT2(x)       do { } while (0)
#define TSRAW(a, b, c, d) do { } while (0)

/* String helpers. These only serve UMA's sysctl-node naming, which this
 * build compiles out - but the code that calls them is still compiled, so
 * they have to exist and be correct rather than be stubs. */
/* strlen/strchr/strcmp/strlcpy come from the VENDORED <sys/libkern.h> now,
 * which is where upstream declares them - and which #defines strcpy/strcmp/
 * strlen to the compiler builtins, so a static inline of the same name here
 * is a redefinition error. The out-of-line definitions those builtins fall
 * back to live in kernel/driver/lkpi_kernel.c. */
char *genesis_bsd_strdup(const char *s);
#define strdup(s, type)  genesis_bsd_strdup(s)
int genesis_bsd_sprintf(char *buf, const char *fmt, ...);
#define sprintf          genesis_bsd_sprintf

#ifndef howmany
#define howmany(x, y)   (((x) + ((y) - 1)) / (y))
#endif
#ifndef rounddown
#define rounddown(x, y) (((x) / (y)) * (y))
#endif


/* DELAY(usec) - the spin-wait every FreeBSD driver uses for the short waits a
 * datasheet specifies. Upstream this is machine-dependent and calibrated
 * against the TSC; here it goes to the same place LinuxKPI's udelay does, and
 * carries the same limitation: the resolution is one timer tick (10ms), so a
 * DELAY(50) waits far longer than 50us. Slower than asked, never faster,
 * which is the safe direction for a hardware delay. */
void udelay(unsigned long usecs);
#define DELAY(n) udelay((unsigned long)(n))

/* snprintf. Genesis's kprintf family writes to the console, not to a buffer,
 * so this is a real small implementation in kernel/bsd/ifnet.c rather than a
 * redirect. Driver source uses it to build a name or a sysctl string. */
int snprintf(char *buf, unsigned long size, const char *fmt, ...);
int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap);

/* bootverbose - upstream's "the operator asked for a chatty boot" flag, set
 * from the loader. Genesis has no loader variables, and a driver reads this
 * to decide whether to print extra detail. Zero: a driver's normal output is
 * already the interesting part, and turning every driver's verbose branch on
 * at once would bury it. */
extern int bootverbose;

/* pause(9) - sleep for a bounded number of ticks, by name, without a wait
 * channel. Driver code uses it where it must wait and may be preempted.
 *
 * It used to busy-wait through udelay (in kernel/bsd/busdma.c). It is a real
 * idle-and-wake now, in kernel/bsd/kern_synch.c - which matters because the
 * socket layer pauses for whole ticks at a time and spinning through those
 * with interrupts effectively ignored is how a driver's own interrupt gets
 * delayed by the thing waiting for it. */
int pause(const char *wmesg, int timo);

/* msleep with a sbintime deadline. kern/uipc_sockbuf.c uses it to honour a
 * socket's SO_RCVTIMEO. The precision argument and flags are upstream's and
 * are dropped: this clock has one tick of resolution either way. */
#define msleep_sbt(chan, mtx, pri, wmesg, sbt, pr, flags) \
        genesis_mtx_sleep((chan), (mtx), (pri), (wmesg), \
                          (sbt) > 0 ? (int)(((sbt) * hz) >> 32) : 0)

/* Socket-layer sleep priority. Upstream's value; nothing here reads a
 * priority, but it appears as an argument. */
#ifndef PSOCK
#define	PSOCK		24
#define	PVM		20
#define	PRIBIO		26
#define	PZERO		22
#define	PCATCH		0x100
#define	PDROP		0x200
#define	PNOLOCK		0x400
#endif

/* asprintf(9) - format into a freshly malloc'd buffer, returning its length.
 * Used by if_ethersubr.c's ether_gen_addr to build the string it hashes.
 * Real, in kernel/bsd/ifnet.c: the alternative is a fixed stack buffer, and
 * upstream chose a heap one because the jail name has no bound. */
struct malloc_type;
int asprintf(char **ret, struct malloc_type *type, const char *fmt, ...);

#endif /* GENESIS_NET_COMPAT_SYS_SYSTM_H */
