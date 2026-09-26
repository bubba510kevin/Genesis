#include "acl.h"
#include "device.h"
#include "fs.h"
#include "gnfs.h"
#include "gnfs_dev.h"
#include "gnfs_layout.h"
#include "kheap.h"
#include "kprintf.h"
#include "typesk.h"
#include "volume.h"

/* gnfs behind the filesystem vtable - the device-facing half. Everything
 * that touches a disk lives here; kernel/gnfs/gnfs_format.c and
 * kernel/gnfs/gnfs_object.c stay pure, the same split kernel/fs/acl.c and
 * kernel/fs/ntsec.c already draw between logic and I/O.
 *
 * --- what this mounts now -------------------------------------------------
 * A real, if small, filesystem: files and directories, created, written,
 * read, truncated, unlinked. Every block a write touches is a NEW block -
 * gnfs_cow_write_block never overwrites one already referenced by the
 * mounted root - which is what makes a retained old root record (a future
 * snapshot) a real, untouched copy of the graph as it stood, not something
 * this layer has to special-case later. See kernel/include/gnfs_layout.h's
 * own note on the one piece that is NOT yet solved: the allocator does not
 * yet know to refuse a block a retained snapshot still needs, because
 * nothing retains one yet.
 *
 * Bounds stated rather than discovered: GNFS_MAX_OBJECTS objects, and a
 * file or directory's data capped at GNFS_OBJ_DIRECT * GNFS_BLOCK_SIZE bytes
 * (no indirect blocks in this foundation). Both refuse cleanly (-ENOSPC,
 * -EFBIG) rather than doing something surprising at the edge.
 */

#define GNFS_MAX 4

#define ENOENT       2
#define EIO          5
#define ENOMEM      12
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define EFBIG       27
#define ENOSPC      28
#define ENAMETOOLONG 36
#define ENOTEMPTY   39

typedef struct gnfs_mount {
    device_t   *dev;
    gnfs_root_t root;         /* the current (highest valid txg) root record */
    uint8      *bitmap;        /* malloc'd: root.bitmap_blocks * BLOCK_SIZE */
    uint8      *obj_table;     /* malloc'd: root.obj_table_blocks * BLOCK_SIZE */
} gnfs_mount_t;

static fs_volume_t   gnfs_fs_slots[GNFS_MAX];
static gnfs_mount_t  gnfs_mounts[GNFS_MAX];
static int           gnfs_used[GNFS_MAX];
static int           gnfs_retired[GNFS_MAX];

/* --- fs_node_t::priv, which carries just an object number -------------------
 *
 * Packed and unpacked byte by byte rather than cast through a uint64*,
 * because fs_node_t::priv is a plain uint8[FS_PRIVATE_MAX] with no alignment
 * guarantee for anything wider - fatfs.c's own node_entry() can cast
 * directly only because fat_entry_t happens to need no stricter alignment
 * than a byte array already provides. Not worth relying on the same
 * coincidence here. */
static uint64 node_objnum(const fs_node_t *n) {
    const uint8 *p = n->priv;
    uint64 v = 0;
    int i;

    for (i = 0; i < 8; i++) {
        v |= ((uint64)p[i]) << (8 * i);
    }
    return v;
}

static void node_set_objnum(fs_node_t *n, uint64 objnum) {
    uint8 *p = n->priv;
    int i;

    for (i = 0; i < 8; i++) {
        p[i] = (uint8)(objnum >> (8 * i));
    }
}

/* --- mounting --------------------------------------------------------------- */

static int gnfs_read_ring_slot(device_t *dev, int slot, gnfs_root_t *out) {
    int64 got = dev_read(dev, (uint64)slot * GNFS_BLOCK_SIZE, out,
                        sizeof(*out));
    if (got != (int64)sizeof(*out)) {
        return -EIO;           /* a short read reads as "not a root" too,
                               * since the caller only acts on a valid one */
    }
    return 0;
}

/* Scan every ring slot and keep the valid one with the highest txg. Exactly
 * ZFS's own uberblock-mount rule, reused rather than reinvented: a torn
 * write only ever lands in the slot the CURRENT commit was advancing into,
 * so the highest-txg valid slot is always the last complete commit. */
static int gnfs_find_current(device_t *dev, gnfs_root_t *best) {
    int slot;
    int found = 0;
    gnfs_root_t r;

    for (slot = 0; slot < (int)GNFS_RING_SLOTS; slot++) {
        if (gnfs_read_ring_slot(dev, slot, &r) != 0) {
            continue;
        }
        if (!gnfs_root_valid(&r)) {
            continue;
        }
        if (!found || r.txg > best->txg) {
            *best = r;
            found = 1;
        }
    }
    return found ? 0 : -19;   /* -ENODEV: no valid root anywhere in the ring */
}

/* --- committing --------------------------------------------------------------
 *
 * Advances the whole volume by one txg: the (whole, bounded) bitmap and
 * object table each go to whichever of their two fixed regions the CURRENT
 * root does not point at, then a new root record naming both is written into
 * the ring. See kernel/include/gnfs_layout.h for why a torn write here still
 * leaves a mountable volume - the argument is the same one the bitmap-only
 * version of this function already made, now covering both structures it
 * ping-pongs. tests/host/gnfs_test.c exercises this directly against a fake
 * device_t, including slot reuse and a torn-write simulation; gnfs_commit
 * below is what calls it for a live mount. */
int gnfs_txg_commit(device_t *dev, gnfs_root_t *cur, const uint8 *bitmap,
                    uint64 bitmap_bytes_used, const uint8 *obj_table,
                    uint64 obj_table_bytes_used) {
    uint64 next_txg = cur->txg + 1;
    uint64 bmp_a = gnfs_bitmap_region_a(cur);
    uint64 bmp_b = gnfs_bitmap_region_b(cur);
    uint64 obt_a = gnfs_obj_table_region_a(cur);
    uint64 obt_b = gnfs_obj_table_region_b(cur);
    uint64 next_bitmap_block = (cur->bitmap_block == bmp_a) ? bmp_b : bmp_a;
    uint64 next_obj_table_block = (cur->obj_table_block == obt_a)
        ? obt_b : obt_a;
    gnfs_root_t next;
    int64 rc;

    if (bitmap_bytes_used > cur->bitmap_blocks * (uint64)GNFS_BLOCK_SIZE) {
        return -EINVAL;
    }
    if (obj_table_bytes_used >
        cur->obj_table_blocks * (uint64)GNFS_BLOCK_SIZE) {
        return -EINVAL;
    }

    rc = dev_write(dev, next_bitmap_block * GNFS_BLOCK_SIZE, bitmap,
                  bitmap_bytes_used);
    if (rc != (int64)bitmap_bytes_used) {
        return (rc < 0) ? (int)rc : -EIO;
    }

    rc = dev_write(dev, next_obj_table_block * GNFS_BLOCK_SIZE, obj_table,
                  obj_table_bytes_used);
    if (rc != (int64)obj_table_bytes_used) {
        return (rc < 0) ? (int)rc : -EIO;
    }

    next = *cur;
    next.txg             = next_txg;
    next.bitmap_block    = next_bitmap_block;
    next.obj_table_block = next_obj_table_block;
    gnfs_root_seal(&next);

    rc = dev_write(dev, (next_txg % GNFS_RING_SLOTS) * GNFS_BLOCK_SIZE,
                  &next, sizeof(next));
    if (rc != (int64)sizeof(next)) {
        return (rc < 0) ? (int)rc : -EIO;
    }

    *cur = next;
    return 0;
}

static int gnfs_commit(gnfs_mount_t *m) {
    return gnfs_txg_commit(m->dev, &m->root, m->bitmap,
                          m->root.bitmap_blocks * (uint64)GNFS_BLOCK_SIZE,
                          m->obj_table,
                          m->root.obj_table_blocks * (uint64)GNFS_BLOCK_SIZE);
}

/* --- block allocation, translating between the bitmap's RELATIVE indices
 * and the ABSOLUTE numbers everything else here uses - see gnfs_onode_t's
 * own note on this in kernel/include/gnfs_layout.h ------------------------- */

/* Allocate ONE fresh block, write `content` into it, and hand back its
 * ABSOLUTE number. Never overwrites a block already referenced by anything -
 * every caller that is replacing a block's content calls this for the NEW
 * copy and frees the old one itself, afterwards, only once the new one is
 * durably in the in-memory table (about to be committed) - see gnfs_vfs.c's
 * own top comment on why that order is what makes this real COW. */
static int gnfs_cow_write_block(gnfs_mount_t *m, const uint8 *content,
                                uint64 *abs_out) {
    uint64 rel;
    uint64 abs;
    int rc;
    int64 wrc;

    rc = gnfs_alloc_blocks(m->bitmap, m->root.total_blocks, 1, &rel);
    if (rc != 0) {
        return rc;
    }
    abs = gnfs_alloc_region_start(&m->root) + rel;
    wrc = dev_write(m->dev, abs * GNFS_BLOCK_SIZE, content, GNFS_BLOCK_SIZE);
    if (wrc != (int64)GNFS_BLOCK_SIZE) {
        /* The allocation never became durable - nothing yet points at this
         * block, so undoing it in the in-memory bitmap is safe and correct,
         * not merely convenient. */
        gnfs_free_blocks(m->bitmap, m->root.total_blocks, rel, 1);
        return (wrc < 0) ? (int)wrc : -EIO;
    }
    *abs_out = abs;
    return 0;
}

static void gnfs_cow_free_block(gnfs_mount_t *m, uint64 abs_block) {
    uint64 start;

    if (abs_block == 0) {
        return;
    }
    start = gnfs_alloc_region_start(&m->root);
    gnfs_free_blocks(m->bitmap, m->root.total_blocks, abs_block - start, 1);
}

/* --- directory data, one block per directory in this foundation ------------ */

static int gnfs_dir_block_read(gnfs_mount_t *m, uint64 dir_objnum,
                               uint8 *block) {
    gnfs_onode_t *o = gnfs_onode_at(m->obj_table, dir_objnum);

    if (o == NULL || o->nblocks == 0) {
        return -EIO;           /* a directory object with no block is a
                               * format-time bug, not a normal condition -
                               * every directory this layer creates gets one
                               * before it is ever reachable */
    }
    if (dev_read(m->dev, o->direct[0] * GNFS_BLOCK_SIZE, block,
                GNFS_BLOCK_SIZE) != (int64)GNFS_BLOCK_SIZE) {
        return -EIO;
    }
    return 0;
}

/* Read-modify-write a directory's single data block through the COW path:
 * a new block is written with the updated content, the directory's onode is
 * repointed at it, and only then is the old block freed - matching this
 * file's own top-comment argument for why data never changes in place. */
static int gnfs_dir_mutate(gnfs_mount_t *m, uint64 dir_objnum,
                          const char *name, uint64 name_len,
                          int add, uint64 add_objnum, int add_is_dir) {
    gnfs_onode_t *dir_onode = gnfs_onode_at(m->obj_table, dir_objnum);
    uint8 block[GNFS_BLOCK_SIZE];
    uint64 old_block;
    uint64 new_block;
    int rc;

    if (dir_onode == NULL) {
        return -EIO;
    }
    old_block = dir_onode->direct[0];
    rc = gnfs_dir_block_read(m, dir_objnum, block);
    if (rc != 0) {
        return rc;
    }

    if (add) {
        rc = gnfs_dir_add(block, name, name_len, add_objnum, add_is_dir);
    } else {
        rc = gnfs_dir_remove(block, name, name_len);
    }
    if (rc != 0) {
        return rc;
    }

    rc = gnfs_cow_write_block(m, block, &new_block);
    if (rc != 0) {
        return rc;
    }
    dir_onode->direct[0] = new_block;
    gnfs_cow_free_block(m, old_block);
    return 0;
}

/* --- path resolution ---------------------------------------------------------
 *
 * abs_path is already absolute and normalized - fs_ops_t::lookup's own
 * contract (kernel/include/fs.h) - so this walks component by component with
 * no ".."/"."/double-slash handling of its own. "/" itself resolves with the
 * loop body never running, which is what makes it fall out of this walk
 * rather than needing its own special case. */
static int gnfs_resolve(gnfs_mount_t *m, const char *abs_path,
                        uint64 *objnum_out, int *is_dir_out) {
    uint64 cur = m->root.root_dir_objnum;
    int cur_is_dir = 1;
    uint64 i;

    if (abs_path[0] != '/') {
        return -EINVAL;
    }
    i = 1;
    while (abs_path[i] != '\0') {
        uint64 start = i;
        uint64 complen;
        gnfs_onode_t *dir_onode;
        uint8 block[GNFS_BLOCK_SIZE];
        uint64 next_objnum;
        int next_is_dir;
        int rc;

        while (abs_path[i] != '\0' && abs_path[i] != '/') {
            i++;
        }
        complen = i - start;

        if (!cur_is_dir) {
            return -ENOTDIR;
        }
        dir_onode = gnfs_onode_at(m->obj_table, cur);
        if (dir_onode == NULL) {
            return -ENOENT;
        }
        rc = gnfs_dir_block_read(m, cur, block);
        if (rc != 0) {
            return rc;
        }
        rc = gnfs_dir_find(block, abs_path + start, complen, &next_objnum,
                          &next_is_dir);
        if (rc != 0) {
            return rc;
        }
        cur = next_objnum;
        cur_is_dir = next_is_dir;
        if (abs_path[i] == '/') {
            i++;
        }
    }
    *objnum_out = cur;
    *is_dir_out = cur_is_dir;
    return 0;
}

/* Splits an absolute, normalized path into the parent directory's path and
 * the leaf name, both as views into `abs_path` - no copy here. The root's
 * own parent is "/" itself. */
static void split_path(const char *abs_path, uint64 *parent_len,
                       const char **leaf, uint64 *leaf_len) {
    uint64 len = 0;
    int64 last_slash = -1;
    uint64 i;

    while (abs_path[len] != '\0') {
        len++;
    }
    for (i = 0; i < len; i++) {
        if (abs_path[i] == '/') {
            last_slash = (int64)i;
        }
    }
    *parent_len = (last_slash <= 0) ? 1 : (uint64)last_slash;
    *leaf     = abs_path + last_slash + 1;
    *leaf_len = len - (uint64)(last_slash + 1);
}

#define GNFS_PATH_SCRATCH 512u

static int gnfs_resolve_parent(gnfs_mount_t *m, const char *abs_path,
                               uint64 *parent_objnum, const char **leaf,
                               uint64 *leaf_len) {
    uint64 parent_len;
    char parent_buf[GNFS_PATH_SCRATCH];
    int parent_is_dir;
    uint64 k;
    int rc;

    split_path(abs_path, &parent_len, leaf, leaf_len);
    if (parent_len >= sizeof(parent_buf)) {
        return -ENAMETOOLONG;
    }
    if (*leaf_len == 0 || *leaf_len > GNFS_NAME_MAX) {
        return -EINVAL;
    }
    for (k = 0; k < parent_len; k++) {
        parent_buf[k] = abs_path[k];
    }
    parent_buf[parent_len] = '\0';

    rc = gnfs_resolve(m, parent_buf, parent_objnum, &parent_is_dir);
    if (rc != 0) {
        return rc;
    }
    if (!parent_is_dir) {
        return -ENOTDIR;
    }
    return 0;
}

/* --- file data, direct blocks only in this foundation ----------------------- */

static int64 gnfs_read_data(gnfs_mount_t *m, const gnfs_onode_t *o,
                           uint64 offset, void *buf, uint64 max) {
    uint8 *dst = (uint8 *)buf;
    uint64 avail, want, done = 0;

    if (offset >= o->size) {
        return 0;              /* past EOF, not an error */
    }
    avail = o->size - offset;
    want = (max < avail) ? max : avail;

    while (done < want) {
        uint64 idx = (offset + done) / GNFS_BLOCK_SIZE;
        uint64 in_off = (offset + done) % GNFS_BLOCK_SIZE;
        uint64 chunk = GNFS_BLOCK_SIZE - in_off;
        uint64 z;

        if (chunk > want - done) {
            chunk = want - done;
        }
        if (idx >= GNFS_OBJ_DIRECT || o->direct[idx] == 0) {
            /* A hole reads as zero - the same rule every real filesystem's
             * sparse files follow, and the one FAT's own write path had to
             * learn the hard way (see kernel/fs/fat.c and its host test) not
             * to get backwards. */
            for (z = 0; z < chunk; z++) {
                dst[done + z] = 0;
            }
        } else {
            uint8 block[GNFS_BLOCK_SIZE];
            if (dev_read(m->dev, o->direct[idx] * GNFS_BLOCK_SIZE, block,
                        GNFS_BLOCK_SIZE) != (int64)GNFS_BLOCK_SIZE) {
                return -EIO;
            }
            for (z = 0; z < chunk; z++) {
                dst[done + z] = block[in_off + z];
            }
        }
        done += chunk;
    }
    return (int64)done;
}

/* Writes [offset, offset+len) into `o`'s direct blocks, COW throughout: every
 * touched block - whether newly allocated or replacing one that already held
 * data - is a freshly allocated block, never an in-place rewrite. A block
 * only partly covered by the write is read (or zero-initialised, if it did
 * not exist before) so the untouched part of it survives the copy. */
static int gnfs_write_data(gnfs_mount_t *m, gnfs_onode_t *o, uint64 offset,
                          const void *buf, uint64 len) {
    const uint8 *src = (const uint8 *)buf;
    uint64 end = offset + len;
    uint64 first_idx, last_idx, idx;

    if (len == 0) {
        return 0;
    }
    if (end > (uint64)GNFS_OBJ_DIRECT * GNFS_BLOCK_SIZE) {
        return -EFBIG;
    }
    first_idx = offset / GNFS_BLOCK_SIZE;
    last_idx  = (end - 1) / GNFS_BLOCK_SIZE;

    for (idx = first_idx; idx <= last_idx; idx++) {
        uint8 block[GNFS_BLOCK_SIZE];
        uint64 old_block = o->direct[idx];
        uint64 blk_start = idx * GNFS_BLOCK_SIZE;
        uint64 copy_start = (offset > blk_start) ? offset - blk_start : 0;
        uint64 copy_end = (end < blk_start + GNFS_BLOCK_SIZE)
            ? end - blk_start : GNFS_BLOCK_SIZE;
        uint64 src_base = (blk_start + copy_start) - offset;
        uint64 new_block;
        uint64 k;
        int rc;

        if (old_block != 0) {
            if (dev_read(m->dev, old_block * GNFS_BLOCK_SIZE, block,
                        GNFS_BLOCK_SIZE) != (int64)GNFS_BLOCK_SIZE) {
                return -EIO;
            }
        } else {
            for (k = 0; k < GNFS_BLOCK_SIZE; k++) {
                block[k] = 0;
            }
        }
        for (k = copy_start; k < copy_end; k++) {
            block[k] = src[src_base + (k - copy_start)];
        }

        rc = gnfs_cow_write_block(m, block, &new_block);
        if (rc != 0) {
            return rc;
        }
        o->direct[idx] = new_block;
        if (idx + 1 > o->nblocks) {
            o->nblocks = idx + 1;
        }
        gnfs_cow_free_block(m, old_block);
    }
    if (end > o->size) {
        o->size = end;
    }
    return 0;
}

/* --- fs_ops_t ----------------------------------------------------------------- */

static int gnfs_op_lookup(fs_volume_t *v, const char *abs_path,
                          fs_node_t *out) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint64 objnum;
    int is_dir;
    gnfs_onode_t *o;
    int rc;

    rc = gnfs_resolve(m, abs_path, &objnum, &is_dir);
    if (rc != 0) {
        return rc;
    }
    o = gnfs_onode_at(m->obj_table, objnum);
    if (o == NULL) {
        return -EIO;
    }
    out->size   = o->size;
    out->ino    = objnum;
    out->is_dir = is_dir;
    out->mode   = o->mode;
    out->uid    = o->uid;
    out->gid    = o->gid;
    node_set_objnum(out, objnum);
    return 0;
}

static int64 gnfs_op_read(fs_volume_t *v, const fs_node_t *n, uint64 offset,
                          void *buf, uint64 max) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o = gnfs_onode_at(m->obj_table, node_objnum(n));

    if (o == NULL) {
        return -EIO;
    }
    if (n->is_dir) {
        return -EISDIR;
    }
    return gnfs_read_data(m, o, offset, buf, max);
}

static int64 gnfs_op_write(fs_volume_t *v, fs_node_t *n, uint64 offset,
                          const void *buf, uint64 max) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o = gnfs_onode_at(m->obj_table, node_objnum(n));
    int rc;

    if (o == NULL) {
        return -EIO;
    }
    if (n->is_dir) {
        return -EISDIR;
    }
    rc = gnfs_write_data(m, o, offset, buf, max);
    if (rc != 0) {
        return rc;
    }
    rc = gnfs_commit(m);
    if (rc != 0) {
        return rc;
    }
    n->size = o->size;
    return (int64)max;
}

typedef struct {
    fs_dir_cb cb;
    void     *ctx;
} gnfs_iter_adapt_t;

static int gnfs_iter_adapt_cb(const char *name, uint64 name_len,
                              uint64 objnum, int is_dir, void *ctx) {
    gnfs_iter_adapt_t *a = (gnfs_iter_adapt_t *)ctx;
    fs_dirent_t d;
    uint64 i;
    uint64 cap = (name_len < FS_NAME_MAX - 1) ? name_len : FS_NAME_MAX - 1;

    for (i = 0; i < cap; i++) {
        d.name[i] = name[i];
    }
    d.name[cap] = '\0';
    d.ino     = objnum;
    d.is_dir  = is_dir;
    return a->cb(&d, a->ctx);
}

static int gnfs_op_iterate(fs_volume_t *v, const fs_node_t *dir,
                           fs_dir_cb cb, void *ctx) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint8 block[GNFS_BLOCK_SIZE];
    gnfs_iter_adapt_t adapt;
    int rc = gnfs_dir_block_read(m, node_objnum(dir), block);

    if (rc != 0) {
        return rc;
    }
    adapt.cb  = cb;
    adapt.ctx = ctx;
    return gnfs_dir_iterate(block, gnfs_iter_adapt_cb, &adapt);
}

static int gnfs_op_statfs(fs_volume_t *v, fs_statfs_t *out) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint64 free_blocks = 0;
    uint64 i;

    for (i = 0; i < m->root.total_blocks; i++) {
        if (!gnfs_bitmap_test(m->bitmap, i)) {
            free_blocks++;
        }
    }
    out->block_size  = GNFS_BLOCK_SIZE;
    out->blocks      = m->root.total_blocks;
    out->blocks_free = free_blocks;
    out->name_max    = GNFS_NAME_MAX;
    return 0;
}

/* --- ACL storage: one serialised acl_t per object, in its own COW block ---
 *
 * kernel/include/acl.h is the type; a build-time check that it fits is
 * worth having for the same reason gnfs_root_t and gnfs_onode_t each get
 * one - an ACL is written and read as raw bytes, and a struct that grew past
 * one block would silently truncate rather than fail to build. */
typedef char gnfs_acl_fits_block[(sizeof(acl_t) <= GNFS_BLOCK_SIZE) ? 1 : -1];

/* Just the STORED ACL, or -ENOENT if there is none - the exact shape
 * fs_ops_t::getacl promises, so fs_getacl's own projection fallback
 * (kernel/fs/vfs.c) applies unchanged and this file states the rule only
 * once. */
static int gnfs_read_stored_acl(gnfs_mount_t *m, const gnfs_onode_t *o,
                                acl_t *out) {
    uint8 block[GNFS_BLOCK_SIZE];
    const acl_t *stored;

    if (o->acl_block == 0) {
        return -ENOENT;
    }
    if (dev_read(m->dev, o->acl_block * GNFS_BLOCK_SIZE, block,
                GNFS_BLOCK_SIZE) != (int64)GNFS_BLOCK_SIZE) {
        return -EIO;
    }
    stored = (const acl_t *)(const void *)block;
    if (stored->count > ACL_ACE_MAX) {
        return -EIO;       /* a corrupt record - refused, not read past its
                           * own ace[] array */
    }
    *out = *stored;
    return 0;
}

/* The EFFECTIVE ACL: stored if there is one, projected from the mode
 * otherwise. Used internally by create/mkdir below, which need a PARENT's
 * real, current view before there is an fs_node_t for it to call fs_getacl
 * through - anything with a node uses fs_getacl instead, so the projection
 * rule itself still lives in exactly the one place vfs.c states it. */
static int gnfs_effective_acl(gnfs_mount_t *m, const gnfs_onode_t *o,
                              acl_t *out) {
    int rc = gnfs_read_stored_acl(m, o, out);

    if (rc == -ENOENT) {
        acl_from_mode(o->mode, o->uid, o->gid, out);
        return 0;
    }
    return rc;
}

/* Write `a` into a fresh COW block, point `o` at it, and free whatever it
 * pointed at before - the same discipline every other mutation here follows.
 * Also refreshes `o->mode` from the new ACL (acl_to_mode), so a caller that
 * stats the object without going through getacl still sees a mode that
 * agrees with what was just set, rather than a stale projection of the old
 * one. The special bits are `special` when it is not FS_SPECIAL_KEEP, and
 * otherwise carried over - acl_to_mode keeps them from its type_bits
 * argument, since no ACE can say anything about them. */
static int gnfs_store_acl(gnfs_mount_t *m, gnfs_onode_t *o, const acl_t *a,
                          uint32 special) {
    uint8 block[GNFS_BLOCK_SIZE];
    uint64 old_block = o->acl_block;
    uint64 new_block;
    uint64 i;
    int rc;

    for (i = 0; i < sizeof(block); i++) {
        block[i] = 0;
    }
    *(acl_t *)(void *)block = *a;
    rc = gnfs_cow_write_block(m, block, &new_block);
    if (rc != 0) {
        return rc;
    }
    o->acl_block = new_block;
    o->mode = acl_to_mode(a, o->mode);
    if (special != FS_SPECIAL_KEEP) {
        o->mode = (o->mode & ~(S_ISUID | S_ISGID | S_ISVTX)) |
                  (special & (S_ISUID | S_ISGID | S_ISVTX));
    }
    gnfs_cow_free_block(m, old_block);
    return 0;
}

/* Give a newly allocated object its inherited ACL, if its parent's grants
 * anything worth storing - see acl_inherit's own header comment on why a
 * purely-trivial result is left unstored (acl_block stays 0, projecting from
 * the mode exactly as if this were never called). Failures here are
 * swallowed rather than failing the create/mkdir that is calling this: a
 * parent onode this cannot resolve, or a read that fails, means the child
 * simply gets no inherited entries - a plain mode-derived ACL - which is a
 * strictly narrower grant than intended, never a wider one, and is the safe
 * direction for this to be wrong in. */
static void gnfs_apply_inheritance(gnfs_mount_t *m, uint64 parent_objnum,
                                   gnfs_onode_t *child, int child_is_dir) {
    gnfs_onode_t *parent = gnfs_onode_at(m->obj_table, parent_objnum);
    acl_t parent_acl;
    acl_t child_acl;

    if (parent == NULL) {
        return;
    }
    if (gnfs_effective_acl(m, parent, &parent_acl) != 0) {
        return;
    }
    acl_inherit(&parent_acl, child_is_dir, child->mode, child->uid,
               child->gid, &child_acl);
    if (child_acl.trivial) {
        return;             /* nothing inherited - acl_block stays 0 */
    }
    gnfs_store_acl(m, child, &child_acl, FS_SPECIAL_KEEP);
}

static int gnfs_op_getacl(fs_volume_t *v, const fs_node_t *n,
                          struct acl *out) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o = gnfs_onode_at(m->obj_table, node_objnum(n));

    if (o == NULL) {
        return -EIO;
    }
    return gnfs_read_stored_acl(m, o, (acl_t *)out);
}

static int gnfs_op_setacl(fs_volume_t *v, fs_node_t *n,
                          const struct acl *a, uint32 special) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o = gnfs_onode_at(m->obj_table, node_objnum(n));
    const acl_t *acl = (const acl_t *)a;
    int rc;

    if (o == NULL) {
        return -EIO;
    }
    if (acl->count > ACL_ACE_MAX) {
        return -EINVAL;
    }
    rc = gnfs_store_acl(m, o, acl, special);
    if (rc != 0) {
        return rc;
    }
    rc = gnfs_commit(m);
    if (rc != 0) {
        return rc;
    }
    n->mode = o->mode;
    return 0;
}

/* Two copies of the owner/group pair can exist - the onode's, and the one
 * inside a stored ACL (acl_t::owner/group, which is what acl_access resolves
 * owner@ and group@ against). Both move, the ACL's first: if that COW write
 * fails, nothing has changed yet, and the onode never names an owner its own
 * stored ACL disagrees with. */
static int gnfs_op_setowner(fs_volume_t *v, fs_node_t *n, uint32 uid,
                            uint32 gid) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o = gnfs_onode_at(m->obj_table, node_objnum(n));
    acl_t stored;
    int rc;

    if (o == NULL) {
        return -EIO;
    }
    rc = gnfs_read_stored_acl(m, o, &stored);
    if (rc == 0) {
        stored.owner = uid;
        stored.group = gid;
        rc = gnfs_store_acl(m, o, &stored, FS_SPECIAL_KEEP);
        if (rc != 0) {
            return rc;
        }
    } else if (rc != -ENOENT) {
        return rc;
    }
    o->uid = uid;
    o->gid = gid;
    rc = gnfs_commit(m);
    if (rc != 0) {
        return rc;
    }
    n->uid = uid;
    n->gid = gid;
    return 0;
}

/* Who owns a new object, and whether it is born setgid.
 *
 * The creator's euid, always. Its group is the creator's egid - unless the
 * parent directory is setgid, in which case it is the PARENT's group, and a
 * new DIRECTORY is born setgid itself so the rule keeps applying below it.
 * That is System V's rule and Linux's; BSD's "always the parent's group" is
 * the other common one. The setgid form is what lets a shared project
 * directory work without every member chgrp'ing every file they make.
 *
 * NULL `c` is the kernel creating something for itself, which is root's -
 * but a setgid parent still decides the group, for the same reason. */
static void gnfs_creator_ids(gnfs_mount_t *m, uint64 parent_objnum,
                             const struct cred *c, int is_dir, uint32 *mode,
                             uint32 *uid, uint32 *gid) {
    const cred_t *cr = (const cred_t *)c;
    gnfs_onode_t *parent = gnfs_onode_at(m->obj_table, parent_objnum);

    *uid = (cr != NULL) ? cr->euid : 0;
    *gid = (cr != NULL) ? cr->egid : 0;
    if (parent != NULL && (parent->mode & S_ISGID)) {
        *gid = parent->gid;
        if (is_dir) {
            *mode |= S_ISGID;
        }
    }
}

static int gnfs_op_create(fs_volume_t *v, const char *abs_path,
                          const struct cred *c) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint64 parent_objnum, leaf_len, new_objnum;
    const char *leaf;
    gnfs_onode_t *new_onode;
    uint32 mode = 0100644u;              /* S_IFREG | rw-r--r-- */
    uint32 uid, gid;
    int rc;

    rc = gnfs_resolve_parent(m, abs_path, &parent_objnum, &leaf, &leaf_len);
    if (rc != 0) {
        return rc;
    }
    rc = gnfs_onode_alloc(&m->root, &new_objnum);
    if (rc != 0) {
        return rc;
    }
    new_onode = gnfs_onode_at(m->obj_table, new_objnum);
    gnfs_creator_ids(m, parent_objnum, c, 0, &mode, &uid, &gid);
    gnfs_onode_init(new_onode, mode, uid, gid);
    gnfs_apply_inheritance(m, parent_objnum, new_onode, 0);

    rc = gnfs_dir_mutate(m, parent_objnum, leaf, leaf_len, 1, new_objnum, 0);
    if (rc != 0) {
        /* The onode slot leaks - objects are never reused in this
         * foundation regardless (see gnfs_onode_alloc), so a failed create
         * costs one object number rather than introducing a second, harder
         * failure mode (a slot in use by nothing). */
        return rc;
    }
    return gnfs_commit(m);
}

static int gnfs_op_mkdir(fs_volume_t *v, const char *abs_path,
                         const struct cred *c) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint64 parent_objnum, leaf_len, new_objnum, new_block;
    const char *leaf;
    uint8 block[GNFS_BLOCK_SIZE];
    gnfs_onode_t *new_onode;
    uint32 mode = 0040755u;              /* S_IFDIR | rwxr-xr-x */
    uint32 uid, gid;
    int rc;

    rc = gnfs_resolve_parent(m, abs_path, &parent_objnum, &leaf, &leaf_len);
    if (rc != 0) {
        return rc;
    }
    rc = gnfs_onode_alloc(&m->root, &new_objnum);
    if (rc != 0) {
        return rc;
    }
    gnfs_dir_init_block(block);
    rc = gnfs_cow_write_block(m, block, &new_block);
    if (rc != 0) {
        return rc;
    }
    new_onode = gnfs_onode_at(m->obj_table, new_objnum);
    gnfs_creator_ids(m, parent_objnum, c, 1, &mode, &uid, &gid);
    gnfs_onode_init(new_onode, mode, uid, gid);
    new_onode->direct[0] = new_block;
    new_onode->nblocks   = 1;
    new_onode->size      = GNFS_BLOCK_SIZE;
    gnfs_apply_inheritance(m, parent_objnum, new_onode, 1);

    rc = gnfs_dir_mutate(m, parent_objnum, leaf, leaf_len, 1, new_objnum, 1);
    if (rc != 0) {
        gnfs_cow_free_block(m, new_block);
        return rc;
    }
    return gnfs_commit(m);
}

/* Shared by unlink and rmdir: resolve the parent and the target, with the
 * caller deciding what "the wrong type" means for it (unlink on a directory
 * is -EISDIR; rmdir on a file is -ENOTDIR). */
static int gnfs_resolve_for_removal(gnfs_mount_t *m, const char *abs_path,
                                    uint64 *parent_objnum, const char **leaf,
                                    uint64 *leaf_len, uint64 *target_objnum,
                                    int *target_is_dir) {
    uint8 block[GNFS_BLOCK_SIZE];
    int rc;

    rc = gnfs_resolve_parent(m, abs_path, parent_objnum, leaf, leaf_len);
    if (rc != 0) {
        return rc;
    }
    rc = gnfs_dir_block_read(m, *parent_objnum, block);
    if (rc != 0) {
        return rc;
    }
    return gnfs_dir_find(block, *leaf, *leaf_len, target_objnum,
                         target_is_dir);
}

static int gnfs_op_unlink(fs_volume_t *v, const char *abs_path) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint64 parent_objnum, leaf_len, target_objnum;
    const char *leaf;
    int target_is_dir;
    gnfs_onode_t *target;
    uint64 i;
    int rc;

    rc = gnfs_resolve_for_removal(m, abs_path, &parent_objnum, &leaf,
                                 &leaf_len, &target_objnum, &target_is_dir);
    if (rc != 0) {
        return rc;
    }
    if (target_is_dir) {
        return -EISDIR;
    }
    target = gnfs_onode_at(m->obj_table, target_objnum);
    if (target == NULL) {
        return -EIO;
    }

    rc = gnfs_dir_mutate(m, parent_objnum, leaf, leaf_len, 0, 0, 0);
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < target->nblocks; i++) {
        gnfs_cow_free_block(m, target->direct[i]);
        target->direct[i] = 0;
    }
    target->nblocks = 0;
    target->size    = 0;
    /* The onode SLOT is not freed - see gnfs_root_t::next_objnum's own
     * comment on why object numbers are never reused in this foundation. */
    return gnfs_commit(m);
}

static int rmdir_has_entry_cb(const char *name, uint64 name_len,
                              uint64 objnum, int is_dir, void *ctx) {
    (void)name; (void)name_len; (void)objnum; (void)is_dir;
    *(int *)ctx = 1;
    return 1;                  /* stop at the first entry found */
}

static int gnfs_op_rmdir(fs_volume_t *v, const char *abs_path) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint64 parent_objnum, leaf_len, target_objnum;
    const char *leaf;
    int target_is_dir;
    gnfs_onode_t *target;
    uint8 block[GNFS_BLOCK_SIZE];
    int has_entry = 0;
    uint64 i;
    int rc;

    rc = gnfs_resolve_for_removal(m, abs_path, &parent_objnum, &leaf,
                                 &leaf_len, &target_objnum, &target_is_dir);
    if (rc != 0) {
        return rc;
    }
    if (!target_is_dir) {
        return -ENOTDIR;
    }
    if (target_objnum == m->root.root_dir_objnum) {
        return -EINVAL;        /* the root cannot be removed */
    }
    rc = gnfs_dir_block_read(m, target_objnum, block);
    if (rc != 0) {
        return rc;
    }
    gnfs_dir_iterate(block, rmdir_has_entry_cb, &has_entry);
    if (has_entry) {
        return -ENOTEMPTY;
    }

    target = gnfs_onode_at(m->obj_table, target_objnum);
    if (target == NULL) {
        return -EIO;
    }
    rc = gnfs_dir_mutate(m, parent_objnum, leaf, leaf_len, 0, 0, 0);
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < target->nblocks; i++) {
        gnfs_cow_free_block(m, target->direct[i]);
        target->direct[i] = 0;
    }
    target->nblocks = 0;
    target->size    = 0;
    return gnfs_commit(m);
}

static int gnfs_op_truncate(fs_volume_t *v, fs_node_t *n, uint64 size) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o = gnfs_onode_at(m->obj_table, node_objnum(n));
    int rc;

    if (o == NULL) {
        return -EIO;
    }
    if (size > (uint64)GNFS_OBJ_DIRECT * GNFS_BLOCK_SIZE) {
        return -EFBIG;
    }

    if (size < o->size) {
        uint64 keep_blocks = (size + GNFS_BLOCK_SIZE - 1) / GNFS_BLOCK_SIZE;
        uint64 i;

        for (i = keep_blocks; i < o->nblocks; i++) {
            gnfs_cow_free_block(m, o->direct[i]);
            o->direct[i] = 0;
        }
        o->nblocks = keep_blocks;
        o->size    = size;
        /* The tail of the new last kept block, if any, already reads as
         * zero past `size` - gnfs_write_data never leaves anything else
         * there, and truncating never rewrites a block it keeps. */
    } else if (size > o->size) {
        uint64 new_last_idx = (size - 1) / GNFS_BLOCK_SIZE;
        uint64 idx;

        for (idx = o->nblocks; idx <= new_last_idx; idx++) {
            uint8 zero[GNFS_BLOCK_SIZE];
            uint64 new_block;
            uint64 z;

            for (z = 0; z < GNFS_BLOCK_SIZE; z++) {
                zero[z] = 0;
            }
            rc = gnfs_cow_write_block(m, zero, &new_block);
            if (rc != 0) {
                return rc;
            }
            o->direct[idx] = new_block;
        }
        o->nblocks = new_last_idx + 1;
        o->size    = size;
    }

    rc = gnfs_commit(m);
    if (rc != 0) {
        return rc;
    }
    n->size = o->size;
    return 0;
}

static void gnfs_op_unmount(fs_volume_t *v) {
    int slot;

    for (slot = 0; slot < GNFS_MAX; slot++) {
        if (&gnfs_fs_slots[slot] == v) {
            /* Retired, not freed as a SLOT - kernel/include/fs.h's own
             * unmount comment explains why (a handle opened before unmount
             * may still hold an fs_node_t pointing at this fs_volume_t). The
             * in-memory bitmap and object table ARE freed here, though -
             * they are this mount's own cache of the volume, not something a
             * stale handle needs: fs_node_t only ever carries an object
             * number, and `mounted` (already cleared before this runs) is
             * what stops anything from dereferencing it after this point. */
            kfree(gnfs_mounts[slot].bitmap);
            kfree(gnfs_mounts[slot].obj_table);
            gnfs_mounts[slot].bitmap    = NULL;
            gnfs_mounts[slot].obj_table = NULL;
            gnfs_retired[slot] = 1;
            return;
        }
    }
}

static const fs_ops_t gnfs_ops = {
    /* Designated initializers throughout, matching kernel/fs/fatfs.c and
     * kernel/zfs/zfs_vfs.c rather than this file's own earlier positional
     * list - adding a slot (setacl, this session) to a positional
     * initializer risks every field after it silently shifting by one,
     * which is exactly the quiet bug this whole codebase's culture hunts
     * for elsewhere; a designated one cannot have that bug at all. */
    .name     = "gnfs",
    .lookup   = gnfs_op_lookup,
    .read     = gnfs_op_read,
    .write    = gnfs_op_write,
    .iterate  = gnfs_op_iterate,
    .statfs   = gnfs_op_statfs,
    .create   = gnfs_op_create,
    .truncate = gnfs_op_truncate,
    .mkdir    = gnfs_op_mkdir,
    .rmdir    = gnfs_op_rmdir,
    .unlink   = gnfs_op_unlink,
    /* .rename - not yet; see this file's header */
    .unmount  = gnfs_op_unmount,
    .getacl   = gnfs_op_getacl,
    .setacl   = gnfs_op_setacl,
    .setowner = gnfs_op_setowner
};

/* --- the prober --------------------------------------------------------------
 *
 * Silent for "not gnfs" (gnfs_find_current's -ENODEV), exactly as the FAT and
 * ZFS probers already are and for the same reason: this runs against every
 * volume on the machine, and a prober that complains about every volume it
 * is not is a prober nobody reads. */
fs_volume_t *gnfs_probe(device_t *dev) {
    int slot;
    gnfs_root_t root;
    uint8 *bitmap;
    uint8 *obj_table;

    for (slot = 0; slot < GNFS_MAX; slot++) {
        if (!gnfs_used[slot]) {
            break;
        }
    }
    if (slot == GNFS_MAX) {
        for (slot = 0; slot < GNFS_MAX; slot++) {
            if (gnfs_retired[slot]) {
                break;
            }
        }
    }
    if (slot == GNFS_MAX) {
        return NULL;
    }

    if (gnfs_find_current(dev, &root) != 0) {
        return NULL;
    }

    bitmap = (uint8 *)kmalloc(root.bitmap_blocks * (uint64)GNFS_BLOCK_SIZE);
    obj_table = (uint8 *)kmalloc(root.obj_table_blocks *
                                (uint64)GNFS_BLOCK_SIZE);
    if (bitmap == NULL || obj_table == NULL) {
        kfree(bitmap);
        kfree(obj_table);
        return NULL;
    }
    if (dev_read(dev, root.bitmap_block * GNFS_BLOCK_SIZE, bitmap,
                root.bitmap_blocks * (uint64)GNFS_BLOCK_SIZE) !=
        (int64)(root.bitmap_blocks * (uint64)GNFS_BLOCK_SIZE) ||
        dev_read(dev, root.obj_table_block * GNFS_BLOCK_SIZE, obj_table,
                root.obj_table_blocks * (uint64)GNFS_BLOCK_SIZE) !=
        (int64)(root.obj_table_blocks * (uint64)GNFS_BLOCK_SIZE)) {
        kfree(bitmap);
        kfree(obj_table);
        return NULL;
    }

    gnfs_retired[slot] = 0;
    gnfs_mounts[slot].dev       = dev;
    gnfs_mounts[slot].root      = root;
    gnfs_mounts[slot].bitmap    = bitmap;
    gnfs_mounts[slot].obj_table = obj_table;

    gnfs_fs_slots[slot].ops        = &gnfs_ops;
    gnfs_fs_slots[slot].body       = &gnfs_mounts[slot];
    gnfs_fs_slots[slot].block_size = GNFS_BLOCK_SIZE;
    gnfs_fs_slots[slot].mounted    = 1;
    gnfs_used[slot] = 1;

    kprintf("gnfs: volume on %s, txg %lx, %lx allocatable blocks, %lx "
           "objects in use\n",
           dev->ns_name, root.txg, root.total_blocks,
           root.next_objnum - root.root_dir_objnum);
    return &gnfs_fs_slots[slot];
}

void gnfs_init(void) {
    volume_register_fs("gnfs", gnfs_probe);
}
