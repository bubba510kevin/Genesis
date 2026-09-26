#ifndef ZFSCOMPAT_SYS_PARAM_H
#define ZFSCOMPAT_SYS_PARAM_H
#include <sys/cdefs.h>
#include <stddef.h>
#include <stdint.h>
#ifndef NBBY
#define NBBY 8
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif
#ifndef roundup
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#endif
#endif
/* From sys/fs/zfs.h upstream; the vendored reader uses only this one. */
/* FreeBSD spells "elements in this array" this way. */
#ifndef nitems
#define nitems(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef ZFS_MAXNAMELEN
#define ZFS_MAXNAMELEN 256
#endif
