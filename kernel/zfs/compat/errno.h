/* Genesis compat shim: the errnos the vendored reader returns.
 * POSITIVE, because that is what the FreeBSD code returns; the Genesis
 * binding in zfs_vfs.c is the one place they are negated. */
#ifndef ZFSCOMPAT_ERRNO_H
#define ZFSCOMPAT_ERRNO_H
#define EPERM        1
#define ENOENT       2
#define EIO          5
#define ENXIO        6
#define ENOMEM      12
#define EACCES      13
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define ENOSPC      28
#define EROFS       30
#define ERANGE      34
#define ENAMETOOLONG 36
#define ENOSYS      38
#define EMLINK      31
#define ELOOP       40
#define ESTALE      70
#define EOVERFLOW   75
#define EOPNOTSUPP  95
#define ENOTSUP     95
/* Added for the ZFS write path: a transaction whose allocations did not
 * settle within its pass budget is ABANDONED, and the caller is told to try
 * again rather than told the pool is broken - because it is not. Nothing was
 * made visible, so the pool is exactly what it was. */
#define EAGAIN      11
#define E2BIG        7
#define ECKSUM      97
#endif
