# ZFS pool fixtures

Four pool images, gzipped. `tests/host/run.sh` unpacks them into
`build/hosttest/zfs/` and `tests/host/zfs_test.c` reads them through the same
`device_ops_t` the kernel uses.

Three of them were written by real OpenZFS on real machines and are shipped in
the OpenZFS source tree as test-suite artifacts (CDDL, like the rest of
OpenZFS):

| file | origin | what it is for |
|---|---|---|
| `unclean_export.dat.gz` | `tests/zfs-tests/tests/functional/cli_root/zpool_import/blockfiles/` | pool version 5000, little-endian, System Attributes, lzjb metadata. An empty root directory. |
| `cryptv0.dat.gz` | same directory | v5000 with an encrypted child dataset. The pool's own root filesystem is not encrypted and mounts; the one entry in it is the child's mount point. |
| `zfs-pool-v1.dat.gz` | `tests/zfs-tests/tests/functional/cli_root/zpool_upgrade/blockfiles/` | pool version 1, **big-endian** (Solaris/SPARC, 2006), old `znode_phys_t` attributes, 301 files. The vendored reader refuses it; see the test. |

The last two are ours:

| file | origin | what it is for |
|---|---|---|
| `genesispool.dat.gz` | `tests/host/mkzpl.c` (via `mkzplpool.sh`) | ZPL **version 1**: attributes in a `znode_phys_t`, and an ACL in that buffer's `zp_acl`. The only fixture exercising the pre-SA paths. |
| `genesisacl.dat.gz` | real `zpool create`, then `tests/host/zplsetacl.c` | a ZPL **version 5** pool whose attributes live in the SA registry, with a **non-trivial ACL** on one file. |

`mkzpl.c` does not write ZFS itself - it drives **libzpool**, OpenZFS's own
kernel code built for userland, through `spa_create` and the DMU/ZAP calls.
Every block in the image is laid out, compressed and checksummed by OpenZFS.
That is the property that makes the fixture worth anything: a reader and a
writer written by the same hand agree with each other and prove nothing about
the format.

It is also verifiable independently - `zdb -e -p <dir> genesispool -dddd`
reads it and prints the same three directory entries the test asserts on.

To rebuild it on a machine with the OpenZFS development headers:

    gcc -o mkzpl tests/host/mkzpl.c -I/usr/include/libzfs -I/usr/include/libspl \
        -D_GNU_SOURCE -lzpool -lnvpair -lzfs_core -luutil
    rm -f /etc/zfs/zpool.cache
    truncate -s 128M pool.img
    ./mkzpl pool.img genesispool

No ZFS kernel module is needed - that is what libzpool is for, and it is how
`ztest` works.


## `genesisacl.dat.gz` - the ACL fixture

`genesispool.dat.gz` is deliberately ZPL version 1: its attributes sit in a
`znode_phys_t` bonus buffer at fixed offsets. That is a format the reader must
handle, but it is not the one a pool made this decade uses, and it has no ACL
worth reading. This fixture is the other half.

It was built by **real OpenZFS**, not by libzpool alone:

    truncate -s 192M pool.img
    zpool create -d -o ashift=9 \
        -O acltype=nfsv4 -O aclmode=passthrough -O aclinherit=passthrough \
        -O compression=off -O atime=off -O xattr=sa -O normalization=none \
        -m /mnt/genesisacl genesisacl $PWD/pool.img
    # ... create readable.txt, secret.txt, open.txt, etc/motd; chmod; chown
    zpool export genesisacl
    ./zplsetacl $PWD genesisacl <objnum of secret.txt>
    gzip -9 -c pool.img > genesisacl.dat.gz

All of which `tests/host/fixtures/mkaclpool.sh` does, and that script is what
should be run rather than the steps above - it reads secret.txt's object
number off the filesystem instead of assuming it, which is the one part of
this that changes whenever the file list does.

`-d` creates the pool with **no features enabled**, which matters: the
vendored reader gates on eighteen `features_for_read` and refuses a pool that
uses anything else. `zdb -e -p <dir> genesisacl` reports an empty
`features_for_read:` list, so there is nothing here for it to refuse. A
fixture rebuilt without `-d` will not mount, and the failure will look like a
reader bug rather than a fixture bug.

### What is on it

Object 34 is the root directory; ZPL version is 5 and `SA_ATTRS` is object 32.
The SA layout every file uses is number 2:

    [ MODE SIZE GEN UID GID PARENT FLAGS ATIME MTIME CTIME CRTIME LINKS
      DACL_COUNT DACL_ACES ]

Thirteen fixed-size attributes totalling 136 bytes, then `ZPL_DACL_ACES`,
which is **variable length** - its size is not in the registry, it is in the
header's `sa_lengths[]` array. That is the whole reason the reader needs a
real SA walk rather than the fixed `SA_MODE_OFFSET` constants: those constants
can reach `mode` and `uid`, and they can never reach the ACL.

| object | name | mode | owner | notes |
|---|---|---|---|---|
| 2 | `readable.txt` | 0644 | 1000:1000 | trivial ACL |
| 3 | `secret.txt` | 0600 | 1000:1000 | **non-trivial ACL** |
| 4 | `open.txt` | 0666 | 0:0 | trivial ACL |
| 128 | `sub` | 0755 | 0:0 | directory |
| 5 | `sub/motd` | 0640 | 0:0 | trivial ACL |

### The ACL on `secret.txt`, and why it is that one

    ALLOW owner@       read,write,append,attrs,write_attrs,acl,write_acl,owner,sync
    ALLOW user 1001    read_data,read_attributes,read_acl,synchronize
    DENY  everyone@    read_data,write_data,append_data,execute
    ALLOW everyone@    read_attributes,read_acl,synchronize

Four ACEs, 40 bytes: 8 + **16** + 8 + 8. The 16-byte one is the point. An ACE
naming `owner@`, `group@` or `everyone@` stores its who in the flags and is a
bare 8-byte `zfs_ace_hdr_t`; an ACE naming an actual id needs 8 more bytes to
put it in. So the ACE stream is **variable width and must be walked**, never
indexed - and a fixture whose ACEs are all one size cannot catch a reader that
assumes they are.

The ACL is chosen so that the POSIX view and the ACL view *have* to disagree.
uid 1001 is not the owner and not in the owning group, and it may read a file
whose mode is 0600. No mode word means that. A reader consulting only
`st_mode` and a reader consulting the ACL give different answers for uid 1001,
which is what makes it possible to tell, from a test, which one is running.

Note the mode really is 0600 and that is not an inconsistency: `owner@` allows
read and write, `group@` is not mentioned, and `everyone@` is allowed no data
access - project those three onto rwx triples the way `zfs_mode_compute` does
and you get 0600 exactly. The named-user ACE contributes nothing to the mode,
because there is nowhere in a mode word for it to go.

### ZFS_ACL_TRIVIAL, which cost a debugging round

`zplsetacl` clears bit 0x4 (`ZFS_ACL_TRIVIAL`) in the znode's `pflags` when it
writes the ACL, and the first version did not. `ZFS_ACL_TRIVIAL` means "this
file's ACL says exactly what its mode bits say", and `zfs_zaccess` uses it to
skip loading the ACL at all. With a non-trivial ACL on disk and the flag still
set, real ZFS denied uid 1001 the access the ACL granted - it never looked.
Nothing was corrupt and no read failed; the ACL was simply never consulted.

Any reader of ZFS permissions has to honour that flag, and the fixture's
`readable.txt` (flag set, trivial ACL) and `secret.txt` (flag clear,
non-trivial ACL) exist so both branches are exercised.

### Linux will not enforce this ACL, and that is not a fixture bug

`acltype=nfsv4` on Linux stores and preserves NFSv4 ACLs but does not act on
them. From `module/os/linux/zfs/zfs_vfsops.c:acltype_changed_cb`:

    case ZFS_ACLTYPE_NFSV4:
    case ZFS_ACLTYPE_OFF:
            zfsvfs->z_acl_type = ZFS_ACLTYPE_OFF;

So `nfsv4` collapses to `off` and Linux falls back to mode bits: import this
pool on Linux and uid 1001 is denied `secret.txt`, even though the ACL grants
it. The bytes are right - they were written by OpenZFS's own `sa.c` - and
FreeBSD or illumos would honour them. It does mean the host cannot be used to
verify enforcement, only to verify the on-disk encoding, and it means
enforcing this ACL is something Genesis does that Linux-on-ZFS does not.

### Rebuilding `zplsetacl`

`libzpool`'s headers need one shim: `sys/abd_os.h` lives in
`/usr/include/libzpool/` rather than under a `sys/` directory, so point a
symlink at it.

    mkdir -p inc && ln -sfn /usr/include/libzpool inc/sys
    gcc -o zplsetacl tests/host/zplsetacl.c \
        -I/usr/include/libzfs -I/usr/include/libspl -Iinc \
        -D_GNU_SOURCE -lzpool -lnvpair -lzfs_core -luutil


## `genesispool.dat.gz` - the version 1 fixture

Rebuilt with `tests/host/fixtures/mkzplpool.sh`. Needs no root and no ZFS
kernel module: `libzpool` is OpenZFS's own code built for userland, which is
how `ztest` works. The vdev path must be ABSOLUTE - `spa_create` returns
EINVAL for a relative one, which reads as "the pool is invalid" rather than
"the path is".

It is ZPL version 1 deliberately. Its attributes live in a `znode_phys_t` at
fixed offsets rather than in the System Attribute registry, and its ACL lives
in that structure's 88 bytes of `zp_acl`. That is the format no modern pool
uses and the one nothing else in the tree exercises.

`secret.txt` on it carries the SAME four entries as `secret.txt` on
`genesisacl.dat` - same mode, same owner, same grant to uid 1001 - in a
completely different encoding. The two fixtures agree about what the ACL says
and disagree about how it is written down, so the decoded results can be
compared directly and a difference is the decoder's fault rather than the
fixture's.

### The two version 1 layouts

`z_acl_version` decides which structure occupies the 88 bytes:

| version | offset 8 | offset 14 | entries |
|---|---|---|---|
| 0 (INITIAL) | `uint32` **count** | pad | fixed 12-byte `ace_t` |
| 1 (FUID) | `uint32` **size in bytes** | `uint16` count | variable-width, as v5 |

Both are handled, and which applies is decided per OBJECT from the dnode's
bonus type - a pool upgraded to version 5 still has old objects in it until
something rewrites them.

### The field order, which is the actual trap

    old ace_t          { uint32 who, uint32 mask, uint16 flags, uint16 type }
    modern zfs_ace_hdr { uint16 type, uint16 flags, uint32 mask }  [+ who]

The who comes **first** in one and last in the other; the type moves from
offset 0 to offset 10. Decode an old ACE with the modern layout and the low
half of the uid is read as the ACE type - for uid 1001, `0x03E9`, which is not
a recognised type, so the entry is **skipped rather than misread**. The ACL
quietly loses its most interesting entry and still looks well-formed. That is
why `secret.txt` names uid 1001 specifically, and why the test asserts the
offsets it is pinning rather than just the count.
