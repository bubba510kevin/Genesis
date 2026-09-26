/* Genesis compat shim. zfssubr.c's compression table names lz4_decompress;
 * kernel/zfs/lz4_zfs_glue.c defines it over the vendored LZ4 decoder. */
#ifndef ZFSCOMPAT_LZ4_H
#define ZFSCOMPAT_LZ4_H
#include <stddef.h>
int lz4_decompress(void *src, void *dst, size_t s_len, size_t d_len, int dummy);
#endif
