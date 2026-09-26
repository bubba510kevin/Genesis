/* Allocation: reading ZFS space maps, which is the gate on writing anything.
 *
 * Included from zfs_glue.inc, inside the vendored translation unit, because
 * objset_get_dnode, dnode_read, zap_lookup and load_nvlist are all static in
 * zfsimpl.c.
 *
 * --- why the vendored reader has none of this ------------------------------
 * A boot loader reads. It never allocates, so it never needs to know which
 * blocks are free, so zfsimpl.c contains the string "space_map" exactly zero
 * times. Everything below is new code against a documented on-disk format,
 * not a port of something that was already here.
 *
 * --- what a space map is ---------------------------------------------------
 * Not a bitmap. Each metaslab has a LOG of allocations and frees, appended to
 * in txg order, and the current state is what you get by replaying it from
 * the beginning. That is why this file has a range tree in it: the log says
 * "allocated [a,b), freed [c,d), allocated [e,f)" and the answer to "what is
 * free" is only available after the whole thing has been read.
 *
 * Three entry shapes, distinguished by the top two bits:
 *
 *   prefix 2   a DEBUG entry - txg and sync pass, no allocation information.
 *              Skipped here, but it must be RECOGNISED, because it is one
 *              word and mistaking it for a two-word entry desynchronises the
 *              rest of the log.
 *   prefix 3   a two-word V2 entry: a 36-bit run and a 24-bit vdev id in the
 *              first word, a type bit and a 63-bit offset in the second.
 *   otherwise  a one-word V1 entry: 47-bit offset, 1 type bit, 15-bit run.
 *
 * Offsets and runs are in units of the metaslab's sm_shift (the vdev's
 * ashift), and offsets are relative to the metaslab's own start. A decoder
 * that forgets either of those produces ranges that look plausible and are
 * in the wrong place - which, for an allocator, means handing out blocks that
 * are already in use.
 */

#define SM_DEBUG_PREFIX_G 2
#define SM2_PREFIX_G      3
#define SPA_VDEVBITS_G    24
#define SM2_RUN_BITS_G    36
#define SM2_OFFSET_BITS_G 63
#define SM_OFFSET_BITS_G  47
#define SM_RUN_BITS_G     15

#define SM_ALLOC_G 0
#define SM_FREE_G  1

/* BF64_DECODE, spelled out rather than reused: zfsimpl.h has the macro, but
 * writing the three uses here explicitly keeps the bit positions next to the
 * comment that explains them. */
static uint64_t
zfsg_bf64(uint64_t x, int low, int len)
{
	return ((x >> low) & ((1ULL << len) - 1));
}

/* --- a range tree ----------------------------------------------------------
 *
 * A sorted, coalesced set of non-overlapping [start, start+size) ranges.
 * Add marks a range present, remove marks it absent - and remove is the
 * interesting one, because taking the middle out of a range SPLITS it and
 * that is where an off-by-one turns into an allocator that overlaps a live
 * block.
 *
 * A flat array rather than a tree. Metaslab space maps in a pool this size
 * hold tens of entries, the array is scanned once per allocation, and an
 * AVL tree here would be more code than the thing it accelerates. The cap is
 * enforced rather than assumed: running out of room REFUSES rather than
 * dropping a range, because a dropped allocated-range is a block the
 * allocator will hand out twice. */
#define ZFSG_RT_MAX 1024

typedef struct zfsg_range {
	uint64_t start;
	uint64_t size;
} zfsg_range_t;

typedef struct zfsg_rt {
	zfsg_range_t *seg;
	int           count;
	int           cap;
	int           overflow;   /* set once a range could not be recorded */
} zfsg_rt_t;

static int
zfsg_rt_init(zfsg_rt_t *rt, int cap)
{
	rt->seg = malloc((size_t)cap * sizeof (zfsg_range_t));
	if (rt->seg == NULL)
		return (ENOMEM);
	rt->count = 0;
	rt->cap = cap;
	rt->overflow = 0;
	return (0);
}

static void
zfsg_rt_free(zfsg_rt_t *rt)
{
	if (rt->seg != NULL) {
		free(rt->seg);
		rt->seg = NULL;
	}
	rt->count = 0;
}

/* Insert at `idx`, shifting the tail up. */
static int
zfsg_rt_insert_at(zfsg_rt_t *rt, int idx, uint64_t start, uint64_t size)
{
	int i;

	if (rt->count >= rt->cap) {
		rt->overflow = 1;
		return (E2BIG);
	}
	for (i = rt->count; i > idx; i--)
		rt->seg[i] = rt->seg[i - 1];
	rt->seg[idx].start = start;
	rt->seg[idx].size = size;
	rt->count++;
	return (0);
}

static void
zfsg_rt_remove_at(zfsg_rt_t *rt, int idx)
{
	int i;

	for (i = idx; i + 1 < rt->count; i++)
		rt->seg[i] = rt->seg[i + 1];
	rt->count--;
}

/* Mark [start, start+size) present, coalescing with any neighbour it touches.
 *
 * Overlapping an existing range is not tolerated silently. In a well-formed
 * space map an allocation cannot overlap a live allocation, so an overlap
 * means the log was misdecoded - and continuing would produce an allocator
 * that believes a smaller region is in use than really is. */
static int
zfsg_rt_add(zfsg_rt_t *rt, uint64_t start, uint64_t size)
{
	uint64_t end = start + size;
	int i;

	if (size == 0)
		return (0);

	for (i = 0; i < rt->count; i++) {
		uint64_t s = rt->seg[i].start;
		uint64_t e = s + rt->seg[i].size;

		if (end < s)
			break;                 /* insert before i */
		if (start > e)
			continue;              /* entirely after i */
		/* Touching or overlapping. */
		if (start < e && end > s)
			return (EINVAL);       /* a real overlap: misdecoded */
		/* Adjacent - coalesce. */
		if (end == s) {
			rt->seg[i].start = start;
			rt->seg[i].size += size;
		} else {
			rt->seg[i].size += size;
		}
		/* Coalescing may now have made i and i+1 adjacent. */
		while (i + 1 < rt->count &&
		    rt->seg[i].start + rt->seg[i].size == rt->seg[i + 1].start) {
			rt->seg[i].size += rt->seg[i + 1].size;
			zfsg_rt_remove_at(rt, i + 1);
		}
		return (0);
	}
	return (zfsg_rt_insert_at(rt, i, start, size));
}

/* Mark [start, start+size) absent. Splits a range if the hole is interior. */
static int
zfsg_rt_remove(zfsg_rt_t *rt, uint64_t start, uint64_t size)
{
	uint64_t end = start + size;
	int i;

	if (size == 0)
		return (0);

	for (i = 0; i < rt->count; i++) {
		uint64_t s = rt->seg[i].start;
		uint64_t e = s + rt->seg[i].size;

		if (end <= s)
			break;
		if (start >= e)
			continue;

		if (start <= s && end >= e) {
			zfsg_rt_remove_at(rt, i);   /* whole range goes */
			i--;
			continue;
		}
		if (start > s && end < e) {
			/* Interior hole: keep the head, insert the tail. */
			rt->seg[i].size = start - s;
			return (zfsg_rt_insert_at(rt, i + 1, end, e - end));
		}
		if (start <= s) {
			rt->seg[i].start = end;     /* trim the front */
			rt->seg[i].size = e - end;
		} else {
			rt->seg[i].size = start - s; /* trim the back */
		}
	}
	return (0);
}

/* --- the space map log ----------------------------------------------------- */

typedef struct zfsg_alloc {
	int      valid;
	uint64_t ms_array;    /* MOS object: array of space map object numbers */
	uint64_t ms_shift;    /* log2 of a metaslab's size                     */
	uint64_t ms_count;    /* how many metaslabs this vdev has              */
	uint64_t asize;       /* the vdev's allocatable size                   */
	int      ashift;      /* log2 of the smallest allocation unit          */
} zfsg_alloc_t;

/* The top-level vdev's allocation geometry, from the MOS config nvlist.
 *
 * The reader keeps v_ashift on the vdev but never reads metaslab_array or
 * metaslab_shift - the constants are defined in zfsimpl.h and unused, because
 * nothing that only reads needs them. So the config is loaded again here
 * rather than threaded out of vdev_init_from_nvlist, which destroys the
 * nvlist before it returns. */
static int
zfsg_alloc_geometry(spa_t *spa, zfsg_alloc_t *out)
{
	dnode_phys_t dir;
	nvlist_t *config = NULL, *vdevs = NULL, **kids = NULL;
	uint64_t config_object;
	int nkids = 0, rc;

	out->valid = 0;

	rc = objset_get_dnode(spa, &spa->spa_mos_master,
	    DMU_POOL_DIRECTORY_OBJECT, &dir);
	if (rc != 0)
		return (rc);
	rc = zap_lookup(spa, &dir, DMU_POOL_CONFIG, sizeof (config_object), 1,
	    &config_object);
	if (rc != 0)
		return (rc);
	rc = load_nvlist(spa, config_object, &config);
	if (rc != 0 || config == NULL)
		return (rc != 0 ? rc : EIO);

	if (nvlist_find(config, ZPOOL_CONFIG_VDEV_TREE, DATA_TYPE_NVLIST,
	    NULL, &vdevs, NULL) != 0) {
		nvlist_destroy(config);
		return (ENOENT);
	}
	rc = nvlist_find(vdevs, ZPOOL_CONFIG_CHILDREN, DATA_TYPE_NVLIST_ARRAY,
	    &nkids, &kids, NULL);
	if (rc != 0 || nkids < 1) {
		nvlist_destroy(vdevs);
		nvlist_destroy(config);
		return (rc != 0 ? rc : ENOENT);
	}

	/* The first top-level vdev, and ONLY the first.
	 *
	 * A pool with several top-level vdevs stripes across all of them and
	 * choosing one is a policy decision this code is not equipped to make
	 * - so a multi-vdev pool is refused rather than written to badly. The
	 * fixtures are single-vdev; a mirror or raidz would also need the
	 * write replicated, which is a different piece of work again. */
	if (nkids != 1) {
		rc = ENOTSUP;
	} else {
		uint64_t ms_array = 0, ms_shift = 0, asize = 0, ashift = 0;

		if (nvlist_find(kids[0], ZPOOL_CONFIG_METASLAB_ARRAY,
		    DATA_TYPE_UINT64, NULL, &ms_array, NULL) != 0 ||
		    nvlist_find(kids[0], ZPOOL_CONFIG_METASLAB_SHIFT,
		    DATA_TYPE_UINT64, NULL, &ms_shift, NULL) != 0 ||
		    nvlist_find(kids[0], ZPOOL_CONFIG_ASIZE,
		    DATA_TYPE_UINT64, NULL, &asize, NULL) != 0 ||
		    nvlist_find(kids[0], ZPOOL_CONFIG_ASHIFT,
		    DATA_TYPE_UINT64, NULL, &ashift, NULL) != 0) {
			rc = ENOENT;
		} else {
			out->ms_array = ms_array;
			out->ms_shift = ms_shift;
			out->asize = asize;
			out->ashift = (int)ashift;
			out->ms_count = asize >> ms_shift;
			out->valid = 1;
			rc = 0;
		}
	}

	for (int i = 0; i < nkids; i++)
		nvlist_destroy(kids[i]);
	free(kids);
	nvlist_destroy(vdevs);
	nvlist_destroy(config);
	return (rc);
}

/* Replay one metaslab's space map into `rt`, which ends up holding the
 * ALLOCATED ranges as absolute byte offsets within the vdev.
 *
 * A space map object number of zero is not an error: it means the metaslab
 * has never been allocated from, so nothing is allocated in it. That is the
 * common case on a fresh pool and the whole of the free space on one. */
static int
zfsg_sm_load(spa_t *spa, const zfsg_alloc_t *ga, uint64_t sm_obj,
    uint64_t ms_start, zfsg_rt_t *rt)
{
	dnode_phys_t dn;
	uint64_t *buf;
	uint64_t length;
	size_t i, words;
	int rc;

	if (sm_obj == 0)
		return (0);

	rc = objset_get_dnode(spa, &spa->spa_mos_master, sm_obj, &dn);
	if (rc != 0)
		return (rc);

	/* smp_length is the number of BYTES of the log that are live. The
	 * object itself is usually larger - the tail is stale. Replaying past
	 * smp_length reads entries from a previous txg that have already been
	 * superseded, which produces a range tree describing a pool that no
	 * longer exists. */
	if (dn.dn_bonuslen < sizeof (uint64_t) * 3)
		return (EIO);
	length = ((const uint64_t *)DN_BONUS(&dn))[1];   /* smp_length */
	if (length == 0)
		return (0);
	if ((length & 7) != 0)
		return (EIO);

	buf = malloc((size_t)length);
	if (buf == NULL)
		return (ENOMEM);
	rc = dnode_read(spa, &dn, 0, buf, (size_t)length);
	if (rc != 0) {
		free(buf);
		return (rc);
	}

	words = (size_t)(length / sizeof (uint64_t));
	for (i = 0; i < words; i++) {
		uint64_t w = buf[i];
		uint64_t prefix = zfsg_bf64(w, 62, 2);
		uint64_t off, run, type;

		if (prefix == SM_DEBUG_PREFIX_G) {
			/* txg and sync pass. Carries no allocation, but it is
			 * ONE word - treating it as two desynchronises
			 * everything after it. */
			continue;
		}

		if (prefix == SM2_PREFIX_G) {
			uint64_t w2;

			if (i + 1 >= words) {
				rc = EIO;    /* truncated two-word entry */
				break;
			}
			w2 = buf[++i];
			run = zfsg_bf64(w, SPA_VDEVBITS_G, SM2_RUN_BITS_G) + 1;
			type = zfsg_bf64(w2, SM2_OFFSET_BITS_G, 1);
			off = zfsg_bf64(w2, 0, SM2_OFFSET_BITS_G);
		} else {
			off = zfsg_bf64(w, 16, SM_OFFSET_BITS_G);
			type = zfsg_bf64(w, 15, 1);
			run = zfsg_bf64(w, 0, SM_RUN_BITS_G) + 1;
		}

		/* Units of sm_shift, relative to the metaslab's own start. */
		off = ms_start + (off << ga->ashift);
		run = run << ga->ashift;

		if (type == SM_ALLOC_G)
			rc = zfsg_rt_add(rt, off, run);
		else
			rc = zfsg_rt_remove(rt, off, run);
		if (rc != 0)
			break;
	}

	free(buf);
	return (rc);
}

/* Read the space map object number for metaslab `n`. */
static int
zfsg_ms_object(spa_t *spa, const zfsg_alloc_t *ga, uint64_t n, uint64_t *out)
{
	dnode_phys_t dn;
	int rc;

	rc = objset_get_dnode(spa, &spa->spa_mos_master, ga->ms_array, &dn);
	if (rc != 0)
		return (rc);
	return (dnode_read(spa, &dn, n * sizeof (uint64_t), out,
	    sizeof (uint64_t)));
}

/* --- what the rest of Genesis can ask -------------------------------------
 *
 * Replays every metaslab's space map and reports the totals. This exists to
 * be CHECKED rather than used: `zdb -mmm` and `zpool list` print the same
 * numbers from an independent implementation, so a decoder that is subtly
 * wrong about entry widths or shift units shows up as a byte count that does
 * not match rather than as a corrupted pool six steps later.
 *
 * Nothing allocates yet. This is the half of allocation that can be verified
 * without risking a pool, and it is deliberately landed and tested on its own
 * before anything writes.
 */
int
zfsg_alloc_stats(zfsg_mount_t *mount, unsigned long long *asize_out,
    unsigned long long *alloc_out, unsigned long long *ms_count_out,
    unsigned long long *ms_shift_out)
{
	zfsg_alloc_t ga;
	zfsg_rt_t rt;
	uint64_t total = 0;
	uint64_t n;
	int rc;

	if (mount == NULL || !mount->in_use)
		return (ENXIO);

	rc = zfsg_alloc_geometry(mount->spa, &ga);
	if (rc != 0)
		return (rc);

	if (asize_out != NULL)
		*asize_out = (unsigned long long)ga.asize;
	if (ms_count_out != NULL)
		*ms_count_out = (unsigned long long)ga.ms_count;
	if (ms_shift_out != NULL)
		*ms_shift_out = (unsigned long long)ga.ms_shift;

	rc = zfsg_rt_init(&rt, ZFSG_RT_MAX);
	if (rc != 0)
		return (rc);

	for (n = 0; n < ga.ms_count; n++) {
		uint64_t sm_obj = 0;
		uint64_t ms_start = n << ga.ms_shift;
		int i;

		rc = zfsg_ms_object(mount->spa, &ga, n, &sm_obj);
		if (rc != 0)
			break;

		/* One metaslab at a time: the tree is reused, so the ranges
		 * are counted and dropped rather than accumulated across the
		 * whole vdev. A pool's worth of allocated ranges would not fit
		 * in one tree and does not need to. */
		rt.count = 0;
		rt.overflow = 0;
		rc = zfsg_sm_load(mount->spa, &ga, sm_obj, ms_start, &rt);
		if (rc != 0)
			break;
		if (rt.overflow) {
			/* Refused rather than under-reported. A dropped
			 * allocated range is a block an allocator would hand
			 * out twice. */
			rc = E2BIG;
			break;
		}
		for (i = 0; i < rt.count; i++)
			total += rt.seg[i].size;
	}

	zfsg_rt_free(&rt);
	if (rc != 0)
		return (rc);

	if (alloc_out != NULL)
		*alloc_out = (unsigned long long)total;
	return (0);
}

/* --- the allocator ---------------------------------------------------------
 *
 * Given the allocated ranges a metaslab's space map replays to, find a free
 * run and hand it out. Two things make this harder than a free-list walk and
 * both are worth naming, because getting either wrong corrupts a pool rather
 * than failing a read:
 *
 *   THE SPACE MAP DESCRIBES THE LAST COMMITTED TXG, not the one being built.
 *   Allocations made since then exist only in memory, so a second allocation
 *   in the same transaction must see the first one or the two overlap. That
 *   is what the pending list below is for, and it is checked in the same walk
 *   rather than as an afterthought.
 *
 *   A METASLAB DOES NOT NECESSARILY REACH THE END OF THE VDEV. ms_count is a
 *   truncating divide of asize, so the tail belongs to no metaslab and must
 *   never be allocated - the last metaslab's end is its own start plus its
 *   size, not asize.
 */

/* How many separate allocations one transaction may make. A single small
 * write needs a handful: the data block, a dnode block, an objset block, and
 * whatever MOS blocks change above them. Bounded so a runaway cannot consume
 * the pool, and REFUSED rather than silently wrapping. */
/* The cascade allocates far more than a first estimate suggests. The MOS's
 * dnode array is SIX levels deep on a real pool, so every single dnode update
 * rewrites six blocks - and each space map append is itself a dnode update.
 * Sixty-four was exhausted immediately. */
#define ZFSG_TXG_MAX_ALLOC 256

typedef struct zfsg_txg {
	int          active;
	uint64_t     txg;             /* the txg being built                 */
	zfsg_alloc_t ga;
	zfsg_range_t pending[ZFSG_TXG_MAX_ALLOC];
	int          npending;
} zfsg_txg_t;

/* Does [start,size) overlap anything this transaction has already handed
 * out? The space map cannot know about those - they are not on disk yet. */
static int
zfsg_pending_overlaps(const zfsg_txg_t *tx, uint64_t start, uint64_t size)
{
	int i;

	for (i = 0; i < tx->npending; i++) {
		uint64_t s = tx->pending[i].start;
		uint64_t e = s + tx->pending[i].size;

		if (start < e && s < start + size)
			return (1);
	}
	return (0);
}

/* Find a free run of `size` bytes in metaslab `n`, or ENOSPC.
 *
 * `rt` holds that metaslab's ALLOCATED ranges, sorted. The free space is the
 * complement, so this walks the gaps: before the first range, between each
 * adjacent pair, and after the last. */
static int
zfsg_ms_find_free(const zfsg_txg_t *tx, const zfsg_rt_t *rt, uint64_t ms_start,
    uint64_t ms_size, uint64_t size, uint64_t *out)
{
	uint64_t align = 1ULL << tx->ga.ashift;
	uint64_t cursor = ms_start;
	uint64_t ms_end = ms_start + ms_size;
	int i;

	for (i = 0; i <= rt->count; i++) {
		uint64_t gap_end = (i < rt->count) ? rt->seg[i].start : ms_end;
		uint64_t cand;

		/* Round the candidate up to the vdev's allocation unit. A DVA
		 * offset is stored in units of 512 bytes but must be aligned
		 * to the ashift, and an unaligned allocation on a 4Kn device
		 * is a read-modify-write at best. */
		cand = (cursor + align - 1) & ~(align - 1);

		while (cand + size <= gap_end) {
			if (!zfsg_pending_overlaps(tx, cand, size)) {
				*out = cand;
				return (0);
			}
			/* Step past whatever pending allocation blocked it
			 * rather than giving up on the gap: several small
			 * allocations in one txg land in the same gap. */
			cand += align;
		}

		if (i < rt->count) {
			uint64_t seg_end = rt->seg[i].start + rt->seg[i].size;

			if (seg_end > cursor)
				cursor = seg_end;
		}
	}
	return (ENOSPC);
}

/* Allocate `size` bytes somewhere on the vdev and record it in the
 * transaction. Returns the byte offset within the vdev.
 *
 * First fit across metaslabs in order. Not because it is a good policy - real
 * ZFS weights metaslabs by free space and fragmentation and keeps a cursor
 * per allocator - but because a wrong POLICY costs performance and a wrong
 * IMPLEMENTATION costs the pool, and this is the version whose correctness
 * can be argued in one screen. */
static int
zfsg_alloc_dva(spa_t *spa, zfsg_txg_t *tx, uint64_t size, uint64_t *off_out)
{
	zfsg_rt_t rt;
	uint64_t n;
	int rc;

	if (!tx->active || !tx->ga.valid)
		return (EINVAL);
	if (size == 0 || (size & ((1ULL << tx->ga.ashift) - 1)) != 0)
		return (EINVAL);
	if (tx->npending >= ZFSG_TXG_MAX_ALLOC)
		return (E2BIG);

	rc = zfsg_rt_init(&rt, ZFSG_RT_MAX);
	if (rc != 0)
		return (rc);

	for (n = 0; n < tx->ga.ms_count; n++) {
		uint64_t sm_obj = 0;
		uint64_t ms_start = n << tx->ga.ms_shift;
		uint64_t ms_size = 1ULL << tx->ga.ms_shift;
		uint64_t found;

		/* The tail of the vdev that no metaslab covers. */
		if (ms_start + ms_size > tx->ga.asize) {
			if (ms_start >= tx->ga.asize)
				break;
			ms_size = tx->ga.asize - ms_start;
		}

		rc = zfsg_ms_object(spa, &tx->ga, n, &sm_obj);
		if (rc != 0)
			break;

		rt.count = 0;
		rt.overflow = 0;
		rc = zfsg_sm_load(spa, &tx->ga, sm_obj, ms_start, &rt);
		if (rc != 0)
			break;
		if (rt.overflow) {
			/* This metaslab's allocated set did not fit, so its
			 * free set is unknown. Skipped rather than guessed:
			 * allocating from a metaslab whose state was
			 * truncated is how a live block gets handed out. */
			continue;
		}

		if (zfsg_ms_find_free(tx, &rt, ms_start, ms_size, size,
		    &found) == 0) {
			tx->pending[tx->npending].start = found;
			tx->pending[tx->npending].size = size;
			tx->npending++;
			*off_out = found;
			zfsg_rt_free(&rt);
			return (0);
		}
	}

	zfsg_rt_free(&rt);
	return (rc != 0 ? rc : ENOSPC);
}

/* Open a transaction. The txg is the committed one plus one - the number a
 * new uberblock would carry. */
static int
zfsg_txg_open(spa_t *spa, zfsg_txg_t *tx)
{
	int rc;

	memset(tx, 0, sizeof (*tx));
	rc = zfsg_alloc_geometry(spa, &tx->ga);
	if (rc != 0)
		return (rc);
	tx->txg = spa->spa_uberblock->ub_txg + 1;
	tx->active = 1;
	return (0);
}

/* --- allocating without committing ----------------------------------------
 *
 * Runs the allocator `count` times and hands back the offsets, having written
 * NOTHING. The transaction is opened, used and thrown away, so the pool is
 * untouched and the allocator can be tested on a real pool's real free space
 * before anything is trusted to modify one.
 *
 * It also re-checks each result against the on-disk space maps itself, which
 * is not redundant with the caller's checks: the caller can see that the
 * offsets do not overlap EACH OTHER, and only this side can see whether they
 * overlap something already allocated.
 */
int
zfsg_alloc_probe(zfsg_mount_t *mount, unsigned long long size, int count,
    unsigned long long *out, int *conflicts_out)
{
	zfsg_txg_t tx;
	zfsg_rt_t rt;
	int i, rc, conflicts = 0;

	if (mount == NULL || !mount->in_use || out == NULL || count <= 0)
		return (EINVAL);
	if (count > ZFSG_TXG_MAX_ALLOC)
		return (E2BIG);

	rc = zfsg_txg_open(mount->spa, &tx);
	if (rc != 0)
		return (rc);

	for (i = 0; i < count; i++) {
		uint64_t off = 0;

		rc = zfsg_alloc_dva(mount->spa, &tx, (uint64_t)size, &off);
		if (rc != 0)
			return (rc);
		out[i] = (unsigned long long)off;
	}

	/* Now hold every result against what is really allocated on disk. */
	rc = zfsg_rt_init(&rt, ZFSG_RT_MAX);
	if (rc != 0)
		return (rc);

	for (i = 0; i < count; i++) {
		uint64_t off = (uint64_t)out[i];
		uint64_t n = off >> tx.ga.ms_shift;
		uint64_t ms_start = n << tx.ga.ms_shift;
		uint64_t sm_obj = 0;
		int j;

		if (zfsg_ms_object(mount->spa, &tx.ga, n, &sm_obj) != 0)
			continue;
		rt.count = 0;
		rt.overflow = 0;
		if (zfsg_sm_load(mount->spa, &tx.ga, sm_obj, ms_start,
		    &rt) != 0)
			continue;
		for (j = 0; j < rt.count; j++) {
			uint64_t s = rt.seg[j].start;
			uint64_t e = s + rt.seg[j].size;

			if (off < e && s < off + size)
				conflicts++;
		}
	}

	zfsg_rt_free(&rt);
	if (conflicts_out != NULL)
		*conflicts_out = conflicts;
	return (0);
}

/* --- zio_write -------------------------------------------------------------
 *
 * Allocate space for a block, write it, and fill in the block pointer that
 * describes it. The inverse of the reader's zio_read, and it uses the same
 * checksum table - the write side of a checksum is the same function as the
 * read side, called before the write instead of after the read.
 *
 * --- what is and is not implemented, said plainly --------------------------
 * ONE DVA. A blkptr has three slots and real ZFS fills two for metadata and
 * three for the MOS, so that losing a disk loses no metadata. Writing one
 * copy is legal on disk - BP_GET_NDVAS counts the non-empty slots and a
 * reader is happy with one - but it is less redundant than what ZFS would
 * have written, and that is a difference in DURABILITY rather than in format.
 * Said here rather than discovered later.
 *
 * NO COMPRESSION and NO GANG BLOCKS. Compression off is a legal blkptr and
 * costs only space. Gang blocks are what ZFS does when no single free range
 * is big enough; without them a fragmented pool fails an allocation that real
 * ZFS would have satisfied - ENOSPC where ZFS succeeds, which is the safe
 * direction to be wrong in.
 *
 * NO DEDUP, NO ENCRYPTION. Both change what a blkptr means, and a pool using
 * either is refused at mount by the feature gate rather than mishandled here.
 */

/* The leaf vdev to write to. Single top-level vdev only, which
 * zfsg_alloc_geometry has already enforced - a mirror would need the write
 * replicated to every child and a raidz would need parity, and doing either
 * badly is worse than refusing. */
static vdev_t *
zfsg_write_vdev(spa_t *spa)
{
	vdev_t *vd;

	STAILQ_FOREACH(vd, &spa->spa_root_vdev->v_children, v_childlink) {
		return (vd);
	}
	return (NULL);
}

/* Compute a checksum the same way zio_checksum_verify does, and store it.
 * Only the non-embedded case: the embedded kinds (gang headers and labels)
 * checksum a structure that carries the checksum inside it, and this writes
 * neither. */
static int
zfsg_checksum_compute(spa_t *spa, blkptr_t *bp, void *data, uint64_t size)
{
	unsigned int checksum = BP_GET_CHECKSUM(bp);
	zio_checksum_info_t *ci;
	void *ctx = NULL;

	if (checksum >= ZIO_CHECKSUM_FUNCTIONS)
		return (EINVAL);
	ci = &zio_checksum_table[checksum];
	if (ci->ci_func[0] == NULL)
		return (EINVAL);
	if (ci->ci_flags & ZCHECKSUM_FLAG_EMBEDDED)
		return (ENOTSUP);

	zio_checksum_template_init(checksum, spa);
	ctx = spa->spa_cksum_tmpls[checksum];

	/* The same index the verifier will choose. zio_checksum_verify picks
	 * ci_func[BP_SHOULD_BYTESWAP(bp)], so computing with a different one
	 * produces a checksum that cannot ever verify - which is exactly what
	 * happened while the byteorder bit above was wrong. Deriving the index
	 * from the bp rather than hardcoding 0 means the two sides cannot
	 * disagree again. */
	ci->ci_func[BP_SHOULD_BYTESWAP(bp)](data, size, ctx, &bp->blk_cksum);
	return (0);
}

/* Write one block. `data` must be `size` bytes; `size` must be a multiple of
 * the vdev's ashift. Fills `bp_out` completely. */
static int
zfsg_zio_write(spa_t *spa, zfsg_txg_t *tx, void *data, uint64_t size,
    uint8_t type, uint8_t level, uint64_t fill, blkptr_t *bp_out)
{
	vdev_t *vd = zfsg_write_vdev(spa);
	uint64_t align = 1ULL << tx->ga.ashift;
	uint64_t asize = (size + align - 1) & ~(align - 1);
	uint64_t off = 0;
	int rc;

	if (vd == NULL || vd->v_phys_write == NULL)
		return (EROFS);
	if (size == 0)
		return (EINVAL);

	rc = zfsg_alloc_dva(spa, tx, asize, &off);
	if (rc != 0)
		return (rc);

	BP_ZERO(bp_out);
	DVA_SET_VDEV(&bp_out->blk_dva[0], vd->v_id);
	DVA_SET_OFFSET(&bp_out->blk_dva[0], off);
	DVA_SET_ASIZE(&bp_out->blk_dva[0], asize);
	DVA_SET_GANG(&bp_out->blk_dva[0], 0);

	BP_SET_LSIZE(bp_out, size);
	BP_SET_PSIZE(bp_out, size);
	BP_SET_COMPRESS(bp_out, ZIO_COMPRESS_OFF);
	BP_SET_CHECKSUM(bp_out, ZIO_CHECKSUM_FLETCHER_4);
	BP_SET_TYPE(bp_out, type);
	BP_SET_LEVEL(bp_out, level);
	/* ZFS_HOST_BYTEORDER, which is 1 on a little-endian machine - NOT 0.
	 *
	 * This was written as 0 first, on the reasoning that "byteorder 0
	 * means little-endian". It does not. The bit records the order the
	 * block was written in using the same encoding ZFS_HOST_BYTEORDER
	 * uses, and BP_SHOULD_BYTESWAP is `BP_GET_BYTEORDER(bp) !=
	 * ZFS_HOST_BYTEORDER` - so a 0 here tells every reader on an x86 host
	 * that the block came from a big-endian machine, and the checksum is
	 * then verified against a byteswapped copy of the data.
	 *
	 * The failure was not subtle once it was isolated, and it is worth
	 * recording how it presented: the bytes reached the disk correctly, a
	 * raw read of them matched, and zio_read still returned EIO. The
	 * checksum did not verify against the very buffer it had just been
	 * computed from. */
	BP_SET_BYTEORDER(bp_out, ZFS_HOST_BYTEORDER);
	BP_SET_BIRTH(bp_out, tx->txg, 0);
	BP_SET_FILL(bp_out, fill);

	rc = zfsg_checksum_compute(spa, bp_out, data, size);
	if (rc != 0)
		return (rc);

	/* The DVA offset is relative to the start of the allocatable area;
	 * the physical offset adds the labels and boot block at the front.
	 * vdev_disk_read does the same addition on the way in, and getting it
	 * wrong here writes VDEV_LABEL_START_SIZE bytes before where the
	 * reader will look - which lands inside the labels. */
	rc = vd->v_phys_write(vd, (off_t)(off + VDEV_LABEL_START_SIZE), data,
	    (size_t)asize);
	if (rc != 0)
		return (rc);

	return (0);
}

/* --- a round trip through the write path -----------------------------------
 *
 * Writes one block of `size` bytes into free space, then reads it back
 * through the VENDORED zio_read - the same function the mounted filesystem
 * uses - and compares. Reports whether the bytes matched.
 *
 * That the read side is the vendored one is the entire point. zio_read
 * verifies the checksum before it hands anything back, so a checksum computed
 * with the wrong function, over the wrong length, or stored in the wrong
 * field comes back as EIO rather than as bytes that happen to match. A test
 * that read the block back with this file's own code would prove only that
 * it is self-consistent.
 *
 * This is SAFE to run on a real pool, and the reason is worth stating: the
 * block goes into space the metaslabs say is free, and no metadata anywhere
 * is changed to point at it. Nothing in the pool references the block, so the
 * next mount considers that space free exactly as before. A pool is not
 * modified by writing bytes to it - it is modified by an uberblock that
 * points at them, and this writes no uberblock.
 */
int
zfsg_write_probe(zfsg_mount_t *mount, unsigned long long size,
    unsigned long long *offset_out, int *raw_ok, int *readback_ok)
{
	zfsg_txg_t tx;
	blkptr_t bp;
	unsigned char *out = NULL, *back = NULL;
	unsigned long long i;
	int rc;

	if (mount == NULL || !mount->in_use || readback_ok == NULL)
		return (EINVAL);
	*readback_ok = 0;
	if (raw_ok != NULL)
		*raw_ok = 0;

	rc = zfsg_txg_open(mount->spa, &tx);
	if (rc != 0)
		return (rc);

	out = malloc((size_t)size);
	back = malloc((size_t)size);
	if (out == NULL || back == NULL) {
		free(out);
		free(back);
		return (ENOMEM);
	}

	/* A pattern with structure rather than a constant fill: a
	 * byte-swapped, misaligned or short read of a constant buffer still
	 * compares equal. */
	for (i = 0; i < size; i++)
		out[i] = (unsigned char)((i * 31 + (i >> 8) * 7) & 0xFF);

	rc = zfsg_zio_write(mount->spa, &tx, out, size,
	    DMU_OT_PLAIN_FILE_CONTENTS, 0, 1, &bp);
	if (rc != 0)
		goto done;

	if (offset_out != NULL) {
		*offset_out =
		    (unsigned long long)DVA_GET_OFFSET(&bp.blk_dva[0]);
	}

	/* First: did the bytes reach the medium, at the offset the blkptr
	 * says? Read them straight back through the device, bypassing zio
	 * entirely. This separates three failures that all surface as EIO
	 * from zio_read - a write that landed in the wrong place, a checksum
	 * computed wrongly, and a vdev lookup that found nothing - and
	 * without it the only signal is "-EIO" with three suspects. */
	memset(back, 0, (size_t)size);
	{
		unsigned long long phys =
		    (unsigned long long)DVA_GET_OFFSET(&bp.blk_dva[0]) +
		    VDEV_LABEL_START_SIZE;
		long long got = zfsg_dev_read(mount->ctx, phys, back,
		    (unsigned long long)size);

		if (got != (long long)size) {
			rc = EIO;
			goto done;
		}
		for (i = 0; i < size; i++) {
			if (back[i] != out[i])
				break;
		}
		if (raw_ok != NULL)
			*raw_ok = (i == size);
	}

	/* Second: does the checksum this code computed actually verify against
	 * the buffer it was computed from? That isolates a wrong checksum from
	 * a wrong read - both of which come out of zio_read as EIO. */
	if (zio_checksum_verify(mount->spa, &bp, out) != 0) {
		rc = EINVAL;      /* the checksum is wrong, not the I/O */
		goto done;
	}

	memset(back, 0, (size_t)size);
	rc = zio_read(mount->spa, &bp, back);
	if (rc != 0)
		goto done;

	for (i = 0; i < size; i++) {
		if (back[i] != out[i])
			break;
	}
	*readback_ok = (i == size);
	rc = 0;
done:
	free(out);
	free(back);
	return (rc);
}

/* --- committing a transaction group ----------------------------------------
 *
 * Writing blocks changes nothing. A ZFS pool is what its UBERBLOCK says it
 * is, and until a new uberblock points at new metadata, everything written
 * above is invisible - which is exactly the property that makes copy-on-write
 * safe to interrupt. This is the step that makes a write real, and it is
 * therefore the step that can destroy a pool.
 *
 * --- the uberblock ring ----------------------------------------------------
 * Each of the four labels carries a 128KB ring of uberblock slots, and the
 * slot a txg lands in is `txg % VDEV_UBERBLOCK_COUNT`. That modulo is the
 * whole crash-safety story: a new uberblock never overwrites the one before
 * it, so a torn write during the commit leaves the previous txg's uberblock
 * intact in a different slot, and the pool mounts at the older txg. Writing
 * to a fixed slot instead would make every commit a moment where the pool has
 * no valid uberblock at all.
 *
 * Four labels, two at each end of the device, for the same reason: a torn
 * front leaves the pair at the back.
 *
 * --- the checksum is a different kind ---------------------------------------
 * An uberblock is SELF-checksumming: the checksum lives in the last 40 bytes
 * of the block it covers, and what gets checksummed is the block with a
 * VERIFIER - derived from the block's own physical offset - sitting where the
 * checksum will go. That is what makes an uberblock impossible to relocate:
 * copy it to a different offset and it stops verifying. zfsg_checksum_compute
 * above explicitly refuses this kind; this is the code that handles it, and
 * the idiom is lifted from the vendored vdev_label_write, which does the same
 * dance for the bootenv area.
 */
static int
zfsg_uberblock_write(spa_t *spa, vdev_t *vd, const uberblock_t *ub)
{
	zio_checksum_info_t *ci = &zio_checksum_table[ZIO_CHECKSUM_LABEL];
	size_t ubsize = (size_t)VDEV_UBERBLOCK_SIZE(vd);
	uint64_t n = ub->ub_txg % VDEV_UBERBLOCK_COUNT(vd);
	unsigned char *buf;
	int l, rc = 0;

	if (vd->v_phys_write == NULL)
		return (EROFS);
	if (ubsize < sizeof (uberblock_t) + sizeof (zio_eck_t))
		return (EINVAL);

	buf = malloc(ubsize);
	if (buf == NULL)
		return (ENOMEM);

	for (l = 0; l < VDEV_LABELS; l++) {
		off_t off = vdev_label_offset(vd->v_psize, l,
		    VDEV_UBERBLOCK_OFFSET(vd, n));
		zio_eck_t *eck;
		zio_cksum_t cksum;

		/* Rebuilt per label rather than once, because the verifier
		 * below depends on the OFFSET and so the checksum differs
		 * between the four copies. Computing it once and writing it
		 * four times produces three labels that will never verify. */
		memset(buf, 0, ubsize);
		memcpy(buf, ub, sizeof (uberblock_t));

		eck = (zio_eck_t *)(buf + ubsize) - 1;
		eck->zec_magic = ZEC_MAGIC;
		zio_checksum_label_verifier(&eck->zec_cksum, off);
		ci->ci_func[0](buf, ubsize, NULL, &cksum);
		eck->zec_cksum = cksum;

		rc = vdev_write_phys(vd, buf, off, ubsize);
		if (rc != 0)
			break;
	}

	free(buf);
	(void) spa;
	return (rc);
}

/* Commit a transaction that changes nothing but the txg number.
 *
 * Deliberately the smallest commit that exists, and it is landed on its own
 * because it exercises the entire label and uberblock path - slot selection,
 * the self-checksum, all four labels - with NO metadata rewritten and
 * therefore nothing that can be made inconsistent. The pool afterwards
 * describes exactly the same data it did before, at a txg one higher.
 *
 * If real ZFS imports the result and reports the new txg, the label path is
 * right. If it does not, the label path is wrong, and finding that out here
 * costs nothing - whereas finding it out at the end of a real commit means a
 * pool that points at half-written metadata.
 */
int
zfsg_txg_commit_empty(zfsg_mount_t *mount, unsigned long long *txg_out)
{
	vdev_t *vd;
	uberblock_t ub;
	int rc;

	if (mount == NULL || !mount->in_use)
		return (ENXIO);

	vd = zfsg_write_vdev(mount->spa);
	if (vd == NULL)
		return (ENXIO);

	/* The best uberblock the reader found at mount, with the txg bumped.
	 * ub_rootbp is carried over unchanged - it still describes a MOS that
	 * is really on disk and really consistent, because nothing was
	 * rewritten. Its blk_birth stays at the older txg, which is legal: a
	 * blkptr's birth is when the block was written, not when it was last
	 * referenced. */
	ub = *mount->spa->spa_uberblock;
	ub.ub_txg += 1;
	ub.ub_timestamp += 1;

	rc = zfsg_uberblock_write(mount->spa, vd, &ub);
	if (rc != 0)
		return (rc);

	if (txg_out != NULL)
		*txg_out = (unsigned long long)ub.ub_txg;
	return (0);
}

/* --- copy-on-write, up the block pointer tree ------------------------------
 *
 * The piece that turns "Genesis can write a block" into "Genesis can change a
 * file". Everything above allocates space and puts bytes in it; nothing so far
 * makes those bytes REACHABLE, and in a copy-on-write filesystem that is a
 * separate and much larger job.
 *
 * There is no in-place modify anywhere in ZFS. Changing one byte of a file
 * means, bottom to top:
 *
 *   the data block            -> a new DVA
 *   its indirect blocks       -> each one a new DVA, because it now holds a
 *                                different child pointer
 *   the file's dnode          -> now holds a different dn_blkptr
 *   the dnode-array block     -> the dnode lives inside an object too
 *   the objset                -> holds os_meta_dnode, which just changed
 *   the DSL dataset's ds_bp   -> in a dnode, in the MOS
 *   the MOS's own dnode array, objset, and finally the uberblock
 *
 * Every one of those is an allocation, and every allocation has to be recorded
 * in a space map or the next mount hands the block out again. That recursion -
 * writing the space maps allocates, which must be recorded in the space maps -
 * is what makes this hard, and it is why real ZFS syncs a txg in numbered
 * passes rather than in one sweep.
 */

/* How many block pointers fit in one indirect block. */
static uint32_t
zfsg_bps_per_indirect(const dnode_phys_t *dn)
{
	return (1u << (dn->dn_indblkshift - SPA_BLKPTRSHIFT));
}

/* Read one indirect block into `buf`, or zero it when the pointer is a hole.
 *
 * A hole is not an error and must not be read: BP_IS_HOLE means the range has
 * never been written and reads as zeros, so zio_read on it would fail and a
 * caller that treated that as fatal could not write into a sparse file. */
static int
zfsg_read_indirect(spa_t *spa, const blkptr_t *bp, void *buf, uint32_t size)
{
	uint32_t i;

	if (BP_IS_HOLE(bp)) {
		for (i = 0; i < size; i++) {
			((uint8_t *)buf)[i] = 0;
		}
		return (0);
	}
	return (zio_read(spa, bp, buf));
}

/* Rewrite block `blkid` of the object described by `dn`, updating `dn` in
 * place so that it points at the new copy.
 *
 * `dn` is the caller's own copy of the dnode. It is NOT written anywhere here
 * - making the updated dnode durable is the caller's next problem, and it is
 * the same problem one level up. That split is what lets the whole cascade be
 * this one function applied repeatedly.
 */
static int
zfsg_cow_write(spa_t *spa, zfsg_txg_t *tx, dnode_phys_t *dn, uint64_t blkid,
    void *data, uint32_t size, uint8_t type)
{
	uint32_t indsize = 1u << dn->dn_indblkshift;
	uint32_t per = zfsg_bps_per_indirect(dn);
	blkptr_t newbp;
	int rc;

	if (dn->dn_nlevels == 0) {
		return (EINVAL);
	}

	/* Level 0: the data block itself. */
	rc = zfsg_zio_write(spa, tx, data, size, type, 0, 1, &newbp);
	if (rc != 0) {
		return (rc);
	}

	if (dn->dn_nlevels == 1) {
		/* The dnode points straight at data blocks. dn_nblkptr of them,
		 * and a blkid past that is a block this object cannot address
		 * at one level - growing it is a different operation than
		 * rewriting it. */
		if (blkid >= dn->dn_nblkptr) {
			return (E2BIG);
		}
		dn->dn_blkptr[blkid] = newbp;
		return (0);
	}

	/* Levels 1..n-1: walk down to the leaf, remembering the path, then
	 * rebuild it upward. Each level's block changes because its child
	 * pointer changed, so each one is itself a copy-on-write.
	 *
	 * Bounded by dn_nlevels, which the on-disk format caps well below
	 * this - the array is sized for the worst case rather than the
	 * observed one so that a corrupt dn_nlevels overruns nothing. */
#define ZFSG_MAX_LEVELS 8
	{
		uint8_t  *ind[ZFSG_MAX_LEVELS];
		uint32_t  idx[ZFSG_MAX_LEVELS];
		blkptr_t *slot;
		int lvl, top = dn->dn_nlevels - 1;
		int i;

		if (dn->dn_nlevels > ZFSG_MAX_LEVELS) {
			return (ENOTSUP);
		}
		for (i = 0; i < ZFSG_MAX_LEVELS; i++) {
			ind[i] = NULL;
		}

		/* Descend. At level L the index is blkid / per^L modulo per. */
		{
			uint64_t rem = blkid;
			uint64_t div = 1;

			for (lvl = 0; lvl < top; lvl++) {
				div *= per;
			}
			for (lvl = top; lvl >= 1; lvl--) {
				idx[lvl] = (uint32_t)((rem / div) % per);
				div /= per;
			}
			idx[0] = (uint32_t)(rem % per);
			(void)rem;
		}

		/* Read every indirect block on the path. */
		{
			const blkptr_t *cur;

			if (idx[top] >= dn->dn_nblkptr) {
				return (E2BIG);
			}
			cur = &dn->dn_blkptr[idx[top]];
			for (lvl = top; lvl >= 1; lvl--) {
				ind[lvl] = malloc(indsize);
				if (ind[lvl] == NULL) {
					rc = ENOMEM;
					goto out;
				}
				rc = zfsg_read_indirect(spa, cur, ind[lvl],
				    indsize);
				if (rc != 0) {
					goto out;
				}
				if (lvl > 1) {
					cur = &((blkptr_t *)ind[lvl])[idx[lvl - 1]];
				}
			}
		}

		/* Rebuild upward: patch the new child pointer into level 1,
		 * write level 1, patch THAT into level 2, and so on. */
		slot = &((blkptr_t *)ind[1])[idx[0]];
		*slot = newbp;

		for (lvl = 1; lvl <= top; lvl++) {
			blkptr_t indbp;

			rc = zfsg_zio_write(spa, tx, ind[lvl], indsize,
			    dn->dn_type, (uint8_t)lvl, 1, &indbp);
			if (rc != 0) {
				goto out;
			}
			if (lvl == top) {
				dn->dn_blkptr[idx[top]] = indbp;
			} else {
				((blkptr_t *)ind[lvl + 1])[idx[lvl]] = indbp;
			}
		}
		rc = 0;
out:
		for (i = 0; i < ZFSG_MAX_LEVELS; i++) {
			if (ind[i] != NULL) {
				free(ind[i]);
			}
		}
		return (rc);
	}
}

/* Make an updated dnode durable, by rewriting the block of the dnode array
 * that holds it.
 *
 * A dnode is not a free-standing thing on disk: it lives at byte offset
 * objnum * sizeof(dnode_phys_t) inside object 0, whose own dnode is the
 * objset's os_meta_dnode. So "write this dnode" is "rewrite one block of
 * another object", which is zfsg_cow_write again - and it updates
 * os_meta_dnode, which is why the objset itself then has to be rewritten by
 * the caller. */
static int
zfsg_write_dnode(spa_t *spa, zfsg_txg_t *tx, objset_phys_t *os,
    uint64_t objnum, const dnode_phys_t *dn)
{
	dnode_phys_t *meta = &os->os_meta_dnode;
	uint32_t blksz = (uint32_t)meta->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	uint64_t byteoff = objnum * sizeof (dnode_phys_t);
	uint64_t blkid = byteoff / blksz;
	uint32_t within = (uint32_t)(byteoff % blksz);
	uint8_t *blk;
	int rc;

	if (blksz == 0 || within + sizeof (dnode_phys_t) > blksz) {
		return (EIO);
	}

	blk = malloc(blksz);
	if (blk == NULL) {
		return (ENOMEM);
	}

	/* Read-modify-write, and the READ is not optional: the block holds
	 * many dnodes and the others must survive. Writing a fresh block with
	 * only this dnode in it would delete every object that shares it. */
	rc = dnode_read(spa, meta, blkid * blksz, blk, blksz);
	if (rc != 0) {
		free(blk);
		return (rc);
	}
	memcpy(blk + within, dn, sizeof (dnode_phys_t));

	rc = zfsg_cow_write(spa, tx, meta, blkid, blk, blksz, DMU_OT_DNODE);
	free(blk);
	return (rc);
}

/* --- persisting the allocations --------------------------------------------
 *
 * Every block written above came out of free space, and the only record of
 * that is in memory. If the txg commits without the metaslabs' space maps
 * being updated, the next mount believes those blocks are free and hands them
 * out again - on top of live data. So this is not bookkeeping, it is the
 * difference between a write and a corruption.
 *
 * And it is recursive: appending to a space map is writing to a DMU object,
 * which allocates, which has to be recorded in a space map. Real ZFS resolves
 * that with numbered sync passes that converge.
 *
 * --- why that recursion is safe to get wrong here ---------------------------
 * Nothing is visible until the uberblock. Blocks written into free space that
 * nothing points at are invisible and harmless - the next mount considers that
 * space free, exactly as it did before. So this code is allowed to give up:
 * it iterates a bounded number of passes and, if the allocations have not
 * settled, ABANDONS the whole transaction by simply not writing an uberblock.
 * The pool is then byte-for-byte the pool it was, minus some leaked-but-free
 * space. An abort is a non-event, which is what makes attempting this at all
 * defensible.
 */

/* One V1 space map entry: allocated run at `off` for `run` bytes, both
 * relative to the metaslab and in units of ashift.
 *
 * V1 rather than V2 because these runs are small - a data block, an indirect
 * block, a dnode block - and V1 holds a 47-bit offset and a 15-bit run, which
 * covers a 16MB metaslab at any ashift with room to spare. A run too large for
 * it is refused rather than silently truncated. */
static int
zfsg_sm_encode(uint64_t off_units, uint64_t run_units, uint64_t *out)
{
	if (run_units == 0 || run_units > (1ull << SM_RUN_BITS_G)) {
		return (E2BIG);
	}
	if (off_units >= (1ull << SM_OFFSET_BITS_G)) {
		return (E2BIG);
	}
	/* prefix 0 (single word), offset at bit 16, type at bit 15, run - 1
	 * in the low 15 bits. SM_ALLOC is type 0. */
	*out = (off_units << 16) | ((uint64_t)SM_ALLOC_G << 15) |
	    (run_units - 1);
	return (0);
}

/* Append `n` entries to metaslab `ms`'s space map and update its bonus.
 *
 * The space map object's live length is smp_length in its bonus, NOT the
 * object's size - the tail past it is stale log from previous txgs. Appending
 * means writing at exactly that offset and then moving it. */
static int
zfsg_sm_append(spa_t *spa, zfsg_txg_t *tx, objset_phys_t *mos, uint64_t sm_obj,
    const uint64_t *words, int n, uint64_t alloc_bytes)
{
	dnode_phys_t dn;
	uint64_t *bonus;
	uint64_t length, blksz, blkid, within;
	uint8_t *blk;
	int i, rc;

	if (n == 0) {
		return (0);
	}
	rc = objset_get_dnode(spa, mos, sm_obj, &dn);
	if (rc != 0) {
		return (rc);
	}
	if (dn.dn_bonuslen < sizeof (uint64_t) * 3) {
		return (EIO);
	}
	bonus = (uint64_t *)DN_BONUS(&dn);
	length = bonus[1];                    /* smp_length, in bytes */
	blksz = (uint64_t)dn.dn_datablkszsec << SPA_MINBLOCKSHIFT;
	if (blksz == 0) {
		return (EIO);
	}

	blkid = length / blksz;
	within = length % blksz;

	/* One block's worth per call, and refused rather than split when the
	 * entries would straddle. Splitting is correct and is more code; a
	 * refusal here aborts a transaction that has committed nothing, which
	 * costs only the attempt. */
	if (within + (uint64_t)n * sizeof (uint64_t) > blksz) {
		return (ENOSPC);
	}

	blk = malloc((size_t)blksz);
	if (blk == NULL) {
		return (ENOMEM);
	}
	if (within != 0 || length != 0) {
		rc = dnode_read(spa, &dn, blkid * blksz, blk, (size_t)blksz);
		if (rc != 0) {
			free(blk);
			return (rc);
		}
	} else {
		for (i = 0; i < (int)blksz; i++) {
			blk[i] = 0;
		}
	}
	for (i = 0; i < n; i++) {
		memcpy(blk + within + (size_t)i * sizeof (uint64_t), &words[i],
		    sizeof (uint64_t));
	}

	rc = zfsg_cow_write(spa, tx, &dn, blkid, blk, (uint32_t)blksz,
	    DMU_OT_SPACE_MAP);
	free(blk);
	if (rc != 0) {
		return (rc);
	}

	/* The bonus travels with the dnode, so updating it here and writing
	 * the dnode below is one operation rather than two. */
	bonus[1] = length + (uint64_t)n * sizeof (uint64_t);   /* smp_length */
	bonus[2] += alloc_bytes;                               /* smp_alloc  */

	return (zfsg_write_dnode(spa, tx, mos, sm_obj, &dn));
}

/* --- the cascade -----------------------------------------------------------
 *
 * Overwrite one whole block of one file, and make it visible. This is the
 * function every other piece in this file exists to support.
 *
 * The chain, and every link of it is a copy-on-write:
 *
 *   the file's data block          zfsg_cow_write on the file's dnode
 *   the file's dnode               zfsg_write_dnode -> rewrites a block of the
 *                                  filesystem's dnode array, which updates
 *                                  that objset's os_meta_dnode
 *   the filesystem's objset        written here; its new blkptr is ds_bp
 *   the DSL dataset's dnode        zfsg_write_dnode into the MOS
 *   the space maps                 every allocation above, recorded
 *   the MOS's objset               written here; its new blkptr is ub_rootbp
 *   the uberblock                  zfsg_uberblock_write
 *
 * --- what is deliberately narrow -------------------------------------------
 * WHOLE BLOCKS ONLY, and only blocks that already exist. No growth, no
 * sparse fill, no partial-block read-modify-write. Growth changes dn_maxblkid
 * and possibly dn_nlevels, and a partial write needs the old contents read
 * first - both are real work and neither is needed to prove the cascade.
 *
 * --- and why aborting is free ----------------------------------------------
 * Nothing above is visible until the uberblock is written, so any failure
 * before that last step leaves the pool exactly as it was, minus some blocks
 * written into free space that nothing references. That is the property that
 * makes this safe to attempt: a half-finished transaction is not a damaged
 * pool, it is an unchanged one.
 */
int
zfsg_write_block(zfsg_mount_t *mount, unsigned long long objnum,
    unsigned long long blkid, void *data, unsigned long long len,
    unsigned long long *txg_out)
{
	spa_t *spa = mount->spa;
	zfsg_txg_t tx;
	dnode_phys_t fdn, dsdn;
	objset_phys_t *fsos = NULL, *mosos = NULL;
	dsl_dataset_phys_t *ds;
	blkptr_t bp;
	vdev_t *vd;
	uberblock_t ub;
	uint32_t blksz;
	int rc, pass;
	int recorded_upto = 0;

	if (mount == NULL || !mount->in_use) {
		return (ENXIO);
	}
	vd = zfsg_write_vdev(spa);
	if (vd == NULL || vd->v_phys_write == NULL) {
		return (EROFS);
	}

	rc = zfsg_txg_open(spa, &tx);
	if (rc != 0) {
		return (rc);
	}

	/* Working copies. The originals stay untouched so that an abort needs
	 * no undo - the pool's real state is on disk and is not modified until
	 * the very last write. */
	fsos = malloc(sizeof (objset_phys_t));
	mosos = malloc(sizeof (objset_phys_t));
	if (fsos == NULL || mosos == NULL) {
		rc = ENOMEM;
		goto out;
	}
	memcpy(fsos, &mount->mnt.objset, sizeof (objset_phys_t));
	memcpy(mosos, &spa->spa_mos_master, sizeof (objset_phys_t));

	/* 1. the file's data block, and its dnode */
	rc = objset_get_dnode(spa, fsos, objnum, &fdn);
	if (rc != 0) {
		goto out;
	}
	blksz = (uint32_t)fdn.dn_datablkszsec << SPA_MINBLOCKSHIFT;
	if (blksz == 0 || len != blksz) {
		/* Whole blocks only - see the header comment. A partial write
		 * needs the old block read and merged, which is a different
		 * operation and is refused rather than half-done. */
		rc = EINVAL;
		goto out;
	}
	if (blkid > fdn.dn_maxblkid) {
		rc = E2BIG;                     /* no growth here */
		goto out;
	}

	rc = zfsg_cow_write(spa, &tx, &fdn, blkid, data, blksz, fdn.dn_type);
	if (rc != 0) {
		goto out;
	}
	rc = zfsg_write_dnode(spa, &tx, fsos, objnum, &fdn);
	if (rc != 0) {
		goto out;
	}

	/* 2. the filesystem's objset, whose os_meta_dnode just changed */
	rc = zfsg_zio_write(spa, &tx, fsos, sizeof (objset_phys_t),
	    DMU_OT_OBJSET, 0, 0, &bp);
	if (rc != 0) {
		goto out;
	}

	/* 3. the DSL dataset dnode in the MOS, whose ds_bp names that objset */
	rc = objset_get_dnode(spa, mosos, mount->mnt.rootobj, &dsdn);
	if (rc != 0) {
		goto out;
	}
	ds = (dsl_dataset_phys_t *)&dsdn.dn_bonus;
	ds->ds_bp = bp;
	rc = zfsg_write_dnode(spa, &tx, mosos, mount->mnt.rootobj, &dsdn);
	if (rc != 0) {
		goto out;
	}

	/* 4. the space maps, in bounded passes.
	 *
	 * Each pass records the allocations made so far, and doing so makes
	 * more. It converges because a space map append usually lands in an
	 * already-dirtied block, so a pass allocates strictly less than the
	 * one before. If it has not settled by the last pass, the whole
	 * transaction is abandoned - see the header: an abort costs nothing
	 * because no uberblock has been written. */
	for (pass = 0; pass < 8; pass++) {
		int start = recorded_upto;
		uint64_t done_mask = 0;
		int i;

		if (start == tx.npending) {
			break;                       /* settled */
		}
		recorded_upto = tx.npending;

		/* BATCHED PER METASLAB, which is the difference between
		 * converging and not.
		 *
		 * The first version appended one entry per allocation, and
		 * every append is itself a dnode update costing six more
		 * allocations - so recording N allocations created 7N more and
		 * the budget was gone before the first pass finished. Grouping
		 * every allocation that landed in one metaslab into a single
		 * append makes a pass cost one append per METASLAB touched
		 * rather than one per block, which is what gives the next pass
		 * a chance to be smaller than this one. */
		for (i = start; i < tx.npending; i++) {
			uint64_t n = tx.pending[i].start >> tx.ga.ms_shift;
			uint64_t ms_start = n << tx.ga.ms_shift;
			uint64_t sm_obj = 0;
			uint64_t words[ZFSG_TXG_MAX_ALLOC];
			uint64_t bytes = 0;
			int nw = 0, j;

			if (n < 64 && (done_mask & (1ull << n)) != 0) {
				continue;            /* already batched */
			}
			if (n < 64) {
				done_mask |= (1ull << n);
			}

			for (j = i; j < tx.npending; j++) {
				uint64_t off = tx.pending[j].start;
				uint64_t size = tx.pending[j].size;

				if ((off >> tx.ga.ms_shift) != n) {
					continue;
				}
				rc = zfsg_sm_encode(
				    (off - ms_start) >> tx.ga.ashift,
				    size >> tx.ga.ashift, &words[nw]);
				if (rc != 0) {
					goto out;
				}
				bytes += size;
				nw++;
			}

			rc = zfsg_ms_object(spa, &tx.ga, n, &sm_obj);
			if (rc != 0) {
				goto out;
			}
			if (sm_obj == 0) {
				/* A metaslab never allocated from has no space
				 * map object, and creating one is object
				 * creation - refused rather than attempted. */
				rc = ENOTSUP;
				goto out;
			}
			rc = zfsg_sm_append(spa, &tx, mosos, sm_obj, words, nw,
			    bytes);
			if (rc != 0) {
				goto out;
			}
		}
	}
	if (recorded_upto != tx.npending) {
		/* Did not settle. Abandon - nothing has been made visible. */
		rc = EAGAIN;
		goto out;
	}

	/* 5. the MOS's own objset, and the uberblock that names it.
	 *
	 * The MOS objset block is itself an allocation that no space map
	 * records, and that is a KNOWN LEAK rather than an oversight: it is
	 * one block per transaction, it is not referenced by any space map so
	 * a later allocator may hand it out again - which is why this whole
	 * function is still gated behind an explicit call and not wired to
	 * fs_ops_t::write. Closing it needs the sync-pass structure real ZFS
	 * has, where the final allocations are carried into the next txg. */
	rc = zfsg_zio_write(spa, &tx, mosos, sizeof (objset_phys_t),
	    DMU_OT_OBJSET, 0, 0, &bp);
	if (rc != 0) {
		goto out;
	}

	ub = *spa->spa_uberblock;
	ub.ub_txg = tx.txg;
	ub.ub_timestamp += 1;
	ub.ub_rootbp = bp;

	rc = zfsg_uberblock_write(spa, vd, &ub);
	if (rc != 0) {
		goto out;
	}
	if (txg_out != NULL) {
		*txg_out = (unsigned long long)ub.ub_txg;
	}
	rc = 0;
out:
	if (fsos != NULL) {
		free(fsos);
	}
	if (mosos != NULL) {
		free(mosos);
	}
	return (rc);
}

/* --- the dirty buffer layer, which is what the DMU actually is -------------
 *
 * The cascade above works and does not converge, and the reason is worth
 * stating precisely because it is the whole justification for this code.
 *
 * Recording an allocation means appending to a space map, which is a write to
 * a DMU object, which allocates. That terminates only if each pass allocates
 * strictly less than the one before. Without a cache it does the opposite:
 * every dnode update copy-on-writes the MOS meta-dnode afresh - six blocks,
 * because that array is six levels deep - so recording N allocations creates
 * roughly 7N more, and the transaction is abandoned.
 *
 * --- the fix, in one sentence ----------------------------------------------
 * A block allocated in THIS transaction keeps its DVA: modifying it again
 * before commit changes memory, not the disk, and costs no second allocation.
 *
 * That is the property that collapses the recursion. The first pass allocates
 * for each block it touches; the second pass appends the resulting space map
 * entries into blocks that are ALREADY dirty and ALREADY hold a DVA from this
 * txg, so it allocates nothing, and the third pass has nothing left to record.
 * Copy-on-write is a rule about what is visible between transactions, not a
 * rule about how many times you may write a block inside one.
 *
 * --- what a dbuf is here ---------------------------------------------------
 * One cached block, identified by (objset, object, level, blkid). It holds the
 * contents, whether they have been modified, and the DVA reserved for it if it
 * has been allocated in this txg. Nothing here is an LRU or a shared cache -
 * it lives exactly as long as one transaction, which is the only lifetime that
 * makes the DVA-reuse rule safe.
 */

#define ZFSG_OS_FS   0
#define ZFSG_OS_MOS  1

/* Enough for a small write's full cascade: the data block, the file's dnode
 * block, six levels of the MOS meta-dnode, a handful of space map blocks and
 * their dnode blocks. Exceeding it ABANDONS the transaction rather than
 * evicting - an eviction would drop a dirty block whose DVA is already
 * reserved, which is the one thing this layer must never do. */
#define ZFSG_DBUF_MAX 96

typedef struct zfsg_dbuf {
    int         used;
    int         dirty;
    int         written;      /* synced at least once in this txg          */
    int         os;           /* ZFSG_OS_FS or ZFSG_OS_MOS                 */
    uint64_t    object;
    uint8_t     level;
    uint64_t    blkid;
    uint32_t    size;
    uint8_t    *data;

    /* The DVA reserved for this block in this transaction, and the flag that
     * says the reservation exists. This pair is the mechanism: once set, every
     * later sync of this buffer writes to the same place and allocates
     * nothing. */
    int         has_dva;
    uint64_t    dva_off;
    uint64_t    dva_asize;
} zfsg_dbuf_t;

/* The cache itself, plus the two working objsets it is relative to.
 *
 * Static rather than inside zfsg_txg_t because a transaction is already a
 * stack object in every caller and 96 buffers of pointers is more than a
 * kernel stack should carry. There is exactly one transaction at a time - this
 * kernel is non-preemptive and nothing here yields - which is the same
 * assumption the rest of the ZFS port already makes. */
static zfsg_dbuf_t    zfsg_dbufs[ZFSG_DBUF_MAX];
static int            zfsg_dbuf_count;
static objset_phys_t *zfsg_os[2];          /* [ZFSG_OS_FS], [ZFSG_OS_MOS] */

static void
zfsg_dbuf_reset(void)
{
	int i;

	for (i = 0; i < ZFSG_DBUF_MAX; i++) {
		if (zfsg_dbufs[i].data != NULL) {
			free(zfsg_dbufs[i].data);
		}
		memset(&zfsg_dbufs[i], 0, sizeof (zfsg_dbufs[i]));
	}
	zfsg_dbuf_count = 0;
}

/* The dnode describing object `obj` in objset `os`.
 *
 * Object 0 is the dnode array itself and its dnode is not in the array - it is
 * os_meta_dnode, inside the objset. Everything else lives at a byte offset
 * inside object 0, which means "get a dnode" is "get a block of another
 * object", and the recursion stops because object 0's answer is the base case.
 *
 * The pointer returned points INTO a cached block, so writing through it
 * modifies the cache. That is the intent - but the containing buffer must then
 * be marked dirty, which is what `for_write` does. Reading through a
 * for_write=0 handle and modifying it anyway would produce a change that is
 * never written and never noticed. */
static int zfsg_dbuf_hold(spa_t *spa, int os, uint64_t obj, uint8_t level,
    uint64_t blkid, uint32_t size, int for_write, zfsg_dbuf_t **out);

static int
zfsg_dnode_hold(spa_t *spa, int os, uint64_t obj, int for_write,
    dnode_phys_t **out)
{
	dnode_phys_t *meta;
	zfsg_dbuf_t *db;
	uint32_t blksz;
	uint64_t byteoff, blkid;
	uint32_t within;
	int rc;

	if (zfsg_os[os] == NULL) {
		return (EINVAL);
	}
	if (obj == 0) {
		*out = &zfsg_os[os]->os_meta_dnode;
		return (0);
	}

	meta = &zfsg_os[os]->os_meta_dnode;
	blksz = (uint32_t)meta->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	if (blksz == 0) {
		return (EIO);
	}
	byteoff = obj * sizeof (dnode_phys_t);
	blkid = byteoff / blksz;
	within = (uint32_t)(byteoff % blksz);

	rc = zfsg_dbuf_hold(spa, os, 0, 0, blkid, blksz, for_write, &db);
	if (rc != 0) {
		return (rc);
	}
	*out = (dnode_phys_t *)(db->data + within);
	return (0);
}

/* Read one block at an arbitrary LEVEL of an object.
 *
 * dnode_read only reaches level 0 - it is the vendored reader's data path and
 * a boot loader never wants an indirect block on its own. Writing needs them,
 * because an indirect block that is going to be modified has to be read first.
 * A hole reads as zeros rather than as an error: a sparse region has never
 * been written, and refusing it would make it impossible to write into one. */
static int
zfsg_read_level(spa_t *spa, const dnode_phys_t *dn, uint8_t level,
    uint64_t blkid, void *buf, uint32_t size)
{
	uint32_t per = zfsg_bps_per_indirect(dn);
	uint32_t indsize = 1u << dn->dn_indblkshift;
	blkptr_t bp;
	uint8_t *scratch;
	int lvl, rc;

	if (level >= dn->dn_nlevels) {
		return (EINVAL);
	}

	/* The top level is addressed straight out of the dnode. */
	{
		uint64_t idx = blkid;
		int l;

		for (l = level; l < dn->dn_nlevels - 1; l++) {
			idx /= per;
		}
		if (idx >= dn->dn_nblkptr) {
			return (E2BIG);
		}
		bp = dn->dn_blkptr[idx];
	}
	if (level == dn->dn_nlevels - 1) {
		if (BP_IS_HOLE(&bp)) {
			memset(buf, 0, size);
			return (0);
		}
		return (zio_read(spa, &bp, buf));
	}

	scratch = malloc(indsize);
	if (scratch == NULL) {
		return (ENOMEM);
	}
	for (lvl = dn->dn_nlevels - 1; lvl > level; lvl--) {
		uint64_t idx = blkid;
		int l;

		if (BP_IS_HOLE(&bp)) {
			memset(buf, 0, size);
			free(scratch);
			return (0);
		}
		rc = zio_read(spa, &bp, scratch);
		if (rc != 0) {
			free(scratch);
			return (rc);
		}
		for (l = level; l < lvl - 1; l++) {
			idx /= per;
		}
		bp = ((blkptr_t *)scratch)[idx % per];
	}
	free(scratch);

	if (BP_IS_HOLE(&bp)) {
		memset(buf, 0, size);
		return (0);
	}
	return (zio_read(spa, &bp, buf));
}

static int
zfsg_dbuf_hold(spa_t *spa, int os, uint64_t obj, uint8_t level, uint64_t blkid,
    uint32_t size, int for_write, zfsg_dbuf_t **out)
{
	dnode_phys_t *dn;
	zfsg_dbuf_t *db;
	int i, rc;

	for (i = 0; i < zfsg_dbuf_count; i++) {
		db = &zfsg_dbufs[i];
		if (db->used && db->os == os && db->object == obj &&
		    db->level == level && db->blkid == blkid) {
			if (for_write) {
				db->dirty = 1;
			}
			*out = db;
			return (0);
		}
	}

	if (zfsg_dbuf_count >= ZFSG_DBUF_MAX) {
		/* REFUSED, never evicted. A dirty buffer holds a DVA reserved
		 * in this transaction; dropping it would leave that block
		 * allocated with nothing pointing at it and, worse, would lose
		 * a modification the cascade believes it made. Abandoning the
		 * transaction costs nothing - see the header. */
		return (E2BIG);
	}

	db = &zfsg_dbufs[zfsg_dbuf_count];
	db->data = malloc(size);
	if (db->data == NULL) {
		return (ENOMEM);
	}

	/* Read it in. The dnode is fetched WITHOUT for_write: holding a block
	 * for writing does not by itself modify the dnode that describes it. */
	rc = zfsg_dnode_hold(spa, os, obj, 0, &dn);
	if (rc != 0) {
		free(db->data);
		db->data = NULL;
		return (rc);
	}
	rc = zfsg_read_level(spa, dn, level, blkid, db->data, size);
	if (rc != 0) {
		free(db->data);
		db->data = NULL;
		return (rc);
	}

	db->used = 1;
	db->dirty = for_write ? 1 : 0;
	db->written = 0;
	db->os = os;
	db->object = obj;
	db->level = level;
	db->blkid = blkid;
	db->size = size;
	db->has_dva = 0;
	zfsg_dbuf_count++;
	*out = db;
	return (0);
}

/* Write a block to an offset that has ALREADY been allocated in this
 * transaction, allocating nothing.
 *
 * This is zfsg_zio_write with the allocation removed, and it is the operation
 * the whole dirty-buffer design exists to make available. Rewriting a block
 * that this txg already reserved is not a copy-on-write violation: nothing
 * outside the transaction can see either version until the uberblock lands, so
 * the intermediate contents were never anyone's data. */
static int
zfsg_zio_write_at(spa_t *spa, zfsg_txg_t *tx, void *data, uint64_t size,
    uint64_t off, uint64_t asize, uint8_t type, uint8_t level, uint64_t fill,
    blkptr_t *bp_out)
{
	vdev_t *vd = zfsg_write_vdev(spa);
	int rc;

	if (vd == NULL || vd->v_phys_write == NULL) {
		return (EROFS);
	}

	BP_ZERO(bp_out);
	DVA_SET_VDEV(&bp_out->blk_dva[0], vd->v_id);
	DVA_SET_OFFSET(&bp_out->blk_dva[0], off);
	DVA_SET_ASIZE(&bp_out->blk_dva[0], asize);
	DVA_SET_GANG(&bp_out->blk_dva[0], 0);

	BP_SET_LSIZE(bp_out, size);
	BP_SET_PSIZE(bp_out, size);
	BP_SET_COMPRESS(bp_out, ZIO_COMPRESS_OFF);
	BP_SET_CHECKSUM(bp_out, ZIO_CHECKSUM_FLETCHER_4);
	BP_SET_TYPE(bp_out, type);
	BP_SET_LEVEL(bp_out, level);
	BP_SET_BYTEORDER(bp_out, ZFS_HOST_BYTEORDER);
	BP_SET_BIRTH(bp_out, tx->txg, 0);
	BP_SET_FILL(bp_out, fill);

	rc = zfsg_checksum_compute(spa, bp_out, data, size);
	if (rc != 0) {
		return (rc);
	}
	return (vd->v_phys_write(vd, (off_t)(off + VDEV_LABEL_START_SIZE),
	    data, (size_t)asize));
}

/* Write one cached block, reserving space for it only the first time.
 *
 * The `has_dva` branch is the entire convergence argument: the first sync of a
 * buffer allocates and therefore has to be recorded in a space map; every
 * later sync of the same buffer writes to the same place and creates no new
 * accounting at all. Without it, appending a space map entry dirties blocks
 * whose rewrite allocates, which needs another entry, forever. */
static int
zfsg_dbuf_write(spa_t *spa, zfsg_txg_t *tx, zfsg_dbuf_t *db, uint8_t type,
    uint64_t fill, blkptr_t *bp_out)
{
	uint64_t align = 1ull << tx->ga.ashift;
	uint64_t asize = (db->size + align - 1) & ~(align - 1);
	int rc;

	if (!db->has_dva) {
		uint64_t off = 0;

		rc = zfsg_alloc_dva(spa, tx, asize, &off);
		if (rc != 0) {
			return (rc);
		}
		db->dva_off = off;
		db->dva_asize = asize;
		db->has_dva = 1;
	}

	rc = zfsg_zio_write_at(spa, tx, db->data, db->size, db->dva_off,
	    db->dva_asize, type, db->level, fill, bp_out);
	if (rc == 0) {
		db->dirty = 0;
		db->written = 1;
	}
	return (rc);
}

/* Patch a child's block pointer into whatever holds it: an indirect block one
 * level up, or the dnode when the child is already at the top. */
static int
zfsg_patch_parent(spa_t *spa, int os, uint64_t obj, uint8_t level,
    uint64_t blkid, const blkptr_t *bp)
{
	dnode_phys_t *dn;
	uint32_t per, indsize;
	int rc;

	rc = zfsg_dnode_hold(spa, os, obj, 0, &dn);
	if (rc != 0) {
		return (rc);
	}
	per = zfsg_bps_per_indirect(dn);
	indsize = 1u << dn->dn_indblkshift;

	if (level + 1 >= dn->dn_nlevels) {
		/* The dnode holds it. Re-held for WRITE so the block of the
		 * dnode array that contains this dnode is marked dirty - the
		 * dnode is data inside another object, and forgetting that is
		 * how a change lives only in memory. */
		if (blkid >= dn->dn_nblkptr) {
			return (E2BIG);
		}
		rc = zfsg_dnode_hold(spa, os, obj, 1, &dn);
		if (rc != 0) {
			return (rc);
		}
		dn->dn_blkptr[blkid] = *bp;
		return (0);
	}

	{
		zfsg_dbuf_t *parent;

		rc = zfsg_dbuf_hold(spa, os, obj, (uint8_t)(level + 1),
		    blkid / per, indsize, 1, &parent);
		if (rc != 0) {
			return (rc);
		}
		((blkptr_t *)parent->data)[blkid % per] = *bp;
	}
	return (0);
}

/* Flush every dirty buffer belonging to one objset.
 *
 * Lowest level first, so that a block's pointer is patched into its parent
 * before that parent is itself written. Repeated until nothing is dirty:
 * patching a parent dirties it, and writing a dnode dirties the block of the
 * dnode array holding it, so one pass is never enough - but each pass moves
 * strictly upward, and rewrites cost no allocation, so it terminates.
 */
static int
zfsg_sync_objset(spa_t *spa, zfsg_txg_t *tx, int os)
{
	int round, i, rc;

	for (round = 0; round < 32; round++) {
		int did = 0;
		uint8_t level;

		for (level = 0; level < ZFSG_MAX_LEVELS; level++) {
			for (i = 0; i < zfsg_dbuf_count; i++) {
				zfsg_dbuf_t *db = &zfsg_dbufs[i];
				dnode_phys_t *dn;
				blkptr_t bp;
				uint8_t type;

				if (!db->used || !db->dirty || db->os != os ||
				    db->level != level) {
					continue;
				}
				rc = zfsg_dnode_hold(spa, os, db->object, 0,
				    &dn);
				if (rc != 0) {
					return (rc);
				}
				type = dn->dn_type;

				rc = zfsg_dbuf_write(spa, tx, db, type, 1, &bp);
				if (rc != 0) {
					return (rc);
				}
				rc = zfsg_patch_parent(spa, os, db->object,
				    db->level, db->blkid, &bp);
				if (rc != 0) {
					return (rc);
				}
				did = 1;
			}
		}
		if (!did) {
			return (0);
		}
	}
	/* Did not settle. Abandoned, which costs nothing. */
	return (EAGAIN);
}

/* Record every allocation made so far into the metaslabs' space maps.
 *
 * Batched per metaslab, and appended into CACHED blocks - so the second call
 * finds those blocks already dirty and already holding a DVA, writes them to
 * the same place, and produces no new allocations to record. That is where the
 * recursion stops.
 */
static int
zfsg_sync_spacemaps(spa_t *spa, zfsg_txg_t *tx, int *from)
{
	uint64_t done_mask = 0;
	int i, rc;

	for (i = *from; i < tx->npending; i++) {
		uint64_t n = tx->pending[i].start >> tx->ga.ms_shift;
		uint64_t ms_start = n << tx->ga.ms_shift;
		uint64_t sm_obj = 0;
		dnode_phys_t *dn;
		zfsg_dbuf_t *db;
		uint64_t length, blksz, blkid, within, bytes = 0;
		int j, nw = 0;

		if (n < 64 && (done_mask & (1ull << n)) != 0) {
			continue;
		}
		if (n < 64) {
			done_mask |= (1ull << n);
		}

		rc = zfsg_ms_object(spa, &tx->ga, n, &sm_obj);
		if (rc != 0) {
			return (rc);
		}
		if (sm_obj == 0) {
			/* A metaslab never allocated from has no space map
			 * object; creating one is object creation, refused. */
			return (ENOTSUP);
		}

		rc = zfsg_dnode_hold(spa, ZFSG_OS_MOS, sm_obj, 1, &dn);
		if (rc != 0) {
			return (rc);
		}
		if (dn->dn_bonuslen < sizeof (uint64_t) * 3) {
			return (EIO);
		}
		{
			uint64_t *bonus = (uint64_t *)DN_BONUS(dn);

			length = bonus[1];
			blksz = (uint64_t)dn->dn_datablkszsec <<
			    SPA_MINBLOCKSHIFT;
			if (blksz == 0) {
				return (EIO);
			}
			blkid = length / blksz;
			within = length % blksz;

			rc = zfsg_dbuf_hold(spa, ZFSG_OS_MOS, sm_obj, 0, blkid,
			    (uint32_t)blksz, 1, &db);
			if (rc != 0) {
				return (rc);
			}

			for (j = i; j < tx->npending; j++) {
				uint64_t off = tx->pending[j].start;
				uint64_t size = tx->pending[j].size;
				uint64_t word;

				if ((off >> tx->ga.ms_shift) != n) {
					continue;
				}
				if (within + (uint64_t)(nw + 1) *
				    sizeof (uint64_t) > blksz) {
					/* Would run past this block. Split is
					 * correct and unimplemented; refusing
					 * abandons a transaction that has
					 * committed nothing. */
					return (ENOSPC);
				}
				rc = zfsg_sm_encode(
				    (off - ms_start) >> tx->ga.ashift,
				    size >> tx->ga.ashift, &word);
				if (rc != 0) {
					return (rc);
				}
				memcpy(db->data + within +
				    (size_t)nw * sizeof (uint64_t), &word,
				    sizeof (uint64_t));
				bytes += size;
				nw++;
			}

			bonus[1] = length + (uint64_t)nw * sizeof (uint64_t);
			bonus[2] += bytes;
		}
	}
	*from = tx->npending;
	return (0);
}

/* --- the cascade, on the dirty buffer layer --------------------------------
 *
 * The same chain zfsg_write_block walks, with every step going through the
 * cache. The difference is not tidiness: because a block allocated in this
 * transaction keeps its DVA, the space-map pass below settles instead of
 * feeding itself.
 */
int
zfsg_write_block2(zfsg_mount_t *mount, unsigned long long objnum,
    unsigned long long blkid, void *data, unsigned long long len,
    unsigned long long *txg_out)
{
	spa_t *spa = mount->spa;
	zfsg_txg_t tx;
	objset_phys_t *fsos = NULL, *mosos = NULL;
	dnode_phys_t *fdn, *dsdn;
	dsl_dataset_phys_t *ds;
	zfsg_dbuf_t *db;
	blkptr_t bp;
	vdev_t *vd;
	uberblock_t ub;
	uint32_t blksz;
	uint64_t fs_off = 0, fs_asize = 0, mos_off = 0, mos_asize = 0;
	int recorded = 0, pass, rc;

	if (mount == NULL || !mount->in_use) {
		return (ENXIO);
	}
	vd = zfsg_write_vdev(spa);
	if (vd == NULL || vd->v_phys_write == NULL) {
		return (EROFS);
	}

	rc = zfsg_txg_open(spa, &tx);
	if (rc != 0) {
		return (rc);
	}
	zfsg_dbuf_reset();

	fsos = malloc(sizeof (objset_phys_t));
	mosos = malloc(sizeof (objset_phys_t));
	if (fsos == NULL || mosos == NULL) {
		rc = ENOMEM;
		goto out;
	}
	memcpy(fsos, &mount->mnt.objset, sizeof (objset_phys_t));
	memcpy(mosos, &spa->spa_mos_master, sizeof (objset_phys_t));
	zfsg_os[ZFSG_OS_FS] = fsos;
	zfsg_os[ZFSG_OS_MOS] = mosos;

	/* 1. the file's data block */
	rc = zfsg_dnode_hold(spa, ZFSG_OS_FS, objnum, 0, &fdn);
	if (rc != 0) {
		goto out;
	}
	blksz = (uint32_t)fdn->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	if (blksz == 0 || len != blksz) {
		rc = EINVAL;                   /* whole existing blocks only */
		goto out;
	}
	if (blkid > fdn->dn_maxblkid) {
		rc = E2BIG;                    /* no growth here */
		goto out;
	}
	rc = zfsg_dbuf_hold(spa, ZFSG_OS_FS, objnum, 0, blkid, blksz, 1, &db);
	if (rc != 0) {
		goto out;
	}
	memcpy(db->data, data, blksz);

	/* 2. flush the filesystem, then write its objset */
	rc = zfsg_sync_objset(spa, &tx, ZFSG_OS_FS);
	if (rc != 0) {
		goto out;
	}
	{
		uint64_t align = 1ull << tx.ga.ashift;

		fs_asize = (sizeof (objset_phys_t) + align - 1) & ~(align - 1);
		rc = zfsg_alloc_dva(spa, &tx, fs_asize, &fs_off);
		if (rc != 0) {
			goto out;
		}
		rc = zfsg_zio_write_at(spa, &tx, fsos, sizeof (objset_phys_t),
		    fs_off, fs_asize, DMU_OT_OBJSET, 0, 0, &bp);
		if (rc != 0) {
			goto out;
		}
	}

	/* 3. the dataset's ds_bp, in the MOS */
	rc = zfsg_dnode_hold(spa, ZFSG_OS_MOS, mount->mnt.rootobj, 1, &dsdn);
	if (rc != 0) {
		goto out;
	}
	ds = (dsl_dataset_phys_t *)&dsdn->dn_bonus;
	ds->ds_bp = bp;

	/* 4. space maps and the MOS, alternating until both are quiet.
	 *
	 * Two passes is the expected shape: the first records everything
	 * allocated so far and dirties the space map blocks; the second finds
	 * those blocks already carrying a DVA, rewrites them in place, and
	 * produces nothing new to record. A third pass proves it settled. */
	for (pass = 0; pass < 6; pass++) {
		int before = tx.npending;

		rc = zfsg_sync_spacemaps(spa, &tx, &recorded);
		if (rc != 0) {
			goto out;
		}
		rc = zfsg_sync_objset(spa, &tx, ZFSG_OS_MOS);
		if (rc != 0) {
			goto out;
		}
		if (tx.npending == before && recorded == tx.npending) {
			break;
		}
	}
	if (recorded != tx.npending) {
		rc = EAGAIN;                   /* abandoned; pool unchanged */
		goto out;
	}

	/* 5. the MOS's own objset, and the uberblock naming it.
	 *
	 * This block's allocation is the one thing no space map records, and
	 * it is a KNOWN, BOUNDED leak of one block per transaction: it is
	 * allocated after the last space-map pass by construction, because
	 * recording it would dirty a space map and require another MOS objset.
	 * Real ZFS breaks the same cycle by carrying the final allocations
	 * into the next txg's accounting. */
	{
		uint64_t align = 1ull << tx.ga.ashift;

		mos_asize = (sizeof (objset_phys_t) + align - 1) & ~(align - 1);
		rc = zfsg_alloc_dva(spa, &tx, mos_asize, &mos_off);
		if (rc != 0) {
			goto out;
		}
		rc = zfsg_zio_write_at(spa, &tx, mosos, sizeof (objset_phys_t),
		    mos_off, mos_asize, DMU_OT_OBJSET, 0, 0, &bp);
		if (rc != 0) {
			goto out;
		}
	}

	ub = *spa->spa_uberblock;
	ub.ub_txg = tx.txg;
	ub.ub_timestamp += 1;
	ub.ub_rootbp = bp;

	rc = zfsg_uberblock_write(spa, vd, &ub);
	if (rc != 0) {
		goto out;
	}
	/* --- publish the new state in memory ---------------------------
	 *
	 * The uberblock on disk now names the new MOS, but this process is
	 * still holding the objsets it read at mount. Without this the very
	 * next read walks the OLD dnode array and returns the old bytes -
	 * which is exactly what happened: the commit was correct, real ZFS
	 * could see the change, and Genesis could not see its own write.
	 *
	 * Done only after the uberblock has landed. Publishing earlier would
	 * mean an abandoned transaction leaves this process believing in a
	 * pool state that was never written. */
	memcpy(&mount->mnt.objset, fsos, sizeof (objset_phys_t));
	memcpy(&spa->spa_mos_master, mosos, sizeof (objset_phys_t));
	if (spa->spa_mos != NULL && spa->spa_mos != &spa->spa_mos_master) {
		memcpy(spa->spa_mos, mosos, sizeof (objset_phys_t));
	}
	spa->spa_uberblock_master = ub;
	if (spa->spa_uberblock != NULL &&
	    spa->spa_uberblock != &spa->spa_uberblock_master) {
		*spa->spa_uberblock = ub;
	}

	if (txg_out != NULL) {
		*txg_out = (unsigned long long)ub.ub_txg;
	}
	rc = 0;
out:
	zfsg_dbuf_reset();
	zfsg_os[0] = zfsg_os[1] = NULL;
	if (fsos != NULL) {
		free(fsos);
	}
	if (mosos != NULL) {
		free(mosos);
	}
	return (rc);
}
