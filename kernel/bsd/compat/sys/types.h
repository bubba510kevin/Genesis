#ifndef GENESIS_NET_COMPAT_SYS_TYPES_H
#define GENESIS_NET_COMPAT_SYS_TYPES_H

/* Genesis shim, not vendored: the FreeBSD type spellings the vendored mbuf
 * files use, mapped onto kernel/include/typesk.h's real ones.
 *
 * typesk.h already provides uint8..uint64, the uintN_t aliases, u_long,
 * u_char, u_short, size_t, uintptr and NULL, so this file only adds what it
 * does NOT have. Deliberately not the other way around (a self-contained set
 * of typedefs here): two definitions of uint32_t in one translation unit is a
 * hard error, and more importantly a second set that only LOOKS the same is
 * how a struct ends up laid out differently in two files. */
#include "typesk.h"

/* <sys/bitcount.h> - population count, vendored. Upstream's sys/types.h
 * includes it from here, and sys/libkern.h's bitcountl() and sys/bitstring.h
 * both use it without including it themselves. */
#include <sys/bitcount.h>

typedef unsigned int    u_int;
typedef char           *caddr_t;
typedef const char     *c_caddr_t;
typedef uintptr         uintptr_t;
typedef int64           intptr_t;
typedef int64           ssize_t;
typedef int64           off_t;
typedef uint32          u_int32_t;
typedef uint16          u_int16_t;
typedef uint8           u_int8_t;
typedef uint64          u_int64_t;

/* A physical address as the vendored files spell it. Same width as
 * typesk.h's phys_addr_t and deliberately typedef'd FROM it rather than from
 * uint64 directly, so that if phys_addr_t ever becomes a distinct type (its
 * own comment in typesk.h says that is the intent) the mbuf tree follows
 * instead of quietly staying an integer. */
typedef phys_addr_t     vm_paddr_t;
typedef uintptr         vm_offset_t;

typedef _Bool           bool;
#define true            1
#define false           0

/* From <sys/_timeval.h> upstream. Reached only by mbuf.h's
 * mbuf_tstmp2timeval(), which converts a receive timestamp - nothing here
 * produces one, but the inline still has to compile. */
/* struct timeval comes from the VENDORED <sys/_timeval.h> now - upstream's
 * home for it, and the one every vendored network header agrees with. It was
 * defined here while that file was absent. */
#include <sys/_timeval.h>

/* struct timespec, and the clock ids beside it, from the VENDORED
 * <sys/timespec.h>. It was hand-written here (two int64s, which is the right
 * layout) until <sys/time.h> itself was vendored - and upstream's time.h
 * includes the real header, so a second definition here became a hard
 * redefinition error rather than a duplicate that happened to agree. */
#include <sys/timespec.h>

/* sbintime_t - a signed 64-bit fixed-point count of SECONDS with the binary
 * point at bit 32, so SBT_1S is 1 << 32 and adding two deadlines is plain
 * integer addition. Upstream declares it here, in sys/types.h, and the whole
 * callout and timecounter API is written in terms of it.
 *
 * It lived in the Genesis shim <sys/time.h> while that file was hand-written.
 * When time.h was replaced with the vendored one the typedef had to move to
 * where upstream keeps it, because upstream's time.h uses the type without
 * defining it. */
typedef __sbintime_t    sbintime_t;

/* Forward declarations for the FreeBSD types sys/mbuf.h names in prototypes
 * but never dereferences: the network interface, the socket layer, the
 * scatter/gather I/O descriptor, the TLS session. None of them exist in this
 * tree and none of the vendored functions that take one were vendored.
 *
 * Declared here rather than left to C's implicit rule because a struct tag
 * first seen inside a prototype's parameter list gets PROTOTYPE scope, not
 * file scope - so every prototype mentioning it declares a NEW, incompatible
 * type, and gcc warns about exactly that. One file-scope declaration makes
 * them all the same type again. */
struct uio;
struct ifnet;
struct socket;
struct sockaddr;
struct ktls_session;
struct sbuf;
struct domain;
struct thread;
struct malloc_type;
struct label;

/* uintmax_t / intmax_t - the widest integer types, which driver source uses
 * for a printf argument it wants widened rather than for storage. 64-bit
 * here, which is genuinely the widest this kernel has. */
typedef uint64 uintmax_t;
typedef int64  intmax_t;

/* time_t - seconds. int64 rather than int32 deliberately: a 32-bit time_t
 * overflows in 2038, and while nothing here will still be running then, a
 * struct laid out with the wrong width is a struct that disagrees with every
 * other FreeBSD source file compiled against it. */
typedef int64  time_t;

/* Identifier types named by vendored headers. pid_t is signed because a
 * negative value means a process GROUP in the POSIX APIs that use it. */
typedef int32  pid_t;
typedef int32  uid_t;
typedef int32  gid_t;
typedef uint16 ether_vlanid_t;
typedef int32  cpusetid_t;
typedef uint32 in_addr_t;
typedef uint16 in_port_t;

/* --- the POSIX id and file types ----------------------------------------
 *
 * Upstream's sys/types.h defines these from the __-prefixed forms in
 * <sys/_types.h>, and this file already carries that underscore family. They
 * arrived when net/if.c was vendored whole: its ioctl path reaches
 * <sys/conf.h> and <sys/file.h>, which are full of them.
 *
 * Spelled as the typedef-from-underscore-form that upstream uses rather than
 * from the underlying integer directly, so that changing a width means
 * changing it in one place and both spellings follow. */
typedef __mode_t        mode_t;
typedef __ino_t         ino_t;
typedef __nlink_t       nlink_t;
typedef __dev_t         dev_t;
typedef __accmode_t     accmode_t;
typedef __lwpid_t       lwpid_t;
typedef __id_t          id_t;
typedef __clockid_t     clockid_t;
typedef __fflags_t      fflags_t;
typedef __uintfptr_t    uintfptr_t;
typedef int32           blksize_t;
typedef int64           blkcnt_t;
typedef uint64          fsblkcnt_t;
typedef uint64          fsfilcnt_t;
typedef long            key_t;
typedef uint32          useconds_t;

/* A byte offset into a VM object. 64-bit unsigned upstream, and unsigned is
 * the part that matters: it is used for file-backed mappings whose offsets
 * are not signed quantities. */
typedef uint64          vm_ooffset_t;

/* Upstream's spelling of a truth value in the VM and device layers, distinct
 * from C's bool because it predates it. Same width as upstream's. */
typedef unsigned int    boolean_t;

/* One argument of a system call, widened to a register. <sys/ktrace.h>
 * records an array of them. */
typedef int64           syscallarg_t;

/* register_t - a machine word, the width of a general-purpose register.
 * Named by sys/fnv_hash.h to size its accumulator. */
typedef int64  register_t;
typedef uint64 u_register_t;

/* ksize_t / kvaddr_t - the kernel's own size and address types, as the
 * crash-dump and socket-statistics headers spell them. Distinct names
 * upstream so a userland tool inspecting a core can size them independently
 * of its own; identical here. */
typedef uint64 ksize_t;
typedef uint64 kvaddr_t;
typedef uint64 kpaddr_t;

/* An enum whose underlying type is one byte, so it can be a struct field of a
 * known width. Copied CHARACTER FOR CHARACTER out of upstream's sys/types.h,
 * including the GCC-13 version test and the note about it - netinet/ip_var.h
 * declares struct pf_mtag_dir with it, and a spelling that differs by one
 * attribute changes the layout of a struct that vendored code reads.
 *
 * It lived in the Genesis <sys/cdefs.h> shim until that file was replaced by
 * the vendored one. Upstream keeps it here, so here is where it goes. */
#if (defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 13))
#define __enum_uint8_decl(name)	enum enum_ ## name ## _uint8 : uint8_t
#define __enum_uint8(name)	enum enum_ ## name ## _uint8
#else
#define __enum_uint8_decl(name)	enum __attribute__((packed)) enum_ ## name ## _uint8
#define __enum_uint8(name)	enum __attribute__((packed)) enum_ ## name ## _uint8
#endif

#endif /* GENESIS_NET_COMPAT_SYS_TYPES_H */
