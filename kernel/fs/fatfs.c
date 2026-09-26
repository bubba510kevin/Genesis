#include "fat.h"
#include "fatfs.h"
#include "fs.h"
#include "typesk.h"

/* FAT16 behind the filesystem vtable.
 *
 * This file is the boundary. Above it nothing knows what a cluster is; below
 * it, fat.c is unchanged and still speaks FAT_ERR_. Everything that used to
 * make syscall.c and fileobj.c FAT-aware lives in these ninety lines:
 * attribute bits, the 8.3 name, the first cluster as an inode number, and the
 * FAT_ERR_ to errno table that used to sit in syscall.c as fat_errno().
 *
 * The test for whether this boundary is real is whether a second filesystem
 * can be added without touching anything above it. That is what the ext4 work
 * will find out, and it is the reason this landed as its own change. */

/* fat_entry_t has to fit in the node's opaque area. A negative array size is
 * the assertion idiom this kernel already uses for every ABI structure: the
 * check costs nothing and fires at compile time, rather than as a memcpy that
 * runs off the end of a node into whatever the object pool holds next. */
typedef char fat_priv_fits[(sizeof(fat_entry_t) <= FS_PRIVATE_MAX) ? 1 : -1];

/* One pair per mountable volume rather than one pair total.
 *
 * The single static pair was correct while there was exactly one filesystem
 * on exactly one disk. With a partition table there can be several, and two
 * volumes sharing one fat_volume_t means the second mount silently
 * repurposes the first - every open file on volume 1 starts reading volume
 * 2's geometry, with no error anywhere. */
#define FATFS_MAX 4

static fat_volume_t fat_vols[FATFS_MAX];
static fs_volume_t  fat_fss[FATFS_MAX];
static int          fat_used[FATFS_MAX];

/* A slot that has been unmounted but whose fs_volume_t may still be pointed
 * at by an fs_node_t inside a handle somebody has open. See fs_ops_t::unmount
 * in fs.h: retired rather than free, and reused only when there is no free
 * slot left, because handing this fs_volume_t to a different disk turns a
 * stale handle's honest -ENODEV into a successful read of another
 * filesystem. */
static int          fat_retired[FATFS_MAX];

/* The FAT_ERR_ to errno table, moved here from syscall.c.
 *
 * The distinction fat_lookup draws between "no such name" and "a component
 * was not a directory" is worth carrying all the way out: ash prints
 * different messages for them, and they mean different things about what the
 * user typed. */
static int fat_to_errno(int rc) {
    switch (rc) {
        case FAT_OK:             return 0;
        case FAT_ERR_NOTFOUND:   return -2;    /* -ENOENT        */
        case FAT_ERR_NOTDIR:     return -20;   /* -ENOTDIR       */
        case FAT_ERR_ISDIR:      return -21;   /* -EISDIR        */
        case FAT_ERR_BADPATH:    return -36;   /* -ENAMETOOLONG  */
        case FAT_ERR_NOMEM:      return -12;   /* -ENOMEM        */
        case FAT_ERR_TOOBIG:     return -27;   /* -EFBIG         */
        /* The mutation errors. Each maps to the errno a caller actually
         * branches on - EEXIST and ENOTEMPTY in particular, because `mkdir
         * -p` tests for the first and `rm -r` for the second, and folding
         * either into EIO makes both misbehave. */
        case FAT_ERR_EXISTS:     return -17;   /* -EEXIST        */
        case FAT_ERR_NOTEMPTY:   return -39;   /* -ENOTEMPTY     */
        case FAT_ERR_FULL:       return -28;   /* -ENOSPC        */
        case FAT_ERR_INVAL:      return -22;   /* -EINVAL        */
        default:                 return -5;    /* -EIO           */
    }
}

static const fat_entry_t *node_entry(const fs_node_t *n) {
    return (const fat_entry_t *)(const void *)n->priv;
}

static int fatfs_lookup(fs_volume_t *v, const char *abs_path, fs_node_t *out) {
    fat_volume_t *vol = (fat_volume_t *)v->body;
    fat_entry_t ent;
    fat_entry_t *slot;
    int rc;

    rc = fat_lookup(vol, abs_path, &ent);
    if (rc != FAT_OK) {
        return fat_to_errno(rc);
    }

    out->size   = ent.size;
    out->is_dir = (ent.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
    /* The first cluster as the inode number: stable for the life of the file
     * and unique among files that have contents. Empty files all report 0,
     * which is wrong and harmless - nothing compares inodes yet, and a real
     * filesystem will supply a real one. */
    out->ino    = ent.cluster;

    /* FAT16 stores no owner and no permission bits, so these are a stated
     * convention rather than a fact read off the disk - which is exactly why
     * they are stated HERE, once, instead of being invented by each caller
     * that needs a mode. syscall.c used to write 0100755 into a stat buffer
     * as a literal for this reason; now there is somewhere for the answer to
     * come from.
     *
     * Owned by root and world-readable, not world-writable: the read-only
     * half is the truth (anyone may read a FAT volume, there is nothing to
     * check against), and the write half is decided by fs_writable_vol rather
     * than by these bits, so claiming 0666 here would only produce an
     * access(W_OK) that says yes and a write that says -EROFS.
     *
     * The read-only marker is honoured because it is the one permission fact
     * FAT does record. */
    out->uid = 0;
    out->gid = 0;
    out->mode = (out->is_dir ? 0040000u : 0100000u) |
                ((ent.attr & FAT_ATTR_READONLY) ? 0555u : 0755u);

    slot = (fat_entry_t *)(void *)out->priv;
    *slot = ent;
    return 0;
}

static int64 fatfs_read(fs_volume_t *v, const fs_node_t *n, uint64 offset,
                        void *buf, uint64 max) {
    fat_volume_t *vol = (fat_volume_t *)v->body;
    uint32 got = 0;
    int rc;

    if (max > 0x7FFFFFFFULL) {
        max = 0x7FFFFFFFULL;
    }
    rc = fat_read_entry_at(vol, node_entry(n), offset, buf, (uint32)max, &got);
    if (rc != FAT_OK) {
        return fat_to_errno(rc);
    }
    return (int64)got;
}

/* File CONTENT writing - the last NULL slot in this vtable, and the oldest
 * entry in the ordered TODO's Owed list.
 *
 * The arrangement it completes is worth restating, because it is what made
 * the gap honest while it lasted: fs_writable() derives its answer from this
 * pointer, and access(W_OK) derives its answer from fs_writable(). While the
 * slot was NULL, access(W_OK) said -EACCES and it was TRUE. Filling in the
 * slot is what changes access(2)'s answer - there is no second flag to
 * remember, which is exactly why a stub returning -EROFS was refused.
 *
 * `n` is non-const here where fatfs_read's is const, and that asymmetry is
 * load-bearing: a write that extends a file changes its size and, for a file
 * that was empty, its first cluster. Both live in this node's copy of the
 * directory entry, and a node left describing the old size reports it to
 * fstat and truncates the next read. fat_write_entry_at updates the entry in
 * place; this copies the size back up to the node the VFS layer holds. */
static int64 fatfs_write(fs_volume_t *v, fs_node_t *n, uint64 offset,
                         const void *buf, uint64 max) {
    fat_volume_t *vol = (fat_volume_t *)v->body;
    fat_entry_t  *ent = (fat_entry_t *)(void *)n->priv;
    uint32 wrote = 0;
    int rc;

    if (max > 0x7FFFFFFFULL) {
        max = 0x7FFFFFFFULL;
    }
    rc = fat_write_entry_at(vol, ent, offset, buf, (uint32)max, &wrote);
    if (rc != FAT_OK) {
        return fat_to_errno(rc);
    }

    /* Back up to the node, both fields. `ino` is the first cluster, so a file
     * that was empty gets its inode number here - it had none before, which
     * is the "empty files all report 0" case fatfs_lookup already notes. */
    n->size = ent->size;
    n->ino  = ent->cluster;
    return (int64)wrote;
}

struct iter_ctx {
    fs_dir_cb cb;
    void     *ctx;
};

static int iter_shim(const fat_entry_t *entry, void *ctx) {
    struct iter_ctx *ic = (struct iter_ctx *)ctx;
    fs_dirent_t out;
    int i;

    for (i = 0; i < 12 && entry->name[i] != '\0'; i++) {
        out.name[i] = entry->name[i];
    }
    out.name[i] = '\0';
    out.ino     = entry->cluster;
    out.is_dir  = (entry->attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
    return ic->cb(&out, ic->ctx);
}

static int fatfs_iterate(fs_volume_t *v, const fs_node_t *dir, fs_dir_cb cb,
                         void *ctx) {
    fat_volume_t *vol = (fat_volume_t *)v->body;
    struct iter_ctx ic;
    int rc;

    ic.cb  = cb;
    ic.ctx = ctx;
    rc = fat_iterate_dir(vol, node_entry(dir), iter_shim, &ic);
    /* fat_iterate_dir returns a positive value when the callback stopped the
     * walk early, which is not an error and must not go through the errno
     * table - -EIO for "the caller's buffer filled" would make a partial
     * getdents look like a broken disk. */
    if (rc > 0) {
        return rc;
    }
    return fat_to_errno(rc);
}

/* --- mutation ------------------------------------------------------------
 *
 * Thin: fat.c does the work and this converts the error. The paths arriving
 * here are already volume-relative (vfs.c strips the mount point), which is
 * exactly what fat_mkdir and friends want.
 */

static int fatfs_statfs(fs_volume_t *v, fs_statfs_t *out) {
    fat_volume_t *vol = (fat_volume_t *)v->body;
    uint32 free_clusters = 0;
    int rc;

    rc = fat_free_clusters(vol, &free_clusters);
    if (rc != FAT_OK) {
        return fat_to_errno(rc);
    }
    /* The CLUSTER is the allocation unit, not the sector. df wants the unit
     * the filesystem actually hands out, because that is what determines how
     * much a one-byte file costs - reporting sectors would say a volume has
     * four times the free space it can really allocate. */
    out->block_size  = (uint64)vol->sectors_per_cluster * vol->bytes_per_sector;
    out->blocks      = vol->total_clusters;
    out->blocks_free = free_clusters;
    /* 8.3 plus the dot. Not FS_NAME_MAX, which is the VFS buffer size and is
     * a fact about this kernel rather than about FAT16 - a caller sizing a
     * buffer from it would be right, and one deciding whether a name will fit
     * would be wrong. */
    out->name_max    = 12;
    return 0;
}

static int fatfs_create(fs_volume_t *v, const char *abs_path,
                        const struct cred *c, uint32 mode) {
    fat_volume_t *vol = (fat_volume_t *)v->body;

    (void)c;                             /* FAT records no owner */
    (void)mode;                          /* ...and no permission bits */
    return fat_to_errno(fat_create(vol, abs_path));
}

static int fatfs_truncate(fs_volume_t *v, fs_node_t *n, uint64 size) {
    fat_volume_t *vol = (fat_volume_t *)v->body;
    fat_entry_t  *ent = (fat_entry_t *)(void *)n->priv;
    int rc;

    /* The on-disk length is 32 bits, so a larger request cannot be recorded.
     * -EFBIG rather than a silent clamp: a caller that asked for 5GB and got
     * a 4GB file has been lied to in a way it cannot detect. */
    if (size > 0xFFFFFFFFULL) {
        return -27;                      /* -EFBIG */
    }
    rc = fat_truncate(vol, ent, (uint32)size);
    if (rc != FAT_OK) {
        return fat_to_errno(rc);
    }
    /* Back to the node, both fields - the same reason fatfs_write does it.
     * A truncate to zero frees the chain, so the first cluster (and with it
     * the inode number) really does change. */
    n->size = ent->size;
    n->ino  = ent->cluster;
    return 0;
}

static int fatfs_mkdir(fs_volume_t *v, const char *abs_path,
                       const struct cred *c, uint32 mode) {
    (void)c;                             /* FAT records no owner */
    (void)mode;                          /* ...and no permission bits */
    return fat_to_errno(fat_mkdir((fat_volume_t *)v->body, abs_path));
}

static int fatfs_rmdir(fs_volume_t *v, const char *abs_path) {
    return fat_to_errno(fat_rmdir((fat_volume_t *)v->body, abs_path));
}

static int fatfs_unlink(fs_volume_t *v, const char *abs_path) {
    return fat_to_errno(fat_unlink((fat_volume_t *)v->body, abs_path));
}

static int fatfs_rename(fs_volume_t *v, const char *old_path,
                        const char *new_path) {
    return fat_to_errno(fat_rename((fat_volume_t *)v->body, old_path,
                                   new_path));
}

/* Defined below, next to the slot table it retires into. */
static void fatfs_unmount(fs_volume_t *v);

static const fs_ops_t fat_ops = {
    .name    = "fat16",
    .lookup  = fatfs_lookup,
    .read    = fatfs_read,
    /* File CONTENT - extending a file and allocating clusters as it grows.
     * This was the last NULL slot; the four below are the NAMESPACE, which
     * landed first because rewriting a directory record and growing a cluster
     * chain are different problems. fs_writable now answers yes, and
     * access(W_OK) with it. */
    .write   = fatfs_write,
    .iterate = fatfs_iterate,
    .statfs   = fatfs_statfs,
    .create   = fatfs_create,
    .truncate = fatfs_truncate,
    .mkdir   = fatfs_mkdir,
    .rmdir   = fatfs_rmdir,
    .unlink  = fatfs_unlink,
    .rename  = fatfs_rename,
    .unmount = fatfs_unmount
};

fs_volume_t *fatfs_mount(device_t *dev) {
    int slot;
    int rc;

    /* A free slot first, a retired one only if there is none. Two passes
     * rather than one test, so that reuse of a retired slot is a deliberate
     * fallback rather than something that happens whenever a retired slot
     * happens to come first in the array. */
    for (slot = 0; slot < FATFS_MAX; slot++) {
        if (!fat_used[slot]) {
            break;
        }
    }
    if (slot == FATFS_MAX) {
        for (slot = 0; slot < FATFS_MAX; slot++) {
            if (fat_retired[slot]) {
                break;
            }
        }
    }
    if (slot == FATFS_MAX) {
        return NULL;
    }
    fat_retired[slot] = 0;

    rc = fat_mount(&fat_vols[slot], dev);
    if (rc != FAT_OK) {
        return NULL;
    }
    fat_fss[slot].ops        = &fat_ops;
    fat_fss[slot].body       = &fat_vols[slot];
    fat_fss[slot].block_size = fat_vols[slot].sectors_per_cluster *
                               fat_vols[slot].bytes_per_sector;
    fat_fss[slot].mounted    = 1;
    fat_used[slot]           = 1;
    return &fat_fss[slot];
}

/* fs_ops_t::unmount. See fs.h for the retire-rather-than-free rule and why it
 * is not simply `fat_used[slot] = 0`.
 *
 * What is actually released here is small - fat_volume_t is a static struct
 * and holds no allocation once fat_mount has returned - so this is mostly
 * bookkeeping. It exists anyway, because the slot leak it fixes is the same
 * one on both filesystems and a hook that only one of them implements is a
 * hook whose contract nobody can rely on. */
static void fatfs_unmount(fs_volume_t *v) {
    int i;

    for (i = 0; i < FATFS_MAX; i++) {
        if (fat_used[i] && &fat_fss[i] == v) {
            fat_vols[i].mounted = 0;
            fat_vols[i].dev     = NULL;
            fat_fss[i].body     = NULL;
            fat_retired[i]      = 1;
            return;
        }
    }
}

/* The prober. Identical body to fatfs_mount today, and it is still a separate
 * function: fat_mount distinguishes FAT_ERR_NOTFAT from FAT_ERR_IO, and the
 * day this reports "the disk is unreadable" differently from "this is not
 * FAT" - which is the difference between a broken stick and an ext2 one -
 * that logic goes here and not into the mount path. */
fs_volume_t *fatfs_probe(device_t *dev) {
    return fatfs_mount(dev);
}
