/* The glue, and it lives INSIDE the vendored translation unit on purpose.
 *
 * Every useful entry point in zfsimpl.c - zfs_lookup, dnode_read,
 * zfs_dnode_stat, zfs_mount_impl, vdev_probe, objset_get_dnode - is static.
 * That is not an oversight upstream: stand/libsa/zfs/zfs.c, the FreeBSD
 * consumer, #includes zfsimpl.c into itself for exactly this reason. This
 * file is Genesis's zfs.c, included the same way, and that is what keeps the
 * vendored files unmodified.
 *
 * NOT vendored code. Everything here is Genesis's, except zfsg_iterate, whose
 * ZAP walk is modelled closely on zfs.c's zfs_readdir - the micro/fat split
 * and the chunk-chasing are the format's, not a design choice, and writing
 * them differently would only mean getting them wrong differently.
 */

#include "zfs_genesis.h"
#include "acl_abi.h"

/* --- the System Attribute map --------------------------------------------
 *
 * Everything that uses this is in zfs_sa.inc, and it would live there too,
 * except that zfs_sa.inc's functions take a zfsg_mount_t and so have to be
 * included AFTER this struct - which cannot hold a type declared later. So
 * the type is here and the code is there, and zfs_sa.inc carries a
 * compile-time check that ZFSG_SA_NNAMES still matches its own name table.
 *
 * Cached per mount because building it is a couple of dozen ZAP lookups, and
 * because a filesystem's attribute numbering does not change while it is
 * mounted. */
#define ZFSG_SA_NNAMES 23   /* the ZPL attributes, ZPL_ATIME .. ZPL_SEQ    */
#define ZFSG_SA_MAXNUM 64   /* attribute numbers above this read as unknown */

typedef struct zfsg_sa_map {
    int      built;         /* 0 not yet, 1 ready, -1 this fs has no SA    */
    uint16_t num[ZFSG_SA_NNAMES];
    int      have[ZFSG_SA_NNAMES];
    uint16_t bynum[ZFSG_SA_MAXNUM];  /* registered length, 0 = variable    */
    int      known[ZFSG_SA_MAXNUM];
    uint64_t layouts_obj;
} zfsg_sa_map_t;

struct zfsg_mount {
    struct zfsmount mnt;
    spa_t          *spa;
    void           *ctx;
    int             in_use;
    zfsg_sa_map_t   sa;
};

/* Two, because the interesting first case is a ZFS pool mounted beside the
 * FAT root - "read-only second volume first", from the ordered TODO. */
#define ZFSG_MOUNT_MAX 4

static struct zfsg_mount zfsg_mounts[ZFSG_MOUNT_MAX];
static int zfsg_inited;

/* The System Attribute walk, and the ACL that only it can reach. Included
 * here rather than compiled separately for the same reason this file is:
 * objset_get_dnode and zap_lookup are static in zfsimpl.c. It has to come
 * before zfsg_fill, which uses it, and after struct zfsg_mount, which it
 * uses. */
#include "zfs_sa.inc"

/* Allocation - reading the space maps, which is the gate on writing. Also
 * needs zfsg_mount_t, and needs the ACE size helper zfs_sa.inc defines, so it
 * comes after it. */
#include "zfs_write.inc"

/* --- the vdev's I/O ------------------------------------------------------
 *
 * vdev_probe takes a read function and an opaque cookie and never looks
 * inside the cookie. Genesis passes the device through it, so this is the
 * whole of the I/O port: five lines, and the reason the vendored reader could
 * be dropped into a kernel it has never heard of. */

static int
zfsg_vdev_read(vdev_t *vdev __unused, void *priv, off_t offset, void *buf,
    size_t bytes)
{
	long long got = zfsg_dev_read(priv, (unsigned long long)offset, buf,
	    (unsigned long long)bytes);

	if (got < 0)
		return (EIO);
	/* A short read is a failure HERE even though it is not one at the
	 * device: the vendored caller asked for a specific block and has no
	 * way to express a partial one. Returning success with half a block
	 * would hand it a buffer whose tail is whatever was there before. */
	if ((size_t)got != bytes)
		return (EIO);
	return (0);
}

static int
zfsg_vdev_write(vdev_t *vdev, off_t offset, void *buf, size_t bytes)
{
	long long put;

	/* This used to return EROFS unconditionally, and the comment here
	 * said so: "it is the one place this port says read-only". It is no
	 * longer that place. The medium is written through the same context
	 * pointer the read path uses, so a device with no write op still
	 * answers -EROFS - the read-only case is now a property of the DEVICE
	 * rather than a decision made here.
	 *
	 * Note what still cannot happen: nothing calls this except
	 * zfsg_zio_write and vdev_probe's bootenv path, and zfsg_zio_write
	 * only ever targets space the metaslabs say is free. A pool is not
	 * modified by writing to it; it is modified by an UBERBLOCK that
	 * points at what was written. */
	if (vdev == NULL || vdev->v_priv == NULL)
		return (EROFS);

	put = zfsg_dev_write(vdev->v_priv, (unsigned long long)offset, buf,
	    (unsigned long long)bytes);
	if (put < 0)
		return (EIO);
	if ((size_t)put != bytes)
		return (EIO);
	return (0);
}

/* vdev_probe needs the medium's size to find the two labels at the end of the
 * device. Upstream gets it from the loader's device layer; Genesis has it on
 * the device_t. */
uint64_t
ldi_get_size(void *priv)
{
	return (zfsg_dev_size(priv));
}

/* --- mount --------------------------------------------------------------- */

int
zfsg_mount(void *ctx, char *name_out, unsigned int name_cap,
    zfsg_mount_t **out)
{
	struct zfsg_mount *m = NULL;
	spa_t *spa;
	int i, rc;

	*out = NULL;

	/* The vendored reader's own one-time setup: the pool list head and the
	 * CRC tables the ZAP hash needs. Upstream's zfs.c calls it from the
	 * loader's startup; Genesis has no equivalent moment, so it happens on
	 * the first mount attempt. Getting this wrong is not subtle - the pool
	 * list head is a STAILQ that has never been initialised, and the first
	 * successful probe walks off it. */
	if (!zfsg_inited) {
		zfs_init();
		zfsg_inited = 1;

		/* zfs_init allocates dnode_cache_buf - one SPA_MAXBLOCKSIZE
		 * buffer, 16MB - and does not check the result, because in a
		 * boot loader the allocation cannot fail. In a kernel with a
		 * capped heap it can, and the consequence would be a null
		 * dereference three calls deeper, inside vendored code, on a
		 * path that reads like a corrupt pool.
		 *
		 * Checking it here is possible only because this file is
		 * compiled INTO the vendored translation unit and can see its
		 * statics. That is the one advantage of the arrangement the
		 * static functions forced. */
		if (dnode_cache_buf == NULL) {
			printf("zfs: no memory for the dnode cache "
			    "(%llu bytes); not mounting\n",
			    (unsigned long long)SPA_MAXBLOCKSIZE);
			zfsg_inited = 0;
			return (ENOMEM);
		}
	}

	for (i = 0; i < ZFSG_MOUNT_MAX; i++) {
		if (!zfsg_mounts[i].in_use) {
			m = &zfsg_mounts[i];
			break;
		}
	}
	if (m == NULL)
		return (ENFILE);

	memset(m, 0, sizeof (*m));
	m->ctx = ctx;

	/* Reads the four labels, picks the newest uberblock that verifies, and
	 * builds the vdev tree. ENXIO means there was no ZFS label here at
	 * all, which is the answer a prober wants; anything else means there
	 * WAS a pool and something about it was refused, and vdev_probe has
	 * already printed which. */
	rc = vdev_probe(zfsg_vdev_read, zfsg_vdev_write, ctx, &spa);
	if (rc != 0)
		return (rc);

	/* Loads the MOS, the object directory and the pool's properties. This
	 * is where a pool with a feature the reader does not implement is
	 * refused - by name, in the vendored code's own words. */
	rc = zfs_spa_init(spa);
	if (rc != 0)
		return (rc);

	rc = zfs_mount_impl(spa, 0, &m->mnt);
	if (rc != 0)
		return (rc);

	m->spa = spa;
	m->in_use = 1;
	if (name_out != NULL && name_cap > 0)
		strlcpy(name_out, spa->spa_name, name_cap);
	*out = m;
	return (0);
}

void
zfsg_unmount(zfsg_mount_t *mount)
{
	if (mount != NULL)
		mount->in_use = 0;
}

/* --- objects -------------------------------------------------------------- */

static void
zfsg_fill(zfsg_mount_t *m, uint64_t objnum, dnode_phys_t *dn,
    zfsg_node_t *out)
{
	struct stat sb;
	unsigned long long v;

	memset(&sb, 0, sizeof (sb));
	(void) zfs_dnode_stat(m->spa, dn, &sb);

	out->objnum = objnum;
	out->size = sb.st_size;
	out->mode = sb.st_mode;
	out->uid = (unsigned int)sb.st_uid;
	out->gid = (unsigned int)sb.st_gid;
	out->pflags = 0;

	/* zfs_dnode_stat already produced uid and gid, from SA_UID_OFFSET and
	 * SA_GID_OFFSET. Those constants are right for the default ZPL layout
	 * and this overwrites them anyway, from a real walk of the layout the
	 * object actually uses.
	 *
	 * Not because the constants are known to be wrong - on every layout
	 * seen they agree, and the ZFS selftest asserts that they agree - but
	 * because "right for the default layout" is a property of the pool,
	 * not of this code, and the walk is right for any layout. The fields
	 * that made the walk necessary in the first place (the ACL) cannot be
	 * reached by constants at all, so the walk exists regardless; using it
	 * for uid and gid too costs one extra lookup against a block already
	 * in the cache, and removes a silent assumption.
	 *
	 * A failure here is not an error. A pre-v5 filesystem has no SA layout
	 * to walk, and zfs_dnode_stat's znode_phys_t path is then the only
	 * source there is - so the values it found stand. */
	if (zfsg_sa_u64(m, dn, ZFSG_SA_I_UID, &v) == 0)
		out->uid = (unsigned int)v;
	if (zfsg_sa_u64(m, dn, ZFSG_SA_I_GID, &v) == 0)
		out->gid = (unsigned int)v;
	if (zfsg_sa_u64(m, dn, ZFSG_SA_I_FLAGS, &v) == 0)
		out->pflags = v;
	/* From the MODE rather than from the dnode type. They agree, but the
	 * mode is what stat(2) reports and deriving both from one field means
	 * a directory can never be reported as a file by one caller and a
	 * directory by another. */
	out->is_dir = S_ISDIR(sb.st_mode) ? 1 : 0;
}

/* The ZPL root directory object: master node (object 1) is a ZAP whose ROOT
 * entry names it. zfs_lookup does this internally on every call; doing it
 * once at mount would mean caching it, and it is one ZAP lookup against a
 * block the cache is holding. */
static int
zfsg_root_obj(zfsg_mount_t *mount, uint64_t *out)
{
	dnode_phys_t dn;
	int rc;

	rc = objset_get_dnode(mount->spa, &mount->mnt.objset, MASTER_NODE_OBJ,
	    &dn);
	if (rc != 0)
		return (rc);
	return (zap_lookup(mount->spa, &dn, ZFS_ROOT_OBJ, 8, 1, out));
}

/* Resolve a path to an OBJECT NUMBER, component by component.
 *
 * --- Why this is here rather than a call to zfs_lookup --------------------
 * zfs_lookup returns a dnode_phys_t and not the object number it came from,
 * and Genesis needs the number: fs_node_t's opaque area is 64 bytes, a dnode
 * is 512, so what a node can carry is the number and nothing else. There is
 * no way to ask the vendored function for it afterwards - a dnode does not
 * record its own position.
 *
 * So the walk is driven from here, using the vendored zap_lookup and
 * objset_get_dnode for every step. The machinery is still the vendored
 * machinery; what is written here is the loop over path components.
 *
 * The cost of not calling zfs_lookup is SYMLINKS: it follows them and this
 * does not, so a symlink resolves to the link itself rather than its target.
 * That is a real limitation and not a hidden one - zfs_dnode_readlink is the
 * vendored function that closes it, and the check that would catch a
 * regression is a symlink in the fixture pool, which none of the three real
 * pool images has. */
static int
zfsg_walk(zfsg_mount_t *mount, const char *path, uint64_t *objnum_out,
    dnode_phys_t *dn_out)
{
	uint64_t objnum;
	dnode_phys_t dn;
	const char *p = path;
	int rc;

	if (path == NULL || path[0] != '/')
		return (EINVAL);

	rc = zfsg_root_obj(mount, &objnum);
	if (rc != 0)
		return (rc);
	rc = objset_get_dnode(mount->spa, &mount->mnt.objset, objnum, &dn);
	if (rc != 0)
		return (rc);

	while (*p != '\0') {
		char element[256];
		size_t n = 0;
		uint64_t value;
		struct stat sb;

		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		while (*p != '\0' && *p != '/') {
			if (n + 1 >= sizeof (element))
				return (ENAMETOOLONG);
			element[n++] = *p++;
		}
		element[n] = '\0';

		/* Descending through something that is not a directory is
		 * ENOTDIR and not ENOENT: the shell prints different messages
		 * for them and they mean different things about what was
		 * typed. */
		memset(&sb, 0, sizeof (sb));
		rc = zfs_dnode_stat(mount->spa, &dn, &sb);
		if (rc != 0)
			return (rc);
		if (!S_ISDIR(sb.st_mode))
			return (ENOTDIR);

		rc = zap_lookup(mount->spa, &dn, element, 8, 1, &value);
		if (rc != 0)
			return (rc == ENOENT ? ENOENT : rc);

		objnum = ZFS_DIRENT_OBJ(value);
		rc = objset_get_dnode(mount->spa, &mount->mnt.objset, objnum,
		    &dn);
		if (rc != 0)
			return (rc);
	}

	*objnum_out = objnum;
	*dn_out = dn;
	return (0);
}

int
zfsg_lookup(zfsg_mount_t *mount, const char *path, zfsg_node_t *out)
{
	dnode_phys_t dn;
	uint64_t objnum;
	int rc;

	if (mount == NULL || !mount->in_use)
		return (ENXIO);

	rc = zfsg_walk(mount, path, &objnum, &dn);
	if (rc != 0)
		return (rc);

	zfsg_fill(mount, objnum, &dn, out);
	return (0);
}

int
zfsg_stat(zfsg_mount_t *mount, unsigned long long objnum, zfsg_node_t *out)
{
	dnode_phys_t dn;
	int rc;

	if (mount == NULL || !mount->in_use)
		return (ENXIO);
	rc = objset_get_dnode(mount->spa, &mount->mnt.objset, objnum, &dn);
	if (rc != 0)
		return (rc);
	zfsg_fill(mount, objnum, &dn, out);
	return (0);
}

long long
zfsg_read(zfsg_mount_t *mount, unsigned long long objnum,
    unsigned long long offset, void *buf, unsigned long long len)
{
	dnode_phys_t dn;
	struct stat sb;
	int rc;

	if (mount == NULL || !mount->in_use)
		return (-ENXIO);

	rc = objset_get_dnode(mount->spa, &mount->mnt.objset, objnum, &dn);
	if (rc != 0)
		return (-rc);

	memset(&sb, 0, sizeof (sb));
	rc = zfs_dnode_stat(mount->spa, &dn, &sb);
	if (rc != 0)
		return (-rc);

	/* Clamped to the file's size, because dnode_read reads BLOCKS: a read
	 * that runs off the end of the file but stays inside the last block
	 * would otherwise return the padding as data, and the caller would
	 * have no way to tell. */
	if (offset >= sb.st_size)
		return (0);
	if (len > sb.st_size - offset)
		len = sb.st_size - offset;
	if (len == 0)
		return (0);

	rc = dnode_read(mount->spa, &dn, (off_t)offset, buf, (size_t)len);
	if (rc != 0)
		return (-rc);
	return ((long long)len);
}

/* --- directories ----------------------------------------------------------
 *
 * The ZAP in both its forms. A small directory is a MICROZAP - fixed 64-byte
 * entries in one block - and a large one is a FATZAP, a tree of leaf blocks
 * whose entries chase chunk chains to reassemble a name.
 *
 * Modelled on zfs.c's zfs_readdir, which is the reference for this walk. The
 * shape is the format's: an entry names its first name-chunk, each chunk
 * holds 21 bytes and the index of the next, and a leaf's chunk count comes
 * from the block size. */

int
zfsg_iterate(zfsg_mount_t *mount, unsigned long long objnum, zfsg_dir_cb cb,
    void *ctx)
{
	dnode_phys_t dn;
	uint64_t zap_type;
	size_t bsize;
	int rc;

	if (mount == NULL || !mount->in_use)
		return (ENXIO);
	rc = objset_get_dnode(mount->spa, &mount->mnt.objset, objnum, &dn);
	if (rc != 0)
		return (rc);

	bsize = dn.dn_datablkszsec << SPA_MINBLOCKSHIFT;
	rc = dnode_read(mount->spa, &dn, 0, &zap_type, sizeof (zap_type));
	if (rc != 0)
		return (rc);

	if (zap_type == ZBT_MICRO) {
		size_t off;

		for (off = offsetof(mzap_phys_t, mz_chunk); off < bsize;
		    off += sizeof (mzap_ent_phys_t)) {
			mzap_ent_phys_t mze;
			char name[MZAP_NAME_LEN + 1];

			rc = dnode_read(mount->spa, &dn, off, &mze,
			    sizeof (mze));
			if (rc != 0)
				return (rc);
			if (mze.mze_name[0] == '\0')
				continue;

			strlcpy(name, mze.mze_name, sizeof (name));
			rc = cb(name, ZFS_DIRENT_OBJ(mze.mze_value),
			    S_ISDIR((uint64_t)ZFS_DIRENT_TYPE(mze.mze_value) << 12), ctx);
			if (rc != 0)
				return (rc);
		}
		return (0);
	}

	if (zap_type != ZBT_HEADER) {
		/* Neither form. A directory object that is not a ZAP at all is
		 * a corrupt pool, not a format this reader is missing. */
		return (EIO);
	}

	{
		uint64_t num_leafs;
		zap_leaf_t zl;
		uint8_t *leaf;
		uint64_t blk;

		rc = dnode_read(mount->spa, &dn,
		    offsetof(zap_phys_t, zap_num_leafs), &num_leafs,
		    sizeof (num_leafs));
		if (rc != 0)
			return (rc);

		leaf = malloc(bsize);
		if (leaf == NULL)
			return (ENOMEM);

		zl.l_bs = ilog2(bsize);
		zl.l_phys = (zap_leaf_phys_t *)leaf;

		for (blk = 1; blk <= num_leafs; blk++) {
			int chunk;

			rc = dnode_read(mount->spa, &dn, blk * bsize, leaf,
			    bsize);
			if (rc != 0) {
				free(leaf);
				return (rc);
			}
			if (zl.l_phys->l_hdr.lh_block_type != ZBT_LEAF)
				continue;

			for (chunk = 0; chunk < ZAP_LEAF_NUMCHUNKS(&zl);
			    chunk++) {
				zap_leaf_chunk_t *zc = &ZAP_LEAF_CHUNK(&zl,
				    chunk);
				zap_leaf_chunk_t *nc;
				char name[256];
				size_t namelen;
				size_t n = 0;
				uint64_t value;
				int i;

				if (zc->l_entry.le_type != ZAP_CHUNK_ENTRY)
					continue;

				namelen = zc->l_entry.le_name_numints;
				if (namelen > sizeof (name))
					namelen = sizeof (name);

				/* The name is a chain of 21-byte chunks. */
				nc = &ZAP_LEAF_CHUNK(&zl,
				    zc->l_entry.le_name_chunk);
				while (namelen > 0) {
					size_t take = namelen;

					if (take > ZAP_LEAF_ARRAY_BYTES)
						take = ZAP_LEAF_ARRAY_BYTES;
					for (i = 0; i < (int)take; i++)
						name[n++] = nc->l_array.la_array[i];
					namelen -= take;
					if (namelen == 0)
						break;
					if (nc->l_array.la_next >=
					    ZAP_LEAF_NUMCHUNKS(&zl))
						break;
					nc = &ZAP_LEAF_CHUNK(&zl,
					    nc->l_array.la_next);
				}
				name[n < sizeof (name) ? n : sizeof (name) - 1]
				    = '\0';

				/* A directory entry's value is one 64-bit
				 * integer; anything else in a directory is a
				 * ZAP this reader should not be walking. */
				if (zc->l_entry.le_value_intlen != 8 ||
				    zc->l_entry.le_value_numints != 1)
					continue;
				nc = &ZAP_LEAF_CHUNK(&zl,
				    zc->l_entry.le_value_chunk);
				value = 0;
				for (i = 0; i < 8; i++)
					value = (value << 8) |
					    nc->l_array.la_array[i];

				rc = cb(name, ZFS_DIRENT_OBJ(value),
				    S_ISDIR((uint64_t)ZFS_DIRENT_TYPE(value) << 12), ctx);
				if (rc != 0) {
					free(leaf);
					return (rc);
				}
			}
		}
		free(leaf);
	}
	return (0);
}
