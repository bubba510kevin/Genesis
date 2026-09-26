#include "gnfs_layout.h"
#include "typesk.h"

/* The pure half of gnfs: checksums, root records, the bitmap allocator, and
 * formatting. No I/O, no allocation, no device - see kernel/fs/acl.c's own
 * header comment for why that is worth stating explicitly: it is what lets
 * tests/host/gnfs_test.c check every rule here with no disk and no kernel,
 * and it is what lets gnfs_format be called identically from
 * kernel/gnfs/gnfs_vfs.c (writing through dev_write) and tools/mkgnfs.c
 * (writing through fwrite).
 */

#define ENOSPC 28

/* --- checksum --------------------------------------------------------------- */

uint64 gnfs_checksum(const void *data, uint64 len) {
    const uint8 *p = (const uint8 *)data;
    /* FNV-1a's own published constants for the 64-bit variant. Not derived,
     * not tunable - a checksum whose constants drift between the writer and
     * the reader is a checksum that always mismatches, so there is exactly
     * one place these two numbers are written down. */
    uint64 hash = 0xCBF29CE484222325ULL;
    uint64 i;

    for (i = 0; i < len; i++) {
        hash ^= (uint64)p[i];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

/* --- root records ------------------------------------------------------------ */

void gnfs_root_init(gnfs_root_t *r, uint64 total_blocks,
                    uint64 bitmap_block, uint64 bitmap_blocks,
                    uint64 obj_table_block, uint64 obj_table_blocks,
                    uint64 max_objects, uint64 root_dir_objnum) {
    uint64 i;

    r->magic           = GNFS_MAGIC;
    r->version         = GNFS_VERSION;
    r->block_size      = GNFS_BLOCK_SIZE;
    r->txg             = 1;
    r->total_blocks    = total_blocks;
    r->bitmap_block    = bitmap_block;
    r->bitmap_blocks   = bitmap_blocks;
    r->obj_table_block  = obj_table_block;
    r->obj_table_blocks = obj_table_blocks;
    r->max_objects      = max_objects;
    r->root_dir_objnum  = root_dir_objnum;
    r->next_objnum      = root_dir_objnum + 1;   /* the root dir is already
                                                  * taken - see this
                                                  * function's own header */
    r->checksum      = 0;
    for (i = 0; i < sizeof(r->pad); i++) {
        r->pad[i] = 0;
    }
    gnfs_root_seal(r);
}

void gnfs_root_seal(gnfs_root_t *r) {
    r->checksum = 0;
    r->checksum = gnfs_checksum(r, sizeof(*r));
}

int gnfs_root_valid(const gnfs_root_t *r) {
    uint64 want;
    gnfs_root_t tmp;

    if (r->magic != GNFS_MAGIC) {
        return 0;
    }
    if (r->version != GNFS_VERSION) {
        return 0;
    }
    if (r->block_size != GNFS_BLOCK_SIZE) {
        return 0;
    }
    /* Same reasoning as block_size just above: a build with a different
     * compiled-in object-table size must refuse a volume sized for another
     * one rather than silently misreading its layout. */
    if (r->max_objects != (uint64)GNFS_MAX_OBJECTS ||
        r->obj_table_blocks != (uint64)GNFS_OBJ_TABLE_BLOCKS) {
        return 0;
    }
    /* Recompute over a copy with checksum zeroed, rather than trying to
     * subtract the stored checksum's own contribution back out - the same
     * reason gnfs_root_seal computes into a zeroed field instead of an
     * incremental update. Simple and correct beats clever for something a
     * corrupted disk gets to feed adversarial input to. */
    tmp = *r;
    tmp.checksum = 0;
    want = gnfs_checksum(&tmp, sizeof(tmp));
    return want == r->checksum;
}

/* --- the fixed layout, derived rather than stored --------------------------- */

uint64 gnfs_bitmap_region_a(const gnfs_root_t *r) {
    (void)r;    /* fixed, not derived from the record - kept as a parameter
                * for symmetry with the rest of this family and so a future
                * change (a variable-size ring, say) has somewhere to read
                * from without changing every caller's signature */
    return (uint64)GNFS_RING_SLOTS;
}

uint64 gnfs_bitmap_region_b(const gnfs_root_t *r) {
    return gnfs_bitmap_region_a(r) + r->bitmap_blocks;
}

uint64 gnfs_obj_table_region_a(const gnfs_root_t *r) {
    return gnfs_bitmap_region_b(r) + r->bitmap_blocks;
}

uint64 gnfs_obj_table_region_b(const gnfs_root_t *r) {
    return gnfs_obj_table_region_a(r) + r->obj_table_blocks;
}

uint64 gnfs_alloc_region_start(const gnfs_root_t *r) {
    return gnfs_obj_table_region_b(r) + r->obj_table_blocks;
}

/* --- the bitmap --------------------------------------------------------------- */

uint64 gnfs_bitmap_bytes(uint64 total_blocks) {
    return (total_blocks + 7) / 8;
}

void gnfs_bitmap_set(uint8 *bitmap, uint64 blk) {
    bitmap[blk / 8] |= (uint8)(1u << (blk % 8));
}

void gnfs_bitmap_clear(uint8 *bitmap, uint64 blk) {
    bitmap[blk / 8] &= (uint8)~(1u << (blk % 8));
}

int gnfs_bitmap_test(const uint8 *bitmap, uint64 blk) {
    return (bitmap[blk / 8] >> (blk % 8)) & 1u;
}

int gnfs_alloc_blocks(uint8 *bitmap, uint64 total_blocks, uint64 count,
                      uint64 *out_start) {
    uint64 start;
    uint64 run;
    uint64 i;

    if (count == 0 || count > total_blocks) {
        return -ENOSPC;
    }

    start = 0;
    run = 0;
    for (i = 0; i < total_blocks; i++) {
        if (gnfs_bitmap_test(bitmap, i)) {
            run = 0;
            start = i + 1;
            continue;
        }
        run++;
        if (run == count) {
            for (i = start; i < start + count; i++) {
                gnfs_bitmap_set(bitmap, i);
            }
            *out_start = start;
            return 0;
        }
    }
    return -ENOSPC;
}

void gnfs_free_blocks(uint8 *bitmap, uint64 total_blocks, uint64 start,
                      uint64 count) {
    uint64 i;

    (void)total_blocks;
    for (i = start; i < start + count; i++) {
        gnfs_bitmap_clear(bitmap, i);
    }
}

/* --- formatting ---------------------------------------------------------------
 *
 * Layout, in blocks from the start of the volume:
 *   [0, R)                     the root record ring (R = GNFS_RING_SLOTS)
 *   [R, R+B)                   bitmap region A
 *   [R+B, R+2B)                bitmap region B
 *   [R+2B, R+2B+T)             object-table region A
 *   [R+2B+T, R+2B+2T)          object-table region B
 *   [R+2B+2T, total_blocks)    allocatable - block 0 of this region is the
 *                              root directory's data block, allocated here
 * where B is bitmap_blocks and T is GNFS_OBJ_TABLE_BLOCKS, both sized for
 * what is left after everything before them - none of the ring, bitmap or
 * object-table regions are ever freed or reallocated, so none of them needs
 * a bit of its own or an object number of its own. */
int gnfs_format(void *ctx, gnfs_write_fn write, uint64 total_bytes) {
    uint64 total_blocks_all;
    uint64 alloc_blocks;
    uint64 bitmap_blocks;
    uint64 bitmap_a, obj_table_a, alloc_start;
    /* One block at a time, reused for every block of every region below -
     * never one buffer sized for a whole region. gnfs_format promises "no
     * allocation" in this file's own header comment, and GNFS_OBJ_TABLE_
     * BLOCKS * GNFS_BLOCK_SIZE (32KB at the current GNFS_MAX_OBJECTS) would
     * not fit this kernel's 16KB stacks even if the promise were dropped. */
    uint8 block[GNFS_BLOCK_SIZE];
    uint64 i;
    gnfs_root_t root;
    int64 rc;

    total_blocks_all = total_bytes / GNFS_BLOCK_SIZE;
    if (total_blocks_all <= GNFS_RING_SLOTS) {
        return -ENOSPC;
    }

    /* Solve for bitmap_blocks such that the allocatable region it describes
     * is everything left over after the ring, two copies of itself, and two
     * copies of the object table. Sized generously (one bit could otherwise
     * describe a block that is itself bitmap or object-table storage) rather
     * than solved exactly - this runs once, at format time, and clarity here
     * costs nothing a mount ever pays for. */
    alloc_blocks = total_blocks_all - GNFS_RING_SLOTS;
    bitmap_blocks = (gnfs_bitmap_bytes(alloc_blocks) + GNFS_BLOCK_SIZE - 1) /
                    GNFS_BLOCK_SIZE;
    if (bitmap_blocks * 2 + (uint64)GNFS_OBJ_TABLE_BLOCKS * 2 + 1 >=
        alloc_blocks) {
        return -ENOSPC;             /* the volume is too small to hold itself */
    }
    alloc_blocks -= bitmap_blocks * 2 + (uint64)GNFS_OBJ_TABLE_BLOCKS * 2;
    alloc_blocks -= 1;              /* the root directory's own data block */

    /* A draft record carrying only the two fields gnfs_bitmap_region_a and
     * friends actually read, so this arithmetic is computed in exactly one
     * place (those four functions) rather than duplicated here by hand - the
     * real root record does not exist yet, since it needs alloc_start, which
     * these produce. */
    {
        gnfs_root_t draft;
        draft.bitmap_blocks    = bitmap_blocks;
        draft.obj_table_blocks = (uint64)GNFS_OBJ_TABLE_BLOCKS;
        bitmap_a    = gnfs_bitmap_region_a(&draft);
        obj_table_a = gnfs_obj_table_region_a(&draft);
        /* Region B of each is left exactly as `write` already had it - the
         * first commit is what writes it - so this function never needs
         * gnfs_obj_table_region_b's own answer. */
        alloc_start = gnfs_alloc_region_start(&draft);
    }

    /* Bitmap region A: block 0's bit 0 set (the root directory's data block,
     * allocated below), everything else free. */
    for (i = 0; i < sizeof(block); i++) {
        block[i] = 0;
    }
    gnfs_bitmap_set(block, 0);
    rc = write(ctx, bitmap_a * GNFS_BLOCK_SIZE, block, GNFS_BLOCK_SIZE);
    if (rc != (int64)GNFS_BLOCK_SIZE) {
        return (rc < 0) ? (int)rc : -5;
    }
    for (i = 0; i < sizeof(block); i++) {
        block[i] = 0;               /* back to all-zero for the rest */
    }
    for (i = 1; i < bitmap_blocks; i++) {
        rc = write(ctx, (bitmap_a + i) * GNFS_BLOCK_SIZE, block,
                  GNFS_BLOCK_SIZE);
        if (rc != (int64)GNFS_BLOCK_SIZE) {
            return (rc < 0) ? (int)rc : -5;
        }
    }

    /* Object-table region A: block 0 holds the root directory's onode at its
     * natural index (GNFS_ROOT_DIR_OBJNUM), everything else in that block -
     * and every other block of the region - zero, i.e. every other object
     * slot free. */
    {
        gnfs_onode_t *root_onode = gnfs_onode_at(block, GNFS_ROOT_DIR_OBJNUM);
        gnfs_onode_init(root_onode, 0040755u /* S_IFDIR | rwxr-xr-x */, 0, 0);
        root_onode->nblocks    = 1;
        root_onode->direct[0]  = alloc_start;   /* block 0 of the allocatable
                                                 * region, absolute */
        root_onode->size       = GNFS_BLOCK_SIZE;   /* one directory block */
    }
    rc = write(ctx, obj_table_a * GNFS_BLOCK_SIZE, block, GNFS_BLOCK_SIZE);
    if (rc != (int64)GNFS_BLOCK_SIZE) {
        return (rc < 0) ? (int)rc : -5;
    }
    for (i = 0; i < sizeof(block); i++) {
        block[i] = 0;
    }
    for (i = 1; i < (uint64)GNFS_OBJ_TABLE_BLOCKS; i++) {
        rc = write(ctx, (obj_table_a + i) * GNFS_BLOCK_SIZE, block,
                  GNFS_BLOCK_SIZE);
        if (rc != (int64)GNFS_BLOCK_SIZE) {
            return (rc < 0) ? (int)rc : -5;
        }
    }

    /* The root directory's data block: all-zero IS an empty directory - see
     * gnfs_dir_init_block, whose whole job is exactly this, and which a
     * zeroed block already satisfies without calling it. */
    rc = write(ctx, alloc_start * GNFS_BLOCK_SIZE, block, GNFS_BLOCK_SIZE);
    if (rc != (int64)GNFS_BLOCK_SIZE) {
        return (rc < 0) ? (int)rc : -5;
    }

    gnfs_root_init(&root, alloc_blocks, bitmap_a, bitmap_blocks,
                  obj_table_a, (uint64)GNFS_OBJ_TABLE_BLOCKS,
                  (uint64)GNFS_MAX_OBJECTS, (uint64)GNFS_ROOT_DIR_OBJNUM);
    /* Slot (txg % GNFS_RING_SLOTS) = slot 1, for txg 1. Every other slot is
     * left as `write` already had it - gnfs_root_valid rejects an unwritten
     * slot exactly as it would a torn one, which is the property that makes
     * "leave the rest alone" a correct format rather than an incomplete
     * one. */
    rc = write(ctx, (root.txg % GNFS_RING_SLOTS) * GNFS_BLOCK_SIZE,
              &root, sizeof(root));
    if (rc != (int64)sizeof(root)) {
        return (rc < 0) ? (int)rc : -5;       /* -EIO */
    }
    return 0;
}
