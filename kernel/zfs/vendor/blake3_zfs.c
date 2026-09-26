/* NOT VENDORED. Genesis stand-in for the upstream blake3_zfs.c.
 *
 * zfssubr.c includes this file by name and its checksum table names these
 * four symbols. Upstream's version pulls in the whole BLAKE3 implementation;
 * this one refuses.
 *
 * Refusing means producing a checksum that cannot match, so a block protected
 * by BLAKE3 fails verification and the read fails - loudly, at the block,
 * rather than by handing back unverified bytes. A pool whose datasets are set
 * to checksum=blake3 therefore reads nothing, which is the honest state of
 * affairs until this file is replaced by the real one. */

static void
zio_checksum_blake3_native(const void *buf, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	(void) buf; (void) size; (void) ctx_template;
	printf("zfs: blake3 checksums are not built into this reader\n");
	ZIO_SET_CHECKSUM(zcp, 0, 0, 0, 0);
}

static void
zio_checksum_blake3_byteswap(const void *buf, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	zio_checksum_blake3_native(buf, size, ctx_template, zcp);
}

static void *
zio_checksum_blake3_tmpl_init(const zio_cksum_salt_t *salt)
{
	(void) salt;
	return (NULL);
}

static void
zio_checksum_blake3_tmpl_free(void *ctx_template)
{
	(void) ctx_template;
}
