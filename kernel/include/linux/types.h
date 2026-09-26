#ifndef LINUX_TYPES_H
#define LINUX_TYPES_H

#include "typesk.h"

/* Real Linux driver source (vendsrc/sys/contrib/dev/...) expects these
 * names bare and unprefixed. Aliasing onto typesk.h's already-existing
 * fixed-width types costs nothing and keeps exactly one definition of
 * "what a u32 is" in this kernel - typesk.h is still the source of truth,
 * this file only renames it for code that was never written against
 * Genesis's own names.
 *
 * bool is hand-rolled rather than pulled from <stdbool.h>: this kernel has
 * no prior use of that header (unlike <stdarg.h>, confirmed already in use
 * under these exact build flags by kernel/zfs/zfs_shim.c), and typesk.h's
 * own convention is to define every type by hand rather than lean on a
 * compiler-supplied header nothing else here has proven yet. */
typedef uint8  u8;
typedef uint16 u16;
typedef uint32 u32;
typedef uint64 u64;
typedef int8   s8;
typedef int16  s16;
typedef int32  s32;
typedef int64  s64;

typedef int bool;
#define true  1
#define false 0

#endif
