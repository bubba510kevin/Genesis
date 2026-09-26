/* NOT VENDORED. Genesis stand-in for the upstream gzip.c, which is a wrapper
 * over zlib's inflate. Genesis has no zlib, and inflate is larger than
 * everything else in this directory put together.
 *
 * A gzip-compressed block fails its read with EOPNOTSUPP and says so, which
 * is the difference between "this pool has compression=gzip and this reader
 * does not" and a file of plausible garbage. */

static int
gzip_decompress(void *s_start, void *d_start, size_t s_len, size_t d_len,
    int n)
{
	(void) s_start; (void) d_start; (void) s_len; (void) d_len; (void) n;
	printf("zfs: gzip decompression is not built into this reader\n");
	return (-1);
}
