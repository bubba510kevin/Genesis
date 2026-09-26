#ifndef ZFS_H
#define ZFS_H

/* The ONE header kernel/zfs/ exposes to the rest of the kernel, and it
 * deliberately names no ZFS type at all.
 *
 * The ordered TODO asked for the CDDL boundary to be turned into the same
 * kind of proof the fs vtable work used - syscall.c and fileobj.c no longer
 * including fat.h was the check that boundary was real. This is that check's
 * subject: tests/host/check_zfs_boundary.py asserts that nothing outside
 * kernel/zfs/ includes anything from kernel/zfs/, that this file mentions no
 * ZFS structure, and that kernel/zfs/ itself includes only a short whitelist
 * of Genesis headers.
 *
 * Two things follow from that, and both were the point:
 *
 *   The CDDL code is quarantined. Everything under kernel/zfs/ is CDDL (or,
 *   for the LZ4 decoder, BSD-2-Clause); the rest of Genesis is BSD-2-Clause
 *   and links against a boundary rather than mixing with it.
 *
 *   The filesystem is replaceable. flk.c calls zfs_init() and nothing else,
 *   so deleting the directory deletes the feature and breaks one line. */

/* Register the ZFS prober with volume.c. Call once at boot, BEFORE
 * volume_init - a prober registered after the scan is one nothing will ever
 * be probed against, and the symptom is a pool reported as an unrecognised
 * volume, which reads like a bad disk rather than a boot-order mistake. */
void zfs_init(void);

/* --- allocation state, for checking the space map decoder ------------------
 *
 * Reports the vdev's allocation geometry and how many bytes of it are in use,
 * by replaying every metaslab's space map. Nothing in the kernel needs this;
 * it exists so the decoder can be held against `zdb -mmm` and `zpool list`,
 * which compute the same numbers from an implementation this tree did not
 * write.
 *
 * Space maps are the gate on writing - ZFS is copy-on-write, so every write
 * allocates, and allocation is the half of a write that destroys a pool
 * rather than failing a read when it is wrong. This is the part of it that
 * can be verified without risking anything, so it is landed and checked on
 * its own first.
 *
 * `struct fs_volume` is forward-declared rather than included: this header
 * names no ZFS type by design (see above), and it should not drag fs.h into
 * everything that includes it either.
 *
 * Returns 0 or a negative errno. -ENOTSUP means the pool has more than one
 * top-level vdev, which is refused rather than guessed at. */
struct fs_volume;
int zfs_alloc_stats(const struct fs_volume *v, unsigned long long *asize,
                    unsigned long long *allocated,
                    unsigned long long *ms_count,
                    unsigned long long *ms_shift);

/* Allocate `count` runs of `size` bytes from this pool's free space and report
 * where they would go, WITHOUT writing anything. `conflicts` receives how many
 * of them collide with space the metaslabs say is already in use, and the only
 * acceptable value is zero. */
int zfs_alloc_probe(const struct fs_volume *v, unsigned long long size,
                    int count, unsigned long long *out, int *conflicts);

/* Write a block into this pool's free space and read it back through the
 * vendored reader, checksum verification and all. Nothing is made to point at
 * it, so the pool is unchanged. `readback_ok` receives 1 when the bytes came
 * back identical. */
int zfs_write_probe(const struct fs_volume *v, unsigned long long size,
                    unsigned long long *offset, int *raw_ok,
                    int *readback_ok);

/* Write a new uberblock carrying txg+1 and the same pool contents, into all
 * four labels. The smallest possible commit: it makes no metadata change, so
 * the pool afterwards describes exactly what it did before, one txg later. */
int zfs_txg_commit_empty(const struct fs_volume *v, unsigned long long *txg);

/* Overwrite one whole block of one object on this pool and commit it. See
 * kernel/zfs/zfs_genesis.h for the constraints - whole existing blocks only.
 * Returns 0 or a negative errno; -EAGAIN means the transaction was abandoned
 * and the pool is unchanged. */
int zfs_write_block(const struct fs_volume *v, unsigned long long objnum,
                    unsigned long long blkid, void *data,
                    unsigned long long len, unsigned long long *txg);

#endif
