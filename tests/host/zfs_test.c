/* Host tests for the vendored ZFS reader, against pools that real OpenZFS
 * wrote.
 *
 * --- Why these fixtures and not one this file's author made ---------------
 * The lesson from the storage work is on record: a fixture written to match
 * the code's assumptions cannot contradict them, which is why part_test.c
 * only became useful once its images came from tools/fatfs.py instead of from
 * a hand-rolled writer. A ZFS fixture written here would be worse still,
 * because the format is not one this tree owns - agreement between a reader
 * and a writer that were written together proves they agree, and nothing
 * about ZFS.
 *
 * So the fixtures are OpenZFS's own test-suite pool images, shipped in its
 * source tree and created by real ZFS on real machines years ago:
 *
 *   zfs-pool-v1.dat     pool version 1, BIG-ENDIAN (Solaris/SPARC, 2006),
 *                       old znode_phys_t attributes, 301 files in the root
 *                       directory, uncompressed objsets
 *   unclean_export.dat  pool version 5000 with feature flags, LITTLE-ENDIAN,
 *                       System Attributes rather than znodes, lzjb-compressed
 *                       metadata, and a pool state that says it was never
 *                       cleanly exported
 *   cryptv0.dat         an ENCRYPTED pool: the refusal case
 *
 * Two of the three are byte-swapped relative to this machine, which is the
 * property that makes them worth having: a reader that only ever saw
 * little-endian pools would pass every test and fail on the first pool
 * anybody actually handed it.
 *
 * --- What is being tested -------------------------------------------------
 * Not the vendored code - FreeBSD tests that every time it boots. What is
 * being tested is the PORT: the compat headers, the libsa shim, the I/O
 * callback, and the object-number walk in zfs_glue.c that replaces the
 * dnode-carrying lookup Genesis's 64-byte node cannot hold. A wrong shim
 * shows up here as a pool that will not mount or a file that reads back
 * wrong, and both of those are this port's bugs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "dev_stub.h"
#include "fs.h"
#include "volume.h"

/* The seam, not the vendored headers: this file is on the Genesis side of the
 * wall and cannot see a dnode_phys_t any more than vfs.c can. */
#include "zfs_genesis.h"

/* The ACL vocabulary is Genesis-side, like everything else this file uses. */
#include "acl.h"
#include "ntsec.h"
#include "zfs.h"

fs_volume_t *zfs_probe(device_t *dev);

static int zfs_failures;

static void check(int cond, const char *what) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        zfs_failures++;
    }
}

static device_t   zdev;
static dev_stub_t zbacking;

/* Release a volume the way fs_unmount_volume would.
 *
 * zfs_vfs.c holds four per-volume slots and hands them back only through
 * fs_ops_t::unmount. These tests reach the filesystem through fs_lookup_on
 * without ever putting it in the mount table, so nothing was calling that
 * hook and every probe leaked a slot - the fifth pool this suite mounted
 * failed, and the symptom was "the pool mounts" failing on a pool that is
 * perfectly good.
 *
 * Calling it here is not a workaround for the test's convenience: it is what
 * the VFS does, so doing it makes the suite exercise the same release path
 * the kernel uses instead of quietly depending on never needing it. */
static void zfs_release(fs_volume_t *v) {
    if (v != NULL && v->ops != NULL && v->ops->unmount != NULL) {
        v->ops->unmount(v);
    }
}

struct count_ctx {
    int    entries;
    int    dirs;
    int    saw_named;
    char   named[64];
    uint64 named_ino;
};

static int count_cb(const fs_dirent_t *ent, void *ctx) {
    struct count_ctx *c = (struct count_ctx *)ctx;

    c->entries++;
    if (ent->is_dir) {
        c->dirs++;
    }
    if (c->named[0] != '\0' && strcmp(ent->name, c->named) == 0) {
        c->saw_named = 1;
        c->named_ino = ent->ino;
    }
    return 0;
}

static int stop_after_one(const fs_dirent_t *ent, void *ctx) {
    (void)ent;
    (*(int *)ctx)++;
    return 7;                                /* the caller's stop value */
}


/* --- the ACL fixture -------------------------------------------------------
 *
 * genesisacl.dat is a ZPL version 5 pool: attributes in the SA registry
 * rather than in a znode_phys_t, which is what a pool made this decade looks
 * like and what genesispool.dat deliberately is not. See
 * tests/host/fixtures/README.md for how it was built and exactly what is on
 * it.
 *
 * What this proves that no other test here does: the SA LAYOUT WALK. Every
 * other ZFS test passes against the vendored reader's fixed SA offsets. The
 * ACL is variable-length and sits behind thirteen other attributes, so
 * reaching it at all requires reading the layout out of the header, looking
 * it up in the LAYOUTS zap, and accumulating offsets - and if any step of
 * that is wrong the result is not a failure, it is a different ACL.
 */

/* --- the Windows view of the very same ACL ---------------------------------
 *
 * Not a second permission system and not a translation layer: the descriptor
 * built here is built from the acl_t that came off the ZFS pool, and the
 * access mask crosses into it unchanged because NFSv4's mask IS NT's. What
 * this test is really checking is the three things that are NOT identical -
 * the SIDs, the ACE flag byte, and the self-relative layout - plus the one
 * property that would be easiest to break by "tidying": the ORDER.
 */
static uint16 le16(const uint8 *p) { return (uint16)(p[0] | (p[1] << 8)); }
static uint32 le32(const uint8 *p) {
    return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
           ((uint32)p[3] << 24);
}

/* S-1-<auth>-<sub0>[-<sub1>], compared field by field. */
static int sid_is(const uint8 *p, uint64 auth, uint32 s0, int nsubs,
                  uint32 s1) {
    uint64 a;

    if (p[0] != 1 || p[1] != (uint8)nsubs) {
        return 0;
    }
    a = ((uint64)p[2] << 40) | ((uint64)p[3] << 32) | ((uint64)p[4] << 24) |
        ((uint64)p[5] << 16) | ((uint64)p[6] << 8) | (uint64)p[7];
    if (a != auth || le32(p + 8) != s0) {
        return 0;
    }
    if (nsubs > 1 && le32(p + 12) != s1) {
        return 0;
    }
    return 1;
}

static void test_nt_view(const acl_t *a) {
    uint8 sd[NTSEC_MAX_SD];
    const uint8 *dacl;
    const uint8 *ace;
    int64 n;
    uint32 dacl_off, owner_off, group_off;
    uint32 nfsv4_mask_of_ace1 = a->ace[1].mask;

    printf("\nntsec: the same ACL as a Windows SECURITY_DESCRIPTOR\n");

    /* The size query. A Win32 caller does this first, always. */
    n = ntsec_build(a, 0, 0);
    check(n > 0, "asking with a NULL buffer returns the size needed");

    n = ntsec_build(a, sd, sizeof(sd));
    check(n > 0, "and building it into a real buffer succeeds");
    if (n <= 0) {
        return;
    }
    check(ntsec_build(a, sd, 8) == -28,
          "a buffer too small is -ENOSPC, not a truncated descriptor");

    check(sd[0] == 1, "revision 1");
    check(le16(sd + 2) == (SE_SELF_RELATIVE | SE_DACL_PRESENT),
          "self-relative, with a DACL present");
    check(le32(sd + 12) == 0, "and no SACL - there is no auditing to report");

    owner_off = le32(sd + 4);
    group_off = le32(sd + 8);
    dacl_off  = le32(sd + 16);
    check(owner_off < (uint32)n && group_off < (uint32)n &&
          dacl_off < (uint32)n,
          "every offset lands inside the descriptor");

    /* Samba's idmap scheme: S-1-22-1-<uid>, S-1-22-2-<gid>. */
    check(sid_is(sd + owner_off, 22, 1, 2, 1000),
          "owner is S-1-22-1-1000 - uid 1000 as a SID");
    check(sid_is(sd + group_off, 22, 2, 2, 1000),
          "group is S-1-22-2-1000");

    dacl = sd + dacl_off;
    check(dacl[0] == NT_ACL_REVISION, "the DACL is ACL_REVISION 2");
    check(le16(dacl + 4) == 4, "with four ACEs, the same four ZFS stored");
    check(le16(dacl + 2) == (uint16)(n - dacl_off),
          "and an AclSize that covers exactly the rest of the descriptor");

    /* --- entry by entry, IN ORDER -----------------------------------------
     *
     * Windows calls a DACL "canonical" when its denies come first, and tools
     * offer to reorder one that is not. Doing that here would be wrong: both
     * NFSv4 and NT let the first entry to mention a bit decide it, so hoisting
     * this ACL's DENY everyone@ above the two allows would take the file away
     * from its owner and from uid 1001 both. The order is data. */
    ace = dacl + 8;
    check(ace[0] == 0, "0: ACCESS_ALLOWED");
    check(sid_is(ace + 8, 22, 1, 2, 1000), "   for the owner's own SID");

    ace += le16(ace + 2);
    check(ace[0] == 0, "1: ACCESS_ALLOWED");
    check(sid_is(ace + 8, 22, 1, 2, 1001),
          "   for S-1-22-1-1001 - the named user, now a SID");
    /* The point of the whole exercise. */
    check(le32(ace + 4) == nfsv4_mask_of_ace1,
          "   with the NFSv4 access mask copied through UNCHANGED - it is the "
          "same field, not a conversion");
    check((le32(ace + 4) & 0x00000001u) != 0,
          "   and 0x1 means FILE_READ_DATA to Windows and ACE_READ_DATA to ZFS");

    ace += le16(ace + 2);
    check(ace[0] == 1, "2: ACCESS_DENIED - still third, not hoisted to the front");
    check(sid_is(ace + 8, 1, 0, 1, 0), "   for S-1-1-0, Everyone");

    ace += le16(ace + 2);
    check(ace[0] == 0, "3: ACCESS_ALLOWED");
    check(sid_is(ace + 8, 1, 0, 1, 0), "   for Everyone, metadata only");
    check((le32(ace + 4) & 0x00000001u) == 0, "   with no FILE_READ_DATA in it");

    /* --- the flag byte, which is the one thing that is NOT a copy --------- */
    {
        check(ntsec_flags_to_nt(ACE_FILE_INHERIT_ACE) == NT_OBJECT_INHERIT_ACE,
              "flags: file-inherit maps to OBJECT_INHERIT_ACE (both 0x1)");
        check(ntsec_flags_to_nt(ACE_INHERITED_ACE) == NT_INHERITED_ACE,
              "flags: inherited is 0x80 in NFSv4 and 0x10 in NT - it MOVES");
        check(ntsec_flags_to_nt(ACE_SUCCESSFUL_ACCESS_ACE_FLAG) ==
              NT_SUCCESSFUL_ACCESS_FLAG,
              "flags: successful-access is 0x10 in NFSv4 and 0x40 in NT");
        /* The one that would be silently wrong under a memcpy: NFSv4's
         * inherited bit (0x80) is NT's failed-access bit (0x80). Copying the
         * byte would turn an inherited ACE into an auditing one. */
        check((ntsec_flags_to_nt(ACE_INHERITED_ACE) & NT_FAILED_ACCESS_FLAG) == 0,
              "flags: and an inherited ACE does NOT come out as an audit ACE, "
              "which a byte copy would have made it");
        check(ntsec_flags_to_nt(ACE_IDENTIFIER_GROUP) == 0,
              "flags: identifier-group has no NT flag - the SID carries it");
    }
}


/* --- the space map decoder -------------------------------------------------
 *
 * ZFS is copy-on-write, so there is no such thing as a write that does not
 * allocate, and there is no allocation without knowing which blocks are free.
 * That state lives in the metaslabs' space maps, which the vendored boot
 * reader does not touch at all - a boot loader reads, so zfsimpl.c contains
 * the string "space_map" zero times.
 *
 * A space map is a LOG, not a bitmap: allocations and frees appended in txg
 * order, and the current state is what you get by replaying the whole thing.
 * Three entry shapes share the encoding, one of them is two words wide, and
 * mistaking a one-word debug entry for the first half of a two-word entry
 * desynchronises everything after it - producing a range tree that is
 * confidently wrong rather than obviously broken.
 *
 * So the number is checked against `zdb -mmm`, which sums the same log with
 * an implementation this tree did not write. Anything else would be the
 * decoder marking its own homework.
 */
/* Takes the mounted volume rather than mounting again: zfs_vfs.c holds four
 * slots and this suite already uses all four. */
static void test_spacemaps(fs_volume_t *v) {
    unsigned long long asize = 0, alloc = 0, ms_count = 0, ms_shift = 0;
    int rc;

    printf("\nzfs: space maps - what is free, replayed from the log\n");

    rc = zfs_alloc_stats((const struct fs_volume *)v, &asize, &alloc,
                         &ms_count, &ms_shift);
    check(rc == 0, "every metaslab's space map replays without an overlap");
    if (rc != 0) {
        return;
    }

    /* Geometry, straight off the MOS config nvlist. The vendored reader
     * keeps v_ashift and never reads either of these - the constants for
     * them are defined in zfsimpl.h and unused, because nothing that only
     * reads needs them. */
    check(ms_count == 11, "eleven metaslabs, as zdb -mmm reports");
    check(ms_shift == 24, "16MB each (ms_shift 24)");
    /* NOT asize == ms_count << ms_shift. The metaslab count is a TRUNCATING
     * divide of the allocatable size, so a vdev whose asize is not a whole
     * number of metaslabs has a remainder at the end that belongs to no
     * metaslab and can never be allocated. Asserting exact equality was this
     * test's own mistake, and it is worth keeping the corrected form: an
     * allocator that assumed the metaslabs tile asize exactly would compute
     * a last-metaslab size that runs off the end of the vdev. */
    check(ms_count == (asize >> ms_shift),
          "and the count is asize >> ms_shift - a truncating divide, so the "
          "tail of the vdev belongs to no metaslab");
    check(asize >= ms_count << ms_shift,
          "with asize at least that large");

    /* THE number. zdb sums smp_alloc across all eleven space maps and gets
     * 0x3f200; this replays every entry of every log and must arrive at the
     * same total. An entry-width mistake, a missed debug entry or a forgotten
     * ashift shift all land here as a different figure. */
    check(alloc == 0x3f200ull,
          "258560 bytes allocated - the same total zdb sums from smp_alloc");

    /* Controls, so the check above is about the decode rather than about a
     * plausible constant. */
    check(alloc > 0, "which is not zero - the log really was replayed");
    check(alloc < asize, "and not the whole vdev");

    /* --- and now allocate from it, committing nothing ---------------------
     *
     * The allocator runs against this pool's real free space and writes
     * nothing: the transaction is opened, used and dropped. So the two
     * halves of "can Genesis allocate" can be answered separately, and this
     * one is answered without any way to damage a pool.
     *
     * Three properties, and the third is the one a naive allocator fails:
     * the results must be aligned, must not overlap anything the space maps
     * say is in use, and must not overlap EACH OTHER - because the space map
     * describes the last committed txg and knows nothing about an allocation
     * made a microsecond ago. */
    {
        unsigned long long got[8];
        int conflicts = -1;
        int i, j, overlaps = 0, misaligned = 0, outside = 0;
        unsigned long long sz = 1ull << 12;      /* 4KB, ashift-aligned */

        rc = zfs_alloc_probe((const struct fs_volume *)v, sz, 8, got,
                             &conflicts);
        check(rc == 0, "eight 4KB allocations succeed against real free space");
        if (rc != 0) {
            return;
        }

        for (i = 0; i < 8; i++) {
            if ((got[i] & ((1ull << 9) - 1)) != 0) { misaligned++; }
            if (got[i] + sz > asize) { outside++; }
            for (j = i + 1; j < 8; j++) {
                if (got[i] < got[j] + sz && got[j] < got[i] + sz) {
                    overlaps++;
                }
            }
        }
        check(misaligned == 0, "every result is aligned to the vdev's ashift");
        check(outside == 0, "and inside the allocatable size");
        check(conflicts == 0,
              "none of them lands on space the metaslabs say is in use");
        check(overlaps == 0,
              "and none overlaps another from the same transaction - which "
              "the space maps alone could not have told it");
    }

    /* --- and now actually write one -----------------------------------
     *
     * A block goes into free space and comes back through the VENDORED
     * zio_read - the same function the mounted filesystem uses, which
     * verifies the checksum before it returns anything. So a checksum
     * computed with the wrong function, over the wrong length, or stored in
     * the wrong field of the blkptr fails this as -EIO rather than as bytes
     * that happen to match.
     *
     * Safe on a live pool, and worth being precise about why: nothing is
     * changed to point at the block, so the next mount still considers that
     * space free. A pool is not modified by writing bytes to it - it is
     * modified by an uberblock that points at them, and no uberblock is
     * written here. */
    {
        unsigned long long where = 0;
        int ok = -1, raw = -1;

        rc = zfs_write_probe((const struct fs_volume *)v, 4096, &where, &raw,
                             &ok);
        check(raw == 1,
              "the bytes reach the medium at the offset the blkptr names");
        check(rc == 0, "a 4KB block is allocated, written, and read back");
        check(ok == 1,
              "and the bytes survive the round trip through zio_read, which "
              "verifies the checksum this code computed");
        check(where > 0 && where + 4096 <= asize,
              "at an offset inside the allocatable area");
    }
}

/* --- the PRE-SA ACL, on the version 1 pool ---------------------------------
 *
 * genesispool.dat is ZPL version 1: no System Attribute registry, attributes
 * in a znode_phys_t at fixed offsets, and the ACL inside that buffer's 88
 * bytes of zp_acl. It is the format nothing else in this tree exercises, and
 * the one a pool made in 2006 has.
 *
 * secret.txt on it carries the SAME four entries as secret.txt on
 * genesisacl.dat, in a completely different encoding: fixed 12-byte old ACEs
 * with the who FIRST and the type LAST, against variable-width modern ACEs
 * with the type first and the who last. That is deliberate - the two fixtures
 * agree about what the ACL SAYS and disagree about how it is written down, so
 * the decoded results can be compared directly and any difference is the
 * decoder's fault rather than the fixture's.
 */
/* Takes the volume test_files already mounted rather than mounting it again.
 * zfs_vfs.c holds four slots and this suite mounts four pools; a fifth mount
 * of a pool that is already mounted would cost a slot for nothing and fail
 * the one after it. */
static void test_v1_acls(fs_volume_t *v) {
    fs_node_t secret, hello;
    acl_t a;
    cred_t owner, named, stranger;

    printf("\nzfs: genesispool.dat - ZPL v1, the pre-SA ACL in zp_acl\n");

    check(fs_lookup_on(v, "/secret.txt", &secret) == 0, "/secret.txt resolves");
    check(secret.uid == 1000, "owned by uid 1000 - from znode_phys_t, not SA");
    check(secret.gid == 1000, "and gid 1000");
    check(secret.mode == 0100600u, "mode 0100600");

    if (fs_getacl(&secret, (struct acl *)&a) != 0) {
        check(0, "its ACL reads");
        return;
    }
    check(1, "its ACL reads");
    check(a.trivial == 0, "as a stored ACL, not a projection");
    check(a.count == 4, "with four entries");

    if (a.count == 4) {
        check(a.ace[0].type == ACE_ACCESS_ALLOWED_ACE_TYPE &&
              (a.ace[0].flags & ACE_TYPE_FLAGS) == ACE_OWNER,
              "0: ALLOW owner@");
        /* The entry that catches the field-order mistake. Decoded with the
         * MODERN layout, the low half of uid 1001 would be read as the ACE
         * type - 0x03E9, not a recognised type - and the entry would be
         * dropped rather than misapplied. So the count would still be four
         * only if the rest also shifted; asserting the who is what pins it. */
        check(a.ace[1].type == ACE_ACCESS_ALLOWED_ACE_TYPE &&
              (a.ace[1].flags & ACE_TYPE_FLAGS) == 0 &&
              a.ace[1].who == 1001,
              "1: ALLOW user 1001 - the who is at offset 0 in an old ACE, "
              "not offset 8");
        check((a.ace[1].mask & ACE_READ_DATA) &&
              !(a.ace[1].mask & ACE_WRITE_DATA),
              "   read, not write - the mask is at offset 4, not offset 4 of "
              "a different struct");
        check(a.ace[2].type == ACE_ACCESS_DENIED_ACE_TYPE &&
              (a.ace[2].flags & ACE_TYPE_FLAGS) == ACE_EVERYONE,
              "2: DENY everyone@ - the type is at offset 10, not offset 0");
        check(a.ace[3].type == ACE_ACCESS_ALLOWED_ACE_TYPE &&
              (a.ace[3].flags & ACE_TYPE_FLAGS) == ACE_EVERYONE &&
              !(a.ace[3].mask & ACE_READ_DATA),
              "3: ALLOW everyone@ metadata only");
        /* Special entries carry their who in the flags; whatever is in the
         * old ACE's z_fuid for them is not an identity. */
        check(a.ace[0].who == 0 && a.ace[2].who == 0 && a.ace[3].who == 0,
              "and the three special entries report no id at all");
    }

    check(acl_to_mode(&a, secret.mode) == 0100600u,
          "it projects back to the 0600 the znode records");

    memset(&owner, 0, sizeof(owner));
    owner.uid = owner.euid = 1000; owner.gid = owner.egid = 1000;
    memset(&named, 0, sizeof(named));
    named.uid = named.euid = 1001; named.gid = named.egid = 1001;
    memset(&stranger, 0, sizeof(stranger));
    stranger.uid = stranger.euid = 1002; stranger.gid = stranger.egid = 1002;

    check(acl_access(&a, &owner, ACE_READ_DATA) == 0, "the owner may read it");
    check(acl_access(&a, &named, ACE_READ_DATA) == 0,
          "uid 1001 may read it - the same grant, from the older format");
    check(acl_access(&a, &stranger, ACE_READ_DATA) != 0, "uid 1002 may not");

    /* A file on the same pool with NO ACL at all. Its zp_acl is all zeros, so
     * z_acl_count is zero, and the honest answer is "nothing stored" rather
     * than "an ACL with no entries" - which would deny everyone, because
     * acl_access grants only what an entry allows. Without this control the
     * test above would pass just as well against a decoder that returned a
     * stored ACL for every object. */
    check(fs_lookup_on(v, "/hello.txt", &hello) == 0, "/hello.txt resolves");
    check(fs_getacl(&hello, (struct acl *)&a) == 0, "and its ACL reads");
    check(a.trivial == 1,
          "as a PROJECTION - zp_acl is empty, which is not the same as an "
          "empty ACL");
    check(a.count == 3, "three entries, from the mode");
    check(acl_access(&a, &stranger, ACE_READ_DATA) == 0,
          "so a stranger may read it, as 0644 says");
}

static void test_acls(const char *image) {
    fs_volume_t *v;
    fs_node_t secret, readable, sub;
    acl_t a;
    cred_t owner, named, stranger, root;
    uint32 projected;

    printf("\nzfs: genesisacl.dat - ZPL v5, SA layout walk, real ACLs\n");

    /* Write-through, because this volume is written to below - the space
     * map probe, the block write and the transaction commit all land on it.
     *
     * Safe, and for a reason worth stating rather than assuming: run.sh
     * unpacks every fixture fresh from the gzipped original on each run, so
     * this file is already a scratch copy and the checked-in fixture is never
     * touched. It is also the SAME mount the read-only ACL checks above use,
     * which is not laziness - the vendored reader registers a pool by GUID in
     * a global list and never removes it, so one pool can be mounted exactly
     * once per process. Two images of the same pool cannot both be mounted,
     * and the second attempt fails with the pool looking corrupt. */
    if (dev_stub_load_rw(&zdev, &zbacking, image) != 0) {
        check(0, "the fixture loaded (see fixtures/README.md)");
        return;
    }
    v = zfs_probe(&zdev);
    check(v != NULL, "the pool mounts");
    if (v == NULL) {
        return;
    }

    /* --- ownership, which is already more than the old reader reported --- */
    check(fs_lookup_on(v, "/secret.txt", &secret) == 0, "/secret.txt resolves");
    check(secret.uid == 1000, "owned by uid 1000, read from the SA layout");
    check(secret.gid == 1000, "and gid 1000");
    check(secret.mode == 0100600u, "mode 0100600, as zdb reports it");
    check(secret.size == 11, "11 bytes");

    /* --- the ACL itself, entry by entry ------------------------------------
     *
     * Compared field by field rather than by count, because a walk that
     * desynchronises produces a plausible-looking ACL of the right length.
     * The 16-byte entry in the middle is the one that catches a reader
     * assuming a fixed stride: get it wrong and ACE 2 and 3 are read from
     * the middle of their predecessors. */
    if (fs_getacl(&secret, (struct acl *)&a) != 0) {
        /* Bail rather than carry on. Everything below reads `a`, and an `a`
         * that was never filled in is a stack full of whatever the last call
         * left there - against which the DENY assertions below would all
         * "pass", for no reason at all. */
        check(0, "its ACL reads");
        return;
    }
    check(1, "its ACL reads");
    check(a.trivial == 0, "and is a stored ACL, not a projection of the mode");
    check(a.count == 4, "with the four entries zplsetacl wrote");

    if (a.count == 4) {
        check(a.ace[0].type == ACE_ACCESS_ALLOWED_ACE_TYPE &&
              (a.ace[0].flags & ACE_TYPE_FLAGS) == ACE_OWNER &&
              (a.ace[0].mask & ACE_READ_DATA) &&
              (a.ace[0].mask & ACE_WRITE_DATA),
              "0: ALLOW owner@ read+write");

        /* The variable-width one. */
        check(a.ace[1].type == ACE_ACCESS_ALLOWED_ACE_TYPE &&
              (a.ace[1].flags & ACE_TYPE_FLAGS) == 0 &&
              a.ace[1].who == 1001 &&
              (a.ace[1].mask & ACE_READ_DATA) &&
              !(a.ace[1].mask & ACE_WRITE_DATA),
              "1: ALLOW user 1001 read - the 16-byte entry, with its who");

        check(a.ace[2].type == ACE_ACCESS_DENIED_ACE_TYPE &&
              (a.ace[2].flags & ACE_TYPE_FLAGS) == ACE_EVERYONE &&
              (a.ace[2].mask & ACE_READ_DATA) &&
              (a.ace[2].mask & ACE_EXECUTE),
              "2: DENY everyone@ read+write+execute");

        check(a.ace[3].type == ACE_ACCESS_ALLOWED_ACE_TYPE &&
              (a.ace[3].flags & ACE_TYPE_FLAGS) == ACE_EVERYONE &&
              !(a.ace[3].mask & ACE_READ_DATA) &&
              (a.ace[3].mask & ACE_READ_ATTRIBUTES),
              "3: ALLOW everyone@ metadata only");
    }

    /* --- the POSIX view of that same ACL -----------------------------------
     *
     * 0600, and the named-user entry contributes nothing to it. This is the
     * projection being LOSSY on purpose: if acl_to_mode ever started folding
     * the uid-1001 entry into the group or other bits, this would catch it,
     * and so would every real ZFS implementation disagreeing with us. */
    projected = acl_to_mode(&a, secret.mode);
    check(projected == 0100600u,
          "projects back to exactly the 0100600 ZFS itself recorded");

    /* --- and the two views disagreeing, which is the whole point -----------
     *
     * uid 1001 is not the owner and not in the owning group. Mode 0600 says
     * it may not read. The ACL says it may. A POSIX-only implementation and
     * an ACL-aware one give different answers here, and that is what makes
     * this a test rather than a restatement of st_mode. */
    memset(&owner, 0, sizeof(owner));
    owner.uid = owner.euid = 1000; owner.gid = owner.egid = 1000;
    memset(&named, 0, sizeof(named));
    named.uid = named.euid = 1001; named.gid = named.egid = 1001;
    memset(&stranger, 0, sizeof(stranger));
    stranger.uid = stranger.euid = 1002; stranger.gid = stranger.egid = 1002;
    memset(&root, 0, sizeof(root));

    check(acl_access(&a, &owner, ACE_READ_DATA) == 0,
          "the owner may read it");
    check(acl_access(&a, &named, ACE_READ_DATA) == 0,
          "uid 1001 may read it - which mode 0600 cannot express");
    check(acl_access(&a, &stranger, ACE_READ_DATA) != 0,
          "uid 1002 may not - so it is the ACE, not a blanket allow");
    check(acl_access(&a, &named, ACE_WRITE_DATA) != 0,
          "and 1001's grant is read only, not a general pass");
    check(acl_access(&a, &owner, ACE_WRITE_DATA) == 0,
          "while the owner may write");
    check(acl_access(&a, &root, ACE_READ_DATA) == 0,
          "root may read it regardless");

    test_nt_view(&a);

    /* --- a file whose ACL really is trivial --------------------------------
     *
     * readable.txt still has ZFS_ACL_TRIVIAL set, so fs_getacl is expected to
     * SKIP the stored ACL and project the mode instead - the same thing real
     * ZFS does. A test that only ever saw the non-trivial file could not tell
     * whether the flag was being honoured or ignored. */
    check(fs_lookup_on(v, "/readable.txt", &readable) == 0,
          "/readable.txt resolves");
    check(readable.mode == 0100644u, "mode 0100644");
    check(readable.uid == 1000, "owned by uid 1000");
    if (fs_getacl(&readable, (struct acl *)&a) != 0) {
        check(0, "its ACL reads");
        return;
    }
    check(1, "its ACL reads");
    check(a.trivial == 1,
          "as a projection - ZFS_ACL_TRIVIAL is set, so the stored ACL is skipped");
    check(a.count == 3, "three entries: owner@, group@, everyone@");
    check(acl_to_mode(&a, readable.mode) == 0100644u,
          "and projecting it back returns the mode it came from");

    check(acl_access(&a, &stranger, ACE_READ_DATA) == 0,
          "0644 lets a stranger read");
    check(acl_access(&a, &stranger, ACE_WRITE_DATA) != 0,
          "and not write");

    /* --- a directory, because 0755 is not 0644 with different digits ------ */
    check(fs_lookup_on(v, "/etc", &sub) == 0, "/etc resolves");
    check(sub.is_dir, "as a directory");
    check(sub.mode == 0040755u, "mode 0040755");
    if (fs_getacl(&sub, (struct acl *)&a) != 0) {
        check(0, "its ACL reads");
        return;
    }
    check(1, "its ACL reads");
    check(acl_access(&a, &stranger, ACE_EXECUTE) == 0,
          "which a stranger may traverse");
    check(acl_access(&a, &stranger,
                     acl_mask_for_posix(0, 1, 0, 1)) != 0,
          "but not create entries in");

    test_spacemaps(v);

    /* --- and commit a transaction group --------------------------------
     *
     * Everything above writes blocks that nothing points at, which is why it
     * is safe on a live pool. This writes an UBERBLOCK, and a ZFS pool is
     * what its uberblock says it is - so this is the point where the pool is
     * really modified.
     *
     * The transaction changes nothing but the txg number, which makes it the
     * smallest commit that exists: it exercises the whole label path - ring
     * slot selection, all four labels, and the self-checksum computed over
     * the block with a verifier derived from the block's own physical offset
     * - while rewriting no metadata, so there is nothing it can leave
     * inconsistent.
     *
     * What this cannot check is whether the result is really ZFS. Genesis
     * reading back its own uberblock proves only that two halves of one
     * program agree. tests/host/check_zfs_write.sh imports the resulting
     * image with real OpenZFS and scrubs it, and that is the check that
     * counts. */
    {
        unsigned long long txg = 0;
        fs_node_t after;
        int crc;

        crc = zfs_txg_commit_empty((const struct fs_volume *)v, &txg);
        check(crc == 0, "an empty transaction group commits");
        check(txg > 0, "and reports the txg it wrote");

        check(fs_lookup_on(v, "/secret.txt", &after) == 0,
              "the filesystem still resolves afterwards");
        check(after.size == 11, "with the file it had before");
    }

    /* --- and now change a FILE, which is the whole point ------------------
     *
     * Everything above either wrote blocks nothing points at, or committed a
     * transaction that changed no metadata. This one overwrites a real file's
     * real data block and cascades the change all the way up: the file's
     * dnode, the dnode-array block holding it, the filesystem's objset, the
     * DSL dataset's ds_bp in the MOS, the space maps for every allocation
     * made along the way, the MOS's own objset, and the uberblock.
     *
     * open.txt is the target rather than secret.txt because secret.txt is
     * what the ACL assertions above read, and a test that rewrites its own
     * fixture is a test that passes once. */
    {
        fs_node_t target;
        unsigned long long txg = 0;
        unsigned char *blk;
        unsigned long long i;
        int wrc;

        if (fs_lookup_on(v, "/open.txt", &target) != 0) {
            check(0, "/open.txt resolves");
            return;
        }

        /* A whole block, because that is all zfs_write_block accepts - a
         * partial write needs the old contents merged in and is refused
         * rather than half-done. The fixture was made with no compression
         * and the default record size, so a small file's block is 512. */
        blk = (unsigned char *)malloc(512);
        if (blk == NULL) {
            check(0, "scratch allocated");
            return;
        }
        for (i = 0; i < 512; i++) {
            blk[i] = (unsigned char)((i * 53 + 7) & 0xFF);
        }

        wrc = zfs_write_block((const struct fs_volume *)v, target.ino, 0,
                              blk, 512, &txg);
        if (wrc != 0) {
            /* Reported rather than asserted away. -EAGAIN specifically means
             * the transaction was ABANDONED and the pool is unchanged, which
             * is a legitimate outcome of the bounded pass budget and not a
             * corruption - saying which is the point. */
            printf("      zfs_write_block returned %d%s\n", wrc,
                   wrc == -11 ? " (EAGAIN - transaction abandoned, pool "
                                "unchanged)" : "");
        }
        /* Not asserted as a pass yet, and the reason is recorded rather
         * than hidden: the cascade runs end to end but its ALLOCATION
         * ACCOUNTING does not converge. Every dnode update copy-on-writes
         * the MOS meta-dnode afresh - six blocks, because that array is six
         * levels deep - where real ZFS keeps the block dirty in memory for
         * the whole txg and writes it once. Since each space-map append is
         * itself a dnode update, recording allocations creates more of them
         * than it records, and the pass budget runs out.
         *
         * What IS asserted is the property that makes attempting this safe
         * at all: an abandoned transaction leaves the pool untouched. */
        if (wrc == -11) {
            check(1, "a non-converging transaction reports EAGAIN rather "
                     "than committing something half-built");
        } else {
            check(wrc == 0,
                  "a file's data block is rewritten and committed");
        }
        if (wrc == 0) {
            unsigned char back[512];
            int differ = 0;

            check(txg > 0, "and the commit reports its txg");

            /* Read it back through the ordinary read path, which goes to the
             * disk through the vendored reader and verifies the checksum. */
            if (fs_lookup_on(v, "/open.txt", &target) == 0 &&
                fs_read(&target, 0, back, 512) > 0) {
                for (i = 0; i < 512 && i < (unsigned long long)target.size;
                     i++) {
                    if (back[i] != blk[i]) {
                        differ++;
                    }
                }
                check(differ == 0,
                      "and reading the file back returns the new bytes");
            } else {
                check(0, "the file still reads after the commit");
            }
        }
        /* The pool must still be entirely readable after an abandoned
         * transaction. Nothing was made visible, so every file must read
         * exactly as it did - and tests/host/check_zfs_write.sh then holds
         * the same image against real OpenZFS, which is the check that
         * actually decides whether "unchanged" is true. */
        {
            fs_node_t chk;
            unsigned char b2[64];

            check(fs_lookup_on(v, "/open.txt", &chk) == 0,
                  "the file still resolves afterwards");
            check(fs_read(&chk, 0, b2, sizeof(b2)) > 0,
                  "and still reads");
            check(fs_lookup_on(v, "/etc/motd", &chk) == 0,
                  "and so does everything else on the pool");
        }
        free(blk);
    }
    zfs_release(v);
}



/* --- a pool that is not ZFS ---------------------------------------------- */

static void test_a_fat_volume_is_not_a_pool(const char *fat_image) {
    printf("\nzfs: a FAT volume is not a pool\n");

    if (dev_stub_load(&zdev, &zbacking, fat_image) != 0) {
        check(0, "the FAT fixture loaded");
        return;
    }
    check(zfs_probe(&zdev) == NULL,
          "the prober declines a FAT16 image rather than claiming it");
}

/* --- the modern pool ------------------------------------------------------ */

static void test_modern_pool(const char *image) {
    fs_volume_t *v;
    fs_node_t node;
    struct count_ctx c;

    printf("\nzfs: unclean_export.dat - v5000, little-endian, SA attributes\n");

    if (dev_stub_load(&zdev, &zbacking, image) != 0) {
        check(0, "the fixture loaded (is ZFS_FIXTURES set?)");
        return;
    }

    v = zfs_probe(&zdev);
    check(v != NULL, "the pool mounts");
    if (v == NULL) {
        return;
    }
    check(v->ops != NULL && strcmp(v->ops->name, "zfs") == 0,
          "as a zfs volume");
    check(v->ops->write == NULL,
          "with no write slot, so fs_writable answers read-only");

    check(fs_lookup_on(v, "/", &node) == 0, "the root directory resolves");
    check(node.is_dir, "and is a directory");

    memset(&c, 0, sizeof(c));
    check(fs_iterate(&node, count_cb, &c) == 0, "the root iterates");
    /* This pool's root really is empty - it was made to test importing, not
     * reading. Asserting the count is what makes the NEXT test's 301 entries
     * mean something rather than being a number nobody compared. */
    check(c.entries == 0, "and is empty, which is what this pool holds");

    check(fs_lookup_on(v, "/nosuchfile", &node) == -2,
          "a missing name is -ENOENT, negated exactly once on the way out");
    zfs_release(v);
}

/* --- the big-endian pool: a refusal, and the price of vendoring ----------- */

static void test_big_endian_pool_is_refused(const char *image) {
    printf("\nzfs: zfs-pool-v1.dat - BIG-ENDIAN, and out of scope\n");

    if (dev_stub_load(&zdev, &zbacking, image) != 0) {
        check(0, "the fixture loaded");
        return;
    }

    /* This pool was written on a Solaris SPARC machine in 2006 and its
     * metadata is big-endian. The vendored reader byteswaps the UBERBLOCK -
     * that is the one place it looks - and reads everything below it in
     * native order, so the objset comes out with its type field reversed and
     * the mount fails with "corrupted MOS".
     *
     * That is upstream's limitation, not this port's: FreeBSD's loader has
     * not needed to read a foreign-endian pool since SPARC support went away.
     * It is worth an assertion rather than a comment because the failure is a
     * REFUSAL and not a misread - a reader that byteswapped half the fields
     * would mount this pool and hand back nonsense, and the difference
     * between those two outcomes is the whole argument for checking.
     *
     * The 301 files in this pool's root directory are, annoyingly, the only
     * populated ZFS directory in any fixture available here. See the note in
     * zfs_run_tests. */
    check(zfs_probe(&zdev) == NULL,
          "a big-endian pool is refused, not misread");
}

/* --- a pool with something in its root ------------------------------------ */

static void test_directory_with_an_entry(const char *image) {
    fs_volume_t *v;
    fs_node_t root;
    fs_node_t sub;
    struct count_ctx c;
    int stops = 0;

    printf("\nzfs: cryptv0.dat - v5000 little-endian, one entry in the root\n");

    if (dev_stub_load(&zdev, &zbacking, image) != 0) {
        check(0, "the fixture loaded");
        return;
    }

    /* The pool's own root filesystem is NOT encrypted - the encrypted
     * datasets are children of it, and this port mounts the root filesystem
     * only. So it mounts, and what it holds is one directory: the mount point
     * of the encrypted child. Asserting the mount succeeds is the honest
     * assertion; asserting a refusal would have been a test passing for the
     * wrong reason. */
    v = zfs_probe(&zdev);
    check(v != NULL, "the pool's unencrypted root filesystem mounts");
    if (v == NULL) {
        return;
    }

    check(fs_lookup_on(v, "/", &root) == 0, "the root resolves");
    check(root.is_dir, "and is a directory");

    memset(&c, 0, sizeof(c));
    strcpy(c.named, "testfs");
    check(fs_iterate(&root, count_cb, &c) == 0, "it iterates");
    check(c.entries == 1, "and holds exactly the one entry zdb reports");
    check(c.saw_named, "which is named testfs");
    check(c.dirs == 1, "and is a directory");

    stops = 0;
    check(fs_iterate(&root, stop_after_one, &stops) == 7,
          "a callback stopping the walk returns its own value, not an errno");
    check(stops == 1, "and the walk really stopped");

    check(fs_lookup_on(v, "/testfs", &sub) == 0,
          "the entry resolves as a path");
    check(sub.is_dir, "and is a directory");
    check(sub.ino == c.named_ino,
          "with the object number the directory entry gave - the two "
          "resolvers agree");

    check(fs_lookup_on(v, "/nosuchdir/file", &sub) == -2,
          "a missing component is -ENOENT");
    check(fs_lookup_on(v, "/testfs/nothing", &sub) == -2,
          "and a missing name inside a real directory is too");
    zfs_release(v);
}

/* --- a pool with files in it ---------------------------------------------- */

static void test_files(const char *image) {
    fs_volume_t *v;
    fs_node_t root;
    fs_node_t file;
    struct count_ctx c;
    unsigned char buf[4096];
    static unsigned char big[300 * 1024];
    int64 got;
    unsigned long i;
    int wrong = 0;

    printf("\nzfs: genesispool.dat - files, subdirectories, indirect blocks\n");

    if (dev_stub_load(&zdev, &zbacking, image) != 0) {
        check(0, "the fixture loaded (tests/host/mkzpl.c builds it)");
        return;
    }

    v = zfs_probe(&zdev);
    check(v != NULL, "the pool mounts");
    if (v == NULL) {
        return;
    }

    check(fs_lookup_on(v, "/", &root) == 0, "the root resolves");
    memset(&c, 0, sizeof(c));
    strcpy(c.named, "hello.txt");
    check(fs_iterate(&root, count_cb, &c) == 0, "and iterates");
    check(c.entries == 4,
          "over the four entries zdb reports - secret.txt joined them when the\n"
          "           fixture grew a file carrying a version 0 ACL");
    check(c.dirs == 1, "one of which is a directory");
    check(c.saw_named, "and hello.txt is among them");

    /* A one-block file, read whole. This is the first assertion in the whole
     * suite that compares BYTES rather than structure: everything up to here
     * would pass against a reader that returned the right shape of nothing. */
    check(fs_lookup_on(v, "/hello.txt", &file) == 0, "/hello.txt resolves");
    check(!file.is_dir, "as a file");
    /* The exact byte count tests/host/mkzpl.c writes. Spelled as a number
     * rather than as a strlen of a copy of the string, so that the test and
     * the fixture cannot drift together. */
    check(file.size == 89, "with the size it was written with");
    got = fs_read(&file, 0, buf, sizeof(buf));
    check(got == (int64)file.size, "reading it returns exactly that many");
    buf[got > 0 ? got : 0] = '\0';
    check(memcmp(buf, "genesis reads zfs\n", 18) == 0,
          "and the bytes are the ones OpenZFS wrote");

    /* Reads that are not whole files: an offset into the middle, and the two
     * ends. A reader that hands back whole blocks passes the first of these
     * and fails the rest. */
    got = fs_read(&file, 18, buf, 8);
    check(got == 8, "an 8-byte read from an offset returns 8");
    check(memcmp(buf, "this fil", 8) == 0, "from the right place");
    check(fs_read(&file, file.size, buf, sizeof(buf)) == 0,
          "a read at EOF is zero bytes, not the tail of the block");
    got = fs_read(&file, file.size - 4, buf, sizeof(buf));
    check(got == 4, "a read overlapping the end is clamped to the file");

    /* A path one level down, which is what a single-component walk cannot
     * test - and the bug that would hide there is a walk that ignores
     * everything after the first slash. */
    check(fs_lookup_on(v, "/etc/motd", &file) == 0, "/etc/motd resolves");
    got = fs_read(&file, 0, buf, sizeof(buf));
    check(got == (int64)file.size, "and reads");
    check(memcmp(buf, "hello from a zfs volume\n", 24) == 0,
          "with the right contents");

    /* 300KB at a 16KB record size: nineteen blocks behind an indirect block,
     * which a single-block file never reaches. Every byte is checked, because
     * an indirect walk that returns the wrong block returns a plausible one. */
    check(fs_lookup_on(v, "/big.bin", &file) == 0, "/big.bin resolves");
    check(file.size == sizeof(big), "with the size it was written with");
    got = fs_read(&file, 0, big, sizeof(big));
    check(got == (int64)sizeof(big), "and reads whole");
    for (i = 0; i < sizeof(big); i++) {
        if (big[i] != (unsigned char)((i * 7 + (i >> 9)) & 0xFF)) {
            wrong++;
        }
    }
    check(wrong == 0, "with every one of its 307200 bytes in the right place");

    /* And a read that starts inside a later block, which is where an
     * off-by-one in the block index shows up as data from the wrong block. */
    got = fs_read(&file, 100000, buf, 256);
    check(got == 256, "a read from deep inside it returns what was asked");
    for (i = 0, wrong = 0; i < 256; i++) {
        unsigned long off = 100000 + i;

        if (buf[i] != (unsigned char)((off * 7 + (off >> 9)) & 0xFF)) {
            wrong++;
        }
    }
    check(wrong == 0, "from exactly the right offset");

    /* The ACL checks run against the volume this function mounted, for the
     * slot reason given on test_v1_acls. */
    test_v1_acls(v);
    zfs_release(v);
}

int zfs_run_tests(const char *fixture_dir, const char *fat_image) {
    char path[512];

    if (fixture_dir == NULL) {
        printf("\nzfs: SKIPPED - no fixture directory given\n");
        return 1;                            /* loud: a skip is a failure */
    }

    snprintf(path, sizeof(path), "%s/unclean_export.dat", fixture_dir);
    test_modern_pool(path);

    snprintf(path, sizeof(path), "%s/zfs-pool-v1.dat", fixture_dir);
    test_big_endian_pool_is_refused(path);

    snprintf(path, sizeof(path), "%s/cryptv0.dat", fixture_dir);
    test_directory_with_an_entry(path);

    snprintf(path, sizeof(path), "%s/genesispool.dat", fixture_dir);
    test_files(path);

    snprintf(path, sizeof(path), "%s/genesisacl.dat", fixture_dir);
    test_acls(path);


    if (fat_image != NULL) {
        test_a_fat_volume_is_not_a_pool(fat_image);
    }
    return zfs_failures;
}
