/* Genesis compat shim. x86-64 is little-endian; these are the four
 * conversions the vendored reader uses and nothing more. */
#ifndef ZFSCOMPAT_SYS_ENDIAN_H
#define ZFSCOMPAT_SYS_ENDIAN_H
#include <stdint.h>
/* x86-64 only, and asserted rather than detected: a big-endian Genesis does
 * not exist, and a wrong guess here silently byteswaps every nvlist. */
#define _LITTLE_ENDIAN 1234
#define _BIG_ENDIAN    4321
#define _BYTE_ORDER    _LITTLE_ENDIAN
#define LITTLE_ENDIAN  _LITTLE_ENDIAN
#define BIG_ENDIAN     _BIG_ENDIAN
#define BYTE_ORDER     _BYTE_ORDER
static inline uint16_t zfs_bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint32_t zfs_bswap32(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8) |
           ((v & 0x0000FF00u) << 8)  | ((v & 0x000000FFu) << 24);
}
static inline uint64_t zfs_bswap64(uint64_t v) {
    return ((uint64_t)zfs_bswap32((uint32_t)v) << 32) |
            (uint64_t)zfs_bswap32((uint32_t)(v >> 32));
}
#define htobe16(x) zfs_bswap16(x)
#define htobe32(x) zfs_bswap32(x)
#define htobe64(x) zfs_bswap64(x)
#define be16toh(x) zfs_bswap16(x)
#define be32toh(x) zfs_bswap32(x)
#define be64toh(x) zfs_bswap64(x)
#define htole16(x) (x)
#define htole32(x) (x)
#define htole64(x) (x)
#define le16toh(x) (x)
#define le32toh(x) (x)
#define le64toh(x) (x)

/* The decode helpers nvlist.c uses to walk an XDR stream. Big-endian by
 * definition - XDR is - so these are conversions and not byte order
 * assumptions about the host. */
static inline uint32_t be32dec(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}
static inline uint64_t be64dec(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    return ((uint64_t)be32dec(b) << 32) | (uint64_t)be32dec(b + 4);
}
static inline void be32enc(void *p, uint32_t v) {
    unsigned char *b = (unsigned char *)p;
    b[0] = (unsigned char)(v >> 24); b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);  b[3] = (unsigned char)v;
}
static inline void be64enc(void *p, uint64_t v) {
    be32enc(p, (uint32_t)(v >> 32));
    be32enc((unsigned char *)p + 4, (uint32_t)v);
}
#endif
