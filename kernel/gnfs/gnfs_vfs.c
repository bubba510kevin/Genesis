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
 * --- what it is -------------------------------------------------------------
 * A copy-on-write filesystem: files and directories of any size the block
 * map reaches (about 1GB), created, written, read, truncated, renamed,
 * removed, with owners, NFSv4 ACLs and setuid/setgid/sticky bits. Every
 * block a change touches is a NEW block - gnfs_cow_write_block never
 * overwrites one the committed state still references, and the old one is
 * freed only after the new one is in place - and every operation ends in a
 * commit that advances the root-record ring. A crash mid-operation leaves
 * the previous commit intact.
 *
 * --- snapshots ---------------------------------------------------------------
 * Read-only, whole-volume, reachable at /.snapshots/<name>/... ; `mkdir
 * /.snapshots/<name>` takes one and `rmdir` deletes it. See gnfs_layout.h's
 * gnfs_snap_t for the format and the allocation rule that keeps them valid.
 *
 * --- stack discipline -----------------------------------------------------
 * Kernel stacks are 16KB and a block is 4KB, and a double-indirect mapping
 * change needs three blocks in hand at once. So the block buffers live in
 * the mount (gnfs_mount_t::scr), one per ROLE, and the roles never nest into
 * each other: SCR_DIR for directory blocks, SCR_DATA for file data,
 * SCR_P1/SCR_P2 for the two levels of pointer block, SCR_SNAP for the
 * snapshot directory. The kernel runs one filesystem operation at a time,
 * which is what makes per-mount (rather than per-call) buffers correct.
 */

#define GNFS_MAX 4

#define ENOENT       2
#define EIO          5
#define ENOMEM      12
#define EEXIST      17
#define EBUSY       16
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define EFBIG       27
#define ENOSPC      28
#define EROFS       30
#define ENAMETOOLONG 36
#define ENOTEMPTY   39

enum { SCR_DIR = 0, SCR_DATA, SCR_P1, SCR_P2, SCR_SNAP, SCR_COUNT };

#define SNAPDIR_NAME     ".snapshots"
#define SNAPDIR_NAME_LEN 10u

typedef struct gnfs_mount {
    device_t   *dev;
    gnfs_root_t root;          /* the current (highest valid txg) root record */
    uint8      *bitmap;        /* live free-space bitmap                      */
    uint8      *held;          /* OR of every snapshot's reference map: a
                                * block set here is not free even when the
                                * live bitmap says it is                     */
    uint8      *obj_table;     /* the live object table                      */
    uint8      *obt_shadow;    /* the table as of the last commit            */
    uint8      *obt_dirty;     /* per table block: bit 0 = region A still
                                * needs it, bit 1 = region B                 */
    uint8      *snap_table;    /* one snapshot's table, cached for reads     */
    int         snap_cached;   /* which snapshot slot it holds, or -1        */
    uint64      snap_cached_txg;
    uint8      *scr[SCR_COUNT];
} gnfs_mount_t;

static fs_volume_t   gnfs_fs_slots[GNFS_MAX];
static gnfs_mount_t  gnfs_mounts[GNFS_MAX];
static int           gnfs_used[GNFS_MAX];
static int           gnfs_retired[GNFS_MAX];

static uint64 bytes_of(uint64 blocks) {
    return blocks * (uint64)GNFS_BLOCK_SIZE;
}

static void zero(uint8 *p, uint64 n) {
    uint64 i;

    for (i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static void copy(uint8 *dst, const uint8 *src, uint64 n) {
    uint64 i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static int same(const uint8 *a, const uint8 *b, uint64 n) {
    uint64 i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* --- fs_node_t::priv ---------------------------------------------------------
 *
 * Bytes 0..7 the object number, byte 8 what KIND of node it is, byte 9 the
 * snapshot slot and 10..17 that snapshot's txg (so a node from a deleted
 * snapshot whose slot was reused is recognised as stale rather than read
 * through the new snapshot's table). Packed byte by byte because priv has no
 * alignment guarantee for anything wider. */
enum { NODE_LIVE = 0, NODE_SNAPDIR = 1, NODE_SNAP = 2 };

static uint64 get64(const uint8 *p) {
    uint64 v = 0;
    int i;

    for (i = 0; i < 8; i++) {
        v |= ((uint64)p[i]) << (8 * i);
    }
    return v;
}

static void put64(uint8 *p, uint64 v) {
    int i;

    for (i = 0; i < 8; i++) {
        p[i] = (uint8)(v >> (8 * i));
    }
}

static uint64 node_objnum(const fs_node_t *n)   { return get64(n->priv); }
static int    node_kind(const fs_node_t *n)     { return n->priv[8]; }
static int    node_snap(const fs_node_t *n)     { return n->priv[9]; }
static uint64 node_snap_txg(const fs_node_t *n) { return get64(n->priv + 10); }

static void node_set(fs_node_t *n, uint64 objnum, int kind, int snap,
                     uint64 snap_txg) {
    put64(n->priv, objnum);
    n->priv[8] = (uint8)kind;
    n->priv[9] = (uint8)snap;
    put64(n->priv + 10, snap_txg);
}

/* The live table's onode for `objnum`, or NULL. */
static gnfs_onode_t *live_onode(gnfs_mount_t *m, uint64 objnum) {
    return gnfs_onode_at(m->obj_table, m->root.max_objects, objnum);
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

/* Scan every ring slot and keep the valid one with the highest txg - ZFS's
 * own uberblock-mount rule: a torn write only ever lands in the slot the
 * CURRENT commit was advancing into, so the highest-txg valid slot is always
 * the last complete commit. */
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
 * gnfs_txg_commit is the WHOLE-structure form: both the bitmap and the
 * object table go to the regions the current root does not point at, then
 * a new root naming them goes into the ring. It is what the format's
 * crash-safety argument is stated against and what tests/host/gnfs_test.c
 * drives directly, including a torn-write simulation.
 *
 * gnfs_commit, below, is what a mount actually calls, and it differs in one
 * respect only: it writes just the object-table blocks the target region is
 * missing. Everything else - the order, the root record, the torn-write
 * argument - is the same, because the target region is still the one the
 * current root does not name. */
static int write_next_root(device_t *dev, gnfs_root_t *cur,
                           uint64 next_bitmap_block,
                           uint64 next_obj_table_block) {
    gnfs_root_t next = *cur;
    int64 rc;

    next.txg             = cur->txg + 1;
    next.bitmap_block    = next_bitmap_block;
    next.obj_table_block = next_obj_table_block;
    gnfs_root_seal(&next);

    rc = dev_write(dev, (next.txg % GNFS_RING_SLOTS) * GNFS_BLOCK_SIZE,
                  &next, sizeof(next));
    if (rc != (int64)sizeof(next)) {
        return (rc < 0) ? (int)rc : -EIO;
    }
    *cur = next;
    return 0;
}

int gnfs_txg_commit(device_t *dev, gnfs_root_t *cur, const uint8 *bitmap,
                    uint64 bitmap_bytes_used, const uint8 *obj_table,
                    uint64 obj_table_bytes_used) {
    uint64 bmp_a = gnfs_bitmap_region_a(cur);
    uint64 bmp_b = gnfs_bitmap_region_b(cur);
    uint64 obt_a = gnfs_obj_table_region_a(cur);
    uint64 obt_b = gnfs_obj_table_region_b(cur);
    uint64 next_bitmap_block = (cur->bitmap_block == bmp_a) ? bmp_b : bmp_a;
    uint64 next_obj_table_block = (cur->obj_table_block == obt_a)
        ? obt_b : obt_a;
    int64 rc;

    if (bitmap_bytes_used > bytes_of(cur->bitmap_blocks) ||
        obj_table_bytes_used > bytes_of(cur->obj_table_blocks)) {
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
    return write_next_root(dev, cur, next_bitmap_block, next_obj_table_block);
}

/* Which table blocks changed is found by COMPARING against the table as it
 * was last committed, not by each mutation remembering to mark a block
 * dirty. A missed mark would be silent data loss in one region and nothing
 * else; a comparison cannot miss. */
static int gnfs_commit(gnfs_mount_t *m) {
    gnfs_root_t *cur = &m->root;
    uint64 obt_a = gnfs_obj_table_region_a(cur);
    uint64 obt_b = gnfs_obj_table_region_b(cur);
    uint64 bmp_a = gnfs_bitmap_region_a(cur);
    uint64 bmp_b = gnfs_bitmap_region_b(cur);
    uint64 target = (cur->obj_table_block == obt_a) ? obt_b : obt_a;
    uint8  bit    = (target == obt_a) ? 1u : 2u;
    uint64 next_bitmap_block = (cur->bitmap_block == bmp_a) ? bmp_b : bmp_a;
    uint64 b;
    int64 rc;

    for (b = 0; b < cur->obj_table_blocks; b++) {
        uint8 *now = m->obj_table + bytes_of(b);
        uint8 *was = m->obt_shadow + bytes_of(b);

        if (!same(now, was, GNFS_BLOCK_SIZE)) {
            copy(was, now, GNFS_BLOCK_SIZE);
            m->obt_dirty[b] = 3;               /* both regions now stale */
        }
    }
    for (b = 0; b < cur->obj_table_blocks; b++) {
        if (!(m->obt_dirty[b] & bit)) {
            continue;
        }
        rc = dev_write(m->dev, (target + b) * GNFS_BLOCK_SIZE,
                      m->obj_table + bytes_of(b), GNFS_BLOCK_SIZE);
        if (rc != (int64)GNFS_BLOCK_SIZE) {
            return (rc < 0) ? (int)rc : -EIO;
        }
        m->obt_dirty[b] &= (uint8)~bit;
    }
    rc = dev_write(m->dev, next_bitmap_block * GNFS_BLOCK_SIZE, m->bitmap,
                  bytes_of(cur->bitmap_blocks));
    if (rc != (int64)bytes_of(cur->bitmap_blocks)) {
        return (rc < 0) ? (int)rc : -EIO;
    }
    return write_next_root(m->dev, cur, next_bitmap_block, target);
}

/* --- block allocation --------------------------------------------------------
 *
 * The bitmap is indexed RELATIVE to the allocatable region; everything else
 * uses ABSOLUTE block numbers (gnfs_layout.h states the convention). A
 * block is free only if neither the live bitmap nor `held` - the union of
 * every snapshot's reference map - has it: that single test is the whole of
 * what keeps a snapshot's blocks from being handed out again. */
static int block_free(const gnfs_mount_t *m, uint64 rel) {
    return !gnfs_bitmap_test(m->bitmap, rel) && !gnfs_bitmap_test(m->held, rel);
}

/* First fit, `count` contiguous blocks. Returns 0 with *rel_out, or
 * -ENOSPC. Marks them in the live bitmap. */
static int alloc_run(gnfs_mount_t *m, uint64 count, uint64 *rel_out) {
    uint64 total = m->root.total_blocks;
    uint64 start = 0, run = 0, i;

    if (count == 0 || count > total) {
        return -ENOSPC;
    }
    for (i = 0; i < total; i++) {
        if (!block_free(m, i)) {
            run = 0;
            start = i + 1;
            continue;
        }
        if (++run == count) {
            for (i = start; i < start + count; i++) {
                gnfs_bitmap_set(m->bitmap, i);
            }
            *rel_out = start;
            return 0;
        }
    }
    return -ENOSPC;
}

/* Allocate ONE fresh block, write `content` into it, and hand back its
 * ABSOLUTE number. Never touches a block anything references. */
static int gnfs_cow_write_block(gnfs_mount_t *m, const uint8 *content,
                                uint64 *abs_out) {
    uint64 rel, abs;
    int64 wrc;
    int rc;

    rc = alloc_run(m, 1, &rel);
    if (rc != 0) {
        return rc;
    }
    abs = gnfs_alloc_region_start(&m->root) + rel;
    wrc = dev_write(m->dev, abs * GNFS_BLOCK_SIZE, content, GNFS_BLOCK_SIZE);
    if (wrc != (int64)GNFS_BLOCK_SIZE) {
        /* Nothing points at it yet, so undoing the allocation is correct,
         * not merely convenient. */
        gnfs_bitmap_clear(m->bitmap, rel);
        return (wrc < 0) ? (int)wrc : -EIO;
    }
    *abs_out = abs;
    return 0;
}

/* Drops the block from the LIVE bitmap. If a snapshot still references it,
 * `held` keeps it unavailable - which is exactly right, and is why freeing
 * never has to ask. */
static void gnfs_cow_free_block(gnfs_mount_t *m, uint64 abs_block) {
    uint64 start = gnfs_alloc_region_start(&m->root);

    if (abs_block == 0 || abs_block < start ||
        abs_block - start >= m->root.total_blocks) {
        return;
    }
    gnfs_bitmap_clear(m->bitmap, abs_block - start);
}

static int read_block(gnfs_mount_t *m, uint64 abs, uint8 *buf) {
    if (dev_read(m->dev, abs * GNFS_BLOCK_SIZE, buf, GNFS_BLOCK_SIZE) !=
        (int64)GNFS_BLOCK_SIZE) {
        return -EIO;
    }
    return 0;
}

/* --- the block map -------------------------------------------------------------
 *
 * Logical block `idx` of an object -> ABSOLUTE block, 0 for a hole. See
 * gnfs_layout.h for the shape (12 direct, one indirect, one double). */

static int all_zero_ptrs(const uint8 *blk) {
    const uint64 *p = (const uint64 *)(const void *)blk;
    uint64 i;

    for (i = 0; i < GNFS_PTRS_PER_BLOCK; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int bmap_get(gnfs_mount_t *m, const gnfs_onode_t *o, uint64 idx,
                    uint64 *abs) {
    uint64 *ptr = (uint64 *)(void *)m->scr[SCR_P1];
    int rc;

    *abs = 0;
    if (idx < GNFS_OBJ_DIRECT) {
        *abs = o->direct[idx];
        return 0;
    }
    idx -= GNFS_OBJ_DIRECT;
    if (idx < GNFS_PTRS_PER_BLOCK) {
        if (o->indirect == 0) {
            return 0;
        }
        rc = read_block(m, o->indirect, m->scr[SCR_P1]);
        if (rc == 0) {
            *abs = ptr[idx];
        }
        return rc;
    }
    idx -= GNFS_PTRS_PER_BLOCK;
    if (idx >= (uint64)GNFS_PTRS_PER_BLOCK * GNFS_PTRS_PER_BLOCK) {
        return -EFBIG;
    }
    if (o->dindirect == 0) {
        return 0;
    }
    rc = read_block(m, o->dindirect, m->scr[SCR_P1]);
    if (rc != 0) {
        return rc;
    }
    if (ptr[idx / GNFS_PTRS_PER_BLOCK] == 0) {
        return 0;
    }
    rc = read_block(m, ptr[idx / GNFS_PTRS_PER_BLOCK], m->scr[SCR_P1]);
    if (rc == 0) {
        *abs = ptr[idx % GNFS_PTRS_PER_BLOCK];
    }
    return rc;
}

/* Rewrite one pointer block copy-on-write: `buf` holds its new contents;
 * the result is a fresh block (or 0 if every pointer in it is now a hole,
 * so empty pointer blocks do not accumulate), and the old one is freed. */
static int cow_ptr_block(gnfs_mount_t *m, uint8 *buf, uint64 old_abs,
                         uint64 *new_abs) {
    int rc;

    if (all_zero_ptrs(buf)) {
        *new_abs = 0;
    } else {
        rc = gnfs_cow_write_block(m, buf, new_abs);
        if (rc != 0) {
            return rc;
        }
    }
    gnfs_cow_free_block(m, old_abs);
    return 0;
}

/* Point logical block `idx` at `abs` (0 = make it a hole). Pointer blocks
 * are rewritten copy-on-write on the way; the data block the mapping used
 * to name is NOT freed here - the caller owns that decision, because a COW
 * data write frees the old block and a truncate frees it too, but a rename
 * of a mapping (not needed today) would not. */
static int bmap_set(gnfs_mount_t *m, gnfs_onode_t *o, uint64 idx,
                    uint64 abs) {
    uint64 *p1 = (uint64 *)(void *)m->scr[SCR_P1];
    uint64 *p2 = (uint64 *)(void *)m->scr[SCR_P2];
    uint64 l1_old, l1_new, top_new;
    int rc;

    if (idx < GNFS_OBJ_DIRECT) {
        o->direct[idx] = abs;
        return 0;
    }
    idx -= GNFS_OBJ_DIRECT;
    if (idx < GNFS_PTRS_PER_BLOCK) {
        if (o->indirect != 0) {
            rc = read_block(m, o->indirect, m->scr[SCR_P1]);
            if (rc != 0) {
                return rc;
            }
        } else {
            if (abs == 0) {
                return 0;               /* already a hole */
            }
            zero(m->scr[SCR_P1], GNFS_BLOCK_SIZE);
        }
        p1[idx] = abs;
        rc = cow_ptr_block(m, m->scr[SCR_P1], o->indirect, &top_new);
        if (rc == 0) {
            o->indirect = top_new;
        }
        return rc;
    }
    idx -= GNFS_PTRS_PER_BLOCK;
    if (idx >= (uint64)GNFS_PTRS_PER_BLOCK * GNFS_PTRS_PER_BLOCK) {
        return -EFBIG;
    }
    if (o->dindirect != 0) {
        rc = read_block(m, o->dindirect, m->scr[SCR_P1]);
        if (rc != 0) {
            return rc;
        }
    } else {
        if (abs == 0) {
            return 0;
        }
        zero(m->scr[SCR_P1], GNFS_BLOCK_SIZE);
    }
    l1_old = p1[idx / GNFS_PTRS_PER_BLOCK];
    if (l1_old != 0) {
        rc = read_block(m, l1_old, m->scr[SCR_P2]);
        if (rc != 0) {
            return rc;
        }
    } else {
        if (abs == 0) {
            return 0;
        }
        zero(m->scr[SCR_P2], GNFS_BLOCK_SIZE);
    }
    p2[idx % GNFS_PTRS_PER_BLOCK] = abs;
    rc = cow_ptr_block(m, m->scr[SCR_P2], l1_old, &l1_new);
    if (rc != 0) {
        return rc;
    }
    p1[idx / GNFS_PTRS_PER_BLOCK] = l1_new;
    rc = cow_ptr_block(m, m->scr[SCR_P1], o->dindirect, &top_new);
    if (rc == 0) {
        o->dindirect = top_new;
    }
    return rc;
}

/* Free every data block and pointer block an object has, and clear its map.
 * Walks the pointer blocks directly rather than going through bmap_set per
 * block - freeing a whole tree has no copy-on-write to do. */
static int free_all_blocks(gnfs_mount_t *m, gnfs_onode_t *o) {
    uint64 *p1 = (uint64 *)(void *)m->scr[SCR_P1];
    uint64 *p2 = (uint64 *)(void *)m->scr[SCR_P2];
    uint64 i, j;
    int rc;

    for (i = 0; i < GNFS_OBJ_DIRECT; i++) {
        gnfs_cow_free_block(m, o->direct[i]);
        o->direct[i] = 0;
    }
    if (o->indirect != 0) {
        rc = read_block(m, o->indirect, m->scr[SCR_P1]);
        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < GNFS_PTRS_PER_BLOCK; i++) {
            gnfs_cow_free_block(m, p1[i]);
        }
        gnfs_cow_free_block(m, o->indirect);
        o->indirect = 0;
    }
    if (o->dindirect != 0) {
        rc = read_block(m, o->dindirect, m->scr[SCR_P1]);
        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < GNFS_PTRS_PER_BLOCK; i++) {
            if (p1[i] == 0) {
                continue;
            }
            rc = read_block(m, p1[i], m->scr[SCR_P2]);
            if (rc != 0) {
                return rc;
            }
            for (j = 0; j < GNFS_PTRS_PER_BLOCK; j++) {
                gnfs_cow_free_block(m, p2[j]);
            }
            gnfs_cow_free_block(m, p1[i]);
        }
        gnfs_cow_free_block(m, o->dindirect);
        o->dindirect = 0;
    }
    o->nblocks = 0;
    o->size    = 0;
    return 0;
}

/* An object gone entirely: its blocks, its ACL block, and its table slot -
 * which becomes allocatable again. */
static int free_object(gnfs_mount_t *m, uint64 objnum) {
    gnfs_onode_t *o = live_onode(m, objnum);
    int rc;

    if (o == NULL) {
        return -EIO;
    }
    rc = free_all_blocks(m, o);
    if (rc != 0) {
        return rc;
    }
    gnfs_cow_free_block(m, o->acl_block);
    gnfs_onode_free(o);
    return 0;
}

/* --- file data ------------------------------------------------------------- */

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
        uint64 pos = offset + done;
        uint64 idx = pos / GNFS_BLOCK_SIZE;
        uint64 in_off = pos % GNFS_BLOCK_SIZE;
        uint64 chunk = GNFS_BLOCK_SIZE - in_off;
        uint64 abs, z;
        int rc;

        if (chunk > want - done) {
            chunk = want - done;
        }
        rc = bmap_get(m, o, idx, &abs);
        if (rc != 0) {
            return rc;
        }
        if (abs == 0) {
            /* A hole reads as zero - the rule every sparse file follows. */
            for (z = 0; z < chunk; z++) {
                dst[done + z] = 0;
            }
        } else {
            rc = read_block(m, abs, m->scr[SCR_DATA]);
            if (rc != 0) {
                return rc;
            }
            for (z = 0; z < chunk; z++) {
                dst[done + z] = m->scr[SCR_DATA][in_off + z];
            }
        }
        done += chunk;
    }
    return (int64)done;
}

/* Writes [offset, offset+len) COW: every touched block is a fresh block. A
 * block only partly covered is read (or zeroed, if it was a hole) so the
 * untouched part survives the copy. */
static int gnfs_write_data(gnfs_mount_t *m, gnfs_onode_t *o, uint64 offset,
                          const void *buf, uint64 len) {
    const uint8 *src = (const uint8 *)buf;
    uint64 end = offset + len;
    uint64 first_idx, last_idx, idx;

    if (len == 0) {
        return 0;
    }
    if (end < offset || end > GNFS_MAX_FILE_BLOCKS * GNFS_BLOCK_SIZE) {
        return -EFBIG;
    }
    first_idx = offset / GNFS_BLOCK_SIZE;
    last_idx  = (end - 1) / GNFS_BLOCK_SIZE;

    for (idx = first_idx; idx <= last_idx; idx++) {
        uint8 *block = m->scr[SCR_DATA];
        uint64 blk_start = idx * GNFS_BLOCK_SIZE;
        uint64 copy_start = (offset > blk_start) ? offset - blk_start : 0;
        uint64 copy_end = (end < blk_start + GNFS_BLOCK_SIZE)
            ? end - blk_start : GNFS_BLOCK_SIZE;
        uint64 src_base = (blk_start + copy_start) - offset;
        uint64 old_block, new_block, k;
        int rc;

        rc = bmap_get(m, o, idx, &old_block);
        if (rc != 0) {
            return rc;
        }
        if (old_block != 0 && (copy_start != 0 || copy_end != GNFS_BLOCK_SIZE)) {
            rc = read_block(m, old_block, block);
            if (rc != 0) {
                return rc;
            }
        } else {
            zero(block, GNFS_BLOCK_SIZE);
        }
        for (k = copy_start; k < copy_end; k++) {
            block[k] = src[src_base + (k - copy_start)];
        }
        rc = gnfs_cow_write_block(m, block, &new_block);
        if (rc != 0) {
            return rc;
        }
        rc = bmap_set(m, o, idx, new_block);
        if (rc != 0) {
            gnfs_cow_free_block(m, new_block);
            return rc;
        }
        gnfs_cow_free_block(m, old_block);
        if (idx + 1 > o->nblocks) {
            o->nblocks = idx + 1;
        }
    }
    if (end > o->size) {
        o->size = end;
    }
    return 0;
}

/* Shrink to `size`. Blocks wholly past the new end are freed; the block
 * the new end falls INSIDE has its tail zeroed (copy-on-write), which is
 * what makes a later extension read zeroes there rather than the bytes that
 * were cut off - the previous version skipped that and leaked old contents
 * back out through a truncate-then-grow. */
static int shrink_to(gnfs_mount_t *m, gnfs_onode_t *o, uint64 size) {
    uint64 keep = (size + GNFS_BLOCK_SIZE - 1) / GNFS_BLOCK_SIZE;
    uint64 idx, abs;
    int rc;

    if (size == 0) {
        return free_all_blocks(m, o);
    }
    for (idx = keep; idx < o->nblocks; idx++) {
        rc = bmap_get(m, o, idx, &abs);
        if (rc != 0) {
            return rc;
        }
        if (abs == 0) {
            continue;
        }
        rc = bmap_set(m, o, idx, 0);
        if (rc != 0) {
            return rc;
        }
        gnfs_cow_free_block(m, abs);
    }
    o->nblocks = keep;
    if (size % GNFS_BLOCK_SIZE != 0) {
        uint64 new_block;

        rc = bmap_get(m, o, keep - 1, &abs);
        if (rc != 0) {
            return rc;
        }
        if (abs != 0) {
            rc = read_block(m, abs, m->scr[SCR_DATA]);
            if (rc != 0) {
                return rc;
            }
            zero(m->scr[SCR_DATA] + size % GNFS_BLOCK_SIZE,
                 GNFS_BLOCK_SIZE - size % GNFS_BLOCK_SIZE);
            rc = gnfs_cow_write_block(m, m->scr[SCR_DATA], &new_block);
            if (rc != 0) {
                return rc;
            }
            rc = bmap_set(m, o, keep - 1, new_block);
            if (rc != 0) {
                gnfs_cow_free_block(m, new_block);
                return rc;
            }
            gnfs_cow_free_block(m, abs);
        }
    }
    o->size = size;
    return 0;
}

/* --- views: the live volume, or one snapshot ------------------------------- */

typedef struct {
    uint8 *table;
    uint64 max_objects;
    uint64 root_dir;
    int    snap;             /* -1 for the live volume */
    uint64 snap_txg;
} gnfs_view_t;

static void live_view(gnfs_mount_t *m, gnfs_view_t *v) {
    v->table       = m->obj_table;
    v->max_objects = m->root.max_objects;
    v->root_dir    = m->root.root_dir_objnum;
    v->snap        = -1;
    v->snap_txg    = 0;
}

static gnfs_onode_t *view_onode(const gnfs_view_t *v, uint64 objnum) {
    return gnfs_onode_at(v->table, v->max_objects, objnum);
}

/* --- directories: entries across every block of the directory's map ------- */

static int dir_find(gnfs_mount_t *m, const gnfs_onode_t *dir,
                    const char *name, uint64 name_len, uint64 *objnum_out,
                    int *is_dir_out) {
    uint64 idx, abs;
    int rc;

    for (idx = 0; idx < dir->nblocks; idx++) {
        rc = bmap_get(m, dir, idx, &abs);
        if (rc != 0) {
            return rc;
        }
        if (abs == 0) {
            continue;
        }
        rc = read_block(m, abs, m->scr[SCR_DIR]);
        if (rc != 0) {
            return rc;
        }
        if (gnfs_dir_find(m->scr[SCR_DIR], name, name_len, objnum_out,
                          is_dir_out) == 0) {
            return 0;
        }
    }
    return -ENOENT;
}

static int dir_iterate(gnfs_mount_t *m, const gnfs_onode_t *dir,
                       gnfs_dir_cb cb, void *ctx) {
    uint64 idx, abs;
    int rc;

    for (idx = 0; idx < dir->nblocks; idx++) {
        rc = bmap_get(m, dir, idx, &abs);
        if (rc != 0) {
            return rc;
        }
        if (abs == 0) {
            continue;
        }
        rc = read_block(m, abs, m->scr[SCR_DIR]);
        if (rc != 0) {
            return rc;
        }
        rc = gnfs_dir_iterate(m->scr[SCR_DIR], cb, ctx);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int has_entry_cb(const char *name, uint64 name_len, uint64 objnum,
                        int is_dir, void *ctx) {
    (void)name; (void)name_len; (void)objnum; (void)is_dir;
    *(int *)ctx = 1;
    return 1;
}

static int dir_is_empty(gnfs_mount_t *m, const gnfs_onode_t *dir, int *empty) {
    int has = 0;
    int rc = dir_iterate(m, dir, has_entry_cb, &has);

    if (rc < 0) {
        return rc;
    }
    *empty = !has;
    return 0;
}

/* Rewrite directory block `idx` (whose new contents are in SCR_DIR) COW. */
static int dir_commit_block(gnfs_mount_t *m, gnfs_onode_t *dir, uint64 idx,
                            uint64 old_abs) {
    uint64 new_abs;
    int rc = gnfs_cow_write_block(m, m->scr[SCR_DIR], &new_abs);

    if (rc != 0) {
        return rc;
    }
    rc = bmap_set(m, dir, idx, new_abs);
    if (rc != 0) {
        gnfs_cow_free_block(m, new_abs);
        return rc;
    }
    gnfs_cow_free_block(m, old_abs);
    return 0;
}

/* Add an entry to a live directory: into the first block with a free slot,
 * or into a new block appended to the directory when every block is full.
 * -EEXIST if the name is already anywhere in it. */
static int dir_add(gnfs_mount_t *m, uint64 dir_objnum, const char *name,
                   uint64 name_len, uint64 objnum, int is_dir) {
    gnfs_onode_t *dir = live_onode(m, dir_objnum);
    uint64 idx, abs, dummy;
    int dummy_dir;
    int rc;

    if (dir == NULL) {
        return -EIO;
    }
    if (name_len == 0 || name_len > GNFS_NAME_MAX) {
        return -EINVAL;
    }
    rc = dir_find(m, dir, name, name_len, &dummy, &dummy_dir);
    if (rc == 0) {
        return -EEXIST;
    }
    if (rc != -ENOENT) {
        return rc;
    }
    for (idx = 0; idx < dir->nblocks; idx++) {
        rc = bmap_get(m, dir, idx, &abs);
        if (rc != 0) {
            return rc;
        }
        if (abs == 0) {
            continue;
        }
        rc = read_block(m, abs, m->scr[SCR_DIR]);
        if (rc != 0) {
            return rc;
        }
        rc = gnfs_dir_add(m->scr[SCR_DIR], name, name_len, objnum, is_dir);
        if (rc == -ENOSPC) {
            continue;                      /* this block is full */
        }
        if (rc != 0) {
            return rc;
        }
        return dir_commit_block(m, dir, idx, abs);
    }
    /* Every block full: grow by one. */
    if (dir->nblocks >= GNFS_MAX_FILE_BLOCKS) {
        return -ENOSPC;
    }
    gnfs_dir_init_block(m->scr[SCR_DIR]);
    rc = gnfs_dir_add(m->scr[SCR_DIR], name, name_len, objnum, is_dir);
    if (rc != 0) {
        return rc;
    }
    idx = dir->nblocks;
    rc = dir_commit_block(m, dir, idx, 0);
    if (rc != 0) {
        return rc;
    }
    dir->nblocks = idx + 1;
    dir->size    = bytes_of(dir->nblocks);
    return 0;
}

static int dir_remove(gnfs_mount_t *m, uint64 dir_objnum, const char *name,
                      uint64 name_len) {
    gnfs_onode_t *dir = live_onode(m, dir_objnum);
    uint64 idx, abs;
    int rc;

    if (dir == NULL) {
        return -EIO;
    }
    for (idx = 0; idx < dir->nblocks; idx++) {
        rc = bmap_get(m, dir, idx, &abs);
        if (rc != 0) {
            return rc;
        }
        if (abs == 0) {
            continue;
        }
        rc = read_block(m, abs, m->scr[SCR_DIR]);
        if (rc != 0) {
            return rc;
        }
        if (gnfs_dir_remove(m->scr[SCR_DIR], name, name_len) == 0) {
            return dir_commit_block(m, dir, idx, abs);
        }
    }
    return -ENOENT;
}

/* --- path resolution ------------------------------------------------------------
 *
 * abs_path is absolute and normalized (fs_ops_t::lookup's contract), so this
 * walks component by component. "/" resolves with the loop never running. */
static int resolve_in(gnfs_mount_t *m, const gnfs_view_t *v,
                      const char *abs_path, uint64 *objnum_out,
                      int *is_dir_out) {
    uint64 cur = v->root_dir;
    int cur_is_dir = 1;
    uint64 i = 1;

    if (abs_path[0] != '/') {
        return -EINVAL;
    }
    while (abs_path[i] != '\0') {
        uint64 start = i;
        const gnfs_onode_t *dir;
        uint64 next;
        int next_is_dir;
        int rc;

        while (abs_path[i] != '\0' && abs_path[i] != '/') {
            i++;
        }
        if (!cur_is_dir) {
            return -ENOTDIR;
        }
        dir = view_onode(v, cur);
        if (dir == NULL) {
            return -ENOENT;
        }
        rc = dir_find(m, dir, abs_path + start, i - start, &next,
                      &next_is_dir);
        if (rc != 0) {
            return rc;
        }
        cur = next;
        cur_is_dir = next_is_dir;
        if (abs_path[i] == '/') {
            i++;
        }
    }
    *objnum_out = cur;
    *is_dir_out = cur_is_dir;
    return 0;
}

/* Splits an absolute path into parent length and leaf, as views into it. */
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

static int resolve_parent(gnfs_mount_t *m, const char *abs_path,
                          uint64 *parent_objnum, const char **leaf,
                          uint64 *leaf_len) {
    gnfs_view_t v;
    uint64 parent_len, k;
    char parent_buf[GNFS_PATH_SCRATCH];
    int parent_is_dir;
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

    live_view(m, &v);
    rc = resolve_in(m, &v, parent_buf, parent_objnum, &parent_is_dir);
    if (rc != 0) {
        return rc;
    }
    return parent_is_dir ? 0 : -ENOTDIR;
}

/* --- snapshots ------------------------------------------------------------------ */

static int names_eq(const char *a, uint64 alen, const char *b, uint64 blen) {
    uint64 i;

    if (alen != blen) {
        return 0;
    }
    for (i = 0; i < alen; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* Reads the snapshot directory into SCR_SNAP (all-zero if there is none). */
static int snapdir_load(gnfs_mount_t *m) {
    if (m->root.snap_dir_block == 0) {
        zero(m->scr[SCR_SNAP], GNFS_BLOCK_SIZE);
        return 0;
    }
    return read_block(m, m->root.snap_dir_block, m->scr[SCR_SNAP]);
}

static gnfs_snap_t *snap_rec(gnfs_mount_t *m, int idx) {
    return &((gnfs_snap_t *)(void *)m->scr[SCR_SNAP])[idx];
}

/* Slot index of the snapshot called `name`, or -ENOENT. Leaves the snapshot
 * directory loaded in SCR_SNAP. */
static int snap_find(gnfs_mount_t *m, const char *name, uint64 name_len) {
    int i, rc = snapdir_load(m);

    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < (int)GNFS_SNAPS_PER_BLOCK; i++) {
        gnfs_snap_t *s = snap_rec(m, i);

        if (s->txg != 0 && names_eq(name, name_len, s->name, s->name_len)) {
            return i;
        }
    }
    return -ENOENT;
}

/* held = OR of every snapshot's reference map, read back from disk. */
static int held_recompute(gnfs_mount_t *m) {
    uint64 bytes = bytes_of(m->root.bitmap_blocks);
    uint8 *tmp;
    int i, rc;
    uint64 k;

    zero(m->held, bytes);
    rc = snapdir_load(m);
    if (rc != 0) {
        return rc;
    }
    tmp = (uint8 *)kmalloc(bytes);
    if (tmp == NULL) {
        return -ENOMEM;
    }
    for (i = 0; i < (int)GNFS_SNAPS_PER_BLOCK; i++) {
        gnfs_snap_t s = *snap_rec(m, i);

        if (s.txg == 0) {
            continue;
        }
        if (dev_read(m->dev, s.refmap_block * GNFS_BLOCK_SIZE, tmp, bytes) !=
            (int64)bytes) {
            kfree(tmp);
            return -EIO;
        }
        for (k = 0; k < bytes; k++) {
            m->held[k] |= tmp[k];
        }
    }
    kfree(tmp);
    return 0;
}

/* Make m->snap_table hold snapshot `idx`'s object table. */
static int snap_table_load(gnfs_mount_t *m, int idx, uint64 txg) {
    gnfs_snap_t s;
    int rc;

    if (m->snap_cached == idx && m->snap_cached_txg == txg) {
        return 0;
    }
    rc = snapdir_load(m);
    if (rc != 0) {
        return rc;
    }
    s = *snap_rec(m, idx);
    if (s.txg == 0 || s.txg != txg ||
        s.table_blocks != m->root.obj_table_blocks) {
        return -ENOENT;                /* deleted, or its slot reused */
    }
    if (dev_read(m->dev, s.table_block * GNFS_BLOCK_SIZE, m->snap_table,
                 bytes_of(s.table_blocks)) != (int64)bytes_of(s.table_blocks)) {
        m->snap_cached = -1;
        return -EIO;
    }
    m->snap_cached     = idx;
    m->snap_cached_txg = txg;
    return 0;
}

static int snap_view(gnfs_mount_t *m, int idx, uint64 txg, gnfs_view_t *v) {
    int rc = snap_table_load(m, idx, txg);

    if (rc != 0) {
        return rc;
    }
    v->table       = m->snap_table;
    v->max_objects = m->root.max_objects;
    v->root_dir    = m->root.root_dir_objnum;
    v->snap        = idx;
    v->snap_txg    = txg;
    return 0;
}

static int snapdir_write(gnfs_mount_t *m) {
    uint64 new_abs = 0;
    int i, any = 0, rc;

    for (i = 0; i < (int)GNFS_SNAPS_PER_BLOCK; i++) {
        if (snap_rec(m, i)->txg != 0) {
            any = 1;
        }
    }
    if (any) {
        rc = gnfs_cow_write_block(m, m->scr[SCR_SNAP], &new_abs);
        if (rc != 0) {
            return rc;
        }
    }
    gnfs_cow_free_block(m, m->root.snap_dir_block);
    m->root.snap_dir_block = new_abs;
    return 0;
}

/* Take a snapshot of the committed state (every operation commits, so the
 * in-memory state IS the committed one). */
static int snap_create(gnfs_mount_t *m, const char *name, uint64 name_len) {
    uint64 bmp_bytes = bytes_of(m->root.bitmap_blocks);
    uint64 obt_bytes = bytes_of(m->root.obj_table_blocks);
    uint64 table_rel, refmap_rel, start, k;
    uint8 *refmap;
    gnfs_snap_t *slot = NULL;
    int i, rc;

    if (name_len == 0 || name_len > GNFS_SNAP_NAME_MAX) {
        return -EINVAL;
    }
    for (k = 0; k < name_len; k++) {
        if (name[k] == '/') {
            return -EINVAL;
        }
    }
    rc = snap_find(m, name, name_len);
    if (rc >= 0) {
        return -EEXIST;
    }
    if (rc != -ENOENT) {
        return rc;
    }
    for (i = 0; i < (int)GNFS_SNAPS_PER_BLOCK; i++) {
        if (snap_rec(m, i)->txg == 0) {
            slot = snap_rec(m, i);
            break;
        }
    }
    if (slot == NULL) {
        return -ENOSPC;
    }

    /* The reference map is the live bitmap BEFORE this snapshot's own two
     * runs are allocated: those belong to the snapshot record, are held by
     * the live bitmap for as long as it exists, and are freed with it. */
    refmap = (uint8 *)kmalloc(bmp_bytes);
    if (refmap == NULL) {
        return -ENOMEM;
    }
    copy(refmap, m->bitmap, bmp_bytes);

    start = gnfs_alloc_region_start(&m->root);
    rc = alloc_run(m, m->root.obj_table_blocks, &table_rel);
    if (rc != 0) {
        kfree(refmap);
        return rc;
    }
    rc = alloc_run(m, m->root.bitmap_blocks, &refmap_rel);
    if (rc != 0) {
        gnfs_free_blocks(m->bitmap, m->root.total_blocks, table_rel,
                         m->root.obj_table_blocks);
        kfree(refmap);
        return rc;
    }
    if (dev_write(m->dev, (start + table_rel) * GNFS_BLOCK_SIZE,
                  m->obj_table, obt_bytes) != (int64)obt_bytes ||
        dev_write(m->dev, (start + refmap_rel) * GNFS_BLOCK_SIZE,
                  refmap, bmp_bytes) != (int64)bmp_bytes) {
        gnfs_free_blocks(m->bitmap, m->root.total_blocks, table_rel,
                         m->root.obj_table_blocks);
        gnfs_free_blocks(m->bitmap, m->root.total_blocks, refmap_rel,
                         m->root.bitmap_blocks);
        kfree(refmap);
        return -EIO;
    }

    /* snap_find left the directory in SCR_SNAP; `slot` points into it. */
    zero((uint8 *)slot, sizeof(*slot));
    slot->txg             = m->root.txg;
    slot->table_block     = start + table_rel;
    slot->table_blocks    = m->root.obj_table_blocks;
    slot->refmap_block    = start + refmap_rel;
    slot->refmap_blocks   = m->root.bitmap_blocks;
    slot->max_objects     = m->root.max_objects;
    slot->root_dir_objnum = m->root.root_dir_objnum;
    slot->name_len        = (uint8)name_len;
    for (k = 0; k < name_len; k++) {
        slot->name[k] = name[k];
    }
    rc = snapdir_write(m);
    if (rc != 0) {
        kfree(refmap);
        return rc;
    }
    for (k = 0; k < bmp_bytes; k++) {
        m->held[k] |= refmap[k];
    }
    kfree(refmap);
    return gnfs_commit(m);
}

static int snap_delete(gnfs_mount_t *m, const char *name, uint64 name_len) {
    uint64 start = gnfs_alloc_region_start(&m->root);
    gnfs_snap_t *s;
    int idx = snap_find(m, name, name_len);
    int rc;

    if (idx < 0) {
        return idx;
    }
    s = snap_rec(m, idx);
    gnfs_free_blocks(m->bitmap, m->root.total_blocks, s->table_block - start,
                     s->table_blocks);
    gnfs_free_blocks(m->bitmap, m->root.total_blocks, s->refmap_block - start,
                     s->refmap_blocks);
    zero((uint8 *)s, sizeof(*s));
    rc = snapdir_write(m);
    if (rc != 0) {
        return rc;
    }
    m->snap_cached = -1;
    rc = held_recompute(m);            /* blocks only it held are free now */
    if (rc != 0) {
        return rc;
    }
    return gnfs_commit(m);
}

/* Is `abs_path` "/.snapshots" (kind 1), or inside it (kind 2, with the
 * snapshot's name and the path within it), or neither (0)? */
static int snap_path(const char *abs_path, const char **name,
                     uint64 *name_len, const char **rest) {
    uint64 i;

    if (abs_path[0] != '/') {
        return 0;
    }
    for (i = 0; i < SNAPDIR_NAME_LEN; i++) {
        if (abs_path[1 + i] != SNAPDIR_NAME[i]) {
            return 0;
        }
    }
    if (abs_path[1 + SNAPDIR_NAME_LEN] == '\0') {
        return 1;
    }
    if (abs_path[1 + SNAPDIR_NAME_LEN] != '/') {
        return 0;
    }
    *name = abs_path + 2 + SNAPDIR_NAME_LEN;
    i = 0;
    while ((*name)[i] != '\0' && (*name)[i] != '/') {
        i++;
    }
    *name_len = i;
    *rest = (*name)[i] == '\0' ? "/" : *name + i;
    return 2;
}

/* --- fs_ops_t ---------------------------------------------------------------------- */

static void fill_node(fs_node_t *out, const gnfs_onode_t *o, uint64 objnum,
                      int is_dir, int kind, int snap, uint64 snap_txg) {
    out->size   = o->size;
    out->ino    = objnum | ((uint64)kind << 56) | ((uint64)(snap & 0xFF) << 48);
    out->is_dir = is_dir;
    out->mode   = o->mode;
    out->uid    = o->uid;
    out->gid    = o->gid;
    out->readonly = (kind != NODE_LIVE);
    node_set(out, objnum, kind, snap, snap_txg);
}

static int gnfs_op_lookup(fs_volume_t *v, const char *abs_path,
                          fs_node_t *out) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_view_t view;
    const char *name = NULL, *rest = NULL;
    uint64 name_len = 0, objnum;
    int is_dir, sp, rc;
    gnfs_onode_t *o;

    sp = snap_path(abs_path, &name, &name_len, &rest);
    if (sp == 1) {
        /* The virtual directory itself: root-owned 0755, so only root or
         * the supreme uid can take or remove a snapshot (fs_may_add_entry
         * asks for write on it), and anyone can list and read them. */
        out->size   = 0;
        out->ino    = (uint64)NODE_SNAPDIR << 56;
        out->is_dir = 1;
        out->mode   = 0040755u;
        out->uid    = 0;
        out->gid    = 0;
        out->readonly = 0;   /* mkdir/rmdir in it are how snapshots are made */
        node_set(out, 0, NODE_SNAPDIR, 0, 0);
        return 0;
    }
    if (sp == 2) {
        int idx = snap_find(m, name, name_len);
        uint64 txg;

        if (idx < 0) {
            return idx;
        }
        txg = snap_rec(m, idx)->txg;
        rc = snap_view(m, idx, txg, &view);
        if (rc != 0) {
            return rc;
        }
        rc = resolve_in(m, &view, rest, &objnum, &is_dir);
        if (rc != 0) {
            return rc;
        }
        o = view_onode(&view, objnum);
        if (o == NULL) {
            return -EIO;
        }
        fill_node(out, o, objnum, is_dir, NODE_SNAP, idx, txg);
        return 0;
    }

    live_view(m, &view);
    rc = resolve_in(m, &view, abs_path, &objnum, &is_dir);
    if (rc != 0) {
        return rc;
    }
    o = view_onode(&view, objnum);
    if (o == NULL) {
        return -EIO;
    }
    fill_node(out, o, objnum, is_dir, NODE_LIVE, 0, 0);
    return 0;
}

/* The onode a node names, in whichever view it belongs to; NULL for the
 * snapshot directory (which has none) or a stale snapshot node. */
static gnfs_onode_t *node_onode(gnfs_mount_t *m, const fs_node_t *n) {
    gnfs_view_t view;

    switch (node_kind(n)) {
    case NODE_LIVE:
        return live_onode(m, node_objnum(n));
    case NODE_SNAP:
        if (snap_view(m, node_snap(n), node_snap_txg(n), &view) != 0) {
            return NULL;
        }
        return view_onode(&view, node_objnum(n));
    default:
        return NULL;
    }
}

static int64 gnfs_op_read(fs_volume_t *v, const fs_node_t *n, uint64 offset,
                          void *buf, uint64 max) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o;

    if (n->is_dir) {
        return -EISDIR;
    }
    o = node_onode(m, n);
    if (o == NULL) {
        return -EIO;
    }
    return gnfs_read_data(m, o, offset, buf, max);
}

static int64 gnfs_op_write(fs_volume_t *v, fs_node_t *n, uint64 offset,
                          const void *buf, uint64 max) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o;
    int rc;

    if (node_kind(n) != NODE_LIVE) {
        return -EROFS;                 /* a snapshot is read-only */
    }
    o = live_onode(m, node_objnum(n));
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
    gnfs_iter_adapt_t adapt;
    gnfs_onode_t *o;

    adapt.cb  = cb;
    adapt.ctx = ctx;
    if (node_kind(dir) == NODE_SNAPDIR) {
        int i, rc = snapdir_load(m);

        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < (int)GNFS_SNAPS_PER_BLOCK; i++) {
            gnfs_snap_t s = *snap_rec(m, i);

            if (s.txg == 0) {
                continue;
            }
            rc = gnfs_iter_adapt_cb(s.name, s.name_len, s.root_dir_objnum,
                                    1, &adapt);
            if (rc != 0) {
                return rc;
            }
            rc = snapdir_load(m);      /* the callback may have run I/O */
            if (rc != 0) {
                return rc;
            }
        }
        return 0;
    }
    o = node_onode(m, dir);
    if (o == NULL) {
        return -EIO;
    }
    return dir_iterate(m, o, gnfs_iter_adapt_cb, &adapt);
}

static int gnfs_op_statfs(fs_volume_t *v, fs_statfs_t *out) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint64 free_blocks = 0;
    uint64 i;

    for (i = 0; i < m->root.total_blocks; i++) {
        if (block_free(m, i)) {
            free_blocks++;
        }
    }
    out->block_size  = GNFS_BLOCK_SIZE;
    out->blocks      = m->root.total_blocks;
    out->blocks_free = free_blocks;
    out->name_max    = GNFS_NAME_MAX;
    return 0;
}

/* --- ACL storage: one serialised acl_t per object, in its own COW block ---- */

typedef char gnfs_acl_fits_block[(sizeof(acl_t) <= GNFS_BLOCK_SIZE) ? 1 : -1];

/* Just the STORED ACL, or -ENOENT if there is none - the exact shape
 * fs_ops_t::getacl promises, so fs_getacl's projection fallback applies. */
static int gnfs_read_stored_acl(gnfs_mount_t *m, const gnfs_onode_t *o,
                                acl_t *out) {
    const acl_t *stored;
    int rc;

    if (o->acl_block == 0) {
        return -ENOENT;
    }
    rc = read_block(m, o->acl_block, m->scr[SCR_DATA]);
    if (rc != 0) {
        return rc;
    }
    stored = (const acl_t *)(const void *)m->scr[SCR_DATA];
    if (stored->count > ACL_ACE_MAX) {
        return -EIO;       /* corrupt - refused, not read past ace[] */
    }
    *out = *stored;
    return 0;
}

static int gnfs_effective_acl(gnfs_mount_t *m, const gnfs_onode_t *o,
                              acl_t *out) {
    int rc = gnfs_read_stored_acl(m, o, out);

    if (rc == -ENOENT) {
        acl_from_mode(o->mode, o->uid, o->gid, out);
        return 0;
    }
    return rc;
}

/* Write `a` into a fresh COW block and point `o` at it; refresh o->mode
 * from it (special bits from `special`, or kept for FS_SPECIAL_KEEP). */
static int gnfs_store_acl(gnfs_mount_t *m, gnfs_onode_t *o, const acl_t *a,
                          uint32 special) {
    uint8 *block = m->scr[SCR_DATA];
    uint64 old_block = o->acl_block;
    uint64 new_block;
    int rc;

    zero(block, GNFS_BLOCK_SIZE);
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

/* Give a new object its parent's inheritable entries, if any. Failures are
 * swallowed: no inherited entries is a narrower grant, never a wider one. */
static void gnfs_apply_inheritance(gnfs_mount_t *m, uint64 parent_objnum,
                                   gnfs_onode_t *child, int child_is_dir) {
    gnfs_onode_t *parent = live_onode(m, parent_objnum);
    acl_t parent_acl;
    acl_t child_acl;

    if (parent == NULL || gnfs_effective_acl(m, parent, &parent_acl) != 0) {
        return;
    }
    acl_inherit(&parent_acl, child_is_dir, child->mode, child->uid,
               child->gid, &child_acl);
    if (child_acl.trivial) {
        return;
    }
    gnfs_store_acl(m, child, &child_acl, FS_SPECIAL_KEEP);
}

static int gnfs_op_getacl(fs_volume_t *v, const fs_node_t *n,
                          struct acl *out) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o;

    if (node_kind(n) == NODE_SNAPDIR) {
        return -ENOENT;                /* projected from its 0755 mode */
    }
    o = node_onode(m, n);
    if (o == NULL) {
        return -EIO;
    }
    return gnfs_read_stored_acl(m, o, (acl_t *)out);
}

static int gnfs_op_setacl(fs_volume_t *v, fs_node_t *n,
                          const struct acl *a, uint32 special) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    const acl_t *acl = (const acl_t *)a;
    gnfs_onode_t *o;
    int rc;

    if (node_kind(n) != NODE_LIVE) {
        return -EROFS;
    }
    o = live_onode(m, node_objnum(n));
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

/* Both copies of the owner move - a stored ACL's first, so a failure leaves
 * nothing changed and the onode never disagrees with its own ACL. */
static int gnfs_op_setowner(fs_volume_t *v, fs_node_t *n, uint32 uid,
                            uint32 gid) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o;
    acl_t stored;
    int rc;

    if (node_kind(n) != NODE_LIVE) {
        return -EROFS;
    }
    o = live_onode(m, node_objnum(n));
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

/* Who owns a new object: the creator's euid, and its egid - or a setgid
 * parent's group, with a new directory born setgid too (System V / Linux).
 * NULL `c` is the kernel, which is root's. */
static void gnfs_creator_ids(gnfs_mount_t *m, uint64 parent_objnum,
                             const struct cred *c, int is_dir, uint32 *mode,
                             uint32 *uid, uint32 *gid) {
    const cred_t *cr = (const cred_t *)c;
    gnfs_onode_t *parent = live_onode(m, parent_objnum);

    *uid = (cr != NULL) ? cr->euid : 0;
    *gid = (cr != NULL) ? cr->egid : 0;
    if (parent != NULL && (parent->mode & S_ISGID)) {
        *gid = parent->gid;
        if (is_dir) {
            *mode |= S_ISGID;
        }
    }
}

/* Paths inside /.snapshots are a read-only view, and the name ".snapshots"
 * in the root is reserved for it. */
static int path_is_readonly(const char *abs_path) {
    const char *name, *rest;
    uint64 name_len;

    return snap_path(abs_path, &name, &name_len, &rest) != 0;
}

static int make_object(gnfs_mount_t *m, const char *abs_path,
                       const struct cred *c, uint32 mode, int is_dir) {
    uint64 parent_objnum, leaf_len, new_objnum;
    const char *leaf;
    gnfs_onode_t *o;
    uint32 uid, gid;
    int rc;

    rc = resolve_parent(m, abs_path, &parent_objnum, &leaf, &leaf_len);
    if (rc != 0) {
        return rc;
    }
    rc = gnfs_onode_alloc(&m->root, m->obj_table, &new_objnum);
    if (rc != 0) {
        return rc;
    }
    o = live_onode(m, new_objnum);
    gnfs_creator_ids(m, parent_objnum, c, is_dir, &mode, &uid, &gid);
    gnfs_onode_init(o, mode, uid, gid);
    gnfs_apply_inheritance(m, parent_objnum, o, is_dir);

    rc = dir_add(m, parent_objnum, leaf, leaf_len, new_objnum, is_dir);
    if (rc != 0) {
        /* Nothing refers to the new object yet - give its slot and any ACL
         * block straight back rather than leaking them. */
        gnfs_cow_free_block(m, o->acl_block);
        gnfs_onode_free(o);
        return rc;
    }
    return gnfs_commit(m);
}

static int gnfs_op_create(fs_volume_t *v, const char *abs_path,
                          const struct cred *c, uint32 perm) {
    if (path_is_readonly(abs_path)) {
        return -EROFS;
    }
    return make_object((gnfs_mount_t *)v->body, abs_path, c,
                       S_IFREG | (perm & 0777u), 0);
}

static int gnfs_op_mkdir(fs_volume_t *v, const char *abs_path,
                         const struct cred *c, uint32 perm) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    const char *name, *rest;
    uint64 name_len;
    int sp = snap_path(abs_path, &name, &name_len, &rest);

    if (sp == 1) {
        return -EEXIST;                /* /.snapshots always exists */
    }
    if (sp == 2) {
        if (rest[1] != '\0') {
            return -EROFS;             /* inside a snapshot */
        }
        return snap_create(m, name, name_len);
    }
    return make_object(m, abs_path, c, S_IFDIR | (perm & (0777u | S_ISVTX)),
                       1);
}

static int removal_target(gnfs_mount_t *m, const char *abs_path,
                          uint64 *parent_objnum, const char **leaf,
                          uint64 *leaf_len, uint64 *target, int *is_dir) {
    gnfs_onode_t *parent;
    int rc = resolve_parent(m, abs_path, parent_objnum, leaf, leaf_len);

    if (rc != 0) {
        return rc;
    }
    parent = live_onode(m, *parent_objnum);
    if (parent == NULL) {
        return -EIO;
    }
    return dir_find(m, parent, *leaf, *leaf_len, target, is_dir);
}

static int gnfs_op_unlink(fs_volume_t *v, const char *abs_path) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint64 parent_objnum, leaf_len, target;
    const char *leaf;
    int is_dir, rc;

    if (path_is_readonly(abs_path)) {
        return -EROFS;
    }
    rc = removal_target(m, abs_path, &parent_objnum, &leaf, &leaf_len,
                        &target, &is_dir);
    if (rc != 0) {
        return rc;
    }
    if (is_dir) {
        return -EISDIR;
    }
    rc = dir_remove(m, parent_objnum, leaf, leaf_len);
    if (rc != 0) {
        return rc;
    }
    rc = free_object(m, target);
    if (rc != 0) {
        return rc;
    }
    return gnfs_commit(m);
}

static int gnfs_op_rmdir(fs_volume_t *v, const char *abs_path) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint64 parent_objnum, leaf_len, target;
    const char *leaf, *name, *rest;
    uint64 name_len;
    int is_dir, empty, rc;
    int sp = snap_path(abs_path, &name, &name_len, &rest);

    if (sp == 1) {
        return -EBUSY;                 /* the snapshot directory stays */
    }
    if (sp == 2) {
        if (rest[1] != '\0') {
            return -EROFS;
        }
        return snap_delete(m, name, name_len);
    }
    rc = removal_target(m, abs_path, &parent_objnum, &leaf, &leaf_len,
                        &target, &is_dir);
    if (rc != 0) {
        return rc;
    }
    if (!is_dir) {
        return -ENOTDIR;
    }
    if (target == m->root.root_dir_objnum) {
        return -EINVAL;
    }
    rc = dir_is_empty(m, live_onode(m, target), &empty);
    if (rc != 0) {
        return rc;
    }
    if (!empty) {
        return -ENOTEMPTY;
    }
    rc = dir_remove(m, parent_objnum, leaf, leaf_len);
    if (rc != 0) {
        return rc;
    }
    rc = free_object(m, target);
    if (rc != 0) {
        return rc;
    }
    return gnfs_commit(m);
}

/* rename(2), within this volume (fs_rename has already refused crossing
 * volumes and checked the caller may remove and add these names).
 *
 * POSIX's rules: renaming onto an existing name REPLACES it - a file may
 * replace a file, a directory may replace an EMPTY directory, and nothing
 * else mixes (-EISDIR / -ENOTDIR); two names for the same object is a
 * successful no-op; and a directory may not move inside itself (-EINVAL),
 * which is a path-prefix question here because paths are normalized. */
static int path_is_inside(const char *inner, const char *outer) {
    uint64 i = 0;

    while (outer[i] != '\0') {
        if (inner[i] != outer[i]) {
            return 0;
        }
        i++;
    }
    return inner[i] == '/';
}

static int gnfs_op_rename(fs_volume_t *v, const char *old_path,
                          const char *new_path) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    uint64 old_parent, old_leaf_len, obj;
    uint64 new_parent, new_leaf_len, existing;
    const char *old_leaf, *new_leaf;
    int is_dir, existing_is_dir, rc;
    gnfs_onode_t *np;

    if (path_is_readonly(old_path) || path_is_readonly(new_path)) {
        return -EROFS;
    }
    rc = removal_target(m, old_path, &old_parent, &old_leaf, &old_leaf_len,
                        &obj, &is_dir);
    if (rc != 0) {
        return rc;
    }
    if (obj == m->root.root_dir_objnum) {
        return -EINVAL;
    }
    if (is_dir && path_is_inside(new_path, old_path)) {
        return -EINVAL;
    }
    rc = resolve_parent(m, new_path, &new_parent, &new_leaf, &new_leaf_len);
    if (rc != 0) {
        return rc;
    }
    np = live_onode(m, new_parent);
    if (np == NULL) {
        return -EIO;
    }
    rc = dir_find(m, np, new_leaf, new_leaf_len, &existing, &existing_is_dir);
    if (rc == 0) {
        if (existing == obj) {
            return 0;                  /* the same object - nothing to do */
        }
        if (is_dir && !existing_is_dir) {
            return -ENOTDIR;
        }
        if (!is_dir && existing_is_dir) {
            return -EISDIR;
        }
        if (existing_is_dir) {
            int empty;

            rc = dir_is_empty(m, live_onode(m, existing), &empty);
            if (rc != 0) {
                return rc;
            }
            if (!empty) {
                return -ENOTEMPTY;
            }
        }
        rc = dir_remove(m, new_parent, new_leaf, new_leaf_len);
        if (rc != 0) {
            return rc;
        }
        rc = free_object(m, existing);
        if (rc != 0) {
            return rc;
        }
    } else if (rc != -ENOENT) {
        return rc;
    }

    rc = dir_remove(m, old_parent, old_leaf, old_leaf_len);
    if (rc != 0) {
        return rc;
    }
    rc = dir_add(m, new_parent, new_leaf, new_leaf_len, obj, is_dir);
    if (rc != 0) {
        /* Put it back where it was rather than lose the object. */
        (void)dir_add(m, old_parent, old_leaf, old_leaf_len, obj, is_dir);
        return rc;
    }
    return gnfs_commit(m);
}

static int gnfs_op_truncate(fs_volume_t *v, fs_node_t *n, uint64 size) {
    gnfs_mount_t *m = (gnfs_mount_t *)v->body;
    gnfs_onode_t *o;
    int rc;

    if (node_kind(n) != NODE_LIVE) {
        return -EROFS;
    }
    o = live_onode(m, node_objnum(n));
    if (o == NULL) {
        return -EIO;
    }
    if (size > GNFS_MAX_FILE_BLOCKS * GNFS_BLOCK_SIZE) {
        return -EFBIG;
    }
    if (size < o->size) {
        rc = shrink_to(m, o, size);
        if (rc != 0) {
            return rc;
        }
    } else if (size > o->size) {
        /* Growing is a hole: the new range maps nothing and reads as zero,
         * and the tail of the old last block is already zero past the old
         * size (shrink_to and gnfs_write_data keep that true). */
        o->size    = size;
        o->nblocks = (size + GNFS_BLOCK_SIZE - 1) / GNFS_BLOCK_SIZE;
    }
    rc = gnfs_commit(m);
    if (rc != 0) {
        return rc;
    }
    n->size = o->size;
    return 0;
}

static void free_mount_memory(gnfs_mount_t *mt) {
    int i;

    kfree(mt->bitmap);
    kfree(mt->held);
    kfree(mt->obj_table);
    kfree(mt->obt_shadow);
    kfree(mt->obt_dirty);
    kfree(mt->snap_table);
    mt->bitmap = mt->held = mt->obj_table = NULL;
    mt->obt_shadow = mt->obt_dirty = mt->snap_table = NULL;
    for (i = 0; i < SCR_COUNT; i++) {
        kfree(mt->scr[i]);
        mt->scr[i] = NULL;
    }
}

static void gnfs_op_unmount(fs_volume_t *v) {
    int slot;

    for (slot = 0; slot < GNFS_MAX; slot++) {
        if (&gnfs_fs_slots[slot] == v) {
            /* Retired, not freed as a SLOT - a handle opened before unmount
             * may still hold an fs_node_t pointing at this fs_volume_t, and
             * `mounted` (already clear) is what stops it being used. The
             * mount's own memory is its cache of the volume and goes. */
            free_mount_memory(&gnfs_mounts[slot]);
            gnfs_retired[slot] = 1;
            return;
        }
    }
}

static const fs_ops_t gnfs_ops = {
    /* Designated initializers throughout: adding a slot to a positional
     * initializer risks every field after it silently shifting by one. */
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
    .rename   = gnfs_op_rename,
    .unmount  = gnfs_op_unmount,
    .getacl   = gnfs_op_getacl,
    .setacl   = gnfs_op_setacl,
    .setowner = gnfs_op_setowner
};

/* --- the prober ------------------------------------------------------------------
 *
 * Silent for "not gnfs", as the FAT prober is: this runs against every volume
 * on the machine, and a prober that complains about every volume it is not is
 * a prober nobody reads. */
fs_volume_t *gnfs_probe(device_t *dev) {
    gnfs_mount_t *mt;
    gnfs_root_t root;
    uint64 bmp_bytes, obt_bytes;
    int slot, i;

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

    mt = &gnfs_mounts[slot];
    bmp_bytes = bytes_of(root.bitmap_blocks);
    obt_bytes = bytes_of(root.obj_table_blocks);
    mt->bitmap     = (uint8 *)kmalloc(bmp_bytes);
    mt->held       = (uint8 *)kmalloc(bmp_bytes);
    mt->obj_table  = (uint8 *)kmalloc(obt_bytes);
    mt->obt_shadow = (uint8 *)kmalloc(obt_bytes);
    mt->obt_dirty  = (uint8 *)kmalloc(root.obj_table_blocks);
    mt->snap_table = (uint8 *)kmalloc(obt_bytes);
    for (i = 0; i < SCR_COUNT; i++) {
        mt->scr[i] = (uint8 *)kmalloc(GNFS_BLOCK_SIZE);
    }
    {
        int ok = mt->bitmap && mt->held && mt->obj_table && mt->obt_shadow &&
                 mt->obt_dirty && mt->snap_table;

        for (i = 0; i < SCR_COUNT; i++) {
            ok = ok && mt->scr[i] != NULL;
        }
        if (!ok ||
            dev_read(dev, root.bitmap_block * GNFS_BLOCK_SIZE, mt->bitmap,
                     bmp_bytes) != (int64)bmp_bytes ||
            dev_read(dev, root.obj_table_block * GNFS_BLOCK_SIZE,
                     mt->obj_table, obt_bytes) != (int64)obt_bytes) {
            free_mount_memory(mt);
            return NULL;
        }
    }
    copy(mt->obt_shadow, mt->obj_table, obt_bytes);
    /* The region the root names holds exactly this table; the OTHER one is
     * stale by an unknown amount, so every block is owed to it. */
    {
        uint8 other = (root.obj_table_block == gnfs_obj_table_region_a(&root))
            ? 2u : 1u;
        uint64 b;

        for (b = 0; b < root.obj_table_blocks; b++) {
            mt->obt_dirty[b] = other;
        }
    }
    mt->dev         = dev;
    mt->root        = root;
    mt->snap_cached = -1;
    if (held_recompute(mt) != 0) {
        free_mount_memory(mt);
        return NULL;
    }

    gnfs_retired[slot] = 0;
    gnfs_fs_slots[slot].ops        = &gnfs_ops;
    gnfs_fs_slots[slot].body       = mt;
    gnfs_fs_slots[slot].block_size = GNFS_BLOCK_SIZE;
    gnfs_fs_slots[slot].mounted    = 1;
    gnfs_used[slot] = 1;

    kprintf("gnfs: volume on %s, txg %lx, %lx allocatable blocks, %lx "
           "objects in use of %lx\n",
           dev->ns_name, root.txg, root.total_blocks,
           gnfs_objects_in_use(mt->obj_table, root.max_objects),
           root.max_objects);
    return &gnfs_fs_slots[slot];
}

void gnfs_init(void) {
    volume_register_fs("gnfs", gnfs_probe);
}
