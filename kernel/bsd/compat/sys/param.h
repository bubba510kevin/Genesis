#ifndef GENESIS_NET_COMPAT_SYS_PARAM_H
#define GENESIS_NET_COMPAT_SYS_PARAM_H

/* The machine-dependent constants sys/param.h is layered on: PAGE_SIZE,
 * MAXCPU, CACHE_LINE_SIZE, PHYS_TO_DMAP. Upstream includes it here too. */
#include <machine/param.h>

/* Genesis shim, not vendored - but every VALUE below is copied from
 * vendsrc/sys/sys/param.h and vendsrc/sys/amd64/include/param.h rather than
 * chosen. These are not tunables here: sys/mbuf.h computes MLEN and MHLEN
 * from MSIZE minus real offsetof() results, kern_mbuf.c has a
 * _Static_assert(sizeof(struct mbuf) <= MSIZE), and m_gettype()/m_getzone()
 * switch on the cluster sizes by value. Change one and either the build
 * fails or the mbuf layout silently stops matching what the vendored code
 * was written against. */

#include <sys/cdefs.h>
/* <sys/stdint.h> for the INTn_MAX / UINTn_MAX family. Upstream's own
 * sys/param.h pulls these in transitively; here they were missing until
 * net/route/route_var.h used UINT16_MAX, and the error named the vendored
 * header rather than anything about this file. */
#include <sys/stdint.h>
#include <sys/types.h>

#define PAGE_SHIFT      12
#define PAGE_SIZE       (1 << PAGE_SHIFT)       /* amd64/include/param.h */
#define PAGE_MASK       (PAGE_SIZE - 1)

#define MSIZE           256                     /* size of an mbuf */
#define MCLSHIFT        11                      /* bytes -> mbuf clusters */
#define MCLBYTES        (1 << MCLSHIFT)         /* size of an mbuf cluster */
#define MJUMPAGESIZE    PAGE_SIZE               /* jumbo cluster, one page */
#define MJUM9BYTES      (9 * 1024)              /* jumbo cluster 9k */
#define MJUM16BYTES     (16 * 1024)             /* jumbo cluster 16k */

#define NBBY            8                       /* bits in a byte */

#define nitems(x)       (sizeof((x)) / sizeof((x)[0]))
#define offsetof(t, f)  __builtin_offsetof(t, f)
#define howmany(x, y)   (((x) + ((y) - 1)) / (y))
#define rounddown(x, y) (((x) / (y)) * (y))
#define roundup(x, y)   ((((x) + ((y) - 1)) / (y)) * (y))
#define roundup2(x, y)  (((x) + ((y) - 1)) & (~((y) - 1)))
#define powerof2(x)     ((((x) - 1) & (x)) == 0)   /* upstream's, verbatim */
#define trunc_page(x)   ((x) & ~PAGE_MASK)
#define round_page(x)   (((x) + PAGE_MASK) & ~PAGE_MASK)

#ifndef MIN
#define MIN(a, b)       (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)       (((a) > (b)) ? (a) : (b))
#endif

/* From <sys/limits.h> upstream; kern_mbuf.c's mbufq initialisers use it. */
#define INT_MAX         0x7fffffff
#define UINT_MAX        0xffffffffU

#endif /* GENESIS_NET_COMPAT_SYS_PARAM_H */
/* --- added for Part 12's UMA port --------------------------------------- */

/* Sleep priorities. Genesis's msleep shim ignores them (see
 * kernel/bsd/uma_vendor.c), but the constants have to exist for the call
 * sites to parse. */
#ifndef PVM
#define PVM      84
#define PRIBIO   80
#define PZERO    64
#define PWAIT    96
#define PDROP    0x1000
#define PCATCH   0x2000
#endif

#ifndef __size_t
#define __size_t size_t
/* One cache line. 64 on every x86-64 part this kernel runs on. Used to pad
 * a lock-free ring's producer and consumer indices apart so the two CPUs
 * touching them do not bounce the same line between their caches - which is
 * a real and measurable cost, not a formality. */
#ifndef CACHE_LINE_SHIFT
#define CACHE_LINE_SHIFT 6
#define CACHE_LINE_SIZE  (1 << CACHE_LINE_SHIFT)
#endif

/* Path and hostname limits, named by vendored headers. Genesis's own
 * filesystem limit is FAT_PATH_MAX (kernel/include/fat.h) at 256; 1024 here
 * is upstream's value and is only ever used to SIZE a buffer in vendored
 * code, never to validate a Genesis path - so the two do not have to agree
 * and deliberately are not conflated. */
#ifndef MAXPATHLEN
#define MAXPATHLEN      1024
#endif
#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN  256
#endif
#ifndef NGROUPS
#define NGROUPS         16
#endif

/* Name-length limits upstream keeps in <sys/param.h>. Reached through
 * <sys/conf.h> and <sys/ktrace.h> by net/if.c's ioctl path. Upstream's
 * values, because they size structures that are copied to userland. */
#ifndef MAXCOMLEN
#define	MAXCOMLEN	19		/* max command name remembered */
#endif
#ifndef SPECNAMELEN
#define	SPECNAMELEN	255		/* max length of devicename */
#endif

/* The largest I/O the block layer will issue in one go. A tunable upstream;
 * a constant here, because nothing adjusts it. */
#ifndef maxphys
#define	maxphys		(1024 * 1024)
#endif

/* The <limits.h> spellings, for the two places vendored code uses them
 * instead of the <stdint.h> forms above. Same values. */
#ifndef USHRT_MAX
#define	USHRT_MAX	0xffff
#define	SHRT_MAX	0x7fff
#define	UCHAR_MAX	0xff
#define	CHAR_BIT	8
#endif

#endif
