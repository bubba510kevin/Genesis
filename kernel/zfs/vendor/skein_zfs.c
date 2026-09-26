/* NOT VENDORED. Genesis stand-in for the upstream skein_zfs.c; see
 * blake3_zfs.c in this directory for the argument. Skein needs its own
 * implementation, this refuses, and a skein-checksummed block fails
 * verification rather than being trusted. */

static void
zio_checksum_skein_native(const void *buf, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	(void) buf; (void) size; (void) ctx_template;
	printf("zfs: skein checksums are not built into this reader\n");
	ZIO_SET_CHECKSUM(zcp, 0, 0, 0, 0);
}

static void
zio_checksum_skein_byteswap(const void *buf, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	zio_checksum_skein_native(buf, size, ctx_template, zcp);
}

static void *
zio_checksum_skein_tmpl_init(const zio_cksum_salt_t *salt)
{
	(void) salt;
	return (NULL);
}

static void
zio_checksum_skein_tmpl_free(void *ctx_template)
{
	(void) ctx_template;
}
