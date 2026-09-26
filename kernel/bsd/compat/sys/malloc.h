#ifndef GENESIS_NET_COMPAT_SYS_MALLOC_H
#define GENESIS_NET_COMPAT_SYS_MALLOC_H

/* Genesis shim, not vendored: malloc(9)'s flags and its type-tag machinery.
 *
 * The malloc types (M_PACKET_TAGS and friends) are upstream's per-subsystem
 * accounting buckets, reachable from `vmstat -m`. Genesis has no such
 * reporting, so MALLOC_DEFINE reduces to a real but inert struct - real
 * rather than an empty macro so that the vendored files' MALLOC_DEFINE lines
 * still declare something with the right name, and so adding accounting later
 * means filling in this struct rather than reinstating declarations.
 *
 * --- M_WAITOK is a lie here, and that matters ---------------------------
 * On FreeBSD, malloc(M_WAITOK) cannot fail: it sleeps until memory exists,
 * and callers written against it do not check for NULL. Genesis's kmalloc
 * never sleeps - it fails. So an M_WAITOK allocation CAN return NULL in this
 * tree, and any vendored code that skips the NULL check will fault instead of
 * blocking.
 *
 * Every vendored fragment under kernel/bsd/vendor/ was read for this: the
 * mbuf allocation paths all check, because they are also reachable with
 * M_NOWAIT from interrupt context and upstream has to handle NULL there
 * anyway. A newly vendored file that only ever uses M_WAITOK is the case to
 * watch for, and the reason this is written down here rather than assumed. */

#include <sys/types.h>

#define M_NOWAIT        0x0001  /* do not block */
#define M_WAITOK        0x0002  /* ok to block - see the caveat above */
#define M_ZERO          0x0100  /* zero the allocation */
#define M_NOVM          0x0200
#define M_USE_RESERVE   0x0400
#define M_NODUMP        0x0800
#define M_FIRSTFIT      0x1000
#define M_BESTFIT       0x2000
#define M_EXEC          0x4000
#define M_NEVERFREED    0x8000

struct malloc_type {
    const char *ks_shortdesc;
};

#define MALLOC_DEFINE(type, shortdesc, longdesc) \
    struct malloc_type type[1] = { { shortdesc } }
#define MALLOC_DECLARE(type) \
    extern struct malloc_type type[1]

void *malloc(size_t size, struct malloc_type *type, int flags);
void free(void *addr, struct malloc_type *type);


/* M_TEMP - upstream's catch-all malloc type for short-lived allocations.
 * Genesis's malloc shim ignores the type entirely, so this only has to
 * exist. */
#ifndef M_TEMP
extern struct malloc_type genesis_m_temp[1];
#define M_TEMP  genesis_m_temp
#endif

/* NUMA-aware allocation. One memory domain here, so the domainset argument
 * names the only choice there is and these are the plain forms. Upstream's
 * signatures, so a caller that passes DOMAINSET_PREF(n) still compiles. */
#define malloc_domainset(size, type, ds, flags)  malloc((size), (type), (flags))
#define malloc_domainset_aligned(size, align, type, ds, flags) \
    malloc((size), (type), (flags))
#define free_domain(addr, type)                  free((addr), (type))

/* mallocarray - malloc(n * size) with the multiplication checked.
 *
 * The check is the entire reason the function exists: `n * size` overflowing
 * produces a SMALL allocation for a caller that then writes n elements into
 * it. Upstream returns NULL on overflow, and so does this.
 *
 * The routing table allocates its per-fib array through it, sized from a
 * tunable, which is exactly the shape of caller the overflow check is for. */
static __inline void *
mallocarray(size_t n, size_t size, struct malloc_type *type, int flags)
{
    if (size != 0 && n > (size_t)-1 / size) {
        return (NULL);
    }
    return (malloc(n * size, type, flags));
}

#endif /* GENESIS_NET_COMPAT_SYS_MALLOC_H */
