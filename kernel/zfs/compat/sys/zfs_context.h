/* Genesis compat shim. OpenZFS's lz4.c includes this to get the kernel
 * environment; the decompressor itself needs almost none of it. Not vendored
 * code. */
#ifndef ZFSCOMPAT_SYS_ZFS_CONTEXT_H
#define ZFSCOMPAT_SYS_ZFS_CONTEXT_H
#include <sys/param.h>
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <zfs_libsa.h>
#define ASSERT(x)   ((void)0)
#define ASSERT3U(a, op, b) ((void)0)
#define ASSERT3S(a, op, b) ((void)0)
#define likely(x)   (x)
#define unlikely(x) (x)
#endif
