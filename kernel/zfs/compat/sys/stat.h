/* Genesis compat shim: the mode bits the vendored reader tests. */
#ifndef ZFSCOMPAT_SYS_STAT_H
#define ZFSCOMPAT_SYS_STAT_H
#define S_IFMT   0170000
#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFBLK  0060000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

/* Only the four fields the vendored reader fills. A wider struct stat would
 * be inventing a syscall ABI in a compatibility header, and Genesis's own
 * stat lives in syscall.c where it belongs - zfs_vfs.c converts. */
#include <sys/types.h>
struct stat {
    uint64_t st_mode;
    uint64_t st_size;
    uint32_t st_uid;
    uint32_t st_gid;
};
#endif
