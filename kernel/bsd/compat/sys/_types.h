#ifndef GENESIS_BSD_COMPAT_SYS__TYPES_H
#define GENESIS_BSD_COMPAT_SYS__TYPES_H

/* Genesis shim, not vendored. Upstream's <sys/_types.h> is the namespace-
 * clean set of underscore-prefixed typedefs that other headers build the
 * public names out of. Only the ones the vendored headers here reach for.
 *
 * __sbintime_t is the one that matters: it is a 64-bit fixed-point count of
 * seconds with the binary point at bit 32, so 1 second is exactly 1 << 32.
 * <sys/callout.h> stores every deadline in it, which is why it must be
 * signed and must be exactly 64 bits - a deadline subtraction has to be able
 * to go negative to mean "already due". */

#include "typesk.h"

typedef int64   __sbintime_t;
typedef int64   __time_t;
typedef int64   __suseconds_t;
typedef int64   __off_t;
typedef uint64  __size_t;
typedef int64   __ssize_t;

/* The fixed-width underscore family.
 *
 * FreeBSD headers meant to be included from userland cannot pollute the
 * namespace with uint8_t, so they are written against __uint8_t and each
 * header conditionally typedefs the public name from it. netinet/in.h,
 * sys/endian.h and most of netinet/ do this.
 *
 * Providing them HERE is what lets those headers be vendored VERBATIM
 * instead of hand-written - which is worth a great deal, because
 * netinet/in.h is the definition of an IP address and a sockaddr_in and
 * getting a field width or an order wrong there is a bug that reaches the
 * wire. sys/endian.h was hand-written before this existed, for exactly the
 * want of these six lines.
 *
 * They alias typesk.h's types rather than being independent definitions, so
 * there remains exactly one answer to "what is a 32-bit unsigned" in this
 * kernel. */
typedef uint8   __uint8_t;
typedef uint16  __uint16_t;
typedef uint32  __uint32_t;
typedef uint64  __uint64_t;
typedef int8    __int8_t;
typedef int16   __int16_t;
typedef int32   __int32_t;
typedef int64   __int64_t;
typedef uintptr __uintptr_t;
typedef int64   __intptr_t;
typedef uint32  __in_addr_t;
typedef uint16  __in_port_t;
typedef uint8   __sa_family_t;
typedef uint32  __socklen_t;
typedef int32   __pid_t;
typedef int32   __uid_t;
typedef int32   __gid_t;
typedef uint64  __uintmax_t;
typedef int64   __intmax_t;
typedef __builtin_va_list __va_list;

typedef uint16 __mode_t;
typedef uint64 __dev_t;
typedef uint64 __ino_t;
typedef uint16 __nlink_t;
typedef int64  __clock_t;
typedef int64  __register_t;
typedef uint64 __vm_offset_t;
typedef uint64 __vm_paddr_t;
typedef uint64 __vm_size_t;
typedef uint32 __nl_item;
typedef int    __ct_rune_t;
typedef uint64 __rlim_t;
typedef int64  __key_t;
typedef int32  __lwpid_t;
typedef uint32 __fixpt_t;
typedef uint64 __fsblkcnt_t;
typedef uint64 __fsfilcnt_t;
typedef uint32 __id_t;
typedef int64  __blkcnt_t;
typedef int32  __blksize_t;
typedef int32  __accmode_t;
typedef uint32 __cap_rights_t_placeholder;

/* A pointer widened to 64 bits, so a structure shared with a 32-bit
 * userland has the same layout either way. amd64's spelling, which is the
 * pointer itself. */
typedef __uintptr_t     __uint64ptr_t;

/* __clockid_t was not in the original set - it arrived with <sys/timespec.h>,
 * which declares clock_nanosleep()'s clock argument. */
#ifndef __clockid_t_defined
typedef int32           __clockid_t;
#define __clockid_t_defined
#endif

/* The strictest alignment any scalar type needs. Upstream derives it from the
 * machine's own _types.h; on amd64 that is long double's 16 bytes. Named by
 * netlink's message parser, which aligns each record. */
typedef long double     __max_align_t;

/* The <stdint.h> exact/least/fast families, in the __-prefixed form
 * <sys/stdint.h> typedefs the public names from. amd64's mapping: "least"
 * and "fast" are both just the natural type at each width, except that
 * "fast" prefers a full register for the small ones - which is why
 * __int_fast8_t is 32 bits and not 8.
 *
 * These arrived when <sys/param.h> started including <sys/stdint.h>, which it
 * had to do so that UINT16_MAX exists - net/route/route_var.h uses it. */
typedef __int8_t        __int_least8_t;
typedef __int16_t       __int_least16_t;
typedef __int32_t       __int_least32_t;
typedef __int64_t       __int_least64_t;
typedef __uint8_t       __uint_least8_t;
typedef __uint16_t      __uint_least16_t;
typedef __uint32_t      __uint_least32_t;
typedef __uint64_t      __uint_least64_t;

typedef __int32_t       __int_fast8_t;
typedef __int32_t       __int_fast16_t;
typedef __int32_t       __int_fast32_t;
typedef __int64_t       __int_fast64_t;
typedef __uint32_t      __uint_fast8_t;
typedef __uint32_t      __uint_fast16_t;
typedef __uint32_t      __uint_fast32_t;
typedef __uint64_t      __uint_fast64_t;

/* A pointer widened to 64 bits, signed. The unsigned form is above. */
typedef __intptr_t      __int64ptr_t;

/* File flags (UF_IMMUTABLE and friends) and an instruction-pointer-sized
 * integer. Named by <sys/stat.h> and <sys/resourcevar.h> respectively. */
typedef __uint32_t      __fflags_t;
typedef __uintptr_t     __uintfptr_t;

#endif /* GENESIS_BSD_COMPAT_SYS__TYPES_H */
