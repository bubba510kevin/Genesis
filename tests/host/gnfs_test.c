/* Host tests for gnfs: the pure logic in kernel/gnfs/gnfs_format.c and
 * kernel/gnfs/gnfs_object.c directly, and the device-facing mount/commit
 * ring and object layer in kernel/gnfs/gnfs_vfs.c through
 * tests/host/dev_stub.c's fake device_t - the same device_t fatfs's own
 * tests already mount through, so this suite exercises the real dev_read/
 * dev_write path rather than a stand-in for it.
 *
 * Two properties matter more than "it works" here. One: a torn write during
 * a commit still leaves a mountable volume - the entire argument
 * kernel/include/gnfs_layout.h makes for the ring shape. Two: every
 * committed change - a create, a write, an unlink - actually reaches the
 * medium, checked by tearing the mount down and remounting from the same
 * bytes rather than trusting an in-memory view that might never have been
 * written back at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acl.h"
#include "dev_stub.h"
#include "gnfs_dev.h"
#include "gnfs_layout.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

/* --- pure logic: checksum, root records, the bitmap ------------------------ */

static void test_checksum_is_sensitive(void) {
    uint8 a[16], b[16];
    int i;

    for (i = 0; i < 16; i++) {
        a[i] = (uint8)i;
        b[i] = (uint8)i;
    }
    check(gnfs_checksum(a, 16) == gnfs_checksum(b, 16),
         "identical buffers checksum the same");

    b[7] ^= 0x01;
    check(gnfs_checksum(a, 16) != gnfs_checksum(b, 16),
         "one flipped bit must change the checksum");
}

/* A plausible root record for the pure tests below, sharing exactly one
 * construction rather than each test hand-rolling gnfs_root_init's argument
 * list - gnfs_root_valid requires obj_table_blocks to agree exactly with
 * max_objects, so a stray literal in one test would fail for a reason that
 * has nothing to do with what it is testing. */
#define TEST_MAX_OBJECTS 256u

static void make_test_root(gnfs_root_t *r) {
    gnfs_root_init(r, 1000, GNFS_RING_SLOTS, 4,
                   GNFS_RING_SLOTS + 8,
                   gnfs_obj_table_blocks_for(TEST_MAX_OBJECTS),
                   TEST_MAX_OBJECTS, GNFS_ROOT_DIR_OBJNUM);
}

static void test_root_seal_and_valid(void) {
    gnfs_root_t r;

    make_test_root(&r);
    check(gnfs_root_valid(&r), "a freshly-initialised root must be valid");

    r.magic ^= 1;
    check(!gnfs_root_valid(&r), "a corrupted magic must be rejected");
    r.magic ^= 1;
    check(gnfs_root_valid(&r), "un-corrupting it must make it valid again");

    r.version = GNFS_VERSION + 1;
    check(!gnfs_root_valid(&r), "an unrecognised version must be rejected");
    r.version = GNFS_VERSION;

    r.max_objects = TEST_MAX_OBJECTS * 4;
    gnfs_root_seal(&r);
    check(!gnfs_root_valid(&r),
         "a capacity the recorded table size cannot hold must be refused - "
         "even correctly sealed - rather than one of the two numbers being "
         "trusted over the other");
    r.max_objects = GNFS_MAX_OBJECTS_LIMIT + 1;
    r.obj_table_blocks = gnfs_obj_table_blocks_for(r.max_objects);
    gnfs_root_seal(&r);
    check(!gnfs_root_valid(&r),
         "and a capacity past GNFS_MAX_OBJECTS_LIMIT is refused even when "
         "the table size agrees with it");
    make_test_root(&r);

    r.txg = 42;
    check(!gnfs_root_valid(&r),
         "changing a field WITHOUT resealing must be caught by the checksum - "
         "this is the whole point of gnfs_root_valid recomputing rather than "
         "trusting the stored value");
    gnfs_root_seal(&r);
    check(gnfs_root_valid(&r), "resealing after a legitimate change must pass");
}

static void test_layout_helpers_agree_with_format(void) {
    gnfs_root_t r;

    make_test_root(&r);
    check(gnfs_bitmap_region_a(&r) == GNFS_RING_SLOTS,
         "bitmap region A starts right after the ring");
    check(gnfs_bitmap_region_b(&r) == GNFS_RING_SLOTS + r.bitmap_blocks,
         "bitmap region B starts right after region A");
    check(gnfs_obj_table_region_a(&r) ==
         GNFS_RING_SLOTS + r.bitmap_blocks * 2,
         "object-table region A starts right after both bitmap regions");
    check(gnfs_alloc_region_start(&r) ==
         gnfs_obj_table_region_a(&r) + r.obj_table_blocks * 2,
         "the allocatable region starts right after both object-table "
         "regions");
}

static void test_bitmap_alloc_first_fit(void) {
    uint64 total = 100;
    uint8 *bm = (uint8 *)calloc(1, (size_t)gnfs_bitmap_bytes(total));
    uint64 a, b, c;

    check(gnfs_alloc_blocks(bm, total, 10, &a) == 0 && a == 0,
         "first allocation must land at block 0 on an empty bitmap");
    check(gnfs_alloc_blocks(bm, total, 5, &b) == 0 && b == 10,
         "second allocation must land right after the first (first-fit)");

    gnfs_free_blocks(bm, total, 0, 10);
    check(!gnfs_bitmap_test(bm, 0) && !gnfs_bitmap_test(bm, 9),
         "freed blocks must read as free again");
    check(gnfs_bitmap_test(bm, 10),
         "freeing [0,10) must not disturb the allocation at 10 - a control "
         "for the case above");

    check(gnfs_alloc_blocks(bm, total, 10, &c) == 0 && c == 0,
         "the freed run must be reused rather than skipped");

    {
        uint64 huge;
        check(gnfs_alloc_blocks(bm, total, 1000, &huge) == -28,
             "an allocation bigger than the whole bitmap must return -ENOSPC");
    }

    free(bm);
}

/* --- pure logic: onodes -------------------------------------------------- */

static void test_onode_init_and_access(void) {
    static uint8 table[TEST_MAX_OBJECTS * sizeof(gnfs_onode_t)];
    gnfs_onode_t *o;

    check(gnfs_onode_at(table, TEST_MAX_OBJECTS, 0) == NULL,
         "object number 0 must never resolve - it means \"no object\"");
    check(gnfs_onode_at(table, TEST_MAX_OBJECTS, TEST_MAX_OBJECTS) == NULL,
         "an out-of-range object number must be refused, not read past the "
         "table");

    o = gnfs_onode_at(table, TEST_MAX_OBJECTS, 5);
    check(o != NULL, "an in-range object number must resolve");
    gnfs_onode_init(o, 0100644u, 42, 7);
    check(o->mode == 0100644u && o->uid == 42 && o->gid == 7 &&
         o->size == 0 && o->nblocks == 0,
         "a freshly initialised onode has the identity it was given and no "
         "data yet");
}

/* Version 1 never reused an object number, so a volume was permanently
 * full after its 255th create EVER. Numbers are reused now - checked by
 * filling the table, freeing one slot BEHIND the hint, and requiring the
 * next allocation to find exactly that one. */
static void test_onode_alloc_reuses_and_is_bounded(void) {
    static uint8 table[TEST_MAX_OBJECTS * sizeof(gnfs_onode_t)];
    gnfs_root_t r;
    uint64 a, b, c, n;

    memset(table, 0, sizeof(table));
    make_test_root(&r);
    gnfs_onode_init(gnfs_onode_at(table, TEST_MAX_OBJECTS,
                                  GNFS_ROOT_DIR_OBJNUM), 0040755u, 0, 0);

    check(gnfs_onode_alloc(&r, table, &a) == 0 &&
         a == GNFS_ROOT_DIR_OBJNUM + 1,
         "the first allocation after format is the object after the root");
    gnfs_onode_init(gnfs_onode_at(table, TEST_MAX_OBJECTS, a), 0100644u, 0, 0);
    check(gnfs_onode_alloc(&r, table, &b) == 0 && b == a + 1,
         "the next one follows it");
    gnfs_onode_init(gnfs_onode_at(table, TEST_MAX_OBJECTS, b), 0100644u, 0, 0);

    for (n = b + 1; n < TEST_MAX_OBJECTS; n++) {
        uint64 got;

        if (gnfs_onode_alloc(&r, table, &got) != 0) {
            break;
        }
        gnfs_onode_init(gnfs_onode_at(table, TEST_MAX_OBJECTS, got),
                        0100644u, 0, 0);
    }
    check(gnfs_onode_alloc(&r, table, &c) == -28,
         "a full table is -ENOSPC");
    check(gnfs_objects_in_use(table, TEST_MAX_OBJECTS) ==
         TEST_MAX_OBJECTS - 1, "and every slot but 0 is counted in use");

    gnfs_onode_free(gnfs_onode_at(table, TEST_MAX_OBJECTS, a));
    check(gnfs_onode_alloc(&r, table, &c) == 0 && c == a,
         "a freed slot behind the hint is found again - numbers are reused");
}

/* --- pure logic: directory entries --------------------------------------- */

static void test_directory_entries(void) {
    static uint8 block[GNFS_BLOCK_SIZE];
    uint64 objnum;
    int is_dir;

    gnfs_dir_init_block(block);
    check(gnfs_dir_find(block, "x", 1, &objnum, &is_dir) == -2,
         "a fresh block has nothing in it");

    check(gnfs_dir_add(block, "hello", 5, 10, 0) == 0,
         "adding an entry must succeed");
    check(gnfs_dir_add(block, "hello", 5, 11, 0) == -17,
         "adding the same name twice must be -EEXIST, not silently replace "
         "it");
    check(gnfs_dir_find(block, "hello", 5, &objnum, &is_dir) == 0 &&
         objnum == 10 && !is_dir,
         "the entry must read back exactly what was added");

    check(gnfs_dir_add(block, "sub", 3, 12, 1) == 0,
         "a second, distinct entry must also succeed");
    check(gnfs_dir_remove(block, "hello", 5) == 0,
         "removing an existing entry must succeed");
    check(gnfs_dir_find(block, "hello", 5, &objnum, &is_dir) == -2,
         "a removed entry must no longer be found");
    check(gnfs_dir_find(block, "sub", 3, &objnum, &is_dir) == 0 &&
         objnum == 12 && is_dir,
         "removing one entry must not disturb another - a control for the "
         "case above");

    check(gnfs_dir_add(block, "hello", 5, 99, 0) == 0,
         "the hole left by a removed entry must be reusable");
}

typedef struct {
    int count;
} iter_ctx_t;

static int iter_count_cb(const char *name, uint64 name_len, uint64 objnum,
                         int is_dir, void *ctx) {
    (void)name; (void)name_len; (void)objnum; (void)is_dir;
    ((iter_ctx_t *)ctx)->count++;
    return 0;
}

static void test_directory_iterate_and_full(void) {
    static uint8 block[GNFS_BLOCK_SIZE];
    iter_ctx_t ic;
    uint64 i;

    gnfs_dir_init_block(block);
    ic.count = 0;
    gnfs_dir_iterate(block, iter_count_cb, &ic);
    check(ic.count == 0, "an empty directory iterates zero times");

    for (i = 0; i < GNFS_DIRENTS_PER_BLOCK; i++) {
        char name[2];
        int rc;

        name[0] = (char)('a' + (i % 26));
        name[1] = (char)('0' + (i / 26));
        rc = gnfs_dir_add(block, name, 2, i + 1, 0);
        check(rc == 0, "filling every slot in the block must succeed");
    }
    ic.count = 0;
    gnfs_dir_iterate(block, iter_count_cb, &ic);
    check((uint64)ic.count == GNFS_DIRENTS_PER_BLOCK,
         "iterating a full directory must visit every entry exactly once");

    check(gnfs_dir_add(block, "zz", 2, 999, 0) == -28,
         "a directory with every slot taken must refuse a new entry with "
         "-ENOSPC, not silently drop an existing one");
}

/* --- pure logic: ACL inheritance and chmod --------------------------------- */

static void test_acl_inherit(void) {
    acl_t parent;
    acl_t child;

    acl_from_mode(0040755u, 10, 10, &parent);

    /* An entry inheritable to both files and directories, one to files
     * only, and one marked NO_PROPAGATE - three different fates below. */
    parent.ace[parent.count].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    parent.ace[parent.count].flags = ACE_FILE_INHERIT_ACE |
                                     ACE_DIRECTORY_INHERIT_ACE;
    parent.ace[parent.count].mask  = ACE_READ_DATA;
    parent.ace[parent.count].who   = 1001;
    parent.count++;

    parent.ace[parent.count].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    parent.ace[parent.count].flags = ACE_FILE_INHERIT_ACE;
    parent.ace[parent.count].mask  = ACE_WRITE_DATA;
    parent.ace[parent.count].who   = 1002;
    parent.count++;

    parent.ace[parent.count].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    parent.ace[parent.count].flags = ACE_DIRECTORY_INHERIT_ACE |
                                     ACE_NO_PROPAGATE_INHERIT_ACE;
    parent.ace[parent.count].mask  = ACE_EXECUTE;
    parent.ace[parent.count].who   = 1003;
    parent.count++;

    acl_inherit(&parent, /* child_is_dir */ 0, 0100644u, 20, 20, &child);
    check(!child.trivial, "a child that inherited real entries is not "
         "trivial");
    check(child.count == 3 + 2,
         "a FILE child inherits the both-inheritable entry and the "
         "file-only one, not the directory-only NO_PROPAGATE one");
    check(child.ace[3].who == 1001 &&
         (child.ace[3].flags & ACE_INHERIT_ONLY_ACE) == 0 &&
         (child.ace[3].flags & ACE_INHERITED_ACE) != 0,
         "the inherited entry is effective (not inherit-only) and marked "
         "as inherited");
    check((child.ace[3].flags &
          (ACE_FILE_INHERIT_ACE | ACE_DIRECTORY_INHERIT_ACE)) == 0,
         "a FILE cannot have descendants, so the copy's own inherit bits "
         "are cleared - it must not propagate again");
    check(child.ace[4].who == 1002,
         "the file-only inheritable entry is the second one copied");

    acl_inherit(&parent, /* child_is_dir */ 1, 0040755u, 20, 20, &child);
    check(child.count == 3 + 2,
         "a DIRECTORY child inherits the both-inheritable entry and the "
         "NO_PROPAGATE one (which still applies to this level), not the "
         "file-only one");
    check(child.ace[3].who == 1001 &&
         (child.ace[3].flags &
          (ACE_FILE_INHERIT_ACE | ACE_DIRECTORY_INHERIT_ACE)) != 0,
         "a directory CAN have descendants, so an entry with no "
         "NO_PROPAGATE keeps its inherit bits and keeps propagating");
    check(child.ace[4].who == 1003 &&
         (child.ace[4].flags &
          (ACE_FILE_INHERIT_ACE | ACE_DIRECTORY_INHERIT_ACE)) == 0,
         "NO_PROPAGATE means exactly one level - even a directory child's "
         "copy loses the inherit bits");

    {
        acl_t bare;
        acl_t nothing_inherited;

        acl_from_mode(0040755u, 10, 10, &bare);
        acl_inherit(&bare, 0, 0100644u, 20, 20, &nothing_inherited);
        check(nothing_inherited.trivial && nothing_inherited.count == 3,
             "a parent with nothing inheritable produces exactly the plain "
             "mode projection - not an empty ACL and not the parent's own");
    }

    {
        acl_t from_root;

        acl_inherit(NULL, 0, 0100644u, 20, 20, &from_root);
        check(from_root.trivial && from_root.count == 3,
             "no parent at all (a filesystem root) is the same as nothing "
             "inheritable");
    }
}

static void test_acl_apply_chmod(void) {
    acl_t old, new_acl;

    acl_from_mode(0100644u, 5, 5, &old);
    /* A named grant chmod has no way to express or take away. */
    old.ace[old.count].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    old.ace[old.count].flags = 0;
    old.ace[old.count].mask  = ACE_READ_DATA;
    old.ace[old.count].who   = 9001;
    old.count++;
    old.trivial = 0;

    acl_apply_chmod(&old, 0100600u, &new_acl);
    check(new_acl.count == 4, "chmod does not add or remove entries");
    check(new_acl.ace[3].who == 9001 &&
         new_acl.ace[3].mask == ACE_READ_DATA,
         "the named grant chmod cannot express survives completely "
         "untouched");
    {
        uint32 owner_mode = acl_to_mode(&new_acl, 0100000u);
        check((owner_mode & 0700) == 0600u,
             "the owner class now matches the new mode (0600 -> rw-)");
    }

    acl_apply_chmod(&old, 0100755u, &new_acl);
    {
        uint32 m = acl_to_mode(&new_acl, 0100000u);
        check((m & 0777) == 0755u,
             "a second chmod to a different mode overwrites the rewritten "
             "classes again rather than accumulating");
    }
    check(new_acl.ace[3].who == 9001,
         "and still leaves the named grant alone");

    {
        acl_t missing_owner;
        acl_t fallback;

        acl_from_mode(0100644u, 7, 7, &missing_owner);
        missing_owner.ace[0].flags |= ACE_INHERIT_ONLY_ACE;  /* owner@ no
                                                              * longer
                                                              * effective */
        acl_apply_chmod(&missing_owner, 0100755u, &fallback);
        check(fallback.owner == 7 && fallback.group == 7,
             "when the shape this function expects is not there, it falls "
             "back to a plain projection rather than guessing");
    }
}

/* chown's gate, as pure logic. Every refusal is paired with the one change
 * that flips it, for the reason acl_selftest.c gives: a gate that only ever
 * saw allow-cases would pass these tests while granting everything. */
static void test_acl_chown_permitted(void) {
    acl_t a;
    cred_t owner, stranger, root, supreme;

    acl_from_mode(0100644u, 100, 200, &a);   /* owned by 100, group 200 */

    cred_init_nobody(&owner);
    owner.euid = 100;
    owner.egid = 200;
    owner.groups[0] = 300;
    owner.ngroups = 1;

    cred_init_nobody(&stranger);
    stranger.euid = 555;
    stranger.egid = 300;

    cred_init_nobody(&root);
    root.euid = 0;

    cred_init_nobody(&supreme);
    supreme.euid = 4242;
    supreme.supreme = 1;

    /* The distinction this whole function exists for. */
    check(acl_access(&a, &owner, ACE_WRITE_ACL) == 0,
         "the owner holds WRITE_ACL (the precondition: chmod is its right)");
    check(acl_chown_permitted(&a, &owner, 555, ACL_CHOWN_KEEP) == -1,
         "and still may NOT give the file away - chown is not chmod");
    check(acl_chown_permitted(&a, &root, 555, ACL_CHOWN_KEEP) == 0,
         "root may");
    check(acl_chown_permitted(&a, &supreme, 555, 999) == 0,
         "and so may the supreme uid, to any pair at all");

    check(acl_chown_permitted(&a, &owner, ACL_CHOWN_KEEP, 300) == 0,
         "the owner may chgrp into a supplementary group it is in");
    check(acl_chown_permitted(&a, &owner, ACL_CHOWN_KEEP, 301) == -1,
         "but not into a group it is not in");
    check(acl_chown_permitted(&a, &owner, 100, 200) == 0 &&
         acl_chown_permitted(&a, &stranger, ACL_CHOWN_KEEP,
                             ACL_CHOWN_KEEP) == 0,
         "a request that changes nothing is permitted to anyone");
    check(acl_chown_permitted(&a, &stranger, ACL_CHOWN_KEEP, 300) == -1,
         "a non-owner without WRITE_OWNER may not chgrp, even into its own "
         "group");

    /* An explicit WRITE_OWNER grant: take, never give. */
    a.ace[a.count].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    a.ace[a.count].flags = 0;
    a.ace[a.count].mask  = ACE_WRITE_OWNER;
    a.ace[a.count].who   = 555;
    a.count++;
    a.trivial = 0;
    check(acl_chown_permitted(&a, &stranger, 555, ACL_CHOWN_KEEP) == 0,
         "a WRITE_OWNER holder may take ownership for itself");
    check(acl_chown_permitted(&a, &stranger, 777, ACL_CHOWN_KEEP) == -1,
         "but may not hand it to a third uid");
    check(acl_chown_permitted(&a, &stranger, 555, 300) == 0,
         "and may take it into a group it is in in the same call");
    check(acl_chown_permitted(&a, &stranger, 555, 201) == -1,
         "but not into one it is not in");

    /* A deny placed ahead binds WRITE_OWNER like any other bit. */
    a.ace[3] = a.ace[2];
    a.ace[2].type  = ACE_ACCESS_DENIED_ACE_TYPE;
    a.ace[2].flags = 0;
    a.ace[2].mask  = ACE_WRITE_OWNER;
    a.ace[2].who   = 555;
    a.ace[4].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    a.ace[4].flags = 0;
    a.ace[4].mask  = ACE_WRITE_OWNER;
    a.ace[4].who   = 555;
    a.count = 5;
    check(acl_chown_permitted(&a, &stranger, 555, ACL_CHOWN_KEEP) == -1,
         "a DENY of WRITE_OWNER ahead of the grant takes it away");
    check(acl_chown_permitted(&a, &supreme, 555, ACL_CHOWN_KEEP) == 0,
         "and does not bind supreme, the same as every other deny");
}

/* --- device-facing: format, mount, and the commit ring --------------------- */

typedef struct {
    uint8 *buf;
    uint64 len;
} mem_ctx_t;

static int64 mem_write(void *ctx, uint64 offset, const void *data,
                       uint64 len) {
    mem_ctx_t *m = (mem_ctx_t *)ctx;
    if (offset + len > m->len) {
        return -28;
    }
    memcpy(m->buf + offset, data, (size_t)len);
    return (int64)len;
}

static void test_format_mount_and_commit_ring(void) {
    uint64 image_bytes = 1 * 1024 * 1024;   /* 1 MiB - plenty for the ring,
                                             * two bitmap regions and two
                                             * object-table regions */
    uint8 *image = (uint8 *)calloc(1, (size_t)image_bytes);
    mem_ctx_t mctx;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    gnfs_root_t cur;
    uint8 *bitmap;
    uint8 *obj_table;
    uint64 bitmap_bytes, obj_table_bytes;
    unsigned i;
    uint64 corrupt_offset;

    mctx.buf = image;
    mctx.len = image_bytes;
    check(gnfs_format(&mctx, mem_write, image_bytes) == 0,
         "gnfs_format must succeed on a plausibly-sized image");

    dev_stub_attach(&dev, &stub, image, image_bytes);
    v = gnfs_probe(&dev);
    check(v != NULL, "gnfs_probe must recognise a freshly formatted image");
    if (v == NULL) {
        free(image);
        return;
    }

    /* v->body is a gnfs_mount_t*, but that type is private to gnfs_vfs.c -
     * this test only needs the root record. Read it back the same way
     * gnfs_probe itself found it: scan the ring directly through dev_read,
     * which is public and exactly what a second, independent check should
     * use rather than reaching into gnfs_vfs.c's static state. */
    {
        int slot;
        int found = 0;
        gnfs_root_t r;

        for (slot = 0; slot < (int)GNFS_RING_SLOTS; slot++) {
            if (dev_read(&dev, (uint64)slot * GNFS_BLOCK_SIZE, &r,
                        sizeof(r)) == (int64)sizeof(r) &&
                gnfs_root_valid(&r)) {
                if (!found || r.txg > cur.txg) {
                    cur = r;
                    found = 1;
                }
            }
        }
        check(found && cur.txg == 1,
             "the only valid ring slot right after formatting must be txg 1");
    }

    bitmap_bytes    = cur.bitmap_blocks * (uint64)GNFS_BLOCK_SIZE;
    obj_table_bytes = cur.obj_table_blocks * (uint64)GNFS_BLOCK_SIZE;
    bitmap    = (uint8 *)calloc(1, (size_t)bitmap_bytes);
    obj_table = (uint8 *)calloc(1, (size_t)obj_table_bytes);

    /* Commit past the ring's own length, so this exercises slot reuse - the
     * exact property the crash-safety argument depends on - not just the
     * first few, never-recycled slots. The all-zero bitmap/object-table
     * content committed here is fine for this test: it is checking the ring
     * mechanism, not file content, and this device is discarded afterwards -
     * see test_object_layer_end_to_end for content correctness through the
     * real fs_ops_t. */
    for (i = 0; i < GNFS_RING_SLOTS + 2; i++) {
        uint64 prev_txg = cur.txg;
        int rc = gnfs_txg_commit(&dev, &cur, bitmap, bitmap_bytes,
                                obj_table, obj_table_bytes);

        check(rc == 0, "each commit in the ring must succeed");
        check(cur.txg == prev_txg + 1,
             "a commit must advance txg by exactly one");
    }

    /* --- the property that matters: a torn write to the NEWEST slot must
     * not take the whole volume down with it -------------------------------
     *
     * cur is currently the highest txg committed (call it T, in slot
     * T % GNFS_RING_SLOTS). Corrupt exactly that slot - simulating a commit
     * that started overwriting it and was cut off mid-write - and confirm a
     * fresh scan falls back to the next-highest STILL-VALID txg, which lives
     * in a different slot the corruption never touched, rather than either
     * accepting torn data or reporting no valid volume at all. */
    corrupt_offset = (cur.txg % GNFS_RING_SLOTS) * GNFS_BLOCK_SIZE;
    memset(image + corrupt_offset, 0xFF, 37);   /* an arbitrary partial-write
                                                 * length, not a whole record */
    {
        int slot;
        gnfs_root_t r;
        gnfs_root_t best;
        int found = 0;

        for (slot = 0; slot < (int)GNFS_RING_SLOTS; slot++) {
            if (dev_read(&dev, (uint64)slot * GNFS_BLOCK_SIZE, &r,
                        sizeof(r)) == (int64)sizeof(r) &&
                gnfs_root_valid(&r)) {
                if (!found || r.txg > best.txg) {
                    best = r;
                    found = 1;
                }
            }
        }
        check(found,
             "after corrupting the newest ring slot, the scan must still "
             "find AN older valid record - the entire point of a ring "
             "rather than a single root");
        check(found && best.txg == cur.txg - 1,
             "and it must be exactly the commit before the corrupted one - "
             "the newest surviving txg, not an arbitrary older one");
    }

    v->ops->unmount(v);
    free(obj_table);
    free(bitmap);
    free(image);
}

/* --- device-facing: the object layer through the real fs_ops_t ------------ */

static void test_object_layer_end_to_end(void) {
    uint64 image_bytes = 1 * 1024 * 1024;
    uint8 *image = (uint8 *)calloc(1, (size_t)image_bytes);
    mem_ctx_t mctx;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_node_t root_node, file_node, dir_node;
    int rc;
    uint8 buf[64];

    mctx.buf = image;
    mctx.len = image_bytes;
    check(gnfs_format(&mctx, mem_write, image_bytes) == 0,
         "format for the object-layer test must succeed");

    dev_stub_attach(&dev, &stub, image, image_bytes);
    v = gnfs_probe(&dev);
    check(v != NULL, "mounting the freshly formatted image must succeed");
    if (v == NULL) {
        free(image);
        return;
    }

    rc = v->ops->lookup(v, "/", &root_node);
    check(rc == 0 && root_node.is_dir,
         "the root must resolve and be a directory");
    check(root_node.size == GNFS_BLOCK_SIZE,
         "a freshly formatted root's directory data is exactly one block");

    rc = v->ops->create(v, "/hello.txt", NULL, 0644u);
    check(rc == 0, "creating a file must succeed");
    rc = v->ops->create(v, "/hello.txt", NULL, 0644u);
    check(rc == -17, "creating the same name twice must be -EEXIST");

    rc = v->ops->lookup(v, "/hello.txt", &file_node);
    check(rc == 0 && !file_node.is_dir && file_node.size == 0,
         "a freshly created file must resolve, be a file, and be empty");

    {
        const char *msg = "hello, gnfs";
        int64 wrc = v->ops->write(v, &file_node, 0, msg, 11);
        check(wrc == 11, "writing 11 bytes must report 11 bytes written");
    }
    rc = v->ops->lookup(v, "/hello.txt", &file_node);
    check(rc == 0 && file_node.size == 11,
         "the size must be visible after a re-lookup, not just on the node "
         "the write happened to use");

    {
        int64 rrc = v->ops->read(v, &file_node, 0, buf, sizeof(buf));
        check(rrc == 11, "reading back must return exactly what was written");
        check(memcmp(buf, "hello, gnfs", 11) == 0,
             "and the bytes must match");
    }

    {
        /* A write past the current end must zero-fill the gap - the exact
         * lesson kernel/fs/fat.c's own write path had to learn (see
         * tests/host/fat_write_test.c), applied here on first principles
         * rather than re-learned the same way. */
        int64 wrc = v->ops->write(v, &file_node, 4096 + 10, "TAIL", 4);
        check(wrc == 4, "a write past the first block must still succeed");

        {
            uint8 gap[16];
            int64 rrc = v->ops->read(v, &file_node, 11, gap, sizeof(gap));
            int all_zero = 1;
            int k;

            check(rrc == (int64)sizeof(gap),
                 "reading across the gap must return every byte requested");
            for (k = 0; k < (int)sizeof(gap); k++) {
                if (gap[k] != 0) {
                    all_zero = 0;
                }
            }
            check(all_zero,
                 "every byte of the gap between the two writes must read "
                 "as zero");
        }
    }

    rc = v->ops->mkdir(v, "/sub", NULL, 0755u);
    check(rc == 0, "creating a directory must succeed");
    rc = v->ops->lookup(v, "/sub", &dir_node);
    check(rc == 0 && dir_node.is_dir,
         "the new directory must resolve as a directory");

    rc = v->ops->create(v, "/sub/inner.txt", NULL, 0644u);
    check(rc == 0, "creating a file inside the new directory must succeed");
    {
        fs_node_t inner;
        rc = v->ops->lookup(v, "/sub/inner.txt", &inner);
        check(rc == 0,
             "a nested path must resolve through more than one directory "
             "level");
    }

    rc = v->ops->rmdir(v, "/sub");
    check(rc == -39, "rmdir on a non-empty directory must be -ENOTEMPTY");
    rc = v->ops->unlink(v, "/sub/inner.txt");
    check(rc == 0, "unlinking the file inside it must succeed");
    rc = v->ops->rmdir(v, "/sub");
    check(rc == 0, "rmdir must now succeed once the directory is empty");
    rc = v->ops->lookup(v, "/sub", &dir_node);
    check(rc == -2, "the removed directory must no longer resolve");

    rc = v->ops->unlink(v, "/hello.txt");
    check(rc == 0, "unlinking the file must succeed");
    rc = v->ops->lookup(v, "/hello.txt", &file_node);
    check(rc == -2, "the unlinked file must no longer resolve");

    v->ops->unmount(v);

    /* Re-mount from scratch and confirm every committed change actually
     * reached the medium - the same proof test_format_mount_and_commit_ring
     * already applies to the ring alone, now applied to real file content.
     * An in-memory-only "delete" that never made it to the bytes below would
     * pass every check above and still fail this one. */
    {
        device_t dev2;
        dev_stub_t stub2;
        fs_volume_t *v2;
        fs_node_t n2;

        dev_stub_attach(&dev2, &stub2, image, image_bytes);
        v2 = gnfs_probe(&dev2);
        check(v2 != NULL, "the volume must remount after every operation "
             "above");
        if (v2 != NULL) {
            rc = v2->ops->lookup(v2, "/hello.txt", &n2);
            check(rc == -2,
                 "the unlink must have survived a fresh mount, not just "
                 "lived in memory");
            v2->ops->unmount(v2);
        }
    }

    free(image);
}

/* --- device-facing: the ACL write path, storage, inheritance, and real
 * enforcement through the real fs_ops_t ------------------------------------- */

static void test_acl_end_to_end(void) {
    uint64 image_bytes = 1 * 1024 * 1024;
    uint8 *image = (uint8 *)calloc(1, (size_t)image_bytes);
    mem_ctx_t mctx;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_node_t secure_node, plain_node, child_node, grandchild_node;
    acl_t acl_out;
    int rc;

    mctx.buf = image;
    mctx.len = image_bytes;
    check(gnfs_format(&mctx, mem_write, image_bytes) == 0,
         "format for the ACL test must succeed");

    dev_stub_attach(&dev, &stub, image, image_bytes);
    v = gnfs_probe(&dev);
    check(v != NULL, "mounting the freshly formatted image must succeed");
    if (v == NULL) {
        free(image);
        return;
    }

    rc = v->ops->create(v, "/plain.txt", NULL, 0644u);
    check(rc == 0, "creating a plain file must succeed");
    rc = v->ops->lookup(v, "/plain.txt", &plain_node);
    check(rc == 0, "the plain file must resolve");
    rc = v->ops->getacl(v, &plain_node, &acl_out);
    check(rc == -2,
         "a freshly created file with nothing inherited has no stored ACL - "
         "getacl answers -ENOENT, the convention every filesystem here "
         "follows; fs_getacl one layer up is what actually projects the "
         "mode for a caller, not tested again here");

    rc = v->ops->mkdir(v, "/secure", NULL, 0755u);
    check(rc == 0, "creating the directory to hold an inheritable ACL must "
         "succeed");
    rc = v->ops->lookup(v, "/secure", &secure_node);
    check(rc == 0, "it must resolve");

    {
        acl_t custom;

        acl_from_mode(0040750u, 0, 0, &custom);
        custom.ace[custom.count].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
        custom.ace[custom.count].flags = ACE_FILE_INHERIT_ACE |
                                         ACE_DIRECTORY_INHERIT_ACE;
        /* WRITE_DATA, not READ - both gnfs_op_create and gnfs_op_mkdir use a
         * fixed default mode (0644/0755, no mode parameter reaches fs_ops_t
         * yet), and that default's everyone@ already grants READ_DATA. The
         * enforcement check below needs a bit "everyone" genuinely does not
         * have, so that a stranger being refused actually exercises the
         * named entry's absence rather than passing for an unrelated
         * reason. */
        custom.ace[custom.count].mask  = ACE_WRITE_DATA;
        custom.ace[custom.count].who   = 1001;
        custom.count++;
        custom.trivial = 0;

        rc = v->ops->setacl(v, &secure_node, &custom, FS_SPECIAL_KEEP);
        check(rc == 0, "setacl on the directory must succeed");
    }

    rc = v->ops->getacl(v, &secure_node, &acl_out);
    check(rc == 0 && acl_out.count == 4,
         "the directory's ACL is now stored, not projected, and getacl "
         "reports it back with the entry just set");
    check(acl_out.ace[3].who == 1001, "including the inheritable entry "
         "itself");

    rc = v->ops->create(v, "/secure/child.txt", NULL, 0644u);
    check(rc == 0, "creating a file inside the secured directory must "
         "succeed");
    rc = v->ops->lookup(v, "/secure/child.txt", &child_node);
    check(rc == 0, "it must resolve");
    rc = v->ops->getacl(v, &child_node, &acl_out);
    check(rc == 0,
         "the new file has a REAL stored ACL now - inheritance gave it "
         "something a mode word cannot express");
    check(acl_out.count == 4 && acl_out.ace[3].who == 1001 &&
         (acl_out.ace[3].flags & ACE_INHERITED_ACE) != 0 &&
         (acl_out.ace[3].flags & ACE_INHERIT_ONLY_ACE) == 0,
         "the inherited entry is present, effective, and marked as "
         "inherited rather than explicit");
    check((acl_out.ace[3].flags &
          (ACE_FILE_INHERIT_ACE | ACE_DIRECTORY_INHERIT_ACE)) == 0,
         "and, being a FILE, does not itself propagate any further");

    rc = v->ops->mkdir(v, "/secure/subdir", NULL, 0755u);
    check(rc == 0, "creating a directory inside the secured directory must "
         "succeed");
    rc = v->ops->lookup(v, "/secure/subdir", &grandchild_node);
    check(rc == 0, "it must resolve");
    rc = v->ops->getacl(v, &grandchild_node, &acl_out);
    check(rc == 0 && acl_out.count == 4 &&
         (acl_out.ace[3].flags &
          (ACE_FILE_INHERIT_ACE | ACE_DIRECTORY_INHERIT_ACE)) != 0,
         "a DIRECTORY child keeps the inherit bits, so a file created "
         "inside IT would inherit the same grant a second level down");

    /* Real permission enforcement, not just that the bytes are stored: uid
     * 1001 - granted nothing by the default 0755 mode's "other" class, which
     * has no write bit - but named in the inherited entry - can WRITE to
     * the grandchild; an arbitrary other uid cannot. */
    {
        cred_t writer, stranger;

        cred_init_nobody(&writer);
        writer.euid = 1001;
        check(acl_access(&acl_out, &writer, ACE_WRITE_DATA) == 0,
             "uid 1001 writes via the inherited grant alone");

        cred_init_nobody(&stranger);
        stranger.euid = 424242;
        check(acl_access(&acl_out, &stranger, ACE_WRITE_DATA) == -13,
             "an arbitrary uid the ACL never names is refused - the "
             "control for the check above");
    }

    v->ops->unmount(v);
    free(image);
}

/* chown through the REAL gate (fs_setowner, not the bare slot) against a
 * real gnfs volume: that the refusal refuses, that a permitted change
 * reaches both copies of the owner (onode and stored ACL), and that it
 * survives a remount. */
static void test_chown_end_to_end(void) {
    uint64 image_bytes = 1 * 1024 * 1024;
    uint8 *image = (uint8 *)calloc(1, (size_t)image_bytes);
    mem_ctx_t mctx;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_node_t plain, secured;
    cred_t root, owner;
    acl_t a;
    int rc;

    mctx.buf = image;
    mctx.len = image_bytes;
    check(gnfs_format(&mctx, mem_write, image_bytes) == 0,
         "format for the chown test must succeed");
    dev_stub_attach(&dev, &stub, image, image_bytes);
    v = gnfs_probe(&dev);
    check(v != NULL, "mounting it must succeed");
    if (v == NULL) {
        free(image);
        return;
    }

    cred_init_nobody(&root);
    root.euid = 0;
    cred_init_nobody(&owner);
    owner.euid = 1000;
    owner.egid = 1000;
    owner.groups[0] = 50;
    owner.ngroups = 1;

    /* A file with no stored ACL: only the onode's copy exists. */
    check(v->ops->create(v, "/plain.txt", NULL, 0644u) == 0, "create /plain.txt");
    check(v->ops->lookup(v, "/plain.txt", &plain) == 0, "and resolve it");
    plain.vol = v;               /* fs_lookup_on would stamp this */

    rc = fs_setowner(&plain, (const struct cred *)&owner, 1000, 1000);
    check(rc == -1, "uid 1000 may not seize a file root owns");

    rc = fs_setowner(&plain, (const struct cred *)&root, 1000, 1000);
    check(rc == 0, "root gives it to uid 1000");
    check(plain.uid == 1000 && plain.gid == 1000,
         "and the caller's node reflects it");

    rc = fs_setowner(&plain, (const struct cred *)&owner, 2000,
                     ACL_CHOWN_KEEP);
    check(rc == -1, "the new owner holds WRITE_ACL but still may not give "
         "the file away");
    rc = fs_setowner(&plain, (const struct cred *)&owner, ACL_CHOWN_KEEP, 50);
    check(rc == 0, "but may chgrp it into its own supplementary group");
    rc = fs_setowner(&plain, (const struct cred *)&owner, ACL_CHOWN_KEEP, 51);
    check(rc == -1, "and not into a group it is not in");

    /* A file WITH a stored ACL: both copies must move. */
    check(v->ops->create(v, "/secured.txt", NULL, 0644u) == 0, "create /secured.txt");
    check(v->ops->lookup(v, "/secured.txt", &secured) == 0, "resolve it");
    secured.vol = v;
    acl_from_mode(0100600u, 0, 0, &a);
    a.ace[a.count].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    a.ace[a.count].flags = 0;
    a.ace[a.count].mask  = ACE_READ_DATA;
    a.ace[a.count].who   = 9001;
    a.count++;
    a.trivial = 0;
    check(v->ops->setacl(v, &secured, &a, FS_SPECIAL_KEEP) == 0, "give it a stored ACL");

    rc = fs_setowner(&secured, (const struct cred *)&root, 1000, 1000);
    check(rc == 0, "root chowns the ACL-bearing file");
    check(fs_getacl(&secured, (struct acl *)&a) == 0 && a.owner == 1000 && a.group == 1000,
         "the STORED ACL's owner moved too - otherwise owner@ would still "
         "evaluate against uid 0");
    check(acl_access(&a, &owner, ACE_READ_DATA | ACE_WRITE_DATA) == 0,
         "so the new owner gets owner@'s rw- through it");
    check(a.count == 4 && a.ace[3].who == 9001,
         "and the named grant is untouched");

    /* Both changes must be on the medium, not just in memory. */
    {
        device_t dev2;
        dev_stub_t stub2;
        fs_volume_t *v2;
        fs_node_t n2;

        v->ops->unmount(v);
        v = NULL;
        dev_stub_attach(&dev2, &stub2, image, image_bytes);
        v2 = gnfs_probe(&dev2);
        check(v2 != NULL, "the volume remounts after chown");
        if (v2 != NULL) {
            check(v2->ops->lookup(v2, "/plain.txt", &n2) == 0 &&
                 n2.uid == 1000 && n2.gid == 50,
                 "the plain file's new owner and group survived a remount");
            check(v2->ops->lookup(v2, "/secured.txt", &n2) == 0 &&
                 n2.uid == 1000 &&
                 v2->ops->getacl(v2, &n2, &a) == 0 && a.owner == 1000,
                 "and so did the secured file's, in both copies");
            v2->ops->unmount(v2);
        }
    }

    free(image);
}

/* The creator owns what it creates. Before fs_ops_t::create/mkdir took a
 * credential, every gnfs object belonged to uid 0, so an ordinary user
 * could not even chmod a file it had just made. That last thing is the
 * real check here, through the real gate (fs_setacl), and the stranger
 * refusals are its controls. */
static void test_creator_owns(void) {
    uint64 image_bytes = 1 * 1024 * 1024;
    uint8 *image = (uint8 *)calloc(1, (size_t)image_bytes);
    mem_ctx_t mctx;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_node_t file, dir, kfile;
    cred_t user, stranger;
    acl_t old_acl, new_acl;

    mctx.buf = image;
    mctx.len = image_bytes;
    check(gnfs_format(&mctx, mem_write, image_bytes) == 0,
         "format for the creator test must succeed");
    dev_stub_attach(&dev, &stub, image, image_bytes);
    v = gnfs_probe(&dev);
    check(v != NULL, "mounting it must succeed");
    if (v == NULL) {
        free(image);
        return;
    }

    cred_init_nobody(&user);
    user.euid = 1000;
    user.egid = 100;
    cred_init_nobody(&stranger);
    stranger.euid = 2000;
    stranger.egid = 200;

    check(v->ops->create(v, "/mine.txt", (const struct cred *)&user, 0644u) == 0,
         "uid 1000 creates /mine.txt");
    check(v->ops->lookup(v, "/mine.txt", &file) == 0 &&
         file.uid == 1000 && file.gid == 100,
         "and it is owned by uid 1000, group 100 - the creator's euid and "
         "egid, not root's");
    file.vol = v;

    check(v->ops->mkdir(v, "/mydir", (const struct cred *)&user, 0755u) == 0,
         "uid 1000 makes /mydir");
    check(v->ops->lookup(v, "/mydir", &dir) == 0 &&
         dir.uid == 1000 && dir.gid == 100,
         "and owns that too");

    check(v->ops->create(v, "/kernel.txt", NULL, 0644u) == 0 &&
         v->ops->lookup(v, "/kernel.txt", &kfile) == 0 &&
         kfile.uid == 0 && kfile.gid == 0,
         "a NULL creator is the kernel, which is root");

    check(fs_access(&file, (const struct cred *)&user, ACE_WRITE_DATA) == 0,
         "the owner can write its new 0644 file");
    check(fs_access(&file, (const struct cred *)&stranger,
                    ACE_WRITE_DATA) == -13,
         "a stranger cannot - the control");

    /* chmod, exactly as sys_chmod_node does it. */
    check(fs_getacl(&file, (struct acl *)&old_acl) == 0,
         "read the new file's ACL");
    acl_apply_chmod(&old_acl, 0100600u, &new_acl);
    check(fs_setacl(&file, (const struct cred *)&stranger,
                    (const struct acl *)&new_acl) == -13,
         "a stranger may not chmod it");
    check(fs_setacl(&file, (const struct cred *)&user,
                    (const struct acl *)&new_acl) == 0,
         "but its non-root creator can - impossible before, when gnfs made "
         "every file root's");
    check((file.mode & 0777) == 0600u, "and the mode really changed");

    /* On the medium, not just in memory. */
    {
        device_t dev2;
        dev_stub_t stub2;
        fs_volume_t *v2;
        fs_node_t n2;

        v->ops->unmount(v);
        dev_stub_attach(&dev2, &stub2, image, image_bytes);
        v2 = gnfs_probe(&dev2);
        check(v2 != NULL, "the volume remounts");
        if (v2 != NULL) {
            check(v2->ops->lookup(v2, "/mine.txt", &n2) == 0 &&
                 n2.uid == 1000 && n2.gid == 100,
                 "the creator's ownership survived a remount");
            v2->ops->unmount(v2);
        }
    }

    free(image);
}

/* Adding a name to a directory needs write AND search on that directory.
 * Through fs_create/fs_mkdir - the path-shaped VFS entry points the
 * syscalls use - so the volume is really mounted, not driven through its
 * ops table the way the tests above are. */
static void test_parent_write_check(void) {
    uint64 image_bytes = 1 * 1024 * 1024;
    uint8 *image = (uint8 *)calloc(1, (size_t)image_bytes);
    mem_ctx_t mctx;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_node_t n;
    cred_t root, user, stranger;
    const struct cred *R, *U, *S;
    acl_t old_acl, new_acl;

    mctx.buf = image;
    mctx.len = image_bytes;
    check(gnfs_format(&mctx, mem_write, image_bytes) == 0,
         "format for the parent-check test must succeed");
    dev_stub_attach(&dev, &stub, image, image_bytes);
    v = gnfs_probe(&dev);
    check(v != NULL, "mounting it must succeed");
    if (v == NULL) {
        free(image);
        return;
    }
    check(fs_mount_at("/gp", v) == 0, "and it goes into the mount table");

    cred_init_nobody(&root);
    root.euid = 0;
    cred_init_nobody(&user);
    user.euid = 1000;
    user.egid = 1000;
    cred_init_nobody(&stranger);
    stranger.euid = 2000;
    stranger.egid = 2000;
    R = (const struct cred *)&root;
    U = (const struct cred *)&user;
    S = (const struct cred *)&stranger;

    check(fs_mkdir("/gp/pub", R, 0755u) == 0, "root makes /gp/pub - root's, 0755");
    check(fs_create("/gp/pub/a", U, 0644u) == -13,
         "uid 1000 may not create in a directory it cannot write");
    check(fs_mkdir("/gp/pub/d", U, 0755u) == -13,
         "nor make a subdirectory there");
    check(fs_lookup("/gp/pub/a", &n) == -2,
         "and the refused create really left nothing behind");

    check(fs_lookup("/gp/pub", &n) == 0 &&
         fs_setowner(&n, R, 1000, 1000) == 0,
         "root gives /gp/pub to uid 1000");
    check(fs_create("/gp/pub/a", U, 0644u) == 0,
         "now its owner may create in it - the control for the refusal");
    check(fs_mkdir("/gp/pub/d", U, 0755u) == 0, "and make a subdirectory");
    check(fs_create("/gp/pub/b", S, 0644u) == -13,
         "a stranger still may not - write on the parent is per-caller");

    check(fs_create("/gp/pub/a", S, 0644u) == -17,
         "but O_CREAT on a name that already EXISTS answers -EEXIST, not "
         "-EACCES - opening an existing file never needed the parent's "
         "write bit");
    check(fs_mkdir("/gp/pub/d", S, 0755u) == -17,
         "and mkdir of an existing name is -EEXIST too, as on Linux");

    check(fs_create("/gp/pub/k", NULL, 0644u) == 0,
         "the kernel (NULL cred) is not asked");
    check(fs_create("/gp/rootfile", R, 0644u) == 0,
         "root creates in a root-owned 0755 directory - acl_access's bypass");
    check(fs_create("/gp/nope/x", U, 0644u) == -2,
         "a missing parent is -ENOENT, not a permission answer");

    /* Write without search: -w- on a directory is not enough. */
    check(fs_mkdir("/gp/nox", R, 0755u) == 0 && fs_lookup("/gp/nox", &n) == 0 &&
         fs_setowner(&n, R, 1000, 1000) == 0,
         "root makes /gp/nox and gives it to uid 1000");
    check(fs_getacl(&n, (struct acl *)&old_acl) == 0, "read its ACL");
    acl_apply_chmod(&old_acl, 0040200u, &new_acl);  /* d-w------- */
    check(fs_setacl(&n, U, (const struct acl *)&new_acl) == 0,
         "its owner chmods it to 0200 - write, no search");
    check(fs_create("/gp/nox/f", U, 0644u) == -13,
         "and then cannot create in it - POSIX wants w AND x");
    acl_apply_chmod(&old_acl, 0040300u, &new_acl);  /* d-wx------ */
    check(fs_setacl(&n, U, (const struct acl *)&new_acl) == 0 &&
         fs_create("/gp/nox/f", U, 0644u) == 0,
         "with 0300 it can - the control");

    fs_unmount_volume(v);
    free(image);
}

/* Directory w means "may remove entries" too - ACE_DELETE_CHILD. Without
 * it, access(dir, W_OK) failed for the non-root owner of a 0755 directory,
 * and every mode-only directory would refuse unlink to all but root. */
static void test_dir_write_grants_delete_child(void) {
    acl_t a, b;
    cred_t owner, other;

    cred_init_nobody(&owner);
    owner.euid = 10;
    cred_init_nobody(&other);
    other.euid = 11;

    acl_from_mode(0040755u, 10, 10, &a);
    check(acl_access(&a, &owner, acl_mask_for_posix(0, 1, 0, 1)) == 0,
         "the owner of a 0755 directory passes access(W_OK) - it used to "
         "fail, asking for ACE_DELETE_CHILD that nothing granted");
    check(acl_access(&a, &other, ACE_DELETE_CHILD) == -13,
         "a stranger to a 0755 directory may not delete children in it");

    acl_from_mode(0100644u, 10, 10, &a);
    check((a.ace[0].mask & ACE_DELETE_CHILD) == 0,
         "a FILE's w grants no ACE_DELETE_CHILD - the bit means nothing "
         "there");

    acl_from_mode(0040700u, 10, 10, &a);
    acl_apply_chmod(&a, 0040777u, &b);
    check(acl_access(&b, &other, ACE_DELETE_CHILD) == 0,
         "chmod 0777 on a directory grants it to everyone, as acl_from_mode "
         "would");
    acl_apply_chmod(&b, 0040755u, &a);
    check(acl_access(&a, &other, ACE_DELETE_CHILD) == -13,
         "and chmod back to 0755 takes it away again");
}

static int fake_rename_calls;

static int fake_rename(fs_volume_t *v, const char *o, const char *n) {
    (void)v; (void)o; (void)n;
    fake_rename_calls++;
    return 0;
}

/* Removing a name: ACE_DELETE on the object, or ACE_DELETE_CHILD + search
 * on its directory. Rename is gated as a removal plus an addition; gnfs has
 * no rename slot yet, so a copy of its ops table with a stub rename stands
 * in - the gate is entirely the VFS's, and what is checked is whether the
 * call is let through to the filesystem at all. */
static void test_delete_and_rename_check(void) {
    uint64 image_bytes = 1 * 1024 * 1024;
    uint8 *image = (uint8 *)calloc(1, (size_t)image_bytes);
    mem_ctx_t mctx;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_ops_t ops_with_rename;
    fs_node_t n;
    cred_t root, user, stranger, third;
    const struct cred *R, *U, *S, *T;
    acl_t a;

    mctx.buf = image;
    mctx.len = image_bytes;
    check(gnfs_format(&mctx, mem_write, image_bytes) == 0,
         "format for the delete test must succeed");
    dev_stub_attach(&dev, &stub, image, image_bytes);
    v = gnfs_probe(&dev);
    check(v != NULL, "mounting it must succeed");
    if (v == NULL) {
        free(image);
        return;
    }
    check(fs_mount_at("/gd", v) == 0, "and it goes into the mount table");

    cred_init_nobody(&root);
    root.euid = 0;
    cred_init_nobody(&user);
    user.euid = 1000;
    user.egid = 1000;
    cred_init_nobody(&stranger);
    stranger.euid = 2000;
    stranger.egid = 2000;
    cred_init_nobody(&third);
    third.euid = 3000;
    third.egid = 3000;
    R = (const struct cred *)&root;
    U = (const struct cred *)&user;
    S = (const struct cred *)&stranger;
    T = (const struct cred *)&third;

    /* A file uid 1000 OWNS, in a directory it does not. */
    check(fs_mkdir("/gd/pub", R, 0755u) == 0 && fs_create("/gd/pub/f", R, 0644u) == 0 &&
         fs_lookup("/gd/pub/f", &n) == 0 &&
         fs_setowner(&n, R, 1000, 1000) == 0,
         "root makes /gd/pub/f and gives the FILE to uid 1000");
    check(fs_unlink("/gd/pub/f", U) == -13,
         "owning a file does not let you unlink it - that is the "
         "directory's call");
    check(fs_lookup("/gd/pub/f", &n) == 0, "and it really is still there");

    check(fs_lookup("/gd/pub", &n) == 0 &&
         fs_setowner(&n, R, 1000, 1000) == 0,
         "root gives the DIRECTORY to uid 1000");
    check(fs_unlink("/gd/pub/f", S) == -13,
         "a stranger still may not unlink in it");
    check(fs_unlink("/gd/pub/f", U) == 0,
         "but its owner now may - the control");
    check(fs_lookup("/gd/pub/f", &n) == -2, "and the file is gone");
    check(fs_unlink("/gd/pub/f", U) == -2,
         "unlinking it again is -ENOENT, not a permission answer");

    check(fs_mkdir("/gd/pub/sub", R, 0755u) == 0,
         "root makes a subdirectory in uid 1000's directory");
    check(fs_rmdir("/gd/pub/sub", S) == -13, "a stranger may not rmdir it");
    check(fs_rmdir("/gd/pub/sub", U) == 0,
         "the parent's owner may, though root made it");

    /* ACE_DELETE on the object alone - no right on the parent at all. */
    check(fs_create("/gd/locked", R, 0644u) == 0 &&
         fs_lookup("/gd/locked", &n) == 0,
         "root makes /gd/locked in root's own 0755 directory");
    acl_from_mode(0100644u, 0, 0, &a);
    a.ace[a.count].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    a.ace[a.count].flags = 0;
    a.ace[a.count].mask  = ACE_DELETE;
    a.ace[a.count].who   = 2000;
    a.count++;
    a.trivial = 0;
    check(fs_setacl(&n, R, (const struct acl *)&a) == 0,
         "and grants uid 2000 ACE_DELETE on that one file");
    check(fs_unlink("/gd/locked", T) == -13,
         "uid 3000, named nowhere, may not delete it");
    check(fs_unlink("/gd/locked", S) == 0,
         "uid 2000 may, on the object's own ACE_DELETE, with no right on "
         "the parent - the NFSv4 either-or");

    /* rename, through a stub that only counts arrivals. */
    ops_with_rename = *v->ops;
    ops_with_rename.rename = fake_rename;
    v->ops = &ops_with_rename;
    fake_rename_calls = 0;

    check(fs_create("/gd/pub/r", U, 0644u) == 0, "uid 1000 creates /gd/pub/r");
    check(fs_rename("/gd/pub/r", "/gd/pub/r2", S) == -13 &&
         fake_rename_calls == 0,
         "a stranger may not rename in uid 1000's directory, and the "
         "filesystem is never asked");
    check(fs_rename("/gd/pub/r", "/gd/r3", U) == -13 &&
         fake_rename_calls == 0,
         "nor may the owner move it INTO root's directory - the add half");
    check(fs_create("/gd/pub/victim", R, 0644u) == 0,
         "root puts /gd/pub/victim in uid 1000's directory");
    check(fs_rename("/gd/pub/r", "/gd/pub/r2", U) == 0 &&
         fake_rename_calls == 1,
         "within its own directory the owner may - the control");
    /* Replacing is deleting. A mode word cannot separate the two halves
     * (a directory's w grants add AND delete-child together), so this
     * needs a drop-box ACL: uid 1000 may ADD to /gd/drop, not delete. */
    check(fs_mkdir("/gd/drop", R, 0755u) == 0 && fs_lookup("/gd/drop", &n) == 0,
         "root makes /gd/drop");
    acl_from_mode(0040755u, 0, 0, &a);
    a.ace[a.count].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    a.ace[a.count].flags = 0;
    a.ace[a.count].mask  = ACE_ADD_FILE | ACE_EXECUTE;
    a.ace[a.count].who   = 1000;
    a.count++;
    a.trivial = 0;
    check(fs_setacl(&n, R, (const struct acl *)&a) == 0 &&
         fs_create("/gd/drop/existing", R, 0644u) == 0,
         "grants uid 1000 add-but-not-delete, and holds a file of root's");
    check(fs_rename("/gd/pub/r", "/gd/drop/new", U) == 0 &&
         fake_rename_calls == 2,
         "uid 1000 may move a file INTO the drop box - the add half alone");
    check(fs_rename("/gd/pub/r", "/gd/drop/existing", U) == -13 &&
         fake_rename_calls == 2,
         "but may not rename OVER the file already there - replacing it "
         "would delete it, which that ACL does not allow");
    check(fs_rename("/gd/pub/r", "/gd/drop/existing", R) == 0 &&
         fake_rename_calls == 3,
         "root may");

    fs_unmount_volume(v);
    free(image);
}

/* acl_chmod_mode's one silent rule, as pure logic. */
static void test_acl_chmod_mode(void) {
    acl_t a;
    cred_t member, outsider, supreme;

    acl_from_mode(0100755u, 10, 50, &a);          /* group 50 */
    cred_init_nobody(&member);
    member.euid = 10;
    member.egid = 7;
    member.groups[0] = 50;
    member.ngroups = 1;
    cred_init_nobody(&outsider);
    outsider.euid = 10;
    outsider.egid = 7;
    cred_init_nobody(&supreme);
    supreme.euid = 99;
    supreme.supreme = 1;

    check(acl_chmod_mode(&a, &outsider, 0100000u, 04755u) == 0104755u,
         "setuid is kept for anyone chmod lets through at all");
    check(acl_chmod_mode(&a, &outsider, 0100000u, 02755u) == 0100755u,
         "setgid is silently dropped for a caller not in the file's group");
    check(acl_chmod_mode(&a, &member, 0100000u, 02755u) == 0102755u,
         "and kept for one who is - through a SUPPLEMENTARY group here");
    check(acl_chmod_mode(&a, &supreme, 0100000u, 02755u) == 0102755u,
         "and kept for supreme, who is in no group at all");
    check(acl_chmod_mode(&a, &outsider, 0040000u, 01777u) == 0041777u,
         "sticky on a directory is kept");
    check(acl_chmod_mode(&a, &outsider, 0100000u, 0170755u) == 0100755u,
         "type bits in the request are ignored - the type is the object's");
}

/* chmod's special bits, end to end through fs_chmod on a mounted volume -
 * before, gnfs carried them over from the old mode and no chmod could
 * change them in either direction. */
static void test_chmod_special_bits(void) {
    uint64 image_bytes = 1 * 1024 * 1024;
    uint8 *image = (uint8 *)calloc(1, (size_t)image_bytes);
    mem_ctx_t mctx;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_node_t f, d;
    cred_t root, user, stranger;
    const struct cred *R, *U, *S;
    acl_t a;

    cred_init_nobody(&root);
    root.euid = 0;
    R = (const struct cred *)&root;

    mctx.buf = image;
    mctx.len = image_bytes;
    check(gnfs_format(&mctx, mem_write, image_bytes) == 0,
         "format for the chmod test must succeed");
    dev_stub_attach(&dev, &stub, image, image_bytes);
    v = gnfs_probe(&dev);
    check(v != NULL, "mounting it must succeed");
    if (v == NULL) {
        free(image);
        return;
    }
    check(fs_mount_at("/gc", v) == 0, "and it goes into the mount table");

    cred_init_nobody(&user);
    user.euid = 1000;
    user.egid = 1000;
    cred_init_nobody(&stranger);
    stranger.euid = 2000;
    stranger.egid = 2000;
    U = (const struct cred *)&user;
    S = (const struct cred *)&stranger;

    /* Made by the kernel, then given to uid 1000: /gc itself is root's
     * 0755, and uid 1000 may not create in it. */
    if (fs_create("/gc/prog", NULL, 0644u) != 0 || fs_lookup("/gc/prog", &f) != 0 ||
        fs_setowner(&f, R, 1000, 1000) != 0 ||
        fs_mkdir("/gc/shared", NULL, 0755u) != 0 || fs_lookup("/gc/shared", &d) != 0 ||
        fs_setowner(&d, R, 1000, 1000) != 0) {
        check(0, "setting up /gc/prog and /gc/shared for uid 1000");
        fs_unmount_volume(v);
        free(image);
        return;
    }
    check(fs_chmod(&f, S, 04755u) == -1,
         "a stranger may not chmod it - -EPERM, POSIX's errno for chmod, not the -EACCES an access check gives");
    check(fs_chmod(&f, U, 04755u) == 0 && (f.mode & 07777) == 04755u,
         "its owner sets setuid - the bit is really there now");
    check(fs_chmod(&f, U, 0755u) == 0 && (f.mode & 07777) == 0755u,
         "and clears it again");
    check(fs_chmod(&f, U, 02755u) == 0 && (f.mode & 07777) == 02755u,
         "setgid, into the owner's own group, is kept");

    check(fs_chmod(&d, U, 03777u) == 0 && (d.mode & 07777) == 03777u,
         "a directory takes setgid and sticky together");

    {
        fs_node_t g;

        check(fs_create("/gc/other", NULL, 0644u) == 0 &&
             fs_lookup("/gc/other", &g) == 0 &&
             fs_setowner(&g, R, 1000, 50) == 0,
             "root makes /gc/other, owned by uid 1000 but in group 50");
        check(fs_chmod(&g, U, 02755u) == 0 && (g.mode & 07777) == 0755u,
             "its owner's chmod 02755 SUCCEEDS but without setgid - uid "
             "1000 is not in group 50");
    }

    /* A plain ACL write leaves them alone. */
    check(fs_getacl(&d, (struct acl *)&a) == 0 &&
         fs_setacl(&d, U, (const struct acl *)&a) == 0 &&
         (d.mode & 07000) == 03000u,
         "fs_setacl, which carries no mode, keeps the special bits");

    {
        device_t dev2;
        dev_stub_t stub2;
        fs_volume_t *v2;
        fs_node_t n2;

        fs_unmount_volume(v);
        dev_stub_attach(&dev2, &stub2, image, image_bytes);
        v2 = gnfs_probe(&dev2);
        check(v2 != NULL, "the volume remounts");
        if (v2 != NULL) {
            check(v2->ops->lookup(v2, "/prog", &n2) == 0 &&
                 (n2.mode & 07777) == 02755u,
                 "the file's setgid survived a remount");
            check(v2->ops->lookup(v2, "/shared", &n2) == 0 &&
                 (n2.mode & 07777) == 03777u,
                 "and so did the directory's setgid+sticky");
            v2->ops->unmount(v2);
        }
    }
    free(image);
}

/* The two directory special bits, now that chmod can set them: setgid
 * decides new objects' group, sticky narrows who may remove entries. Each
 * rule is checked against the same operation in a plain 0777 directory, so
 * it is the bit doing the refusing and not something else. */
static void test_setgid_and_sticky_dirs(void) {
    uint64 image_bytes = 1 * 1024 * 1024;
    uint8 *image = (uint8 *)calloc(1, (size_t)image_bytes);
    mem_ctx_t mctx;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_node_t n;
    cred_t root, user, stranger, downer;
    const struct cred *R, *U, *S, *D;
    acl_t a;

    mctx.buf = image;
    mctx.len = image_bytes;
    check(gnfs_format(&mctx, mem_write, image_bytes) == 0,
         "format for the setgid/sticky test must succeed");
    dev_stub_attach(&dev, &stub, image, image_bytes);
    v = gnfs_probe(&dev);
    check(v != NULL, "mounting it must succeed");
    if (v == NULL) {
        free(image);
        return;
    }
    check(fs_mount_at("/gs", v) == 0, "and it goes into the mount table");

    cred_init_nobody(&root);
    root.euid = 0;
    cred_init_nobody(&user);
    user.euid = 1000;
    user.egid = 1000;
    cred_init_nobody(&stranger);
    stranger.euid = 2000;
    stranger.egid = 2000;
    cred_init_nobody(&downer);
    downer.euid = 3000;
    downer.egid = 3000;
    R = (const struct cred *)&root;
    U = (const struct cred *)&user;
    S = (const struct cred *)&stranger;
    D = (const struct cred *)&downer;

    /* --- setgid ---------------------------------------------------------- */
    check(fs_mkdir("/gs/plain", R, 0755u) == 0 && fs_lookup("/gs/plain", &n) == 0 &&
         fs_chmod(&n, R, 0777u) == 0,
         "root makes /gs/plain, 0777, no setgid");
    check(fs_create("/gs/plain/f", U, 0644u) == 0 &&
         fs_lookup("/gs/plain/f", &n) == 0 && n.gid == 1000,
         "a file made there takes its creator's egid - the control");

    check(fs_mkdir("/gs/proj", R, 0755u) == 0 && fs_lookup("/gs/proj", &n) == 0 &&
         fs_setowner(&n, R, ACL_CHOWN_KEEP, 50) == 0 &&
         fs_chmod(&n, R, 02777u) == 0 && (n.mode & 07777) == 02777u,
         "root makes /gs/proj, group 50, setgid");
    check(fs_create("/gs/proj/f", U, 0644u) == 0 &&
         fs_lookup("/gs/proj/f", &n) == 0 && n.uid == 1000 && n.gid == 50,
         "a file made there by uid 1000 is still ITS file, but in group 50 "
         "- the directory's, though uid 1000 is not even a member");
    check((n.mode & S_ISGID) == 0,
         "and a FILE is not born setgid");
    check(fs_mkdir("/gs/proj/sub", U, 0755u) == 0 &&
         fs_lookup("/gs/proj/sub", &n) == 0 && n.gid == 50 &&
         (n.mode & S_ISGID) != 0,
         "a DIRECTORY made there is group 50 AND setgid, so the rule "
         "carries on below it");
    check(fs_create("/gs/proj/sub/deep", U, 0644u) == 0 &&
         fs_lookup("/gs/proj/sub/deep", &n) == 0 && n.gid == 50,
         "one level further down, still group 50");

    /* --- sticky ---------------------------------------------------------- */
    check(fs_create("/gs/plain/g", U, 0644u) == 0 &&
         fs_unlink("/gs/plain/g", S) == 0,
         "in a plain 0777 directory a stranger may delete uid 1000's file "
         "- the control for everything below");

    check(fs_mkdir("/gs/tmp", R, 0755u) == 0 && fs_lookup("/gs/tmp", &n) == 0 &&
         fs_chmod(&n, R, 01777u) == 0 && (n.mode & 07777) == 01777u,
         "root makes /gs/tmp, 01777");
    check(fs_create("/gs/tmp/u", U, 0644u) == 0 && fs_create("/gs/tmp/s", S, 0644u) == 0,
         "uid 1000 and uid 2000 each make a file in it");
    check(fs_unlink("/gs/tmp/u", S) == -1,
         "uid 2000 may NOT delete uid 1000's file there - -EPERM, the "
         "sticky bit, though the directory's 0777 would allow it");
    check(fs_lookup("/gs/tmp/u", &n) == 0, "and it is still there");
    check(fs_unlink("/gs/tmp/u", U) == 0,
         "uid 1000 may delete its own");
    check(fs_unlink("/gs/tmp/s", R) == 0,
         "root - the directory's owner and supreme - may delete uid 2000's");

    check(fs_mkdir("/gs/tmp2", R, 0755u) == 0 && fs_lookup("/gs/tmp2", &n) == 0 &&
         fs_setowner(&n, R, 3000, 3000) == 0 &&
         fs_chmod(&n, D, 01777u) == 0,
         "uid 3000 owns a sticky /gs/tmp2 of its own");
    check(fs_create("/gs/tmp2/u", U, 0644u) == 0 && fs_unlink("/gs/tmp2/u", D) == 0,
         "and, as the DIRECTORY's owner, may delete uid 1000's file in it");

    check(fs_create("/gs/tmp/granted", U, 0644u) == 0 &&
         fs_lookup("/gs/tmp/granted", &n) == 0 &&
         fs_getacl(&n, (struct acl *)&a) == 0, "uid 1000 makes another");
    a.ace[a.count].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    a.ace[a.count].flags = 0;
    a.ace[a.count].mask  = ACE_DELETE;
    a.ace[a.count].who   = 2000;
    a.count++;
    a.trivial = 0;
    check(fs_setacl(&n, U, (const struct acl *)&a) == 0 &&
         fs_unlink("/gs/tmp/granted", S) == 0,
         "an explicit ACE_DELETE its owner grants uid 2000 still works in a "
         "sticky directory - the bit narrows the parent route only");

    fs_unmount_volume(v);
    free(image);
}

/* The mode a create/mkdir asks for is the mode it gets - its permission
 * bits, plus sticky for a directory; setuid/setgid never come from a
 * create. (umask is applied a layer up, in the syscalls.) */
static void test_create_mode(void) {
    uint64 image_bytes = 1 * 1024 * 1024;
    uint8 *image = (uint8 *)calloc(1, (size_t)image_bytes);
    mem_ctx_t mctx;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_node_t n;
    cred_t root;
    const struct cred *R;

    mctx.buf = image;
    mctx.len = image_bytes;
    check(gnfs_format(&mctx, mem_write, image_bytes) == 0,
         "format for the create-mode test must succeed");
    dev_stub_attach(&dev, &stub, image, image_bytes);
    v = gnfs_probe(&dev);
    check(v != NULL, "mounting it must succeed");
    if (v == NULL) {
        free(image);
        return;
    }
    check(fs_mount_at("/gm", v) == 0, "and it goes into the mount table");
    cred_init_nobody(&root);
    root.euid = 0;
    R = (const struct cred *)&root;

    check(fs_create("/gm/secret", R, 0600u) == 0 &&
         fs_lookup("/gm/secret", &n) == 0 && n.mode == 0100600u,
         "create with 0600 makes a 0600 regular file - not the fixed 0644 "
         "it used to be");
    check(fs_mkdir("/gm/private", R, 0700u) == 0 &&
         fs_lookup("/gm/private", &n) == 0 && n.mode == 0040700u,
         "mkdir with 0700 makes a 0700 directory - mkdir -m 700 is no "
         "longer world-readable");
    {
        cred_t other;

        cred_init_nobody(&other);
        other.euid = 4242;
        check(fs_access(&n, (const struct cred *)&other, ACE_READ_DATA) == -13,
             "and a stranger really cannot list it");
    }
    check(fs_mkdir("/gm/tmp", R, 01777u) == 0 &&
         fs_lookup("/gm/tmp", &n) == 0 && n.mode == 0041777u,
         "mkdir keeps the sticky bit");
    check(fs_create("/gm/suid", R, 04755u) == 0 &&
         fs_lookup("/gm/suid", &n) == 0 && n.mode == 0100755u,
         "create drops setuid - that comes from chmod, never a create");
    check(fs_mkdir("/gm/sg", R, 02755u) == 0 &&
         fs_lookup("/gm/sg", &n) == 0 && n.mode == 0040755u,
         "mkdir drops a REQUESTED setgid too");
    check(fs_chmod(&n, R, 02755u) == 0 &&
         fs_mkdir("/gm/sg/child", R, 0700u) == 0 &&
         fs_lookup("/gm/sg/child", &n) == 0 && n.mode == 0042700u,
         "but a setgid PARENT still makes a child directory setgid, on "
         "top of the mode it asked for");

    fs_unmount_volume(v);
    free(image);
}

/* --- gnfs version 2: big files, reuse, big directories, rename, snapshots -- */

typedef struct {
    uint8      *image;
    uint64      bytes;
    device_t    dev;
    dev_stub_t  stub;
    fs_volume_t *v;
} vol_t;

static fs_volume_t *vol_open(vol_t *t, uint64 bytes) {
    mem_ctx_t mctx;

    t->image = (uint8 *)calloc(1, (size_t)bytes);
    t->bytes = bytes;
    mctx.buf = t->image;
    mctx.len = bytes;
    if (t->image == NULL || gnfs_format(&mctx, mem_write, bytes) != 0) {
        return NULL;
    }
    dev_stub_attach(&t->dev, &t->stub, t->image, bytes);
    t->v = gnfs_probe(&t->dev);
    return t->v;
}

/* Tear the mount down and mount the SAME bytes again - the only way to know
 * a change reached the medium rather than living in the mount's memory. */
static fs_volume_t *vol_remount(vol_t *t) {
    t->v->ops->unmount(t->v);
    dev_stub_attach(&t->dev, &t->stub, t->image, t->bytes);
    t->v = gnfs_probe(&t->dev);
    return t->v;
}

static void vol_close(vol_t *t) {
    if (t->v != NULL) {
        t->v->ops->unmount(t->v);
    }
    free(t->image);
}

static uint64 free_blocks(fs_volume_t *v) {
    fs_statfs_t st;

    return v->ops->statfs(v, &st) == 0 ? st.blocks_free : 0;
}

static uint8 pattern(uint64 pos) {
    return (uint8)((pos * 2654435761ULL) >> 13);
}

static void test_large_and_sparse_files(void) {
    vol_t t;
    fs_volume_t *v = vol_open(&t, 16ULL * 1024 * 1024);
    fs_node_t n;
    uint64 before, total = 2560ULL * 1024;     /* 2.5MB: all three levels */
    uint64 chunk = 64 * 1024, off, i;
    uint8 *buf = (uint8 *)malloc((size_t)chunk);
    int ok;

    check(v != NULL && buf != NULL, "big-file volume mounts");
    if (v == NULL || buf == NULL) {
        free(buf);
        vol_close(&t);
        return;
    }
    before = free_blocks(v);
    check(v->ops->create(v, "/big", NULL, 0644u) == 0 &&
         v->ops->lookup(v, "/big", &n) == 0, "create /big");

    ok = 1;
    for (off = 0; off < total; off += chunk) {
        for (i = 0; i < chunk; i++) {
            buf[i] = pattern(off + i);
        }
        if (v->ops->write(v, &n, off, buf, chunk) != (int64)chunk) {
            ok = 0;
            break;
        }
    }
    check(ok, "2.5MB written - through the 12 direct blocks, the whole "
              "indirect block, and into the double-indirect one (v1 refused "
              "anything past 48KB)");

    v = vol_remount(&t);
    check(v != NULL && v->ops->lookup(v, "/big", &n) == 0 && n.size == total,
         "after a remount /big is still 2.5MB");
    ok = 1;
    for (off = 0; v != NULL && off < total; off += chunk) {
        if (v->ops->read(v, &n, off, buf, chunk) != (int64)chunk) {
            ok = 0;
            break;
        }
        for (i = 0; i < chunk; i++) {
            if (buf[i] != pattern(off + i)) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
    }
    check(ok, "and every byte reads back, across every mapping level");

    /* Sparse, right at the edge of what the map can address. */
    {
        uint64 limit = GNFS_MAX_FILE_BLOCKS * (uint64)GNFS_BLOCK_SIZE;
        uint8 tail[16], got[16];

        for (i = 0; i < 16; i++) {
            tail[i] = (uint8)(0xA0 + i);
        }
        check(v->ops->create(v, "/sparse", NULL, 0644u) == 0 &&
             v->ops->lookup(v, "/sparse", &n) == 0 &&
             v->ops->write(v, &n, limit - 16, tail, 16) == 16,
             "a write in the last 16 bytes the map can address (~1GB) "
             "succeeds on a 16MB volume - everything before it is a hole");
        check(n.size == limit, "and the file is that long");
        check(v->ops->read(v, &n, limit - 16, got, 16) == 16 &&
             memcmp(got, tail, 16) == 0, "the bytes read back");
        check(v->ops->read(v, &n, 1ULL << 29, got, 16) == 16 &&
             got[0] == 0 && got[15] == 0, "and a hole in the middle reads "
             "as zeroes");
        check(v->ops->write(v, &n, limit, tail, 1) == -27,
             "one byte past the map is -EFBIG, not a truncated write");
    }

    check(v->ops->unlink(v, "/big") == 0 && v->ops->unlink(v, "/sparse") == 0,
         "both files unlink");
    check(free_blocks(v) == before,
         "and the free count is EXACTLY where it started - every data block "
         "and every pointer block came back");
    free(buf);
    vol_close(&t);
}

static void test_objects_are_reused(void) {
    vol_t t;
    fs_volume_t *v = vol_open(&t, 4ULL * 1024 * 1024);
    int i, ok = 1;

    check(v != NULL, "reuse volume mounts");
    if (v == NULL) {
        vol_close(&t);
        return;
    }
    for (i = 0; i < 2000; i++) {
        if (v->ops->create(v, "/churn", NULL, 0644u) != 0 ||
            v->ops->unlink(v, "/churn") != 0) {
            ok = 0;
            break;
        }
    }
    check(ok, "2000 create/unlink cycles on a volume with 256 object slots "
              "all succeed - v1 never reused a number and was full for "
              "good after 255 creates");
    vol_close(&t);
}

static int count_cb(const fs_dirent_t *e, void *ctx) {
    (void)e;
    (*(int *)ctx)++;
    return 0;
}

static void test_big_directory(void) {
    vol_t t;
    fs_volume_t *v = vol_open(&t, 8ULL * 1024 * 1024);
    fs_node_t d, n;
    char path[64];
    int i, ok = 1, count = 0;

    check(v != NULL, "big-directory volume mounts");
    if (v == NULL) {
        vol_close(&t);
        return;
    }
    check(v->ops->mkdir(v, "/many", NULL, 0755u) == 0, "mkdir /many");
    for (i = 0; i < 300 && ok; i++) {
        snprintf(path, sizeof(path), "/many/file-number-%d", i);
        ok = v->ops->create(v, path, NULL, 0644u) == 0;
    }
    check(ok, "300 entries in one directory - a block holds 51, so it grew "
              "to six blocks (v1 stopped at 51 with -ENOSPC)");
    v = vol_remount(&t);
    ok = v != NULL;
    for (i = 0; ok && i < 300; i++) {
        snprintf(path, sizeof(path), "/many/file-number-%d", i);
        ok = v->ops->lookup(v, path, &n) == 0;
    }
    check(ok, "after a remount every one of them resolves");
    check(v->ops->lookup(v, "/many", &d) == 0 &&
         v->ops->iterate(v, &d, count_cb, &count) == 0 && count == 300,
         "and iterating the directory yields exactly 300");
    check(v->ops->rmdir(v, "/many") == -39,
         "rmdir refuses it while it holds anything");
    ok = 1;
    for (i = 0; ok && i < 300; i++) {
        snprintf(path, sizeof(path), "/many/file-number-%d", i);
        ok = v->ops->unlink(v, path) == 0;
    }
    check(ok && v->ops->rmdir(v, "/many") == 0,
         "and removes it once all 300 are gone");
    vol_close(&t);
}

static int read_all(fs_volume_t *v, const char *path, char *out, int cap) {
    fs_node_t n;
    int64 got;

    if (v->ops->lookup(v, path, &n) != 0) {
        return -1;
    }
    got = v->ops->read(v, &n, 0, out, (uint64)cap - 1);
    if (got < 0) {
        return -1;
    }
    out[got] = '\0';
    return (int)got;
}

static int put(fs_volume_t *v, const char *path, const char *text) {
    fs_node_t n;

    if (v->ops->lookup(v, path, &n) != 0 &&
        v->ops->create(v, path, NULL, 0644u) != 0) {
        return -1;
    }
    if (v->ops->lookup(v, path, &n) != 0) {
        return -1;
    }
    if (v->ops->truncate(v, &n, 0) != 0) {
        return -1;
    }
    return v->ops->write(v, &n, 0, text, strlen(text)) ==
           (int64)strlen(text) ? 0 : -1;
}

static void test_rename(void) {
    vol_t t;
    fs_volume_t *v = vol_open(&t, 4ULL * 1024 * 1024);
    fs_node_t n;
    char buf[64];

    check(v != NULL, "rename volume mounts");
    if (v == NULL) {
        vol_close(&t);
        return;
    }
    check(put(v, "/a", "alpha") == 0 && put(v, "/b", "bravo") == 0 &&
         v->ops->mkdir(v, "/d1", NULL, 0755u) == 0 &&
         v->ops->mkdir(v, "/d2", NULL, 0755u) == 0 &&
         put(v, "/d2/inner", "x") == 0, "set up /a /b /d1 /d2/inner");

    check(v->ops->rename(v, "/a", "/a2") == 0 &&
         v->ops->lookup(v, "/a", &n) == -2 &&
         read_all(v, "/a2", buf, sizeof(buf)) == 5 && strcmp(buf, "alpha") == 0,
         "a rename in place: the old name is gone and the new one has the "
         "contents");
    check(v->ops->rename(v, "/a2", "/d1/a3") == 0 &&
         read_all(v, "/d1/a3", buf, sizeof(buf)) == 5,
         "across directories");
    check(v->ops->rename(v, "/b", "/d1/a3") == 0 &&
         read_all(v, "/d1/a3", buf, sizeof(buf)) == 5 &&
         strcmp(buf, "bravo") == 0 && v->ops->lookup(v, "/b", &n) == -2,
         "onto an existing file REPLACES it");
    check(v->ops->rename(v, "/d1/a3", "/d1/a3") == 0,
         "onto itself is a successful no-op");
    check(v->ops->rename(v, "/d1", "/d2") == -39,
         "a directory onto a NON-empty directory is -ENOTEMPTY");
    check(v->ops->rename(v, "/d1", "/d1/sub") == -22,
         "a directory into itself is -EINVAL");
    check(v->ops->rename(v, "/d1/a3", "/d2") == -21,
         "a file onto a directory is -EISDIR");
    check(v->ops->rename(v, "/d2", "/d1/a3") == -20,
         "a directory onto a file is -ENOTDIR");
    check(v->ops->rename(v, "/nope", "/x") == -2, "a missing source is -ENOENT");
    check(v->ops->mkdir(v, "/empty", NULL, 0755u) == 0 &&
         v->ops->rename(v, "/d1", "/empty") == 0 &&
         read_all(v, "/empty/a3", buf, sizeof(buf)) == 5,
         "a directory onto an EMPTY directory replaces it, contents and all");

    v = vol_remount(&t);
    check(v != NULL && read_all(v, "/empty/a3", buf, sizeof(buf)) == 5 &&
         strcmp(buf, "bravo") == 0 && v->ops->lookup(v, "/d1", &n) == -2,
         "and all of it survived a remount");
    vol_close(&t);
}

/* Truncate a file into the middle of a block, then grow it again: the
 * bytes that were cut off must read back as ZERO. v1 left them in the
 * block's tail and a later grow exposed them. */
static void test_truncate_does_not_resurrect(void) {
    vol_t t;
    fs_volume_t *v = vol_open(&t, 4ULL * 1024 * 1024);
    fs_node_t n;
    uint8 buf[100];
    int i, clean = 1;

    check(v != NULL, "truncate volume mounts");
    if (v == NULL) {
        vol_close(&t);
        return;
    }
    memset(buf, 'S', sizeof(buf));
    check(v->ops->create(v, "/t", NULL, 0644u) == 0 &&
         v->ops->lookup(v, "/t", &n) == 0 &&
         v->ops->write(v, &n, 0, buf, 100) == 100, "write 100 bytes of 'S'");
    check(v->ops->truncate(v, &n, 10) == 0 && n.size == 10,
         "truncate to 10");
    check(v->ops->truncate(v, &n, 100) == 0 && n.size == 100,
         "and grow back to 100");
    memset(buf, 0xEE, sizeof(buf));
    check(v->ops->read(v, &n, 0, buf, 100) == 100, "read it all");
    for (i = 10; i < 100; i++) {
        if (buf[i] != 0) {
            clean = 0;
        }
    }
    check(buf[0] == 'S' && buf[9] == 'S' && clean,
         "bytes 10..99 read as ZERO, not the 'S' that was cut off");
    vol_close(&t);
}

static void test_snapshots(void) {
    vol_t t;
    fs_volume_t *v = vol_open(&t, 8ULL * 1024 * 1024);
    fs_node_t n, sd;
    char buf[64];
    uint64 before_snap, after_delete;
    int count = 0;

    check(v != NULL, "snapshot volume mounts");
    if (v == NULL) {
        vol_close(&t);
        return;
    }
    check(put(v, "/keep", "original") == 0 && put(v, "/gone", "doomed") == 0,
         "set up /keep and /gone");
    before_snap = free_blocks(v);

    check(v->ops->mkdir(v, "/.snapshots/s1", NULL, 0755u) == 0,
         "mkdir /.snapshots/s1 takes a snapshot");
    check(v->ops->mkdir(v, "/.snapshots/s1", NULL, 0755u) == -17,
         "a second snapshot of the same name is -EEXIST");

    check(put(v, "/keep", "CHANGED!") == 0 && v->ops->unlink(v, "/gone") == 0,
         "then /keep is rewritten and /gone deleted");
    check(read_all(v, "/keep", buf, sizeof(buf)) > 0 &&
         strcmp(buf, "CHANGED!") == 0, "the live /keep has the new contents");
    check(read_all(v, "/.snapshots/s1/keep", buf, sizeof(buf)) > 0 &&
         strcmp(buf, "original") == 0,
         "and the snapshot still has the ORIGINAL - COW left the old blocks "
         "alone and the allocator did not hand them out again");
    check(read_all(v, "/.snapshots/s1/gone", buf, sizeof(buf)) > 0 &&
         strcmp(buf, "doomed") == 0,
         "a file deleted since is still there in the snapshot");

    check(v->ops->lookup(v, "/.snapshots/s1/keep", &n) == 0 &&
         v->ops->write(v, &n, 0, "x", 1) == -30,
         "a snapshot is read-only: a write is -EROFS");
    check(v->ops->create(v, "/.snapshots/s1/new", NULL, 0644u) == -30,
         "and so is creating in it");
    check(v->ops->lookup(v, "/.snapshots", &sd) == 0 &&
         v->ops->iterate(v, &sd, count_cb, &count) == 0 && count == 1,
         "/.snapshots lists the one snapshot");

    /* Fill the volume. If the allocator ever handed out a block the
     * snapshot still references, the snapshot's contents would change. */
    {
        uint8 fill[4096];
        fs_node_t f;
        uint64 off = 0;

        memset(fill, 0x55, sizeof(fill));
        check(v->ops->create(v, "/fill", NULL, 0644u) == 0 &&
             v->ops->lookup(v, "/fill", &f) == 0, "create /fill");
        while (v->ops->write(v, &f, off, fill, sizeof(fill)) ==
               (int64)sizeof(fill)) {
            off += sizeof(fill);
        }
        check(off > 0, "and write to it until the volume is full");
        check(read_all(v, "/.snapshots/s1/keep", buf, sizeof(buf)) > 0 &&
             strcmp(buf, "original") == 0 &&
             read_all(v, "/.snapshots/s1/gone", buf, sizeof(buf)) > 0 &&
             strcmp(buf, "doomed") == 0,
             "with the volume full, the snapshot is still intact - no block "
             "it holds was reused");
        check(v->ops->unlink(v, "/fill") == 0, "remove /fill");
    }

    v = vol_remount(&t);
    check(v != NULL && read_all(v, "/.snapshots/s1/keep", buf, sizeof(buf)) > 0
         && strcmp(buf, "original") == 0,
         "the snapshot survives a remount");

    check(v->ops->rmdir(v, "/.snapshots/s1") == 0,
         "rmdir /.snapshots/s1 deletes it");
    check(v->ops->lookup(v, "/.snapshots/s1/keep", &n) == -2,
         "after which it is gone");
    after_delete = free_blocks(v);
    check(after_delete > before_snap,
         "and the blocks only it was holding - /gone's, /keep's old ones - "
         "are free again (more free than before the snapshot, since /gone "
         "is deleted now)");
    v = vol_remount(&t);
    check(v != NULL && free_blocks(v) == after_delete,
         "and a remount agrees about exactly how much is free");
    vol_close(&t);
}

int gnfs_run_tests(void) {
    failures = 0;
    printf("\ngnfs:\n");

    test_checksum_is_sensitive();
    test_root_seal_and_valid();
    test_layout_helpers_agree_with_format();
    test_bitmap_alloc_first_fit();
    test_onode_init_and_access();
    test_onode_alloc_reuses_and_is_bounded();
    test_directory_entries();
    test_directory_iterate_and_full();
    test_acl_inherit();
    test_acl_apply_chmod();
    test_acl_chown_permitted();
    test_format_mount_and_commit_ring();
    test_object_layer_end_to_end();
    test_acl_end_to_end();
    test_chown_end_to_end();
    test_creator_owns();
    test_parent_write_check();
    test_dir_write_grants_delete_child();
    test_delete_and_rename_check();
    test_acl_chmod_mode();
    test_chmod_special_bits();
    test_setgid_and_sticky_dirs();
    test_create_mode();
    test_large_and_sparse_files();
    test_objects_are_reused();
    test_big_directory();
    test_rename();
    test_truncate_does_not_resurrect();
    test_snapshots();

    printf("gnfs: %s\n", failures ? "FAILED" : "passed");
    return failures;
}
