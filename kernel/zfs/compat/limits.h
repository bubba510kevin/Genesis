/* <limits.h> for the vendored ZFS tree.
 *
 * Only one file needs it - vendor/lz4_upstream.c, which checks UINT_MAX to
 * decide how wide an int is - and until the kernel build gained -nostdinc it
 * was answered by the host's /usr/include. That is exactly the class of thing
 * -nostdinc exists to stop, so it has to be answered here instead.
 *
 * GCC ships a perfectly good freestanding limits.h and this defers to it
 * rather than restating the numbers. The one wrinkle is that GCC's version
 * assumes it is layered ON TOP OF a system limits.h: it pulls in its own
 * syslimits.h, which does an unconditional `#include_next <limits.h>` and,
 * with no host headers on the path, fails with "no include path in which to
 * search for limits.h" - naming a file that does not need to exist.
 *
 * _LIBC_LIMITS_H_ is the flag GCC's own header tests to skip that step. It is
 * named for glibc because glibc is the usual thing doing the skipping; the
 * mechanism is not glibc-specific and this is the documented way to use that
 * header standalone.
 *
 * #include_next, not #include: this file IS limits.h as far as the search
 * path is concerned (kernel/zfs/compat is prepended for this subtree), so a
 * plain #include would find this file again.
 */
#ifndef GENESIS_ZFS_LIMITS_H
#define GENESIS_ZFS_LIMITS_H

#define _LIBC_LIMITS_H_
#include_next <limits.h>
#undef _LIBC_LIMITS_H_

#endif /* GENESIS_ZFS_LIMITS_H */
