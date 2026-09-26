/* Walking the System Attribute layout, so that variable-length attributes -
 * the ACL among them - can be found at all.
 *
 * Included from zfs_glue.inc, inside the vendored translation unit, for the
 * same reason everything else there is: objset_get_dnode and zap_lookup are
 * static in zfsimpl.c.
 *
 * --- why the vendored reader cannot do this -------------------------------
 * zfs_dnode_stat reads a v5 file's mode, size, uid and gid out of the bonus
 * buffer at CONSTANTS: SA_MODE_OFFSET 0, SA_SIZE_OFFSET 8, SA_UID_OFFSET 24,
 * SA_GID_OFFSET 32. Those constants are not wrong - they are what the default
 * ZPL layout produces, and a boot loader that only has to find /boot/loader
 * needs nothing else. But they are the reason the boot reader cannot reach an
 * ACL, and no amount of adding more constants would fix it:
 *
 *   - ZPL_DACL_ACES is VARIABLE length. Its size is not a property of the
 *     attribute, it is a property of this one file, and it lives in the
 *     header's sa_lengths[] array. A constant cannot express it.
 *   - Every attribute after a variable-length one therefore moves. On the
 *     fixture's layout the ACL is last, so nothing moves today; ZPL_SYMLINK
 *     and ZPL_DXATTR are variable too, and on a filesystem with those in play
 *     the constants stop describing anything.
 *   - The layout itself is per-filesystem. zfs_sa.h says so in as many words:
 *     "Two different file system could have different numeric values for the
 *     same attributes."
 *
 * So this is a real walk: header -> layout number -> the ordered list of
 * attributes in that layout -> running offset, with each attribute's length
 * coming from the registry if it is fixed and from sa_lengths[] if it is not.
 *
 * --- how the registry is read, and why by name ----------------------------
 * The obvious way to build "attribute number -> length" is to iterate the
 * REGISTRY zap. This does the opposite: it looks up each ZPL attribute by
 * NAME, which is what sa_setup() does in the real ZFS and is the only part of
 * the arrangement that is actually stable. The numbers are assigned per
 * filesystem; the names are compiled into OpenZFS. Looking up "ZPL_MODE" and
 * being told it is attribute 5 with length 8 is correct even on a filesystem
 * that numbered things differently, and it needs no ZAP iteration - the
 * registry is a microzap on every pool seen so far, but it is allowed to be a
 * fat one, and a lookup handles both because zap_lookup does.
 */

/* The ZPL attribute names, in the order OpenZFS registers them
 * (module/zfs/zfs_sa.c:zfs_attr_table). The order is not load-bearing here -
 * everything is keyed by name - but keeping it lets the two tables be
 * compared by eye.
 *
 * ZPL_SEQ is last and is absent from filesystems made before it existed; the
 * fixture's registry has 22 entries, not 23. A missing name is not an error,
 * it is a filesystem that never registered that attribute. */
static const char *const zfsg_sa_names[] = {
	"ZPL_ATIME", "ZPL_MTIME", "ZPL_CTIME", "ZPL_CRTIME", "ZPL_GEN",
	"ZPL_MODE", "ZPL_SIZE", "ZPL_PARENT", "ZPL_LINKS", "ZPL_XATTR",
	"ZPL_RDEV", "ZPL_FLAGS", "ZPL_UID", "ZPL_GID", "ZPL_PAD",
	"ZPL_ZNODE_ACL", "ZPL_DACL_COUNT", "ZPL_SYMLINK", "ZPL_SCANSTAMP",
	"ZPL_DACL_ACES", "ZPL_DXATTR", "ZPL_PROJID", "ZPL_SEQ",
};

/* Indices into the table above, for the attributes this code asks for by
 * hand. Kept as an enum rather than as literals so that inserting a name
 * cannot silently repoint one of them at its neighbour. */
enum {
	ZFSG_SA_I_ATIME = 0, ZFSG_SA_I_MTIME, ZFSG_SA_I_CTIME,
	ZFSG_SA_I_CRTIME, ZFSG_SA_I_GEN, ZFSG_SA_I_MODE, ZFSG_SA_I_SIZE,
	ZFSG_SA_I_PARENT, ZFSG_SA_I_LINKS, ZFSG_SA_I_XATTR, ZFSG_SA_I_RDEV,
	ZFSG_SA_I_FLAGS, ZFSG_SA_I_UID, ZFSG_SA_I_GID, ZFSG_SA_I_PAD,
	ZFSG_SA_I_ZNODE_ACL, ZFSG_SA_I_DACL_COUNT, ZFSG_SA_I_SYMLINK,
	ZFSG_SA_I_SCANSTAMP, ZFSG_SA_I_DACL_ACES, ZFSG_SA_I_DXATTR,
	ZFSG_SA_I_PROJID, ZFSG_SA_I_SEQ
};

/* No layout in a ZPL filesystem comes close to this; the fixture's has 14.
 * The bound exists so that a corrupt or hostile layout cannot walk off the
 * end of the array it is read into. */
#define ZFSG_SA_MAXLAYOUT 48

/* zfsg_sa_map_t is declared in zfs_glue.inc, beside struct zfsg_mount, which
 * has to embed it. ZFSG_SA_NNAMES is spelled out there as a literal because
 * the array it describes is here; this is the check that stops the two from
 * drifting apart, and it fails the build rather than a test. */
typedef char zfsg_sa_names_match[
    ((int)(sizeof (zfsg_sa_names) / sizeof (zfsg_sa_names[0])) ==
     ZFSG_SA_NNAMES) ? 1 : -1];

/* Build the name -> (number, length) map from the filesystem's own registry.
 * Done once per mount and cached; it is a couple of dozen ZAP lookups against
 * one block that the block cache is already holding, but doing it per stat()
 * would still be silly. */
static int
zfsg_sa_build(zfsg_mount_t *m)
{
	dnode_phys_t master, sa_dn, reg_dn;
	uint64_t sa_obj, reg_obj;
	int i, rc;

	if (m->sa.built != 0)
		return (m->sa.built > 0 ? 0 : ENOENT);

	memset(&m->sa, 0, sizeof (m->sa));

	/* SA_ATTRS on the master node is what makes this a v5 filesystem. A
	 * pre-v5 one has no registry, its attributes are a znode_phys_t at
	 * fixed offsets, and there is nothing here to walk - so ENOENT is the
	 * honest answer and the caller falls back. */
	rc = objset_get_dnode(m->spa, &m->mnt.objset, MASTER_NODE_OBJ,
	    &master);
	if (rc != 0)
		goto fail;
	rc = zap_lookup(m->spa, &master, ZFS_SA_ATTRS, 8, 1, &sa_obj);
	if (rc != 0)
		goto fail;

	rc = objset_get_dnode(m->spa, &m->mnt.objset, sa_obj, &sa_dn);
	if (rc != 0)
		goto fail;
	rc = zap_lookup(m->spa, &sa_dn, SA_REGISTRY, 8, 1, &reg_obj);
	if (rc != 0)
		goto fail;
	rc = zap_lookup(m->spa, &sa_dn, SA_LAYOUTS, 8, 1, &m->sa.layouts_obj);
	if (rc != 0)
		goto fail;

	rc = objset_get_dnode(m->spa, &m->mnt.objset, reg_obj, &reg_dn);
	if (rc != 0)
		goto fail;

	for (i = 0; i < ZFSG_SA_NNAMES; i++) {
		uint64_t v;
		uint16_t n, len;

		if (zap_lookup(m->spa, &reg_dn, zfsg_sa_names[i], 8, 1,
		    &v) != 0)
			continue;          /* not registered here; fine */

		n = (uint16_t)SA_ATTR_NUM(v);
		len = (uint16_t)SA_ATTR_LENGTH(v);
		m->sa.num[i] = n;
		m->sa.have[i] = 1;
		if (n < ZFSG_SA_MAXNUM) {
			m->sa.bynum[n] = len;
			m->sa.known[n] = 1;
		}
	}

	/* A registry with no ZPL_MODE in it is not a ZPL filesystem's
	 * registry, and continuing would mean walking layouts whose
	 * attributes mean something else entirely. */
	if (!m->sa.have[ZFSG_SA_I_MODE]) {
		rc = EINVAL;
		goto fail;
	}

	m->sa.built = 1;
	return (0);
fail:
	m->sa.built = -1;
	return (rc != 0 ? rc : ENOENT);
}

/* The bonus buffer, or the spill block if the attributes outgrew the dnode.
 * Returns a pointer the caller must free through *freeme (NULL if it is
 * pointing into the dnode itself), which is the same dance zfs_dnode_stat
 * does and for the same reason. */
static int
zfsg_sa_buffer(const spa_t *spa, dnode_phys_t *dn, void **bufp, size_t *lenp,
    void **freeme)
{
	*freeme = NULL;

	if (dn->dn_bonuslen != 0) {
		*bufp = DN_BONUS(dn);
		*lenp = dn->dn_bonuslen;
		return (0);
	}
	if ((dn->dn_flags & DNODE_FLAG_SPILL_BLKPTR) != 0) {
		blkptr_t *bp = DN_SPILL_BLKPTR(dn);
		size_t size = BP_GET_LSIZE(bp);
		void *buf = malloc(size);
		int rc;

		if (buf == NULL)
			return (ENOMEM);
		rc = zio_read(spa, bp, buf);
		if (rc != 0) {
			free(buf);
			return (rc);
		}
		*bufp = buf;
		*lenp = size;
		*freeme = buf;
		return (0);
	}
	return (EIO);
}

/* Decimal, because that is how sa.c keys the LAYOUTS zap: the layout number
 * is turned into a string and used as the entry name. No snprintf in this
 * compat environment, and the value is at most 10 bits, so four digits is
 * always enough. */
static void
zfsg_sa_layout_key(uint16_t num, char *out)
{
	int i = 0, j;
	char tmp[8];

	if (num == 0) {
		out[0] = '0';
		out[1] = '\0';
		return;
	}
	while (num > 0 && i < (int)sizeof (tmp)) {
		tmp[i++] = (char)('0' + (num % 10));
		num /= 10;
	}
	for (j = 0; j < i; j++)
		out[j] = tmp[i - 1 - j];
	out[i] = '\0';
}

/* Find one attribute in one object.
 *
 * `which` is an index into zfsg_sa_names. On success *offp is the offset of
 * the attribute's data from the start of the SA buffer and *lenp its length.
 * ENOENT means the attribute is not in this object's layout, which is an
 * ordinary answer - a file with a trivial ACL still has DACL_ACES, but a
 * directory with no symlink does not have ZPL_SYMLINK.
 */
static int
zfsg_sa_locate(zfsg_mount_t *m, const void *sabuf, size_t sabuflen, int which,
    size_t *offp, size_t *lenp)
{
	const sa_hdr_phys_t *hdr = sabuf;
	dnode_phys_t layouts_dn;
	uint16_t ids[ZFSG_SA_MAXLAYOUT];
	char key[8];
	uint16_t layout_num;
	size_t hdrsize, off;
	int nids, i, var, rc;

	if (!m->sa.have[which])
		return (ENOENT);
	if (sabuflen < sizeof (sa_hdr_phys_t))
		return (EIO);
	if (hdr->sa_magic != SA_MAGIC)
		return (EIO);

	layout_num = (uint16_t)SA_HDR_LAYOUT_NUM(hdr);
	hdrsize = (size_t)SA_HDR_SIZE(hdr);
	if (hdrsize < sizeof (uint32_t) + sizeof (uint16_t) ||
	    hdrsize > sabuflen)
		return (EIO);

	rc = objset_get_dnode(m->spa, &m->mnt.objset, m->sa.layouts_obj,
	    &layouts_dn);
	if (rc != 0)
		return (rc);

	/* The entry is an array of 16-bit attribute numbers and its length is
	 * not recorded anywhere this reader can ask for. fzap_leaf_array
	 * clamps its copy to the entry's real length and leaves the rest of
	 * the buffer alone, so the count is recovered by pre-filling with a
	 * sentinel and finding where it survived. 0xFFFF is safe: it is not a
	 * legal attribute number in any registry, which is checked below by
	 * the fact that an unknown number aborts the walk. */
	for (i = 0; i < ZFSG_SA_MAXLAYOUT; i++)
		ids[i] = 0xFFFF;

	zfsg_sa_layout_key(layout_num, key);
	rc = zap_lookup(m->spa, &layouts_dn, key, 2, ZFSG_SA_MAXLAYOUT, ids);
	if (rc != 0)
		return (rc);

	for (nids = 0; nids < ZFSG_SA_MAXLAYOUT; nids++) {
		if (ids[nids] == 0xFFFF)
			break;
	}
	if (nids == 0)
		return (EIO);

	/* Walk it. `var` counts variable-length attributes seen so far, which
	 * is the index into sa_lengths[] - the array is dense over the
	 * variable attributes only, not over all of them. */
	off = hdrsize;
	var = 0;
	for (i = 0; i < nids; i++) {
		uint16_t id = ids[i];
		size_t alen;

		if (id >= ZFSG_SA_MAXNUM || !m->sa.known[id]) {
			/* An attribute this filesystem registered under a
			 * name not in the ZPL table. Its length is unknown,
			 * so every offset after it is unknown too. Refusing
			 * is the only honest move: guessing a length here
			 * would silently return the wrong bytes. */
			return (ENOTSUP);
		}

		alen = m->sa.bynum[id];
		if (alen == 0) {
			size_t lenoff = sizeof (uint32_t) + sizeof (uint16_t) +
			    (size_t)var * sizeof (uint16_t);

			if (lenoff + sizeof (uint16_t) > hdrsize)
				return (EIO);
			alen = hdr->sa_lengths[var];
			var++;
		}

		if (off + alen > sabuflen)
			return (EIO);

		if (id == m->sa.num[which]) {
			*offp = off;
			*lenp = alen;
			return (0);
		}
		off += alen;
	}
	return (ENOENT);
}

/* --- the ACL ---------------------------------------------------------------
 *
 * ZFS stores an NFSv4 ACL, which is the NT ACL model. What is on disk is a
 * count (ZPL_DACL_COUNT) and an opaque byte blob (ZPL_DACL_ACES), and the
 * blob is a sequence of VARIABLE WIDTH entries:
 *
 *   8 bytes   an ALLOW or DENY naming owner@, group@ or everyone@ - the who
 *             is in the flags, so there is nothing else to store
 *  16 bytes   an ALLOW or DENY naming an actual id: the same 8-byte header
 *             followed by a 64-bit uid or gid
 *  48 bytes   an object ACE, which additionally carries two 16-byte GUIDs
 *
 * This is why the count matters and why the blob length does not tell you the
 * count. It is also why the walk below advances by a size it computes per
 * entry rather than by a stride: get that wrong on entry two and every
 * subsequent ACE is read from the middle of its predecessor, which does not
 * fail - it produces a well-formed ACL granting things nobody asked for.
 *
 * The size rule is OpenZFS's zfs_ace_fuid_size, reproduced. Note that the
 * object-ACE types have to be recognised even though nothing here interprets
 * their GUIDs, for exactly the desynchronisation reason above.
 */

/* Byte order: the vendored reader refuses a big-endian pool outright (the
 * zfs-pool-v1 fixture is one, and the host test asserts the refusal), so
 * everything reaching this code is little-endian and these loads are direct.
 * If that ever changes, this is one of the places that has to learn to
 * byteswap - which is why the assumption is written down here rather than
 * left implicit in a cast. */
static unsigned int
zfsg_ace_size(unsigned int type, unsigned int flags)
{
	unsigned int entry;

	switch (type) {
	case ACE_ACCESS_ALLOWED_OBJECT_ACE_TYPE:
	case ACE_ACCESS_DENIED_OBJECT_ACE_TYPE:
	case ACE_SYSTEM_AUDIT_OBJECT_ACE_TYPE:
	case ACE_SYSTEM_ALARM_OBJECT_ACE_TYPE:
		/* zfs_ace_t (16) + two 16-byte GUIDs. */
		return (48);
	case ACE_ACCESS_ALLOWED_ACE_TYPE:
	case ACE_ACCESS_DENIED_ACE_TYPE:
		entry = flags & ACE_TYPE_FLAGS;
		if (entry == ACE_OWNER || entry == ACE_OWNING_GROUP ||
		    entry == ACE_EVERYONE)
			return (8);
		return (16);
	default:
		/* Audit and alarm ACEs naming an id. Same shape as a named
		 * allow/deny. */
		return (16);
	}
}


/* --- the PRE-SA ACL --------------------------------------------------------
 *
 * A filesystem older than ZPL version 5 has no System Attribute registry.
 * Its attributes are a znode_phys_t at fixed offsets in the bonus buffer, and
 * its ACL is the 88 bytes of zp_acl inside that. None of the walk above
 * applies; there is nothing to walk.
 *
 * Two structures overlay those 88 bytes and z_acl_version decides which:
 *
 *   version 0 (INITIAL)  offset 8 is a uint32 COUNT, and the entries are
 *                        FIXED 12-byte ace_t
 *   version 1 (FUID)     offset 8 is a uint32 size in BYTES, offset 14 is a
 *                        uint16 count, and the entries are the same
 *                        variable-width form a v5 pool stores
 *
 * The vendored zfsimpl.h declares only the version 0 shape (zfs_znode_acl_t),
 * because a boot loader never reads an ACL at all. Version 1's two fields are
 * therefore read out by offset below rather than through a struct, with the
 * offsets named - which is uglier than a second typedef and has the advantage
 * that the numbers being relied on are visible at the point of use.
 *
 * --- the field order trap --------------------------------------------------
 * An OLD ace_t is {uint32 who, uint32 mask, uint16 flags, uint16 type}. A
 * modern zfs_ace_hdr_t is {uint16 type, uint16 flags, uint32 mask}. The who
 * comes FIRST in one and last in the other, and the type moves from offset 0
 * to offset 8. Decoding an old ACE with the modern layout reads the low half
 * of the uid as the type: for uid 1001 that is 0x03E9, which is not a
 * recognised ACE type, so the entry would be SKIPPED rather than misread -
 * an ACL that quietly loses its most interesting entry and still looks
 * well-formed. genesispool.dat carries exactly that entry so the mistake
 * cannot pass.
 */

#define ZFSG_ZACL_OFF_SIZE     8   /* v1: uint32 size in bytes            */
#define ZFSG_ZACL_OFF_VERSION 12   /* both: uint16                        */
#define ZFSG_ZACL_OFF_COUNT   14   /* v1: uint16 count (v0 pads here)     */
#define ZFSG_ZACL_INLINE_MAX  72   /* ACE_SLOT_CNT * sizeof (ace_t)       */

#define ZFSG_ACL_VERSION_INITIAL 0
#define ZFSG_ACL_VERSION_FUID    1

/* One fixed-width old ACE. Twelve bytes, in the order given above. */
static void
zfsg_decode_oldace(const unsigned char *p, zfsg_ace_t *out)
{
	uint32_t who, mask;
	uint16_t flags, type;

	memcpy(&who, p + 0, sizeof (who));
	memcpy(&mask, p + 4, sizeof (mask));
	memcpy(&flags, p + 8, sizeof (flags));
	memcpy(&type, p + 10, sizeof (type));

	out->type = type;
	out->flags = flags;
	out->mask = mask;
	/* The three special entry types carry their identity in the flags, and
	 * what is in z_fuid for them is whatever the writer happened to leave -
	 * OpenZFS copies the field through without interpreting it. Zeroing it
	 * here means a caller cannot accidentally treat an owner@ entry as
	 * naming uid 0. */
	{
		unsigned int entry = flags & ACE_TYPE_FLAGS;

		if (entry == ACE_OWNER || entry == ACE_OWNING_GROUP ||
		    entry == ACE_EVERYONE) {
			out->who = 0;
		} else {
			out->who = (unsigned long long)who;
		}
	}
}

/* The variable-width decode, shared with the SA path. Returns the number of
 * entries written, or a negative value on a malformed stream. */
static int
zfsg_decode_ace_stream(const unsigned char *p, size_t len, int want,
    zfsg_ace_t *out, int max)
{
	size_t consumed = 0;
	int n = 0;

	while (n < want && n < max) {
		unsigned int type, flags, mask, sz;
		uint16_t t16, f16;
		uint32_t m32;

		if (consumed + 8 > len) {
			return (-1);
		}
		memcpy(&t16, p + consumed + 0, sizeof (t16));
		memcpy(&f16, p + consumed + 2, sizeof (f16));
		memcpy(&m32, p + consumed + 4, sizeof (m32));
		type = t16;
		flags = f16;
		mask = m32;

		sz = zfsg_ace_size(type, flags);
		if (consumed + sz > len) {
			return (-1);
		}

		out[n].type = type;
		out[n].flags = flags;
		out[n].mask = mask;
		out[n].who = 0;
		if (sz >= 16) {
			uint64_t who;

			memcpy(&who, p + consumed + 8, sizeof (who));
			out[n].who = (unsigned long long)who;
		}
		consumed += sz;
		n++;
	}
	return (n);
}

static int
zfsg_getacl_znode(zfsg_mount_t *mount, dnode_phys_t *dn, zfsg_ace_t *out,
    int max, int *count_out, unsigned long long *pflags_out)
{
	const znode_phys_t *zp;
	const unsigned char *za;
	const unsigned char *aces;
	unsigned char *spill = NULL;
	uint64_t extern_obj;
	uint16_t version, count16;
	uint32_t count32;
	size_t avail;
	unsigned long long want;
	int n, rc = 0;

	if (dn->dn_bonuslen < sizeof (znode_phys_t)) {
		/* Not a znode at all, or one truncated past the ACL. Either way
		 * there is nothing here to read. */
		return (ENOENT);
	}
	zp = (const znode_phys_t *)DN_BONUS(dn);
	if (pflags_out != NULL) {
		*pflags_out = (unsigned long long)zp->zp_flags;
	}

	za = (const unsigned char *)&zp->zp_acl;
	memcpy(&extern_obj, za + 0, sizeof (extern_obj));
	memcpy(&version, za + ZFSG_ZACL_OFF_VERSION, sizeof (version));

	if (version == ZFSG_ACL_VERSION_INITIAL) {
		memcpy(&count32, za + ZFSG_ZACL_OFF_SIZE, sizeof (count32));
		want = (unsigned long long)count32;
	} else if (version == ZFSG_ACL_VERSION_FUID) {
		memcpy(&count16, za + ZFSG_ZACL_OFF_COUNT, sizeof (count16));
		want = (unsigned long long)count16;
	} else {
		/* A version this code has never heard of. Refused rather than
		 * guessed: the two known versions disagree about what the
		 * uint32 at offset 8 MEANS, so a third could disagree about
		 * anything, and reading it as either would produce a
		 * confident wrong answer. */
		return (ENOTSUP);
	}

	if (want == 0) {
		return (ENOENT);          /* no ACL stored; project the mode */
	}
	if (want > (unsigned long long)max) {
		return (E2BIG);
	}

	/* Where the entries actually live.
	 *
	 * z_acl_extern_obj is the escape hatch for an ACL too big for the
	 * 72 bytes inline: the entries are in a DMU object of their own and
	 * z_ace_data is not them. Handled rather than refused, because six
	 * entries is a low ceiling - the fixture's ACL uses four - and a file
	 * with a seventh would otherwise read as having no ACL at all, which
	 * is the failure that grants access rather than denying it. */
	if (extern_obj != 0) {
		dnode_phys_t adn;
		size_t need;

		need = (version == ZFSG_ACL_VERSION_INITIAL)
		    ? (size_t)want * sizeof (ace_t)
		    : (size_t)ZAP_MAXVALUELEN;
		if (version != ZFSG_ACL_VERSION_INITIAL) {
			uint32_t bytes;

			memcpy(&bytes, za + ZFSG_ZACL_OFF_SIZE, sizeof (bytes));
			need = (size_t)bytes;
		}
		if (need == 0 || need > (size_t)(max * 48)) {
			return (EIO);
		}

		rc = objset_get_dnode(mount->spa, &mount->mnt.objset,
		    extern_obj, &adn);
		if (rc != 0) {
			return (rc);
		}
		spill = malloc(need);
		if (spill == NULL) {
			return (ENOMEM);
		}
		rc = dnode_read(mount->spa, &adn, 0, spill, need);
		if (rc != 0) {
			free(spill);
			return (rc);
		}
		aces = spill;
		avail = need;
	} else {
		aces = za + 16;           /* past extern_obj/size/version/pad */
		avail = ZFSG_ZACL_INLINE_MAX;
	}

	if (version == ZFSG_ACL_VERSION_INITIAL) {
		unsigned long long i;

		if ((size_t)want * sizeof (ace_t) > avail) {
			rc = EIO;
			goto done;
		}
		for (i = 0; i < want; i++) {
			zfsg_decode_oldace(aces + i * sizeof (ace_t),
			    &out[i]);
		}
		n = (int)want;
	} else {
		n = zfsg_decode_ace_stream(aces, avail, (int)want, out, max);
		if (n < 0) {
			rc = EIO;
			goto done;
		}
	}

	*count_out = n;
	rc = 0;
done:
	if (spill != NULL) {
		free(spill);
	}
	return (rc);
}

int
zfsg_getacl(zfsg_mount_t *mount, unsigned long long objnum, zfsg_ace_t *out,
    int max, int *count_out, unsigned long long *pflags_out)
{
	dnode_phys_t dn;
	void *sabuf, *freeme = NULL;
	size_t sabuflen, aoff, alen, coff, clen, foff, flen;
	const unsigned char *p;
	unsigned long long want;
	int n = 0, rc;

	if (mount == NULL || !mount->in_use)
		return (ENXIO);
	if (out == NULL || count_out == NULL || max <= 0)
		return (EINVAL);

	*count_out = 0;
	if (pflags_out != NULL)
		*pflags_out = 0;

	rc = objset_get_dnode(mount->spa, &mount->mnt.objset, objnum, &dn);
	if (rc != 0)
		return (rc);

	/* A pre-v5 object keeps its ACL in the znode's own bonus buffer, and
	 * there is no attribute layout to walk to find it. Note this is
	 * decided per OBJECT rather than per filesystem: a pool upgraded to
	 * version 5 still has old objects in it until something rewrites them,
	 * so the two paths coexist on one mount and the dnode's bonus TYPE is
	 * the only thing that says which applies. */
	if (dn.dn_bonustype != DMU_OT_SA) {
		return (zfsg_getacl_znode(mount, &dn, out, max, count_out,
		    pflags_out));
	}

	rc = zfsg_sa_build(mount);
	if (rc != 0)
		return (rc);

	rc = zfsg_sa_buffer(mount->spa, &dn, &sabuf, &sabuflen, &freeme);
	if (rc != 0)
		return (rc);

	/* pflags first, because ZFS_ACL_TRIVIAL in it tells the caller it may
	 * skip everything else and project the mode instead. Reported rather
	 * than acted on here: this function's job is to say what is on disk,
	 * and deciding whether the ACL is worth consulting belongs with the
	 * evaluator that knows what is being asked. */
	if (zfsg_sa_locate(mount, sabuf, sabuflen, ZFSG_SA_I_FLAGS, &foff,
	    &flen) == 0 && flen == sizeof (uint64_t) && pflags_out != NULL) {
		uint64_t v;

		memcpy(&v, (const unsigned char *)sabuf + foff, sizeof (v));
		*pflags_out = (unsigned long long)v;
	}

	rc = zfsg_sa_locate(mount, sabuf, sabuflen, ZFSG_SA_I_DACL_COUNT,
	    &coff, &clen);
	if (rc != 0)
		goto done;
	if (clen != sizeof (uint64_t)) {
		rc = EIO;
		goto done;
	}
	{
		uint64_t v;

		memcpy(&v, (const unsigned char *)sabuf + coff, sizeof (v));
		want = (unsigned long long)v;
	}

	rc = zfsg_sa_locate(mount, sabuf, sabuflen, ZFSG_SA_I_DACL_ACES,
	    &aoff, &alen);
	if (rc != 0)
		goto done;

	/* An ACL that does not fit is refused, not truncated. A truncated ACL
	 * is a DIFFERENT ACL, and specifically one missing its tail - which
	 * for NFSv4, where deny entries are conventionally placed after the
	 * allows they qualify, is the half that says no. */
	if (want > (unsigned long long)max) {
		rc = E2BIG;
		goto done;
	}

	/* The same walk the version 1 znode ACL needs, so it is written once.
	 * A negative return means the count and the blob disagree, or an entry
	 * runs off the end of it - believe neither. */
	p = (const unsigned char *)sabuf + aoff;
	n = zfsg_decode_ace_stream(p, alen, (int)want, out, max);
	if (n < 0) {
		rc = EIO;
		goto done;
	}

	*count_out = n;
	rc = 0;
done:
	if (freeme != NULL)
		free(freeme);
	return (rc);
}

/* Read one 64-bit SA attribute by table index. Used for the fields
 * zfs_dnode_stat's fixed offsets do not reach - uid and gid it does reach,
 * but through constants that only happen to be right for the default layout,
 * and pflags it does not reach at all. */
static int
zfsg_sa_u64(zfsg_mount_t *mount, dnode_phys_t *dn, int which,
    unsigned long long *out)
{
	void *sabuf, *freeme = NULL;
	size_t sabuflen, off, len;
	uint64_t v;
	int rc;

	rc = zfsg_sa_build(mount);
	if (rc != 0)
		return (rc);
	if (dn->dn_bonustype != DMU_OT_SA)
		return (ENOENT);

	rc = zfsg_sa_buffer(mount->spa, dn, &sabuf, &sabuflen, &freeme);
	if (rc != 0)
		return (rc);

	rc = zfsg_sa_locate(mount, sabuf, sabuflen, which, &off, &len);
	if (rc == 0) {
		if (len == sizeof (uint64_t)) {
			memcpy(&v, (const unsigned char *)sabuf + off,
			    sizeof (v));
			*out = (unsigned long long)v;
		} else {
			rc = EIO;
		}
	}
	if (freeme != NULL)
		free(freeme);
	return (rc);
}
