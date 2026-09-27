#ifndef GNFS_LAYOUT_H
#define GNFS_LAYOUT_H

#include "typesk.h"

/* Genesis Native FS: the on-disk format and the pure logic over it.
 *
 * --- why this is not a reduced ZFS -----------------------------------------
 * ROADMAP item 7's write path was trying to stay byte-compatible with real
 * OpenZFS, and the object layer that compatibility requires - a dnode array
 * indirected through a meta-dnode six levels deep on a real pool - made every
 * dnode update copy-on-write six blocks, and recording ONE allocation (a
 * space-map append) is itself a dnode update. Writing N allocations cost
 * roughly 7N blocks and the transaction never converged. That is not a bug in
 * this tree's port of it; it is what the real format costs when nothing below
 * it defers a write to end-of-transaction the way ZFS's own DMU does.
 *
 * This format keeps the three ideas worth keeping - copy-on-write,
 * checksummed blocks, crash-consistent transaction groups, and (once the
 * object layer above this lands) snapshots as retained old roots - and drops
 * everything that made the real format expensive to get right: no space-map
 * log to replay, no multi-level meta-dnode, no compression/dedup/encryption,
 * no feature-flag negotiation. It does not aim to be read by real `zdb`.
 *
 * --- the shape ---------------------------------------------------------------
 * A fixed-size ring of root records near the start of the volume, one written
 * per commit at slot (txg % GNFS_RING_SLOTS) - exactly the uberblock ring's
 * own crash-consistency argument, reused rather than reinvented: a torn write
 * to one slot leaves every other slot, including the previous txg's, intact.
 * Mounting means finding the valid slot with the highest txg.
 *
 * A root record points at a free-space bitmap for its own txg. There are only
 * two bitmap regions on disk (not a bump allocator with unbounded copies),
 * and a commit alternates between them by txg parity - a fresh bitmap is
 * written to the region the CURRENT root record does not point at, and only
 * then does a new root record start pointing at it. The old region is left
 * alone, which is what makes a torn commit leave a mountable pool: the root
 * ring slot that would have advanced past it still names the old bitmap.
 *
 * SNAPSHOTS (version 2) are what that shape was for. A snapshot is a
 * retained view of the object graph at one txg: its own copy of the object
 * table, plus a REFERENCE MAP - the live bitmap as it stood - saying which
 * blocks that graph uses. The allocator treats a block as free only if the
 * live bitmap AND every snapshot's reference map say so, so copy-on-write
 * keeps working exactly as before (a freed block just drops out of the live
 * bitmap) while nothing a snapshot still reads can ever be handed out again.
 * Deleting a snapshot drops its map, and every block only it was holding
 * becomes free with no walk of anything. See gnfs_snap_t.
 *
 * --- the object layer, added on top of the above ---------------------------
 * The object table is ANOTHER whole-structure ping-pong, exactly like the
 * bitmap and for the same reason: a commit copies the WHOLE (small, bounded -
 * see GNFS_MAX_OBJECTS below) table to the region the current root does not
 * point at, then the new root names it. This is what real ZFS's write path
 * could not afford - copying the whole multi-level meta-dnode tree costs more
 * the deeper it is - and what this format CAN afford, because there is
 * exactly one level: an object number is a fixed-stride index into a flat
 * array, never an indirect chain of blocks pointing at blocks. Depth is what
 * broke the old cascade; this format has none to break.
 *
 * The table copy is incremental on disk even though it is whole in
 * concept: a commit writes only the table blocks that differ from what the
 * target region last received (kernel/gnfs/gnfs_vfs.c keeps the comparison),
 * which is what lets the table be thousands of objects rather than 256.
 *
 * --- version 2 -----------------------------------------------------------------
 * Version 1 capped a volume at 256 objects EVER (numbers were never reused),
 * a file or directory at 48KB, a directory at one block, and had no rename or
 * snapshots. Version 2: the object table is sized per volume and recorded,
 * freed objects are reused, files map blocks through an indirect and a
 * double-indirect block (about 1GB), directories grow through the same map,
 * and snapshots exist. A version-1 volume is refused, not misread.
 */

#define GNFS_BLOCK_SIZE   4096u
#define GNFS_RING_SLOTS   4u

/* "GNFSROOT" read as bytes off disk, least-significant byte first - spelled
 * out as a shift chain rather than a char[8] union so this header stays
 * plain C89 with no aliasing questions. */
#define GNFS_MAGIC \
    (((uint64)'G')       | ((uint64)'N') << 8  | ((uint64)'F') << 16 | \
     ((uint64)'S') << 24 | ((uint64)'R') << 32 | ((uint64)'O') << 40 | \
     ((uint64)'O') << 48 | ((uint64)'T') << 56)

#define GNFS_VERSION 2u

/* One ring slot's contents. Exactly one per GNFS_BLOCK_SIZE-aligned block -
 * padded out explicitly rather than left to the compiler, because this
 * struct is read and written as raw bytes across a translation-unit
 * boundary (this file, the host format tool, and a future systest) and an
 * implicit-padding disagreement between compilers is exactly the kind of bug
 * that would show up as "reads back different from what was written" with
 * nothing pointing at why. */
typedef struct gnfs_root {
    uint64 magic;           /* GNFS_MAGIC, or this slot is not a root record */
    uint32 version;         /* GNFS_VERSION */
    uint32 block_size;      /* GNFS_BLOCK_SIZE, recorded rather than assumed -
                             * so a mismatched build fails to recognise a disk
                             * instead of misreading it */
    uint64 txg;             /* this record's transaction group number */
    uint64 total_blocks;    /* size of the allocatable region, in blocks */
    uint64 bitmap_block;    /* which of the two bitmap regions is current */
    uint64 bitmap_blocks;   /* how many blocks that region occupies */

    /* --- the object layer --------------------------------------------------
     * obj_table_blocks and max_objects are RECORDED rather than assumed from
     * the build's own GNFS_MAX_OBJECTS/GNFS_OBJ_TABLE_BLOCKS constants, the
     * same reasoning block_size above already follows: a build with a
     * different compiled-in object-table size must refuse a mismatched
     * volume instead of misreading it as if the table were the size THIS
     * build expects. gnfs_root_valid checks both against the constants. */
    uint64 obj_table_block;    /* which of the two object-table regions is
                                * current */
    uint64 obj_table_blocks;   /* how many blocks ONE region occupies */
    uint64 max_objects;        /* object-table capacity */
    uint64 next_objnum;        /* where gnfs_onode_alloc starts looking - a
                                * HINT, not a high-water mark: freed object
                                * numbers are reused */
    uint64 root_dir_objnum;    /* which object is the root directory - always
                                * 1 in practice, stored rather than assumed
                                * for the same one-source-of-truth reason
                                * every other "obviously constant" field here
                                * is a field rather than a #define */

    uint64 snap_dir_block;     /* ABSOLUTE block holding the snapshot
                                * directory (an array of gnfs_snap_t), or 0
                                * for none. Copy-on-write like everything
                                * else: a snapshot create or delete writes a
                                * new block and the next root names it. */
    uint64 reserved[3];        /* zero; room to grow without a version bump */

    uint64 checksum;        /* over every field above, with this field itself
                             * read as 0 while computing it */
    /* Fifteen uint64 fields (120 bytes) plus two uint32 fields (8 bytes)
     * above this line, 128 bytes total with no compiler-inserted gaps. The
     * build-time check right below this struct is what enforces the pad is
     * sized correctly rather than this arithmetic being trusted silently. */
    uint8  pad[GNFS_BLOCK_SIZE - 128];
} gnfs_root_t;

typedef char gnfs_root_fits_one_block
    [(sizeof(gnfs_root_t) == GNFS_BLOCK_SIZE) ? 1 : -1];

/* --- checksum ---------------------------------------------------------------
 *
 * 64-bit FNV-1a over the bytes given. Not cryptographic - nothing here needs
 * it to be, this defends against a torn or stray write, not a forger with a
 * disk editor - and deliberately not reused from kernel/zfs/vendor/: that
 * checksum table lives on the far side of the CDDL wall and behind a
 * spa_t/zio_checksum_info_t API this format has no analogue of. Every caller
 * goes through this one function, so a stronger hash later is a one-function
 * change. */
uint64 gnfs_checksum(const void *data, uint64 len);

/* --- root records ------------------------------------------------------------ */

/* Build a fresh txg-1 root record for a newly formatted volume.
 * next_objnum starts at root_dir_objnum + 1 - the root directory is the one
 * object gnfs_format itself creates, so the allocator cursor must not hand
 * its own number back out. */
void gnfs_root_init(gnfs_root_t *r, uint64 total_blocks,
                    uint64 bitmap_block, uint64 bitmap_blocks,
                    uint64 obj_table_block, uint64 obj_table_blocks,
                    uint64 max_objects, uint64 root_dir_objnum);

/* Fill in r->checksum from every other field. Called before writing a
 * record; also what gnfs_root_valid recomputes and compares against. */
void gnfs_root_seal(gnfs_root_t *r);

/* Magic, version, and checksum all agree. Anything else - a slot that was
 * never written, a torn write, a disk that is not gnfs at all - fails this,
 * which is the only signal a prober or a mount scan gets to tell "no root
 * here" from "a root here I should trust". */
int gnfs_root_valid(const gnfs_root_t *r);

/* --- the fixed layout, derived rather than stored --------------------------
 *
 * Every region's start is a function of the ring's own fixed size and the
 * two sizes the root record already carries (bitmap_blocks, obj_table_
 * blocks) - there is no separate stored "start" field for any of them, so
 * there is nowhere for a stored value to drift from this arithmetic. Both
 * kernel/gnfs/gnfs_format.c (laying a volume down) and kernel/gnfs/
 * gnfs_vfs.c (mounting one, and translating a RELATIVE bitmap index to an
 * ABSOLUTE block number - see gnfs_onode_t's own note on that translation)
 * call these rather than each computing the same offsets by hand, which is
 * exactly the kind of duplicated arithmetic this codebase has been bitten by
 * disagreeing silently before (see acl_abi.h's own #if-verified constants for
 * the same worry in a different shape). */
uint64 gnfs_bitmap_region_a(const gnfs_root_t *r);
uint64 gnfs_bitmap_region_b(const gnfs_root_t *r);
uint64 gnfs_obj_table_region_a(const gnfs_root_t *r);
uint64 gnfs_obj_table_region_b(const gnfs_root_t *r);
/* Where the allocatable region begins - relative bitmap index 0 is this
 * block, absolute. */
uint64 gnfs_alloc_region_start(const gnfs_root_t *r);

/* --- the free-space bitmap, as pure logic over a caller-owned buffer -------
 *
 * One bit per block, set means allocated. A bitmap rather than a log:
 * there is no replay, "what is free" is a bit test, and the encoding bug
 * class ROADMAP.md already recorded once for the real space-map log - a
 * one-word entry mistaken for half of a two-word one - has no analogue here
 * because there is no entry shape to mistake. The cost is size (one bit per
 * block rather than one log entry per extent) and no history, which is
 * exactly the trade a foundation that does not yet need snapshot-aware
 * allocation should make. */

uint64 gnfs_bitmap_bytes(uint64 total_blocks);

void gnfs_bitmap_set(uint8 *bitmap, uint64 blk);
void gnfs_bitmap_clear(uint8 *bitmap, uint64 blk);
int  gnfs_bitmap_test(const uint8 *bitmap, uint64 blk);

/* First-fit: the same policy ROADMAP.md already chose and argued for on the
 * old write path - a wrong allocation POLICY costs performance, a wrong
 * allocation IMPLEMENTATION costs the pool, so simple and correct beats
 * clever here. Returns 0 and sets *out_start, or -ENOSPC. */
int gnfs_alloc_blocks(uint8 *bitmap, uint64 total_blocks, uint64 count,
                     uint64 *out_start);

void gnfs_free_blocks(uint8 *bitmap, uint64 total_blocks, uint64 start,
                     uint64 count);

/* --- the object table --------------------------------------------------------
 *
 * A flat array of fixed-size records, one per object number - no indirect
 * chain of blocks pointing at blocks, which is the property that makes
 * copying the whole table on every commit affordable (see this header's own
 * top comment for why depth, not size, is what broke the old cascade).
 *
 * The capacity is chosen PER VOLUME at format time (one object per four
 * blocks, within the bounds below) and recorded in the root record, and
 * obj_table_blocks must agree with it exactly (gnfs_obj_table_blocks_for) or
 * the volume is refused rather than misread. */
#define GNFS_MIN_OBJECTS        64u
#define GNFS_MAX_OBJECTS_LIMIT  65536u

/* The block map: 12 direct pointers (the classic Unix inode count - 48KB
 * with no indirection at all), then ONE indirect block of GNFS_PTRS_PER_BLOCK
 * pointers, then ONE double-indirect block of pointers to such blocks. That
 * is 12 + 512 + 512*512 blocks, just over 1GB. Every pointer block is copy-
 * on-write like a data block: changing one mapping writes a new pointer
 * block (and a new double-indirect block above it) and frees the old ones.
 * A mapping of 0 is a hole and reads as zeroes; a pointer block of 0 means
 * every mapping below it is a hole. Past the end is refused (-EFBIG), never
 * truncated. */
#define GNFS_OBJ_DIRECT      12u
#define GNFS_PTRS_PER_BLOCK  (GNFS_BLOCK_SIZE / 8u)
#define GNFS_MAX_FILE_BLOCKS ((uint64)GNFS_OBJ_DIRECT + GNFS_PTRS_PER_BLOCK + \
                              (uint64)GNFS_PTRS_PER_BLOCK * GNFS_PTRS_PER_BLOCK)

/* --- a block-number convention worth stating once, precisely ---------------
 *
 * gnfs_onode_t::direct[] below holds ABSOLUTE block numbers - counted from
 * the very start of the volume, the same units dev_read/dev_write's `offset`
 * (divided by GNFS_BLOCK_SIZE) already uses, and directly usable with no
 * translation by whatever reads a file's data.
 *
 * The bitmap (gnfs_bitmap_set/test/gnfs_alloc_blocks, above) is indexed
 * RELATIVE to the start of the allocatable region instead - it is pure logic
 * with no notion of where that region begins, by design, so it cannot be
 * indexed any other way. A caller that allocates a block gets a RELATIVE
 * index back and must add the allocatable region's own start (kept in
 * gnfs_mount_t on the device-facing side, kernel/gnfs/gnfs_vfs.c) before
 * storing it in a direct[] entry; freeing one is the same translation in
 * reverse. Getting this backwards - storing a relative index in direct[], or
 * testing an absolute one against the bitmap - reads or frees the wrong
 * block, silently, which is exactly the shape of bug this note exists to
 * prevent someone from writing twice. */
typedef struct gnfs_onode {
    uint32 mode;         /* S_IFMT + rwx, the same vocabulary fs_node_t uses */
    uint32 uid;
    uint32 gid;
    uint32 nlink;         /* reserved at 1 - no hard links in this format.
                           * 0 in a free slot: mode == 0 is what marks a
                           * table entry unused */
    uint64 size;          /* bytes */
    uint64 nblocks;       /* logical blocks the object spans - how far the
                           * block map is meaningful (entries inside it may
                           * still be holes) */
    uint64 direct[GNFS_OBJ_DIRECT];   /* ABSOLUTE block numbers; unused entries
                                      * are 0, which is never a valid data
                                      * block (block 0 is always inside the
                                      * ring) */
    uint64 indirect;      /* ABSOLUTE block of GNFS_PTRS_PER_BLOCK pointers
                           * for logical blocks 12..523, or 0 */
    uint64 dindirect;     /* ABSOLUTE block of pointers to such blocks, for
                           * everything after that, or 0 */
    uint64 acl_block;     /* ABSOLUTE block holding a serialised acl_t
                           * (kernel/include/acl.h), or 0 - "this object has
                           * no stored ACL", handled by kernel/fs/vfs.c's
                           * fs_getacl the same way a NULL getacl slot is: by
                           * projecting the mode instead. */
} gnfs_onode_t;

/* objnum 0 is never valid - block 0 doubles as "this onode slot is free",
 * the same trick a 0 direct[] entry plays for "this block slot is a hole".
 * objnum 1 is always the root directory (gnfs_root_t::root_dir_objnum says
 * so explicitly rather than this being assumed anywhere else). */
#define GNFS_OBJNUM_NONE 0u
#define GNFS_ROOT_DIR_OBJNUM 1u

#define GNFS_ONODES_PER_BLOCK (GNFS_BLOCK_SIZE / (uint64)sizeof(gnfs_onode_t))

/* Blocks one object-table region needs for `max_objects` entries. */
uint64 gnfs_obj_table_blocks_for(uint64 max_objects);

void gnfs_onode_init(gnfs_onode_t *o, uint32 mode, uint32 uid, uint32 gid);

/* Every byte zero - what a never-used table slot looks like, and what makes
 * it allocatable again (mode == 0). */
void gnfs_onode_free(gnfs_onode_t *o);

/* Bounds-checked access into a caller-owned object-table buffer of
 * `max_objects` entries. NULL for objnum 0 or objnum >= max_objects -
 * "refuse rather than guess". */
gnfs_onode_t *gnfs_onode_at(uint8 *table, uint64 max_objects, uint64 objnum);

/* Claim a free object number (mode == 0), searching from r->next_objnum and
 * wrapping. The slot is left for the caller to fill with gnfs_onode_init.
 * Returns 0 and sets *out, or -ENOSPC if every slot is in use. */
int gnfs_onode_alloc(gnfs_root_t *r, uint8 *table, uint64 *out);

/* How many table slots are in use (for reporting). */
uint64 gnfs_objects_in_use(const uint8 *table, uint64 max_objects);

/* --- snapshots -------------------------------------------------------------
 *
 * One record per snapshot, in the snapshot directory block the root record
 * names (gnfs_root_t::snap_dir_block). A snapshot owns two contiguous runs of
 * blocks: its copy of the object table, and its reference map (a bitmap in
 * the live bitmap's own format, describing every block the snapshot's graph
 * uses). Both runs are allocated in the live bitmap for as long as the
 * snapshot exists, and freed when it is deleted. */
#define GNFS_SNAP_NAME_MAX 40u

typedef struct gnfs_snap {
    uint64 txg;             /* the txg the snapshot captured; 0 = free slot */
    uint64 table_block;     /* ABSOLUTE start of its object-table copy */
    uint64 table_blocks;
    uint64 refmap_block;    /* ABSOLUTE start of its reference map */
    uint64 refmap_blocks;
    uint64 max_objects;
    uint64 root_dir_objnum;
    uint8  name_len;
    uint8  reserved[7];
    char   name[GNFS_SNAP_NAME_MAX];
} gnfs_snap_t;

#define GNFS_SNAPS_PER_BLOCK (GNFS_BLOCK_SIZE / (uint64)sizeof(gnfs_snap_t))

/* --- directory data, as pure logic over ONE caller-owned data block --------
 *
 * A directory is an ordinary object whose blocks hold these entries. The
 * functions below work on one block at a time; kernel/gnfs/gnfs_vfs.c walks
 * the directory's block map over them, and grows the directory by a block
 * when every existing one is full (gnfs_dir_add's -ENOSPC is "this BLOCK is
 * full", not "the directory is"). */
#define GNFS_NAME_MAX 60u

typedef struct gnfs_dirent {
    uint64 objnum;        /* 0 = this slot is free (never written, or deleted) */
    uint8  is_dir;
    uint8  name_len;      /* not NUL-counted; name[name_len] is not read */
    uint8  reserved[6];
    char   name[GNFS_NAME_MAX];
} gnfs_dirent_t;

#define GNFS_DIRENTS_PER_BLOCK (GNFS_BLOCK_SIZE / (uint64)sizeof(gnfs_dirent_t))

/* Zero a block - every dirent's objnum becomes 0, i.e. an empty directory. */
void gnfs_dir_init_block(uint8 *block);

/* 0 with *objnum_out and *is_dir_out set, or -ENOENT. */
int gnfs_dir_find(const uint8 *block, const char *name, uint64 name_len,
                 uint64 *objnum_out, int *is_dir_out);

/* 0, or -EEXIST if the name is already there, or -ENOSPC if every slot in
 * the block is taken. */
int gnfs_dir_add(uint8 *block, const char *name, uint64 name_len,
                uint64 objnum, int is_dir);

/* 0, or -ENOENT. Clears the slot (objnum = 0); does not compact the entries
 * after it - a later gnfs_dir_add may reuse the hole, but nothing shifts
 * entries down to close it. */
int gnfs_dir_remove(uint8 *block, const char *name, uint64 name_len);

/* Return non-zero from `cb` to stop the walk early, the same convention
 * fs_dir_cb (kernel/include/fs.h) already uses. */
typedef int (*gnfs_dir_cb)(const char *name, uint64 name_len, uint64 objnum,
                          int is_dir, void *ctx);
int gnfs_dir_iterate(const uint8 *block, gnfs_dir_cb cb, void *ctx);

/* --- formatting --------------------------------------------------------------
 *
 * Lays down an initial txg-1 volume: an all-free bitmap written to bitmap
 * region A, an object table written to object-table region A holding exactly
 * one live object - GNFS_ROOT_DIR_OBJNUM, an empty directory whose single
 * data block is the first block of the allocatable region - and a root
 * record naming all of it written to ring slot (1 % GNFS_RING_SLOTS). Every
 * other ring slot, bitmap region B and object-table region B are left
 * exactly as `write` already had them (normally zeroed by whatever created
 * the image), which is fine: gnfs_root_valid rejects an unwritten slot the
 * same way it rejects a torn one, and nothing reads region B of anything
 * until the first commit writes it.
 *
 * `write` is a callback rather than a device_t, so this one function is
 * exactly what both the kernel driver (kernel/gnfs/gnfs_vfs.c, writing
 * through dev_write) and the host format tool (tools/mkgnfs.c, writing
 * through fwrite) call - one formatting implementation rather than two that
 * can disagree about what a fresh volume looks like.
 *
 * `write` follows dev_write's own convention (kernel/include/device.h):
 * returns the number of bytes written, or a negative errno. gnfs_format
 * treats anything other than exactly `len` bytes back as failure, a short
 * write included - there is no partial-block case this format can make
 * sense of.
 *
 * Returns 0, or a negative errno. */
typedef int64 (*gnfs_write_fn)(void *ctx, uint64 offset, const void *buf,
                               uint64 len);

int gnfs_format(void *ctx, gnfs_write_fn write, uint64 total_bytes);

#endif /* GNFS_LAYOUT_H */
