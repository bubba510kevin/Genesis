/* The LZ4 decoder, and eight lines of glue.
 *
 * kernel/zfs/vendor/lz4_upstream.c is OpenZFS's copy of LZ4 1.9.3's
 * decompressor (BSD-2-Clause, Yann Collet) - the one file in this directory
 * that is not CDDL, which is worth keeping straight because it is also the
 * one that could move out of kernel/zfs/ if anything else ever wants it.
 *
 * zfssubr.c's compression table names `lz4_decompress` with the signature
 * below; upstream FreeBSD supplies it from stand/libsa. ZFS's framing around
 * an LZ4 block is four bytes of big-endian compressed length in front, and
 * that framing is ZFS's rather than LZ4's - which is why it is unpicked here
 * and not inside the vendored decoder. */

#include <sys/param.h>
#include <sys/types.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <zfs_libsa.h>

#include "vendor/lz4_upstream.c"

int lz4_decompress(void *s_start, void *d_start, size_t s_len, size_t d_len,
    int dummy)
{
	const unsigned char *src = (const unsigned char *)s_start;
	uint32_t bufsiz;

	(void) dummy;
	if (s_len < 4)
		return (1);

	bufsiz = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
	    ((uint32_t)src[2] << 8) | (uint32_t)src[3];

	/* The stored length must fit inside the allocated block. A larger one
	 * is corruption, and passing it through would let the decoder read
	 * past the buffer the caller allocated. */
	if (bufsiz == 0 || bufsiz + 4 > s_len)
		return (1);

	return (LZ4_uncompress_unknownOutputSize((const char *)src + 4,
	    (char *)d_start, bufsiz, d_len) < 0);
}
