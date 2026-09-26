#include "device.h"
#include "fs.h"
#include "screen.h"
#include "typesk.h"
#include "volume.h"
#include "zfs.h"

#include "zfs_genesis.h"

/* The Genesis side of the wall, so this file speaks acl_t while zfs_sa.inc
 * speaks zfsg_ace_t. The two structures are the same four fields; the copy
 * below is what a wall costs, and it is cheap. */
#include "acl.h"

/* ZFS behind the filesystem vtable.
 *
 * This is the only file in kernel/zfs/ that knows what a device_t or an
 * fs_ops_t is, and it is the only file outside kernel/zfs/ that anything
 * includes. tests/host/check_zfs_boundary.py enforces both directions, which
 * is the same proof the FAT work used when syscall.c stopped including fat.h.
 *
 * --- The two worlds and the wall between them ----------------------------
 * Everything below kernel/zfs/vendor/ is FreeBSD's standalone reader,
 * unmodified, compiled against kernel/zfs/compat/ - a miniature libc where
 * size_t is `unsigned long`. This file is compiled against typesk.h, where
 * size_t is `unsigned long long`. They cannot share a translation unit, so
 * they share zfs_genesis.h instead: fixed-width types, no includes, and a
 * handful of functions. See that header for why.
 *
 * --- Errno signs ----------------------------------------------------------
 * The vendored code returns POSITIVE errnos, as FreeBSD does. Genesis is
 * negative-errno throughout. The negation happens HERE, in one place, on
 * every path out - a convention that is converted in two places is a
 * convention that eventually disagrees with itself, and the failure mode is
 * an error that reads as a successful read of 22 bytes. */

/* Four, and the number is doing more work than it looks.
 *
 * A slot used to be taken by a successful probe and never released, because
 * there was no unmount hook in fs_ops_t for it to be released FROM:
 * fs_unmount_volume removed mount-table entries and never told the
 * filesystem. A machine that had seen four pools stopped recognising the
 * fifth. The host tests hit it immediately - they probe four pools in a row -
 * which is how it was found.
 *
 * fs_ops_t::unmount exists now (ROADMAP item 7 called its absence the one
 * item on that list that was a bug rather than a missing feature), and
 * zfs_unmount below is what it calls. Note what that does and does not fix:
 * the slot is RETIRED, not freed, so four is still the number of pools that
 * can be mounted AT ONCE, and it is no longer the number that can be mounted
 * over the life of the machine. Four simultaneous is a real limit; four ever
 * was a leak. */
#define ZFSFS_MAX 4

static fs_volume_t   zfs_fs_slots[ZFSFS_MAX];
static zfsg_mount_t *zfs_mounts[ZFSFS_MAX];
static device_t     *zfs_devs[ZFSFS_MAX];
static int           zfs_used[ZFSFS_MAX];

/* Unmounted, but the fs_volume_t may still be pointed at by an fs_node_t
 * inside somebody's open handle. See fs_ops_t::unmount in fs.h: reused only
 * when no free slot is left, because handing this fs_volume_t to a different
 * pool turns a stale handle's honest -ENODEV into a successful read of
 * another filesystem. */
static int           zfs_retired[ZFSFS_MAX];

/* The node as Genesis carries it. 32 bytes against FS_PRIVATE_MAX's 64; the
 * assertion is the same idiom fatfs.c uses, and it exists so that a future
 * field lands as a build failure rather than as a write past the end of an
 * object body. */
typedef char zfs_priv_fits[(sizeof(zfsg_node_t) <= FS_PRIVATE_MAX) ? 1 : -1];

static zfsg_mount_t *mount_of(const fs_volume_t *v) {
    int i;

    for (i = 0; i < ZFSFS_MAX; i++) {
        if (zfs_used[i] && &zfs_fs_slots[i] == v) {
            return zfs_mounts[i];
        }
    }
    return NULL;
}

/* --- what the vendored reader calls out to ------------------------------- */

long long zfsg_dev_write(void *ctx, unsigned long long offset,
                         const void *buf, unsigned long long n) {
    /* Through dev_write, and therefore through the block cache, which is what
     * keeps a subsequent read of the same block from seeing the stale copy
     * the cache is still holding. A write path that bypassed the cache would
     * be correct on disk and wrong to every reader in this boot - the worst
     * combination, because it survives a reboot and looks like a phantom. */
    return (long long)dev_write((device_t *)ctx, (uint64)offset, buf,
                                (uint64)n);
}

long long zfsg_dev_read(void *ctx, unsigned long long offset, void *buf,
                        unsigned long long n) {
    /* Straight to dev_read, which means straight through the block cache for
     * a disk that has one. That matters more here than anywhere else: the
     * vendored reader re-reads the dnode array on every operation, because a
     * Genesis fs_node_t can only carry an object number (see zfs_genesis.h),
     * and those re-reads hit the same few blocks over and over. Item 5 is
     * what makes item 7's shape affordable. */
    return (long long)dev_read((device_t *)ctx, (uint64)offset, buf,
                               (uint64)n);
}

unsigned long long zfsg_dev_size(void *ctx) {
    device_t *dev = (device_t *)ctx;

    return dev != NULL ? (unsigned long long)dev->size : 0ULL;
}

/* --- the vtable ---------------------------------------------------------- */

static zfsg_node_t *node_priv(const fs_node_t *n) {
    return (zfsg_node_t *)(void *)n->priv;
}

static int zfsfs_lookup(fs_volume_t *v, const char *abs_path, fs_node_t *out) {
    zfsg_mount_t *m = mount_of(v);
    zfsg_node_t node;
    int rc;

    if (m == NULL) {
        return -19;                          /* -ENODEV */
    }
    rc = zfsg_lookup(m, abs_path, &node);
    if (rc != 0) {
        return -rc;
    }

    out->size   = node.size;
    out->ino    = node.objnum;
    out->is_dir = node.is_dir;
    /* Real ownership, off the disk. These come from a walk of the object's
     * own System Attribute layout rather than from the fixed SA_UID_OFFSET
     * constants the vendored reader uses - see kernel/zfs/zfs_sa.inc for why
     * the constants cannot be extended to reach everything, and why the
     * walk that exists for the ACL is used for these too. */
    out->uid    = node.uid;
    out->gid    = node.gid;
    out->mode   = (uint32)node.mode;
    *node_priv(out) = node;
    return 0;
}

/* --- the ACL ---------------------------------------------------------------
 *
 * The one place where "ZFS has ACLs and FAT does not" is expressed. Note what
 * this function does NOT do: it does not decide anything, it does not consult
 * the mode, and it does not fall back. Reporting what is on disk and deciding
 * what it means are different jobs, done in different files, and keeping them
 * apart is what stops a filesystem from quietly having its own idea of who
 * may read a file.
 *
 * ZFS_ACL_TRIVIAL is honoured here rather than ignored. When it is set the
 * stored ACL says exactly what the mode bits say, real ZFS does not read the
 * ACL at all, and neither does this: returning -ENOENT sends fs_getacl to the
 * projection, which produces the same three entries at no I/O cost. On the
 * fixture that is four of the five objects; only secret.txt has the flag
 * clear. Skipping the read is not the point, though - agreeing with what ZFS
 * itself considers authoritative is. */
static int zfsfs_getacl(fs_volume_t *v, const fs_node_t *n, struct acl *out) {
    zfsg_mount_t *m = mount_of(v);
    zfsg_ace_t aces[ACL_ACE_MAX];
    acl_t *a = (acl_t *)out;
    unsigned long long pflags = 0;
    int count = 0;
    int rc, i;

    if (m == NULL) {
        return -19;                          /* -ENODEV */
    }

    rc = zfsg_getacl(m, node_priv(n)->objnum, aces, ACL_ACE_MAX, &count,
                     &pflags);
    if (rc != 0) {
        return -rc;                          /* positive errno across the wall */
    }
    if (pflags & ZFS_ACL_TRIVIAL) {
        return -2;                           /* -ENOENT: project the mode */
    }
    if (count <= 0) {
        /* A count of zero is not an empty ACL meaning "deny everything" - it
         * is an object that never had one stored. An empty ACL really would
         * deny everything (acl_access grants only what an entry allows), so
         * returning it here would make such a file unreadable by anyone. */
        return -2;
    }

    a->count = (uint32)count;
    a->trivial = 0;
    a->owner = n->uid;
    a->group = n->gid;
    for (i = 0; i < count && i < ACL_ACE_MAX; i++) {
        a->ace[i].type  = aces[i].type;
        a->ace[i].flags = aces[i].flags;
        a->ace[i].mask  = aces[i].mask;
        a->ace[i].who   = aces[i].who;
    }
    return 0;
}

static int64 zfsfs_read(fs_volume_t *v, const fs_node_t *n, uint64 offset,
                        void *buf, uint64 max) {
    zfsg_mount_t *m = mount_of(v);
    long long got;

    if (m == NULL) {
        return -19;
    }
    got = zfsg_read(m, node_priv(n)->objnum, offset, buf, max);
    /* zfsg_read already returns a NEGATIVE errno or a byte count - it is the
     * one function across the seam that does, because a read has to be able
     * to return both and a count is never negative. */
    return (int64)got;
}

struct iter_ctx {
    fs_dir_cb cb;
    void     *ctx;
};

static int iter_shim(const char *name, unsigned long long objnum, int is_dir,
                     void *ctx) {
    struct iter_ctx *ic = (struct iter_ctx *)ctx;
    fs_dirent_t ent;
    uint64 i;

    for (i = 0; i + 1 < sizeof(ent.name) && name[i] != '\0'; i++) {
        ent.name[i] = name[i];
    }
    ent.name[i] = '\0';
    ent.ino    = objnum;
    ent.is_dir = is_dir;
    return ic->cb(&ent, ic->ctx);
}

static int zfsfs_iterate(fs_volume_t *v, const fs_node_t *dir, fs_dir_cb cb,
                         void *ctx) {
    zfsg_mount_t *m = mount_of(v);
    struct iter_ctx ic;
    int rc;

    if (m == NULL) {
        return -19;
    }
    if (!dir->is_dir) {
        return -20;                          /* -ENOTDIR */
    }
    ic.cb  = cb;
    ic.ctx = ctx;
    rc = zfsg_iterate(m, node_priv(dir)->objnum, iter_shim, &ic);
    /* A positive return is the CALLER's stop value, passed back unchanged -
     * the same rule fatfs.c follows, and for the same reason: "the caller's
     * buffer filled" must not come back as -EIO. */
    if (rc > 0) {
        return rc;
    }
    return -rc;
}

/* Defined below, next to the slot table it retires into. */
static void zfs_unmount(fs_volume_t *v);

static const fs_ops_t zfs_ops = {
    .name    = "zfs",
    .lookup  = zfsfs_lookup,
    .read    = zfsfs_read,
    /* No write slot, and that is what makes fs_writable answer honestly. The
     * vendored reader has no write path at all: its vdev write callback
     * returns EROFS. "Then write" from the ordered TODO is a different piece
     * of software, not a flag flipped here. */
    .write   = NULL,
    .iterate = zfsfs_iterate,
    .unmount = zfs_unmount,
    .getacl  = zfsfs_getacl
};

/* --- allocation state ------------------------------------------------------
 *
 * The Genesis-side half of zfsg_alloc_stats. See kernel/include/zfs.h for why
 * this exists: it is a check on the space map decoder against zdb, not
 * something the kernel uses. */
int zfs_alloc_stats(const struct fs_volume *v, unsigned long long *asize,
                    unsigned long long *allocated,
                    unsigned long long *ms_count,
                    unsigned long long *ms_shift) {
    zfsg_mount_t *m = mount_of((const fs_volume_t *)v);
    int rc;

    if (m == NULL) {
        return -19;                          /* -ENODEV */
    }
    rc = zfsg_alloc_stats(m, asize, allocated, ms_count, ms_shift);
    return rc == 0 ? 0 : -rc;
}

int zfs_alloc_probe(const struct fs_volume *v, unsigned long long size,
                    int count, unsigned long long *out, int *conflicts) {
    zfsg_mount_t *m = mount_of((const fs_volume_t *)v);
    int rc;

    if (m == NULL) {
        return -19;
    }
    rc = zfsg_alloc_probe(m, size, count, out, conflicts);
    return rc == 0 ? 0 : -rc;
}

int zfs_write_probe(const struct fs_volume *v, unsigned long long size,
                    unsigned long long *offset, int *raw_ok,
                    int *readback_ok) {
    zfsg_mount_t *m = mount_of((const fs_volume_t *)v);
    int rc;

    if (m == NULL) {
        return -19;
    }
    rc = zfsg_write_probe(m, size, offset, raw_ok, readback_ok);
    return rc == 0 ? 0 : -rc;
}

int zfs_txg_commit_empty(const struct fs_volume *v, unsigned long long *txg) {
    zfsg_mount_t *m = mount_of((const fs_volume_t *)v);
    int rc;

    if (m == NULL) {
        return -19;
    }
    rc = zfsg_txg_commit_empty(m, txg);
    return rc == 0 ? 0 : -rc;
}

int zfs_write_block(const struct fs_volume *v, unsigned long long objnum,
                    unsigned long long blkid, void *data,
                    unsigned long long len, unsigned long long *txg) {
    zfsg_mount_t *m = mount_of((const fs_volume_t *)v);
    int rc;

    if (m == NULL) {
        return -19;
    }
    /* zfsg_write_block2: the cascade on the dirty-buffer layer. The original
     * zfsg_write_block is kept beside it because it is the version whose
     * failure PROVED what was missing - it walks the same chain without a
     * cache and reports EAGAIN, and the two together are the argument for why
     * the cache exists. */
    rc = zfsg_write_block2(m, objnum, blkid, data, len, txg);
    return rc == 0 ? 0 : -rc;
}

/* fs_ops_t::unmount - the hook this file's header used to say was missing.
 *
 * zfsg_unmount has existed on the vendored side the whole time and was never
 * called, which is the shape this bug had: not a missing implementation, a
 * missing CALL, because there was no vtable slot to call it from.
 *
 * Retired rather than freed - see fs.h. What is genuinely released is the
 * zfsg_mount_t, which is the expensive half: it holds the spa and everything
 * the vendored reader cached with it. */
static void zfs_unmount(fs_volume_t *v) {
    int i;

    for (i = 0; i < ZFSFS_MAX; i++) {
        if (zfs_used[i] && &zfs_fs_slots[i] == v) {
            if (zfs_mounts[i] != NULL) {
                zfsg_unmount(zfs_mounts[i]);
                zfs_mounts[i] = NULL;
            }
            zfs_devs[i]    = NULL;
            zfs_retired[i] = 1;
            return;
        }
    }
}

/* --- the prober ---------------------------------------------------------- */

fs_volume_t *zfs_probe(device_t *dev) {
    char name[64];
    zfsg_mount_t *m = NULL;
    int slot;
    int rc;

    /* A free slot first, a retired one only if there is none. Two passes
     * rather than one test, so reusing a retired slot - which is the case
     * that can hand a stale handle somebody else's filesystem - happens only
     * when there is genuinely no alternative. */
    for (slot = 0; slot < ZFSFS_MAX; slot++) {
        if (!zfs_used[slot]) {
            break;
        }
    }
    if (slot == ZFSFS_MAX) {
        for (slot = 0; slot < ZFSFS_MAX; slot++) {
            if (zfs_retired[slot]) {
                break;
            }
        }
    }
    if (slot == ZFSFS_MAX) {
        return NULL;
    }
    zfs_retired[slot] = 0;

    name[0] = '\0';
    rc = zfsg_mount(dev, name, sizeof(name), &m);
    if (rc != 0) {
        /* ENXIO is "there is no ZFS label here", which is the normal answer
         * for a FAT volume and must be silent - a prober that complains about
         * every volume it is not is a prober nobody reads. Anything else
         * means there IS a pool and something about it was refused, and the
         * vendored code has already printed which; this line says which
         * volume it was. */
        /* ENXIO is "no ZFS label here" and EIO is "there is something at the
         * label offsets and it did not parse" - which is what a FAT boot
         * sector looks like from four labels away. Both are the normal answer
         * for a volume that is not ours, and both have to be silent: a prober
         * that complains about every volume it is not is a prober nobody
         * reads, and this one runs against every volume on the machine.
         *
         * Everything else means the vendored reader got far enough to have an
         * opinion - a feature it does not implement, a raidz layout, a pool
         * whose MOS would not read - and it has already printed which. This
         * line only says which volume. */
        if (rc != 6 && rc != 5) {            /* ENXIO, EIO */
            print_string("zfs: a pool was found but not mounted on ", 0x0E);
            print_string(dev->ns_name, 0x0E);
            print_string("\n", 0x0E);
        }
        return NULL;
    }

    zfs_fs_slots[slot].ops        = &zfs_ops;
    zfs_fs_slots[slot].body       = NULL;
    zfs_fs_slots[slot].block_size = 512;
    zfs_fs_slots[slot].mounted    = 1;
    zfs_mounts[slot] = m;
    zfs_devs[slot]   = dev;
    zfs_used[slot]   = 1;

    print_string("zfs: pool '", 0x0A);
    print_string(name, 0x0A);
    print_string("' on ", 0x0A);
    print_string(dev->ns_name, 0x0A);
    print_string("\n", 0x0A);
    return &zfs_fs_slots[slot];
}

void zfs_init(void) {
    volume_register_fs("zfs", zfs_probe);
}
