/* Build a real ZFS pool containing a real ZPL filesystem with real files,
 * entirely in userland, using OpenZFS's own libzpool - the same DMU, ZAP,
 * ZIO and checksum code the kernel module runs. No /dev/zfs required; this is
 * the trick ztest uses.
 *
 * The filesystem is built at ZPL version 1, whose file attributes live in a
 * znode_phys_t bonus buffer rather than in the System Attributes registry.
 * That is deliberate: it is a format the vendored reader has to handle
 * anyway, and building it needs no SA layout construction.
 */
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/dmu.h>
#include <sys/zap.h>
#include <sys/dmu_tx.h>
#include <sys/vdev_impl.h>
#include <sys/zfs_znode.h>
#include <sys/fs/zfs.h>
#include <stdio.h>
#include <string.h>

/* znode_phys_t is the pre-SA attribute layout. The Ubuntu dev package does
 * not ship zfs_sa.h's copy of it, and it is a fixed on-disk structure, so it
 * is written out here rather than chased through headers. */
typedef struct {
	uint64_t zp_atime[2];
	uint64_t zp_mtime[2];
	uint64_t zp_ctime[2];
	uint64_t zp_crtime[2];
	uint64_t zp_gen;
	uint64_t zp_mode;
	uint64_t zp_size;
	uint64_t zp_parent;
	uint64_t zp_links;
	uint64_t zp_xattr;
	uint64_t zp_rdev;
	uint64_t zp_flags;
	uint64_t zp_uid;
	uint64_t zp_gid;
	uint64_t zp_zap;
	uint64_t zp_pad[3];
	uint8_t  zp_acl[88];
} znode_phys_t;

extern int dmu_objset_own(const char *name, dmu_objset_type_t type,
    boolean_t readonly, boolean_t decrypt, const void *tag, objset_t **osp);
extern void dmu_objset_disown(objset_t *os, boolean_t decrypt,
    const void *tag);
extern dmu_objset_type_t dmu_objset_type(objset_t *os);

#define ZPL_VERSION_1 1ULL
#define MASTER_NODE   1

/* dmu_tx_assign's second argument was TXG_WAIT until OpenZFS 2.3, which
 * renamed it DMU_TX_WAIT and changed the parameter from an enum to a flags
 * word. Same value, same meaning; spelled here so this file builds against
 * either. */
#ifndef DMU_TX_WAIT
#define DMU_TX_WAIT TXG_WAIT
#endif

/* --- the PRE-SA ACL, which is the whole reason this fixture grew a file ----
 *
 * A ZPL version 1 filesystem keeps its ACL inside the znode's bonus buffer,
 * in the 88 bytes of zp_acl, rather than in the System Attribute registry a
 * version 5 filesystem uses. Two different structures overlay those 88 bytes
 * and z_acl_version says which:
 *
 *   version 0 (INITIAL)   offset 8 is a uint32 COUNT, and the entries are
 *                         fixed 12-byte zfs_oldace_t
 *   version 1 (FUID)      offset 8 is a uint32 SIZE IN BYTES, offset 14 is a
 *                         uint16 count, and the entries are the same
 *                         variable-width form a v5 pool stores
 *
 * This writes version 0, because that is the one a genuinely old pool has and
 * the one no other fixture in the tree exercises.
 *
 * Note the FIELD ORDER of an old ACE against a modern one. Modern:
 * {uint16 type, uint16 flags, uint32 mask}, who afterwards if there is one.
 * Old: {uint32 who, uint32 mask, uint16 flags, uint16 type} - who FIRST and
 * type LAST. A decoder that reuses the modern layout here reads the low half
 * of the uid as the ACE type, which for uid 1001 is type 0x03E9: not a
 * recognised type, so it would be skipped rather than misapplied - quietly
 * losing an entry instead of failing. That is the failure this fixture
 * exists to make visible. */
typedef struct {
	uint32_t z_fuid;
	uint32_t z_access_mask;
	uint16_t z_flags;
	uint16_t z_type;
} zfs_oldace_disk_t;

typedef struct {
	uint64_t z_acl_extern_obj;
	uint32_t z_acl_count;
	uint16_t z_acl_version;
	uint16_t z_acl_pad;
	zfs_oldace_disk_t z_ace_data[6];
} zfs_acl_phys_v0_disk_t;

/* 88 bytes, exactly the size of znode_phys_t.zp_acl. A build failure here
 * means one of the two has been mis-transcribed. */
typedef char acl_v0_fits[(sizeof (zfs_acl_phys_v0_disk_t) == 88) ? 1 : -1];

#define OLD_ACE_ALLOW  0x0000
#define OLD_ACE_DENY   0x0001
#define OLD_ACE_OWNER      0x1000
#define OLD_ACE_GROUP      0x2000
#define OLD_ACE_EVERYONE   0x4000
#define OLD_ACE_IDENT_GROUP 0x0040

#define OLD_READ_DATA    0x00000001
#define OLD_WRITE_DATA   0x00000002
#define OLD_APPEND_DATA  0x00000004
#define OLD_EXECUTE      0x00000020
#define OLD_READ_ATTRS   0x00000080
#define OLD_WRITE_ATTRS  0x00000100
#define OLD_READ_ACL     0x00020000
#define OLD_WRITE_ACL    0x00040000
#define OLD_WRITE_OWNER  0x00080000
#define OLD_SYNCHRONIZE  0x00100000

static const char *secret_body = "v1 acl guards this\n";

static const char *file_body =
    "genesis reads zfs\n"
    "this file was written by OpenZFS itself, in userland, through libzpool\n";

static void
set_znode_owned(objset_t *os, uint64_t obj, uint64_t mode, uint64_t size,
    uint64_t links, uint64_t parent, uint64_t uid, uint64_t gid,
    const zfs_acl_phys_v0_disk_t *acl, dmu_tx_t *tx)
{
	dmu_buf_t *db;
	znode_phys_t *zp;

	VERIFY0(dmu_bonus_hold(os, obj, FTAG, &db));
	dmu_buf_will_dirty(db, tx);
	zp = db->db_data;
	memset(zp, 0, sizeof (*zp));
	zp->zp_mode = mode;
	zp->zp_size = size;
	zp->zp_links = links;
	zp->zp_parent = parent;
	zp->zp_uid = uid;
	zp->zp_gid = gid;
	zp->zp_gen = 1;
	if (acl != NULL) {
		memcpy(zp->zp_acl, acl, sizeof (*acl));
	}
	dmu_buf_rele(db, FTAG);
}

static void
set_znode(objset_t *os, uint64_t obj, uint64_t mode, uint64_t size,
    uint64_t links, uint64_t parent, dmu_tx_t *tx)
{
	set_znode_owned(os, obj, mode, size, links, parent, 0, 0, NULL, tx);
}

/* The same four-entry ACL genesisacl.dat carries, in the old encoding.
 *
 * Deliberately the same shape, so the two fixtures differ ONLY in how the ACL
 * is stored and a test can compare the decoded results directly. uid 1001 may
 * read a file whose mode is 0600 and which it does not own; uid 1002 may not.
 *
 * Order is load-bearing exactly as it is in the modern encoding: the DENY
 * everyone@ has to come after the two allows, or it denies the owner too. */
static void
build_v0_acl(zfs_acl_phys_v0_disk_t *a)
{
	memset(a, 0, sizeof (*a));
	a->z_acl_extern_obj = 0;         /* it fits inline; no spill object */
	a->z_acl_version = 0;            /* ZFS_ACL_VERSION_INITIAL */
	a->z_acl_count = 4;

	a->z_ace_data[0].z_fuid = 0;     /* ignored: the flags carry the who */
	a->z_ace_data[0].z_type = OLD_ACE_ALLOW;
	a->z_ace_data[0].z_flags = OLD_ACE_OWNER;
	a->z_ace_data[0].z_access_mask = OLD_READ_DATA | OLD_WRITE_DATA |
	    OLD_APPEND_DATA | OLD_READ_ATTRS | OLD_WRITE_ATTRS | OLD_READ_ACL |
	    OLD_WRITE_ACL | OLD_WRITE_OWNER | OLD_SYNCHRONIZE;

	a->z_ace_data[1].z_fuid = 1001;  /* a real id - the entry no mode has */
	a->z_ace_data[1].z_type = OLD_ACE_ALLOW;
	a->z_ace_data[1].z_flags = 0;
	a->z_ace_data[1].z_access_mask = OLD_READ_DATA | OLD_READ_ATTRS |
	    OLD_READ_ACL | OLD_SYNCHRONIZE;

	a->z_ace_data[2].z_fuid = 0;
	a->z_ace_data[2].z_type = OLD_ACE_DENY;
	a->z_ace_data[2].z_flags = OLD_ACE_EVERYONE;
	a->z_ace_data[2].z_access_mask = OLD_READ_DATA | OLD_WRITE_DATA |
	    OLD_APPEND_DATA | OLD_EXECUTE;

	a->z_ace_data[3].z_fuid = 0;
	a->z_ace_data[3].z_type = OLD_ACE_ALLOW;
	a->z_ace_data[3].z_flags = OLD_ACE_EVERYONE;
	a->z_ace_data[3].z_access_mask = OLD_READ_ATTRS | OLD_READ_ACL |
	    OLD_SYNCHRONIZE;
}

static void
create_fs(objset_t *os, void *arg, cred_t *cr, dmu_tx_t *tx)
{
	uint64_t moid, dqoid, rootoid, fileoid, bigoid, subdiroid, value;
	size_t len = strlen(file_body);
	char *big;
	size_t i;

	(void) arg; (void) cr;

	/* Object 1 is the master node by definition. */
	/* zap_create_claim returns an ERROR, not an object number: the object
	 * number is the one being claimed. Getting that backwards makes moid
	 * zero, and object zero is the dnode array itself - every subsequent
	 * zap_add lands on it and fails with EINVAL. */
	VERIFY0(zap_create_claim(os, MASTER_NODE, DMU_OT_MASTER_NODE,
	    DMU_OT_NONE, 0, tx));
	moid = MASTER_NODE;

	dqoid = zap_create(os, DMU_OT_UNLINKED_SET, DMU_OT_NONE, 0, tx);

	rootoid = zap_create_norm(os, 0, DMU_OT_DIRECTORY_CONTENTS,
	    DMU_OT_ZNODE, sizeof (znode_phys_t), tx);
	subdiroid = zap_create_norm(os, 0, DMU_OT_DIRECTORY_CONTENTS,
	    DMU_OT_ZNODE, sizeof (znode_phys_t), tx);

	/* A small file: one block, and its contents are checked byte for
	 * byte by the reader's test. */
	fileoid = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, 0,
	    DMU_OT_ZNODE, sizeof (znode_phys_t), tx);
	dmu_write(os, fileoid, 0, len, file_body, tx);

	/* A big one: 300KB at a 16KB record size is several levels of
	 * indirect block, which is the part of the read path a single-block
	 * file never touches. */
	big = umem_alloc(300 * 1024, UMEM_NOFAIL);
	for (i = 0; i < 300 * 1024; i++)
		big[i] = (char)((i * 7 + (i >> 9)) & 0xFF);
	bigoid = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, 16384,
	    DMU_OT_ZNODE, sizeof (znode_phys_t), tx);
	dmu_write(os, bigoid, 0, 300 * 1024, big, tx);
	umem_free(big, 300 * 1024);

	set_znode(os, rootoid, S_IFDIR | 0755, 3, 3, rootoid, tx);
	set_znode(os, subdiroid, S_IFDIR | 0755, 2, 2, rootoid, tx);
	set_znode(os, fileoid, S_IFREG | 0644, len, 1, rootoid, tx);
	set_znode(os, bigoid, S_IFREG | 0644, 300 * 1024, 1, rootoid, tx);

	value = fileoid | (((uint64_t)8) << 60);          /* DT_REG */
	VERIFY0(zap_add(os, rootoid, "hello.txt", 8, 1, &value, tx));
	value = bigoid | (((uint64_t)8) << 60);
	VERIFY0(zap_add(os, rootoid, "big.bin", 8, 1, &value, tx));
	value = subdiroid | (((uint64_t)4) << 60);        /* DT_DIR */
	VERIFY0(zap_add(os, rootoid, "etc", 8, 1, &value, tx));

	/* secret.txt, carrying a version 0 ACL in its bonus buffer.
	 *
	 * Mirrors genesisacl.dat's file of the same name deliberately: same
	 * mode, same owner, same four entries, different ENCODING. Two
	 * fixtures that agree about what the ACL says and disagree about how
	 * it is written down are what make it possible to test that the
	 * decoder handles both and returns the same answer. */
	{
		uint64_t secretoid;
		zfs_acl_phys_v0_disk_t acl;
		size_t slen = strlen(secret_body);

		build_v0_acl(&acl);
		secretoid = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, 0,
		    DMU_OT_ZNODE, sizeof (znode_phys_t), tx);
		dmu_write(os, secretoid, 0, slen, secret_body, tx);
		set_znode_owned(os, secretoid, S_IFREG | 0600, slen, 1,
		    rootoid, 1000, 1000, &acl, tx);
		value = secretoid | (((uint64_t)8) << 60);
		VERIFY0(zap_add(os, rootoid, "secret.txt", 8, 1, &value, tx));
	}

	/* One more level down, so the reader's path walk is tested past a
	 * single component. */
	{
		uint64_t deepoid;
		const char *motd = "hello from a zfs volume\n";

		deepoid = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, 0,
		    DMU_OT_ZNODE, sizeof (znode_phys_t), tx);
		dmu_write(os, deepoid, 0, strlen(motd), motd, tx);
		set_znode(os, deepoid, S_IFREG | 0644, strlen(motd), 1,
		    subdiroid, tx);
		value = deepoid | (((uint64_t)8) << 60);
		VERIFY0(zap_add(os, subdiroid, "motd", 8, 1, &value, tx));
	}

	value = ZPL_VERSION_1;
	VERIFY0(zap_add(os, moid, ZPL_VERSION_STR, 8, 1, &value, tx));
	VERIFY0(zap_add(os, moid, ZFS_ROOT_OBJ, 8, 1, &rootoid, tx));
	VERIFY0(zap_add(os, moid, ZFS_UNLINKED_SET, 8, 1, &dqoid, tx));

	printf("  root=%llu file=%llu big=%llu etc=%llu\n",
	    (unsigned long long)rootoid, (unsigned long long)fileoid,
	    (unsigned long long)bigoid, (unsigned long long)subdiroid);
}

int
main(int argc, char **argv)
{
	nvlist_t *nvroot, *child[1], *props;
	int err;

	if (argc < 3) {
		printf("usage: mkzpl <image> <poolname>\n");
		return (2);
	}
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	child[0] = fnvlist_alloc();
	fnvlist_add_string(child[0], ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(child[0], ZPOOL_CONFIG_PATH, argv[1]);
	fnvlist_add_uint64(child[0], ZPOOL_CONFIG_ASHIFT, 9);
	fnvlist_add_uint64(child[0], ZPOOL_CONFIG_IS_LOG, 0);

	nvroot = fnvlist_alloc();
	fnvlist_add_string(nvroot, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)child, 1);

	props = fnvlist_alloc();
	err = spa_create(argv[2], nvroot, props, NULL, NULL);
	printf("spa_create: %d\n", err);
	if (err != 0)
		return (1);

	/* spa_create has already made the pool's root dataset and an empty
	 * objset for it - in the kernel, `zpool create` fills that in by
	 * calling zfs_create_fs, which libzpool does not export. So the
	 * filesystem layout goes into the EXISTING objset rather than into a
	 * new one. */
	{
		objset_t *os;
		dmu_tx_t *tx;

		err = dmu_objset_own(argv[2], DMU_OST_ANY, B_FALSE, B_TRUE,
		    FTAG, &os);
		printf("dmu_objset_own: %d (type %d)\n", err,
		    err ? -1 : (int)dmu_objset_type(os));
		if (err != 0)
			return (1);

		tx = dmu_tx_create(os);
		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, B_TRUE, NULL);
		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, B_TRUE, NULL);
		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, B_TRUE, NULL);
		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, B_TRUE, NULL);
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, 512 * 1024);
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, 512 * 1024);
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, 512 * 1024);
		err = dmu_tx_assign(tx, DMU_TX_WAIT);
		printf("dmu_tx_assign: %d\n", err);
		if (err != 0)
			return (1);

		create_fs(os, NULL, NULL, tx);
		dmu_tx_commit(tx);
		dmu_objset_disown(os, B_TRUE, FTAG);
	}

	{
		spa_t *spa;

		VERIFY0(spa_open(argv[2], &spa, FTAG));
		txg_wait_synced(spa_get_dsl(spa), 0);
		spa_close(spa, FTAG);
	}
	kernel_fini();
	printf("done\n");
	return (0);
}
