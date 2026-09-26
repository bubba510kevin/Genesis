/* Genesis compat shim for the vendored ZFS reader. Not vendored code. */
#ifndef ZFSCOMPAT_SYS_CDEFS_H
#define ZFSCOMPAT_SYS_CDEFS_H
#ifndef __unused
#define __unused __attribute__((__unused__))
#endif
#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __printflike
#define __printflike(a, b)
#endif
#ifndef __attribute__
#endif
/* FreeBSD's queue.h uses __containerof in STAILQ_LAST. */
#ifndef __containerof
#define __containerof(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - __builtin_offsetof(type, member)))
#endif
#ifndef __DECONST
#define __DECONST(type, var) ((type)(unsigned long)(const void *)(var))
#endif
#ifndef __predict_false
#define __predict_false(x) (x)
#define __predict_true(x)  (x)
#endif
#endif
