#ifndef ZFS_GENESIS_H
#define ZFS_GENESIS_H

/* The seam between the vendored reader and the rest of Genesis.
 *
 * --- Why this header includes nothing -------------------------------------
 * The vendored reader is compiled against kernel/zfs/compat/, which is a
 * miniature libc: <stddef.h> there means gcc's, and size_t is `unsigned
 * long`. The rest of the kernel is compiled against typesk.h, where size_t is
 * `unsigned long long`. Both are correct in their own world and the two
 * cannot meet inside one translation unit - a file that includes fs.h and
 * zfsimpl.h is a file that does not compile, and the error it produces
 * ("conflicting types for size_t") tells you nothing about why.
 *
 * So they do not meet. The vendored side and the Genesis side are separate
 * objects, and everything that crosses between them is declared HERE in terms
 * of `unsigned long long` and `long long` - types that mean the same thing on
 * both sides of the wall by construction rather than by agreement.
 *
 * That is also why the node below carries an OBJECT NUMBER rather than a
 * dnode_phys_t. A dnode is 512 bytes and fs_node_t's opaque area is 64, so a
 * node could not hold one anyway - but the deeper reason is that dnode_phys_t
 * is a vendored type, and putting it in this header would put the vendored
 * world's headers into vfs.c's translation unit. The object number is a
 * number. Re-fetching the dnode per operation costs one read of the dnode
 * array, which the block cache (item 5) is holding.
 */

/* --- provided by the Genesis side, in zfs_vfs.c -------------------------- */

/* Read from the medium a pool was found on. Returns bytes read, or negative.
 * `ctx` is whatever was handed to zfsg_mount. */
long long zfsg_dev_read(void *ctx, unsigned long long offset, void *buf,
                        unsigned long long n);

/* How big that medium is. vdev_probe needs it to find the two labels at the
 * END of the device, which is where a torn front leaves the only good copy. */
unsigned long long zfsg_dev_size(void *ctx);

/* Write to the medium. Returns bytes written, or negative.
 *
 * Separate from zfsg_dev_read rather than a flag on it because the two have
 * genuinely different failure modes and different callers: a failed read is
 * an -EIO the caller retries or reports, and a failed write in the middle of
 * a transaction leaves the pool describing blocks that were never written.
 * A medium with no write path returns -EROFS here and every allocation above
 * it fails early, which is the behaviour a read-only volume should have. */
long long zfsg_dev_write(void *ctx, unsigned long long offset,
                         const void *buf, unsigned long long n);

/* --- provided by the vendored side, in zfs_vendor.c ---------------------- */

/* One mounted pool. Opaque: its innards are vendored types. */
typedef struct zfsg_mount zfsg_mount_t;

/* What a lookup hands back. Fixed-width and small enough for the 64 bytes
 * fs_node_t sets aside. */
typedef struct zfsg_node {
    unsigned long long objnum;
    unsigned long long size;
    unsigned long long mode;
    int                is_dir;

    /* Ownership, and the znode flags that decide whether the ACL is worth
     * reading. These come from a real walk of the SA layout (zfs_sa.inc),
     * not from the fixed SA_UID_OFFSET/SA_GID_OFFSET constants the vendored
     * zfs_dnode_stat uses - see that file for why the constants cannot be
     * extended to reach everything.
     *
     * On a pre-v5 filesystem, whose attributes are a znode_phys_t rather
     * than an SA layout, there is no walk to do and these stay whatever
     * zfs_dnode_stat found. */
    unsigned int       uid;
    unsigned int       gid;
    unsigned long long pflags;
} zfsg_node_t;

/* One access control entry, in the form ZFS stores and NT understands.
 *
 * The constants for `type`, `flags` and `mask` are in kernel/include/acl_abi.h,
 * which is #defines only precisely so that it can be included from both sides
 * of the wall this header describes.
 *
 * `who` is a uid, or a gid when ACE_IDENTIFIER_GROUP is set in `flags`, and
 * is meaningless for the three special ACEs (owner@, group@, everyone@) whose
 * identity lives in the flags instead. */
typedef struct zfsg_ace {
    unsigned int       type;
    unsigned int       flags;
    unsigned int       mask;
    unsigned long long who;
} zfsg_ace_t;

/* Find a pool on this medium and mount its root filesystem.
 *
 * Returns 0, or a POSITIVE errno - the vendored code's convention, kept
 * across the seam deliberately so that the negation happens in exactly one
 * place (zfs_vfs.c) instead of being half-done on both sides. ENXIO means
 * "no ZFS here", which is what a prober needs in order to stay quiet.
 *
 * `name_out` receives the pool name, for the boot report. */
int zfsg_mount(void *ctx, char *name_out, unsigned int name_cap,
               zfsg_mount_t **out);
void zfsg_unmount(zfsg_mount_t *mount);

/* Resolve an absolute path within the mounted filesystem. Symlinks are
 * followed by the vendored code. */
int zfsg_lookup(zfsg_mount_t *mount, const char *path, zfsg_node_t *out);

/* Re-stat an object by number, for a node that has been held across calls. */
int zfsg_stat(zfsg_mount_t *mount, unsigned long long objnum,
              zfsg_node_t *out);

/* Read from a file. Returns bytes read, or a negative errno. A short read at
 * the end of the file is normal. */
long long zfsg_read(zfsg_mount_t *mount, unsigned long long objnum,
                    unsigned long long offset, void *buf,
                    unsigned long long len);

/* Walk a directory. A non-zero return from the callback stops the walk and is
 * returned. */
typedef int (*zfsg_dir_cb)(const char *name, unsigned long long objnum,
                           int is_dir, void *ctx);
int zfsg_iterate(zfsg_mount_t *mount, unsigned long long objnum,
                 zfsg_dir_cb cb, void *ctx);

/* Read an object's ACL.
 *
 * Fills at most `max` entries and reports how many through `count_out`; also
 * reports the znode's pflags, whose ZFS_ACL_TRIVIAL bit says the ACL adds
 * nothing to the mode bits and need not have been read at all.
 *
 * Returns 0, or a POSITIVE errno as everything across this wall does:
 *   ENOENT  this object has no SA ACL - a pre-v5 filesystem, or an object
 *           whose layout has no DACL attributes. Not an error; the caller
 *           should project the mode instead.
 *   E2BIG   the ACL has more entries than `max`. Refused, not truncated: a
 *           truncated ACL is a different ACL, and the entries most likely to
 *           be lost are the deny entries that conventionally trail the
 *           allows.
 *   EIO     the count and the ACE blob disagree, or an entry runs off the
 *           end of it.
 *   ENOTSUP the object's SA layout contains an attribute this reader cannot
 *           size, so every offset after it - including the ACL's - is
 *           unknown. Refused rather than guessed. */
int zfsg_getacl(zfsg_mount_t *mount, unsigned long long objnum,
                zfsg_ace_t *out, int max, int *count_out,
                unsigned long long *pflags_out);

/* --- allocation ------------------------------------------------------------
 *
 * Report the vdev's allocation geometry and how much of it is in use, by
 * replaying every metaslab's space map. Present so the space map decoder can
 * be checked against `zdb -mmm` and `zpool list`, which compute the same
 * numbers from an independent implementation.
 *
 * Returns 0 or a POSITIVE errno, as everything across this wall does.
 * ENOTSUP means the pool has more than one top-level vdev, which this code
 * refuses rather than allocating from one of them and hoping. */
int zfsg_alloc_stats(zfsg_mount_t *mount, unsigned long long *asize_out,
                     unsigned long long *alloc_out,
                     unsigned long long *ms_count_out,
                     unsigned long long *ms_shift_out);

/* Run the allocator `count` times and report the offsets, committing nothing.
 * `conflicts_out` receives the number of results that overlap a range the
 * on-disk space maps say is already allocated - which must be zero. */
int zfsg_alloc_probe(zfsg_mount_t *mount, unsigned long long size, int count,
                     unsigned long long *out, int *conflicts_out);

/* Write one block into free space and read it back through the vendored
 * zio_read. `readback_ok` is set when the bytes matched. Safe on a live pool:
 * nothing is made to point at the block, so the space stays free. */
int zfsg_write_probe(zfsg_mount_t *mount, unsigned long long size,
                     unsigned long long *offset_out, int *raw_ok,
                     int *readback_ok);

/* Commit a transaction group that changes nothing but the txg number. Writes
 * a new uberblock into all four labels, at the ring slot `txg % count`.
 * Exercises the whole label path with no metadata rewritten, so there is
 * nothing it can leave inconsistent. */
int zfsg_txg_commit_empty(zfsg_mount_t *mount, unsigned long long *txg_out);

/* Overwrite one whole block of one object and commit it, making the change
 * really visible to any ZFS implementation.
 *
 * WHOLE BLOCKS ONLY, and only blocks that already exist: `len` must equal the
 * object's block size and `blkid` must not exceed dn_maxblkid. Growth and
 * partial writes are refused rather than half-implemented.
 *
 * Returns 0 or a POSITIVE errno. EAGAIN means the transaction's allocations
 * did not settle within the pass budget and it was ABANDONED - the pool is
 * unchanged and retrying is reasonable. Every other error also leaves the pool
 * unchanged, because nothing is visible until the uberblock. */
int zfsg_write_block(zfsg_mount_t *mount, unsigned long long objnum,
                     unsigned long long blkid, void *data,
                     unsigned long long len, unsigned long long *txg_out);

/* The same operation on the dirty-buffer layer, which is the one that
 * converges. See kernel/zfs/zfs_write.inc. */
int zfsg_write_block2(zfsg_mount_t *mount, unsigned long long objnum,
                      unsigned long long blkid, void *data,
                      unsigned long long len, unsigned long long *txg_out);

#endif
