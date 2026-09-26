/* Put a NON-TRIVIAL ZFS ACL on a file in an exported pool image, using
 * OpenZFS's own System Attribute writer to lay out the bytes.
 *
 * --- why this program exists ----------------------------------------------
 * `zpool create -O acltype=nfsv4` plus chmod gets you a real ZPL v5 pool with
 * real ZPL_DACL_ACES on disk, and that is most of a fixture. What it does NOT
 * get you is an ACL that mode bits cannot express: chmod can only ever
 * produce owner@/group@/everyone@, which is the mode word wearing a hat. A
 * reader that "passes" against a trivial ACL has proved only that it can
 * rediscover st_mode the long way round.
 *
 * Linux has no userland interface for setting NFSv4 ACLs - there is no
 * setfacl for them and no system.nfs4_acl xattr (checked: EOPNOTSUPP) - so
 * the ACE that makes the fixture worth anything has to be written through
 * libzpool instead. That is not a workaround, it is the better answer: every
 * byte below is placed by module/zfs/sa.c, the same code the kernel runs, so
 * the fixture is still something OpenZFS wrote and Genesis merely reads.
 *
 * --- the ACL, and why this particular one ---------------------------------
 * On secret.txt (owned by uid 1000, mode 0600):
 *
 *   ALLOW owner@       read,write,attrs,acl,owner,sync
 *   ALLOW user 1001    read,read_attributes,read_acl,sync
 *   DENY  everyone@    read,write,execute
 *   ALLOW everyone@    read_attributes,read_acl,sync
 *
 * uid 1001 is not the owner and not in the owning group, and it can read a
 * file that mode 0600 says only uid 1000 may read. There is no mode word that
 * means that. So a POSIX-only reader and an ACL-aware reader MUST disagree
 * about uid 1001, and a test can tell which one is running - which is the
 * only reason to ship a fixture at all.
 *
 * Order is load-bearing and is the easiest thing to get wrong. NFSv4 walks
 * ACEs in order and the first entry to mention a bit decides it, so the DENY
 * everyone@ has to come AFTER the two ALLOWs. Put it first and it denies the
 * owner too, and the fixture quietly becomes "nobody can read anything".
 *
 * Build (see tests/host/fixtures/README.md):
 *   gcc -o zplsetacl tests/host/zplsetacl.c -I/usr/include/libzfs \
 *       -I/usr/include/libspl -Iinc -D_GNU_SOURCE \
 *       -lzpool -lnvpair -lzfs_core -luutil
 * where inc/sys is a symlink to /usr/include/libzpool.
 */
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/zap.h>
#include <sys/sa.h>
#include <sys/zfs_acl.h>
#include <sys/zfs_sa.h>
#include <sys/zfs_znode.h>
#include <sys/fs/zfs.h>
#include <sys/sa_impl.h>
#include <sys/zfs_quota.h>
#include <libzutil.h>
#include <stdio.h>
#include <string.h>

extern int dmu_objset_own(const char *name, dmu_objset_type_t type,
    boolean_t readonly, boolean_t decrypt, const void *tag, objset_t **osp);
extern void dmu_objset_disown(objset_t *os, boolean_t decrypt,
    const void *tag);
extern boolean_t zfeature_checks_disable;

/* The DMU keeps per-user and per-group space accounting, and to do that it
 * has to be able to ask "who owns this dnode" every time one is dirtied. That
 * question is ZPL knowledge, not DMU knowledge, so the ZPL registers a
 * callback for it at module load - and libzpool, which has no ZPL, registers
 * nothing. Dirty an accounted dnode without it and dnode_sync trips
 * ASSERT(!(dn_phys->dn_flags & DNODE_FLAG_USERUSED_ACCOUNTED)), which is what
 * this program did on its first run.
 *
 * zhack registers a callback that abort()s, because zhack edits pool metadata
 * that is never user-accounted. This program edits a FILE, so it needs the
 * real answer. This is module/zfs/zfs_quota.c:zpl_get_file_info, reduced to
 * the SA case that a v5 filesystem actually takes.
 *
 * Returning EEXIST for a NULL data pointer is not an error path: it is how
 * the DMU is told "the ids are not changing, reuse the ones you have". This
 * program never changes a file's owner, so that branch is the common one. */
static int
genesis_get_file_info(dmu_object_type_t bonustype, const void *data,
    zfs_file_info_t *zoi)
{
	const sa_hdr_phys_t *sap = data;
	uintptr_t after;
	int hdrsize;

	if (bonustype != DMU_OT_ZNODE && bonustype != DMU_OT_SA)
		return (ENOENT);

	zoi->zfi_project = ZFS_DEFAULT_PROJID;

	if (data == NULL)
		return (EEXIST);

	if (bonustype == DMU_OT_ZNODE) {
		fprintf(stderr, "object is pre-SA (DMU_OT_ZNODE); this tool "
		    "only edits v5 filesystems\n");
		return (ENOENT);
	}

	/* A freshly allocated dnode whose znode has not been filled in yet. */
	if (sap->sa_magic == 0) {
		zoi->zfi_user = zoi->zfi_group = zoi->zfi_generation = 0;
		return (0);
	}
	if (sap->sa_magic != SA_MAGIC)
		return (EINVAL);

	hdrsize = SA_HDR_SIZE(sap);
	if (hdrsize < (int)sizeof (sa_hdr_phys_t))
		return (EINVAL);

	after = (uintptr_t)data + hdrsize;
	zoi->zfi_user = *(uint64_t *)(after + SA_UID_OFFSET);
	zoi->zfi_group = *(uint64_t *)(after + SA_GID_OFFSET);
	zoi->zfi_generation = *(uint64_t *)(after + SA_GEN_OFFSET);
	return (0);
}

/* zhack's import path, trimmed to the one case here: find a pool by name in
 * a directory of image files and import it read-write. */
static void
import_from_dir(const char *dir, const char *pool)
{
	importargs_t args;
	nvlist_t *config;
	char *paths[1];
	int err;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	dmu_objset_register_type(DMU_OST_ZFS, genesis_get_file_info);

	memset(&args, 0, sizeof (args));
	paths[0] = (char *)dir;
	args.path = paths;
	args.paths = 1;
	args.can_be_active = B_FALSE;
	args.scan = B_TRUE;

	libpc_handle_t lpch = {
		.lpc_lib_handle = NULL,
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};

	err = zpool_find_config(&lpch, (char *)pool, &config, &args);
	if (err != 0 || config == NULL) {
		fprintf(stderr, "cannot find pool '%s' under %s\n", pool, dir);
		exit(1);
	}

	zfeature_checks_disable = B_TRUE;
	err = spa_import((char *)pool, config, NULL, ZFS_IMPORT_NORMAL);
	zfeature_checks_disable = B_FALSE;
	fnvlist_free(config);
	if (err == EEXIST)
		err = 0;
	if (err != 0) {
		fprintf(stderr, "spa_import: %s\n", strerror(err));
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	const char *dir, *pool;
	uint64_t objnum, sa_obj, count;
	objset_t *os;
	sa_attr_type_t *sa_attrs;
	sa_handle_t *hdl;
	dmu_tx_t *tx;
	sa_bulk_attr_t bulk[3];
	uint64_t pflags;
	uint8_t aces[64];
	zfs_ace_hdr_t *h;
	zfs_ace_t *f;
	size_t off = 0;
	int err, i = 0, idx = 0;

	if (argc < 4) {
		fprintf(stderr, "usage: zplsetacl <dir> <pool> <objnum>\n");
		return (2);
	}
	dir = argv[1];
	pool = argv[2];
	objnum = strtoull(argv[3], NULL, 0);

	import_from_dir(dir, pool);

	err = dmu_objset_own(pool, DMU_OST_ZFS, B_FALSE, B_TRUE, FTAG, &os);
	if (err != 0) {
		fprintf(stderr, "dmu_objset_own: %s\n", strerror(err));
		return (1);
	}

	/* The SA registry object hangs off the master node, which is object 1
	 * by definition. sa_setup builds the attribute table from it - the
	 * same table zdb prints as "SA attr registration". */
	err = zap_lookup(os, MASTER_NODE_OBJ, ZFS_SA_ATTRS, 8, 1, &sa_obj);
	if (err != 0) {
		fprintf(stderr, "no SA_ATTRS on the master node: %s "
		    "(is this a pre-v5 filesystem?)\n", strerror(err));
		return (1);
	}
	err = sa_setup(os, sa_obj, zfs_attr_table, ZPL_END, &sa_attrs);
	if (err != 0) {
		fprintf(stderr, "sa_setup: %s\n", strerror(err));
		return (1);
	}

	err = sa_handle_get(os, objnum, NULL, SA_HDL_PRIVATE, &hdl);
	if (err != 0) {
		fprintf(stderr, "sa_handle_get(%llu): %s\n",
		    (unsigned long long)objnum, strerror(err));
		return (1);
	}

	/* --- the four ACEs, in evaluation order --------------------------- */
	memset(aces, 0, sizeof (aces));

	/* ALLOW owner@ - 8 bytes, no fuid: the who is in the flags. */
	h = (zfs_ace_hdr_t *)(aces + off);
	h->z_type = ACE_ACCESS_ALLOWED_ACE_TYPE;
	h->z_flags = ACE_OWNER;
	h->z_access_mask = ACE_READ_DATA | ACE_WRITE_DATA | ACE_APPEND_DATA |
	    ACE_READ_ATTRIBUTES | ACE_WRITE_ATTRIBUTES | ACE_READ_ACL |
	    ACE_WRITE_ACL | ACE_WRITE_OWNER | ACE_SYNCHRONIZE;
	off += sizeof (zfs_ace_hdr_t);
	i++;

	/* ALLOW user 1001 - 16 bytes, because a named who needs somewhere to
	 * live. This is the entry no mode word can express. */
	f = (zfs_ace_t *)(aces + off);
	f->z_hdr.z_type = ACE_ACCESS_ALLOWED_ACE_TYPE;
	f->z_hdr.z_flags = 0;
	f->z_hdr.z_access_mask = ACE_READ_DATA | ACE_READ_ATTRIBUTES |
	    ACE_READ_ACL | ACE_SYNCHRONIZE;
	f->z_fuid = 1001;
	off += sizeof (zfs_ace_t);
	i++;

	/* DENY everyone@ the data. After the allows, never before. */
	h = (zfs_ace_hdr_t *)(aces + off);
	h->z_type = ACE_ACCESS_DENIED_ACE_TYPE;
	h->z_flags = ACE_EVERYONE;
	h->z_access_mask = ACE_READ_DATA | ACE_WRITE_DATA | ACE_APPEND_DATA |
	    ACE_EXECUTE;
	off += sizeof (zfs_ace_hdr_t);
	i++;

	/* ALLOW everyone@ the harmless metadata bits, which is what a real
	 * ZFS ACL always trails with. */
	h = (zfs_ace_hdr_t *)(aces + off);
	h->z_type = ACE_ACCESS_ALLOWED_ACE_TYPE;
	h->z_flags = ACE_EVERYONE;
	h->z_access_mask = ACE_READ_ATTRIBUTES | ACE_READ_ACL |
	    ACE_SYNCHRONIZE;
	off += sizeof (zfs_ace_hdr_t);
	i++;

	count = i;

	/* --- and the flag that decides whether any of it is ever read ------
	 *
	 * ZFS_ACL_TRIVIAL (0x4) in the znode's pflags means "this file's ACL
	 * says exactly what its mode bits say", and it is a fast path with
	 * teeth: zfs_zaccess checks it and skips loading the ACL altogether.
	 * Write a non-trivial ACL and leave the flag set and nothing reads
	 * it - the first run of this program did exactly that, and real ZFS
	 * denied uid 1001 the file it had just been granted, because it never
	 * looked. Nothing was corrupt; the ACL was simply never consulted.
	 *
	 * That is worth knowing beyond this program: any reader of ZFS
	 * permissions has to honour this flag, and a reader that always
	 * parses the ACL is doing work ZFS itself skips on most files. */
	err = sa_lookup(hdl, sa_attrs[ZPL_FLAGS], &pflags, sizeof (pflags));
	if (err != 0) {
		fprintf(stderr, "sa_lookup(ZPL_FLAGS): %s\n", strerror(err));
		return (1);
	}
	printf("pflags %#llx -> %#llx (clearing ZFS_ACL_TRIVIAL)\n",
	    (unsigned long long)pflags,
	    (unsigned long long)(pflags & ~(uint64_t)ZFS_ACL_TRIVIAL));
	pflags &= ~(uint64_t)ZFS_ACL_TRIVIAL;

	tx = dmu_tx_create(os);
	dmu_tx_hold_sa(tx, hdl, B_TRUE);
	err = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (err != 0) {
		fprintf(stderr, "dmu_tx_assign: %s\n", strerror(err));
		return (1);
	}

	SA_ADD_BULK_ATTR(bulk, idx, sa_attrs[ZPL_DACL_COUNT], NULL,
	    &count, sizeof (count));
	SA_ADD_BULK_ATTR(bulk, idx, sa_attrs[ZPL_DACL_ACES], NULL,
	    aces, (uint32_t)off);
	SA_ADD_BULK_ATTR(bulk, idx, sa_attrs[ZPL_FLAGS], NULL,
	    &pflags, sizeof (pflags));
	/* idx, not the ACE count: bulk[] is indexed by ATTRIBUTE and there are
	 * three of those, while the four ACEs are one opaque blob inside the
	 * second. SA_ADD_BULK_ATTR increments idx itself, which is why it
	 * needs an lvalue and why passing a literal 0/1 does not compile. */
	err = sa_bulk_update(hdl, bulk, idx, tx);
	if (err != 0) {
		fprintf(stderr, "sa_bulk_update: %s\n", strerror(err));
		return (1);
	}
	dmu_tx_commit(tx);

	printf("object %llu: %llu ACEs, %llu bytes of ACE data\n",
	    (unsigned long long)objnum, (unsigned long long)count,
	    (unsigned long long)off);

	sa_handle_destroy(hdl);
	dmu_objset_disown(os, B_TRUE, FTAG);

	{
		spa_t *spa;
		VERIFY0(spa_open(pool, &spa, FTAG));
		txg_wait_synced(spa_get_dsl(spa), 0);
		spa_close(spa, FTAG);
	}
	spa_export(pool, NULL, B_TRUE, B_FALSE);
	kernel_fini();
	printf("done\n");
	return (0);
}
