/* Genesis compat shim: the Solaris-flavoured integer names the vendored
 * reader's headers use. Not vendored code. */
#ifndef ZFSCOMPAT_SYS_TYPES_H
#define ZFSCOMPAT_SYS_TYPES_H
#include <stddef.h>
#include <stdint.h>
typedef uint8_t  uchar_t;
typedef uint16_t ushort_t;
typedef uint32_t uint_t;
typedef unsigned long ulong_t;
typedef int64_t  offset_t;
typedef uint64_t u_offset_t;
typedef int64_t  off_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int64_t  ssize_t_compat;
#endif
