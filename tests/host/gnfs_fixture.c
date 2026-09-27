/* The gnfs ACL fixture: what the ZFS pools used to be for, on gnfs.
 *
 * systest's access-control section needs a mounted volume holding a file
 * whose ACL says something a mode word CANNOT - secret.txt, mode 0600, owned
 * by uid 1000, with an extra entry granting uid 1001 read. A POSIX-only
 * kernel refuses uid 1001 and an ACL-aware one lets it in, so the section
 * can tell which one is running. Those files used to live on ZFS pools built
 * by OpenZFS; ZFS is gone from this tree, and gnfs - which stores real
 * NFSv4 ACLs of its own - carries them now.
 *
 * Built by the kernel's OWN gnfs code (kernel/gnfs/gnfs_vfs.c through
 * tests/host/dev_stub.c), never by a second implementation of the format,
 * so the image is exactly what the kernel would have written. Three uses:
 *
 *   gnfs_fixture_build     lays the fixture into a buffer;
 *   gnfs_fixture_run_tests mounts one and checks it - the ACL decoded entry
 *                          by entry, the access decisions it must produce,
 *                          and its Windows SECURITY_DESCRIPTOR view (the
 *                          ntsec checks that used to run against ZFS);
 *   vmm_test --gnfs-fixture PATH
 *                          writes it out; tests/host/run.sh compares that
 *                          byte for byte with the committed
 *                          tests/host/fixtures/gnfsfix.img.gz, which is what
 *                          build.py attaches to the guest as /mnt/d.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acl.h"
#include "dev_stub.h"
#include "fs.h"
#include "gnfs_dev.h"
#include "gnfs_layout.h"
#include "ntsec.h"

#define GNFS_FIXTURE_BYTES (4ULL * 1024 * 1024)

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

typedef struct {
    uint8 *buf;
    uint64 len;
} fix_mem_t;

static int64 fix_write(void *ctx, uint64 offset, const void *data,
                       uint64 len) {
    fix_mem_t *m = (fix_mem_t *)ctx;

    if (offset + len > m->len) {
        return -28;
    }
    memcpy(m->buf + offset, data, (size_t)len);
    return (int64)len;
}

/* The four entries of secret.txt, in this order - which matters: NFSv4 and
 * NT both let the FIRST entry to mention a bit decide it, so the deny of
 * everyone@ in third place must come after the two allows it would
 * otherwise override. */
static void secret_acl(acl_t *a) {
    uint32 meta = ACE_READ_ATTRIBUTES | ACE_READ_ACL | ACE_SYNCHRONIZE;

    memset(a, 0, sizeof(*a));
    a->owner   = 1000;
    a->group   = 1000;
    a->trivial = 0;
    a->count   = 4;

    a->ace[0].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    a->ace[0].flags = ACE_OWNER;
    a->ace[0].mask  = ACE_READ_DATA | ACE_WRITE_DATA | ACE_APPEND_DATA |
                      ACE_WRITE_ATTRIBUTES | ACE_WRITE_ACL | meta;

    a->ace[1].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    a->ace[1].flags = 0;                          /* a named user */
    a->ace[1].who   = 1001;
    a->ace[1].mask  = ACE_READ_DATA | meta;

    a->ace[2].type  = ACE_ACCESS_DENIED_ACE_TYPE;
    a->ace[2].flags = ACE_EVERYONE;
    a->ace[2].mask  = ACE_READ_DATA | ACE_WRITE_DATA | ACE_APPEND_DATA |
                      ACE_EXECUTE;

    a->ace[3].type  = ACE_ACCESS_ALLOWED_ACE_TYPE;
    a->ace[3].flags = ACE_EVERYONE;
    a->ace[3].mask  = meta;
}

static int put_file(fs_volume_t *v, const char *path, uint32 uid,
                    uint32 gid, uint32 mode, const char *text) {
    cred_t c;
    fs_node_t n;
    uint64 len = strlen(text);

    cred_init_nobody(&c);
    c.euid = uid;
    c.egid = gid;
    if (v->ops->create(v, path, (const struct cred *)&c, mode) != 0) {
        return -1;
    }
    if (v->ops->lookup(v, path, &n) != 0) {
        return -1;
    }
    if (v->ops->write(v, &n, 0, text, len) != (int64)len) {
        return -1;
    }
    return 0;
}

int gnfs_fixture_build(uint8 *image, uint64 bytes) {
    fix_mem_t m;
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_node_t n;
    acl_t a;
    int rc = -1;

    memset(image, 0, (size_t)bytes);
    m.buf = image;
    m.len = bytes;
    if (gnfs_format(&m, fix_write, bytes) != 0) {
        return -1;
    }
    dev_stub_attach(&dev, &stub, image, bytes);
    v = gnfs_probe(&dev);
    if (v == NULL) {
        return -1;
    }

    /* NULL creator = the kernel = root; modes as named, no umask. */
    if (v->ops->mkdir(v, "/etc", NULL, 0755) != 0 ||
        put_file(v, "/etc/motd", 0, 0, 0644,
                 "hello from gnfs /etc/motd\n") != 0 ||
        put_file(v, "/readable.txt", 1000, 1000, 0644, "readable\n") != 0 ||
        put_file(v, "/open.txt", 0, 0, 0666, "open to all\n") != 0 ||
        put_file(v, "/secret.txt", 1000, 1000, 0600, "hello world") != 0) {
        goto out;
    }
    if (v->ops->lookup(v, "/secret.txt", &n) != 0) {
        goto out;
    }
    secret_acl(&a);
    if (v->ops->setacl(v, &n, (const struct acl *)&a, FS_SPECIAL_KEEP) != 0) {
        goto out;
    }
    rc = 0;
out:
    v->ops->unmount(v);
    return rc;
}

int gnfs_fixture_write(const char *path) {
    uint8 *image = (uint8 *)malloc((size_t)GNFS_FIXTURE_BYTES);
    FILE *f;
    int rc;

    if (image == NULL) {
        return -1;
    }
    rc = gnfs_fixture_build(image, GNFS_FIXTURE_BYTES);
    if (rc == 0) {
        f = fopen(path, "wb");
        if (f == NULL ||
            fwrite(image, 1, (size_t)GNFS_FIXTURE_BYTES, f) !=
                (size_t)GNFS_FIXTURE_BYTES) {
            rc = -1;
        }
        if (f != NULL) {
            fclose(f);
        }
    }
    free(image);
    return rc;
}

/* --- the Windows view of secret.txt's ACL -------------------------------- */

static uint16 le16(const uint8 *p) { return (uint16)(p[0] | (p[1] << 8)); }
static uint32 le32(const uint8 *p) {
    return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
           ((uint32)p[3] << 24);
}

/* S-1-<auth>-<sub0>[-<sub1>], compared field by field. */
static int sid_is(const uint8 *p, uint64 auth, uint32 s0, int nsubs,
                  uint32 s1) {
    uint64 au;

    if (p[0] != 1 || p[1] != (uint8)nsubs) {
        return 0;
    }
    au = ((uint64)p[2] << 40) | ((uint64)p[3] << 32) | ((uint64)p[4] << 24) |
         ((uint64)p[5] << 16) | ((uint64)p[6] << 8) | (uint64)p[7];
    if (au != auth || le32(p + 8) != s0) {
        return 0;
    }
    if (nsubs > 1 && le32(p + 12) != s1) {
        return 0;
    }
    return 1;
}

static void test_nt_view(const acl_t *a) {
    uint8 sd[NTSEC_MAX_SD];
    const uint8 *dacl, *ace;
    int64 n;
    uint32 dacl_off, owner_off, group_off;

    n = ntsec_build(a, 0, 0);
    check(n > 0, "ntsec: a NULL buffer returns the size needed");
    n = ntsec_build(a, sd, sizeof(sd));
    check(n > 0, "ntsec: building into a real buffer succeeds");
    if (n <= 0) {
        return;
    }
    check(ntsec_build(a, sd, 8) == -28,
          "ntsec: a buffer too small is -ENOSPC, not a truncated descriptor");
    check(sd[0] == 1, "ntsec: revision 1");
    check(le16(sd + 2) == (SE_SELF_RELATIVE | SE_DACL_PRESENT),
          "ntsec: self-relative, with a DACL present");
    check(le32(sd + 12) == 0, "ntsec: no SACL - nothing is audited");

    owner_off = le32(sd + 4);
    group_off = le32(sd + 8);
    dacl_off  = le32(sd + 16);
    check(owner_off < (uint32)n && group_off < (uint32)n &&
          dacl_off < (uint32)n, "ntsec: every offset lands inside");

    /* Samba's idmap scheme: S-1-22-1-<uid>, S-1-22-2-<gid>. */
    check(sid_is(sd + owner_off, 22, 1, 2, 1000),
          "ntsec: owner is S-1-22-1-1000");
    check(sid_is(sd + group_off, 22, 2, 2, 1000),
          "ntsec: group is S-1-22-2-1000");

    dacl = sd + dacl_off;
    check(dacl[0] == NT_ACL_REVISION, "ntsec: DACL is ACL_REVISION 2");
    check(le16(dacl + 4) == 4, "ntsec: four ACEs, the same four gnfs stored");
    check(le16(dacl + 2) == (uint16)(n - dacl_off),
          "ntsec: AclSize covers exactly the rest of the descriptor");

    /* IN ORDER. Hoisting the deny "canonically" to the front would take
     * the file away from its owner and from uid 1001 both. */
    ace = dacl + 8;
    check(ace[0] == 0 && sid_is(ace + 8, 22, 1, 2, 1000),
          "ntsec 0: ACCESS_ALLOWED for the owner's SID");
    ace += le16(ace + 2);
    check(ace[0] == 0 && sid_is(ace + 8, 22, 1, 2, 1001),
          "ntsec 1: ACCESS_ALLOWED for S-1-22-1-1001, the named user");
    check(le32(ace + 4) == a->ace[1].mask,
          "ntsec 1: the access mask crosses UNCHANGED - it is the same field");
    ace += le16(ace + 2);
    check(ace[0] == 1 && sid_is(ace + 8, 1, 0, 1, 0),
          "ntsec 2: ACCESS_DENIED for Everyone, still third");
    ace += le16(ace + 2);
    check(ace[0] == 0 && sid_is(ace + 8, 1, 0, 1, 0) &&
          (le32(ace + 4) & 0x1u) == 0,
          "ntsec 3: ACCESS_ALLOWED for Everyone, metadata only");

    /* The flag byte is the one thing that is NOT a copy. */
    check(ntsec_flags_to_nt(ACE_FILE_INHERIT_ACE) == NT_OBJECT_INHERIT_ACE,
          "ntsec flags: file-inherit is OBJECT_INHERIT_ACE (both 0x1)");
    check(ntsec_flags_to_nt(ACE_INHERITED_ACE) == NT_INHERITED_ACE,
          "ntsec flags: inherited MOVES, 0x80 -> 0x10");
    check(ntsec_flags_to_nt(ACE_SUCCESSFUL_ACCESS_ACE_FLAG) ==
          NT_SUCCESSFUL_ACCESS_FLAG, "ntsec flags: successful-access moves");
    check((ntsec_flags_to_nt(ACE_INHERITED_ACE) & NT_FAILED_ACCESS_FLAG) == 0,
          "ntsec flags: an inherited ACE does NOT become an audit ACE, which "
          "a byte copy would make it");
    check(ntsec_flags_to_nt(ACE_IDENTIFIER_GROUP) == 0,
          "ntsec flags: identifier-group has no NT flag - the SID carries it");
}

/* --- the fixture, checked ------------------------------------------------ */

int gnfs_fixture_run_tests(void) {
    uint8 *image = (uint8 *)malloc((size_t)GNFS_FIXTURE_BYTES);
    device_t dev;
    dev_stub_t stub;
    fs_volume_t *v;
    fs_node_t secret, readable, etc;
    acl_t a;
    cred_t owner, named, stranger, root;

    failures = 0;
    printf("\ngnfs fixture (secret.txt's ACL, and its Windows view):\n");
    if (image == NULL || gnfs_fixture_build(image, GNFS_FIXTURE_BYTES) != 0) {
        check(0, "the fixture builds");
        free(image);
        printf("gnfs fixture: FAILED\n");
        return failures;
    }
    dev_stub_attach(&dev, &stub, image, GNFS_FIXTURE_BYTES);
    v = gnfs_probe(&dev);
    check(v != NULL, "it mounts");
    if (v == NULL) {
        free(image);
        printf("gnfs fixture: FAILED\n");
        return failures;
    }
    secret.vol = readable.vol = etc.vol = v;

    check(v->ops->lookup(v, "/secret.txt", &secret) == 0, "/secret.txt");
    secret.vol = v;
    check(secret.uid == 1000 && secret.gid == 1000,
          "owned by uid 1000, group 1000");
    check(secret.mode == 0100600u,
          "mode 0100600 - projected from the ACL, and the uid 1001 entry "
          "contributes nothing to it, as a mode word cannot say it");

    if (fs_getacl(&secret, (struct acl *)&a) != 0 || a.count != 4) {
        check(0, "its stored four-entry ACL reads back");
    } else {
        check(a.trivial == 0, "a stored ACL, not a projection");
        check(a.ace[1].who == 1001 &&
              a.ace[1].type == ACE_ACCESS_ALLOWED_ACE_TYPE,
              "entry 1 is the named-user grant to uid 1001");

        cred_init_nobody(&owner);
        owner.euid = 1000; owner.egid = 1000;
        cred_init_nobody(&named);
        named.euid = 1001; named.egid = 1001;
        cred_init_nobody(&stranger);
        stranger.euid = 1002; stranger.egid = 1002;
        cred_init_nobody(&root);
        root.euid = 0;

        check(acl_access(&a, &owner, ACE_READ_DATA) == 0,
              "the owner may read");
        check(acl_access(&a, &owner, ACE_WRITE_DATA) == 0,
              "and write");
        check(acl_access(&a, &named, ACE_READ_DATA) == 0,
              "uid 1001 may read - which mode 0600 cannot express");
        check(acl_access(&a, &named, ACE_WRITE_DATA) != 0,
              "but not write: the grant is read only");
        check(acl_access(&a, &stranger, ACE_READ_DATA) != 0,
              "uid 1002 may not read - it is the ACE, not a blanket allow");
        check(acl_access(&a, &root, ACE_READ_DATA) == 0,
              "root may, regardless");
        test_nt_view(&a);
    }

    check(v->ops->lookup(v, "/readable.txt", &readable) == 0 &&
          readable.mode == 0100644u, "/readable.txt is 0644");
    readable.vol = v;
    check(fs_getacl(&readable, (struct acl *)&a) == 0 && a.trivial == 1 &&
          a.count == 3, "with no stored ACL - projected from the mode");

    check(v->ops->lookup(v, "/etc", &etc) == 0 && etc.is_dir &&
          etc.mode == 0040755u, "/etc is a 0755 directory");
    etc.vol = v;
    cred_init_nobody(&stranger);
    stranger.euid = 1002;
    check(fs_access(&etc, (const struct cred *)&stranger, ACE_EXECUTE) == 0,
          "which a stranger may traverse");
    check(fs_access(&etc, (const struct cred *)&stranger,
                    acl_mask_for_posix(0, 1, 0, 1)) != 0,
          "but not add to");

    v->ops->unmount(v);
    free(image);
    printf("gnfs fixture: %s\n", failures ? "FAILED" : "passed");
    return failures;
}
