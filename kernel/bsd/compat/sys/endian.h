#ifndef _SYS_ENDIAN_H_
#define _SYS_ENDIAN_H_

#include <sys/types.h>

/* <sys/endian.h> - byte-order conversion, hand-written rather than vendored.
 *
 * The real one WAS tried verbatim first, which is this tree's default. It
 * does not stand alone: it is written against FreeBSD's __uint8_t /
 * __uint16_t / ... underscore type family from <sys/_types.h>, and pulling
 * that in means pulling in FreeBSD's whole type system alongside the one
 * kernel/bsd/compat/sys/types.h already defines - two definitions of
 * uint32_t in one translation unit, which is exactly the failure that file's
 * own header comment warns about.
 *
 * So: endian.h is a DEPENDENCY, not a consumer, and the tree's posture is to
 * vendor consumers and shim dependencies. This is the shim.
 *
 * x86-64 is little-endian, so the le* conversions are identity and the be*
 * ones are real byte swaps. Written as __builtin_bswap rather than by hand:
 * GCC emits a single bswap instruction, and a hand-rolled shift-and-mask
 * version is both slower and a place to make a mistake nobody would see
 * until a packet header came out wrong.
 */

#define _LITTLE_ENDIAN  1234
#define _BIG_ENDIAN     4321
#define _PDP_ENDIAN     3412
#define _BYTE_ORDER     _LITTLE_ENDIAN

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN   _LITTLE_ENDIAN
#define BIG_ENDIAN      _BIG_ENDIAN
#define PDP_ENDIAN      _PDP_ENDIAN
#define BYTE_ORDER      _BYTE_ORDER
#endif

static __inline uint16 __bswap16(uint16 v) { return __builtin_bswap16(v); }
static __inline uint32 __bswap32(uint32 v) { return __builtin_bswap32(v); }
static __inline uint64 __bswap64(uint64 v) { return __builtin_bswap64(v); }

#define bswap16(x) __bswap16(x)
#define bswap32(x) __bswap32(x)
#define bswap64(x) __bswap64(x)

/* Host-to-network and back. Network order is big-endian, so on x86 these
 * are the swaps and NOT the identity - getting this backwards produces a
 * driver that works perfectly against itself and against nothing else. */
#define htobe16(x) __bswap16(x)
#define htobe32(x) __bswap32(x)
#define htobe64(x) __bswap64(x)
#define betoh16(x) __bswap16(x)
#define betoh32(x) __bswap32(x)
#define betoh64(x) __bswap64(x)
#define be16toh(x) __bswap16(x)
#define be32toh(x) __bswap32(x)
#define be64toh(x) __bswap64(x)

#define htole16(x) ((uint16)(x))
#define htole32(x) ((uint32)(x))
#define htole64(x) ((uint64)(x))
#define letoh16(x) ((uint16)(x))
#define letoh32(x) ((uint32)(x))
#define letoh64(x) ((uint64)(x))
#define le16toh(x) ((uint16)(x))
#define le32toh(x) ((uint32)(x))
#define le64toh(x) ((uint64)(x))

#ifndef ntohs
#define ntohs(x) __bswap16(x)
#define ntohl(x) __bswap32(x)
#define htons(x) __bswap16(x)
#define htonl(x) __bswap32(x)
#endif

/* Unaligned load/store at a known byte order. Driver code uses these on
 * packet buffers, where the field is at whatever offset the protocol put it
 * and may straddle a word boundary. Byte-at-a-time on purpose: on x86 an
 * unaligned load is legal but these must also be correct if this ever
 * compiles for an architecture where it is not. */
static __inline uint16 be16dec(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    return (uint16)((b[0] << 8) | b[1]);
}
static __inline uint32 be32dec(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    return ((uint32)b[0] << 24) | ((uint32)b[1] << 16) |
           ((uint32)b[2] << 8) | b[3];
}
static __inline uint16 le16dec(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    return (uint16)((b[1] << 8) | b[0]);
}
static __inline uint32 le32dec(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    return ((uint32)b[3] << 24) | ((uint32)b[2] << 16) |
           ((uint32)b[1] << 8) | b[0];
}
static __inline void be16enc(void *p, uint16 v) {
    unsigned char *b = (unsigned char *)p;
    b[0] = (unsigned char)(v >> 8); b[1] = (unsigned char)v;
}
static __inline void be32enc(void *p, uint32 v) {
    unsigned char *b = (unsigned char *)p;
    b[0] = (unsigned char)(v >> 24); b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);  b[3] = (unsigned char)v;
}
static __inline void le16enc(void *p, uint16 v) {
    unsigned char *b = (unsigned char *)p;
    b[0] = (unsigned char)v; b[1] = (unsigned char)(v >> 8);
}
static __inline void le32enc(void *p, uint32 v) {
    unsigned char *b = (unsigned char *)p;
    b[0] = (unsigned char)v;         b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16); b[3] = (unsigned char)(v >> 24);
}

/* The double-underscore spellings. netinet/in.h's inline ntohl/htonl are
 * written in terms of these, because on some architectures they are compiler
 * builtins or single instructions and upstream wants the option. Here they
 * are the same swaps. */
#define __htonl(x) __bswap32((uint32)(x))
#define __htons(x) __bswap16((uint16)(x))
#define __ntohl(x) __bswap32((uint32)(x))
#define __ntohs(x) __bswap16((uint16)(x))

#endif
