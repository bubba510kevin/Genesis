#ifndef FS_H
#define FS_H

#include "typesk.h"

/* The filesystem vtable, and one mounted volume behind it.
 *
 * --- Why this exists before ext4 rather than as part of it ----------------
 * fs_root() used to return a `fat_volume_t *` and every caller took it from
 * there: fileobj.c cast an object body to a fat_entry_t, sys_stat_path read
 * FAT_ATTR_DIRECTORY out of an attribute byte, exec_read_file called
 * fat_read_path, and sys_faccessat called fat_lookup. Six places in three
 * files knew the on-disk format of a FAT16 directory entry.
 *
 * Adding a second filesystem underneath that means changing all six at once,
 * with no way to tell whether a failure came from the new filesystem or from
 * the abstraction that was supposed to hide it. Doing the abstraction FIRST
 * and separately means the ext4 work lands against an interface already
 * proven by the filesystem that works - and systest's filesystem section
 * running identically on both volumes is what proves it is an abstraction
 * rather than FAT with extra steps.
 *
 * --- Errno, not FAT_ERR_ -------------------------------------------------
 * The vtable returns negative errno directly. FAT_ERR_NOTFOUND and friends
 * stop at the shim in fatfs.c, which is where they belong: a second
 * filesystem has its own error space, and translating two of them at the
 * syscall layer is two translation tables that drift. The distinction
 * fat_lookup draws between "no such name" and "a component was not a
 * directory" survives the translation, because ash prints different messages
 * for them.
 *
 * --- The node ------------------------------------------------------------
 * fs_node_t is what a directory entry looks like from above: a size, a type,
 * a stable identity, and an opaque blob the filesystem uses to find the thing
 * again. The blob is inline rather than a pointer because a node is stored in
 * an object body from a static pool, and a pointer would need a second
 * allocator with its own lifetime rules. */

/* Big enough for any filesystem's own entry. A fat_entry_t is 24 bytes. The
 * build-time assertion in fatfs.c is what stops this silently becoming too
 * small when a filesystem wants more. */
#define FS_PRIVATE_MAX 64

/* Longest name a directory entry can carry, NUL included. 8.3 needs 13;
 * ext2 allows 255, and sizing for that now means getdents does not grow a
 * second buffer later. */
#define FS_NAME_MAX 256

typedef struct fs_volume fs_volume_t;

/* Declared, not included: fs.h is included by a great many files and only a
 * few of them care about access control. kernel/include/acl.h has the
 * definition; a caller of fs_getacl includes it. */
struct acl;
struct cred;

typedef struct fs_node {
    /* Which volume this came from. Carried ON the node rather than passed
     * alongside it, because an open file object outlives the call that
     * resolved it - and the alternative is every caller remembering to keep
     * the two together, which is the bug where a node from one volume is read
     * through another volume's operations. */
    fs_volume_t *vol;

    uint64 size;
    uint64 ino;          /* stable identity, for st_ino                     */
    int    is_dir;

    /* --- ownership and permission ----------------------------------------
     *
     * Carried on the node because every caller that has a node is a caller
     * that may have to make an access decision, and re-resolving the path to
     * find out who owns it would be both slow and racy.
     *
     * A filesystem with no concept of ownership fills these in with whatever
     * it has decided to pretend - fatfs reports a fixed owner and a fixed
     * mode - rather than leaving them zero. Zero is uid 0, and a filesystem
     * that forgets to set them would be reporting every file as root's,
     * which is the wrong direction to fail in. */
    uint32 mode;         /* S_IFMT bits plus rwx, as stat(2) reports        */
    uint32 uid;
    uint32 gid;

    uint8  priv[FS_PRIVATE_MAX];
} fs_node_t;

/* One entry, as reported to a directory walk. */
typedef struct fs_dirent {
    char   name[FS_NAME_MAX];
    uint64 ino;
    int    is_dir;
} fs_dirent_t;

/* Return non-zero to stop the walk early. */
typedef int (*fs_dir_cb)(const fs_dirent_t *ent, void *ctx);

/* What statfs(2) needs from a filesystem, in the filesystem's own terms.
 *
 * Deliberately NOT the Linux struct statfs: that has a magic number, a
 * padded fsid and four spare words, all of which are ABI shape rather than
 * filesystem fact. The syscall assembles those; a filesystem should not have
 * to know the layout of a userspace structure to report how full it is. */
typedef struct fs_statfs {
    uint64 block_size;    /* bytes per allocation unit                     */
    uint64 blocks;        /* total allocation units on the volume          */
    uint64 blocks_free;   /* how many are unused                           */
    uint64 name_max;      /* longest filename this filesystem can hold     */
} fs_statfs_t;

typedef struct fs_ops {
    const char *name;

    /* Resolve an ABSOLUTE, already-normalized path. "/" is the root itself.
     * Returns 0 or a negative errno. */
    int (*lookup)(fs_volume_t *v, const char *abs_path, fs_node_t *out);

    /* Read at an offset. Returns bytes read - a short read at end of file is
     * normal, not an error - or a negative errno. */
    int64 (*read)(fs_volume_t *v, const fs_node_t *n, uint64 offset,
                  void *buf, uint64 max);

    /* Write at an offset, or NULL for a filesystem that cannot.
     *
     * A NULL slot is what makes fs_writable answer honestly, and that is what
     * access(W_OK) reports - so a filesystem gaining write support changes
     * this one pointer and access changes with it, rather than there being a
     * second flag somewhere that has to be remembered. */
    int64 (*write)(fs_volume_t *v, fs_node_t *n, uint64 offset,
                   const void *buf, uint64 max);

    /* Walk a directory. */
    int (*iterate)(fs_volume_t *v, const fs_node_t *dir, fs_dir_cb cb,
                   void *ctx);

    /* --- mutation ---------------------------------------------------------
     *
     * These four were the gap Part 17's capability audit named: fourteen
     * syscalls - mkdir, rmdir, unlink, rename and their *at variants - were
     * all blocked not on syscall work but on this vtable having no way to
     * express directory mutation at all. There was nothing for a mkdir to
     * call.
     *
     * NULL for a filesystem that cannot, exactly like `write` above, and for
     * the same reason: fs_can_mutate reads the pointers rather than a
     * separate flag that has to be remembered.
     *
     * Paths are ABSOLUTE and already normalized, and are relative to the
     * VOLUME - the mount router strips the mount point first, the same
     * convention lookup already follows.
     *
     * rename is a single operation rather than link-then-unlink because FAT
     * has no links: a rename within one directory rewrites eleven bytes in
     * place, and splitting it would mean writing a second entry pointing at
     * the same cluster chain - which is exactly the state fsck calls
     * cross-linked. */
    /* Filesystem-wide numbers, for statfs(2): block size, total blocks, free
     * blocks, and the name length limit.
     *
     * `free_blocks` is the expensive one and the reason this is a vtable slot
     * rather than fields on fs_volume_t cached at mount: on FAT it means
     * counting zero entries across the whole allocation table, which is a
     * disk read per FAT sector and changes on every write. A cached value
     * would be a number that is right at mount and wrong forever after -
     * which is worse than a slow answer, because df would confidently report
     * a full disk as empty.
     *
     * NULL for a filesystem that cannot answer, which statfs reports as -
     * see fs_statfs. */
    int (*statfs)(fs_volume_t *v, fs_statfs_t *out);

    /* Create an empty regular file. -EEXIST if the name is taken.
     *
     * Its own slot rather than a flag on `write`, because "make the name"
     * and "put bytes in it" fail differently and at different layers: a
     * create can fail because the directory is full, a write because the
     * volume is. Collapsing them would make O_CREAT|O_WRONLY report one
     * error for both.
     *
     * NULL for a filesystem that cannot, like every slot here.
     *
     * `c` is the creator, so a filesystem that records owners can record
     * the right one - without it, every object gnfs made belonged to root
     * and chown was the only way a non-root user ever came to own a file.
     * NULL means the kernel itself, which is root's. A filesystem with no
     * owners (FAT) ignores it. The same goes for mkdir below. */
    int (*create)(fs_volume_t *v, const char *abs_path,
                  const struct cred *c);

    /* Set a file's length - shorter or longer. Growing must make the new
     * bytes read as ZERO, which on a filesystem with no holes means really
     * writing them.
     *
     * Takes a node rather than a path because both of its callers have one:
     * ftruncate(2) starts from a descriptor, and O_TRUNC has just resolved
     * the file it is about to open. fs_truncate_at() is the path-shaped
     * wrapper for truncate(2). */
    int (*truncate)(fs_volume_t *v, fs_node_t *n, uint64 size);

    int (*mkdir)(fs_volume_t *v, const char *abs_path, const struct cred *c);
    int (*rmdir)(fs_volume_t *v, const char *abs_path);
    int (*unlink)(fs_volume_t *v, const char *abs_path);
    int (*rename)(fs_volume_t *v, const char *old_path, const char *new_path);

    /* --- unmount ----------------------------------------------------------
     *
     * "The filesystem is no longer reachable through the mount table; release
     * what you hold for this volume." NULL for a filesystem with nothing to
     * release.
     *
     * ROADMAP item 7 named its absence as the one item on that list that was
     * a BUG rather than a missing feature, and kernel/zfs/zfs_vfs.c had
     * documented it against itself: a filesystem's per-volume slot was taken
     * by a successful probe and never given back, because fs_unmount_volume
     * removed mount-table entries and never told the filesystem. A machine
     * that had seen four pools stopped recognising the fifth.
     *
     * --- RETIRE, DO NOT FREE, and this is the part that is easy to get
     * wrong. The obvious implementation - mark the slot unused so the next
     * probe can have it - reintroduces a failure kernel/dev/volume.c
     * deliberately avoids and documents: handles on files that lived on this
     * volume are still out there holding fs_node_t bodies that point at this
     * fs_volume_t. What protects them is `mounted`, which turns their next
     * read into -ENODEV. Handing the same fs_volume_t to a different disk
     * turns that -ENODEV into a successful read of somebody else's
     * filesystem, which is strictly worse than the leak this fixes.
     *
     * So an implementation should release the EXPENSIVE state (the mount
     * object, the cached superblock, the device reference) and leave the slot
     * claimed but retired, reusing a retired slot only when no free one is
     * left. That is exactly the policy volume.c already states - "slots are
     * recovered when the pool is exhausted, not when a device goes away" -
     * which was not true of anything before this hook existed, because
     * nothing recovered them at all.
     *
     * Called with `mounted` already cleared, and only when the LAST
     * mount-table entry referring to this volume has gone: a volume mounted
     * at two points is still reachable through the other one. */
    void (*unmount)(fs_volume_t *v);

    /* --- the access control list ------------------------------------------
     *
     * Fill `out` with this object's ACL. NULL for a filesystem that has none,
     * which is not a limitation the callers have to know about: fs_getacl
     * projects the node's mode into a trivial ACL instead, so FAT and ZFS
     * answer the same question through the same code and only one of them
     * has to implement anything.
     *
     * Returns 0, or a negative errno. -ENOENT specifically means "this object
     * has no stored ACL" - a pre-v5 ZFS filesystem, or one whose object was
     * never given one - and is handled the same way a NULL slot is, by
     * projection. It is not a failure.
     *
     * Why a slot rather than a field on fs_node_t: an ACL is up to 32 entries
     * and fs_node_t has 64 bytes of private area for the whole filesystem to
     * use. It does not fit, it is not needed on most operations, and reading
     * it costs a walk of the object's attribute layout. */
    int (*getacl)(fs_volume_t *v, const fs_node_t *n, struct acl *out);

    /* Replace this object's ACL with `a` wholesale. NULL for a filesystem
     * that cannot, like every mutation slot above - fs_setacl is what makes
     * that answer honest, the same way fs_writable does for `write`.
     *
     * Whole-ACL replacement rather than an add/remove-one-entry API: chmod
     * and a real NT SetSecurityInfo both already produce a complete new ACL
     * before they get here (acl_apply_chmod, acl_inherit - kernel/include/
     * acl.h) precisely because an NFSv4/NT ACL's entries are order-
     * dependent, and an API that edited one entry in place would have to
     * re-invent the same ordering logic on every filesystem that implements
     * this slot instead of once, in acl.c.
     *
     * Returns 0, or a negative errno. */
    int (*setacl)(fs_volume_t *v, fs_node_t *n, const struct acl *a);

    /* Make (uid, gid) this object's owner and owning group - both real ids,
     * never ACL_CHOWN_KEEP; fs_setowner resolves those first. NULL for a
     * filesystem with no owner to change.
     *
     * A slot of its own rather than setacl with a rewritten acl_t::owner:
     * an object with no stored ACL has nowhere to put one, and one WITH a
     * stored ACL keeps a second copy of the pair inside it that must move
     * too, or owner@ goes on evaluating against the old owner. Only the
     * filesystem knows which copies it keeps. Whether the caller MAY is
     * fs_setowner's question (acl_chown_permitted), never this slot's.
     *
     * Returns 0, or a negative errno. */
    int (*setowner)(fs_volume_t *v, fs_node_t *n, uint32 uid, uint32 gid);
} fs_ops_t;

struct fs_volume {
    const fs_ops_t *ops;
    void           *body;      /* the filesystem's own volume state */
    uint32          block_size;
    int             mounted;
};

/* --- the mount table -----------------------------------------------------
 *
 * The thing fs_root's comment said would go here. It did, and the comment was
 * right about the reason: every caller was already asking "which volume owns
 * this path", so the routing landed in fs_lookup and nothing above it
 * changed.
 *
 * --- Longest prefix, and why the path handed down is relative ------------
 * With / and /mnt/usb both mounted, /mnt/usb/notes.txt belongs to the second
 * and /mnt/usbstuff belongs to the first. Longest MATCHING PREFIX ON A
 * COMPONENT BOUNDARY is the rule, and the boundary half is not pedantry -
 * a plain prefix compare gives /mnt/usbstuff to the USB stick and then asks
 * it for a file called "tuff".
 *
 * What the volume receives is the path with the mount point removed, so the
 * filesystem on the stick is asked for "/notes.txt" - the path AS THE VOLUME
 * SEES IT. The alternative, handing down the full path and letting each
 * filesystem strip its own mount point, means every filesystem knows where it
 * is mounted, and a volume mounted at two points has to be two objects.
 *
 * --- Mounting is not the same as naming ----------------------------------
 * A volume can exist, have a drive letter, and not be mounted anywhere - that
 * is precisely what \??\D: with no POSIX mount point is. The NT side
 * resolves through the namespace to the volume object and asks IT to parse
 * the rest; the POSIX side resolves through this table. Two spellings, one
 * volume, and neither is built on the other. */

#define FS_MOUNT_MAX 8

/* Mount `v` at an absolute, normalized POSIX path. "/" is the root mount and
 * is what fs_set_root is now shorthand for.
 *
 * Returns 0, -EBUSY if something is already mounted exactly there, -ENOSPC if
 * the table is full, -EINVAL for a relative or malformed path. Mounting over
 * a path that does not exist on the parent volume is ALLOWED and deliberate:
 * the mount point is a namespace decision, and requiring the directory to
 * exist first means a read-only root cannot mount anything. */
int fs_mount_at(const char *mount_point, fs_volume_t *v);

/* Remove the mount at exactly this path. Returns 0, -ENOENT, or -EBUSY when
 * another mount sits BELOW this one - unmounting /mnt while /mnt/usb is
 * mounted would leave that entry routing through a volume nothing can reach. */
int fs_unmount_at(const char *mount_point);

/* Non-zero if any mount-table entry still refers to `v`.
 *
 * Exported because "was that the last one" is the question that decides
 * whether the filesystem's unmount hook fires, and both unmount paths have to
 * ask it. Two copies of that test would be two chances to release a
 * filesystem that is still reachable through its other mount point. */
int fs_volume_is_mounted(const fs_volume_t *v);

/* Remove every mount of this volume, wherever it sits. What surprise removal
 * calls: the medium is gone and the question of which of its mount points to
 * take down does not arise - all of them, and -EBUSY is not an answer the
 * hardware will accept. */
int fs_unmount_volume(fs_volume_t *v);

/* Which volume owns this path, and what the path looks like to it.
 *
 * *rel points INTO abs_path at the first character the volume should see, or
 * at a static "/" when the path is exactly the mount point. NULL if nothing
 * is mounted that could own it. */
fs_volume_t *fs_volume_for(const char *abs_path, const char **rel);

/* Walk the mount table. `cb` gets the mount point and the volume; a non-zero
 * return stops the walk and is returned. For /proc/mounts, for the boot
 * report, and for the verification check that asserts an unmounted volume
 * left no entry behind. */
int fs_iterate_mounts(int (*cb)(const char *mount_point, fs_volume_t *v,
                                void *ctx), void *ctx);

/* The root volume, or NULL before anything is mounted. Now the volume mounted
 * at "/" - the same question, answered from the table. */
fs_volume_t *fs_root(void);

/* Install the root volume. flk.c calls this once at boot; it is fs_mount_at
 * with "/" and an error that cannot be reported, kept because that call site
 * has nothing useful to do with a failure at that point in boot. */
void fs_set_root(fs_volume_t *v);

/* --- the operations, as callers should reach them ------------------------
 *
 * Wrappers rather than `v->ops->lookup(...)` at each call site. Three
 * reasons, and the third is the one that matters: a NULL volume becomes
 * -ENODEV in one place instead of a null check at every caller; an operation
 * a filesystem does not implement becomes a specific errno instead of a jump
 * through a null pointer; and the day a mount table exists, the volume can be
 * derived from the path HERE rather than at every call site. */
int   fs_lookup(const char *abs_path, fs_node_t *out);

/* Look up on ONE named volume, with the mount table skipped entirely.
 *
 * This is the NT side. \Device\HarddiskVolume2\bin\sh resolves through the
 * namespace to the volume OBJECT and then asks that object to parse the rest
 * (see ns.h on the unparsed remainder) - and at that point there is no POSIX
 * path to route, only a volume and a path relative to it. It also has to work
 * for a volume that is not mounted anywhere on the POSIX side, which is
 * exactly what \??\D: with no mount point is.
 *
 * `rel` is relative to the volume root and starts with '/'. */
int   fs_lookup_on(fs_volume_t *v, const char *rel, fs_node_t *out);

/* Non-zero if this volume can be written. The volume-in-hand form of
 * fs_writable, for the same callers as fs_lookup_on. */
int   fs_writable_vol(const fs_volume_t *v);
int64 fs_read(const fs_node_t *n, uint64 offset, void *buf, uint64 max);
int64 fs_write(fs_node_t *n, uint64 offset, const void *buf, uint64 max);

/* Create an empty regular file at an absolute path. Returns 0, -EEXIST,
 * -EROFS if the filesystem has no create slot, or whatever the filesystem
 * says. The parent must exist; this creates one name, not a path. `c` is
 * the creator (see fs_ops_t::create); NULL means the kernel. */
int fs_create(const char *abs_path, const struct cred *c);

/* Set the length of an already-resolved file. */
int fs_truncate(fs_node_t *n, uint64 size);

/* The same, by path - what truncate(2) needs. */
int fs_truncate_at(const char *abs_path, uint64 size);

/* Filesystem-wide numbers for whichever volume owns `abs_path`. Returns 0, or
 * -ENOSYS if that filesystem cannot answer - which is a real answer and not a
 * placeholder: df on a filesystem with no notion of free space should say so
 * rather than be told zero. */
int fs_statfs(const char *abs_path, fs_statfs_t *out);
int   fs_iterate(const fs_node_t *dir, fs_dir_cb cb, void *ctx);

/* Non-zero if the volume owning this path can be written at all. What
 * access(W_OK) answers from, derived from the vtable rather than stored.
 *
 * Takes a path now, because with a mount table the answer differs per volume:
 * a read-only root with a writable stick mounted under it makes a single
 * global answer wrong in both directions. */
int   fs_writable(const char *abs_path);

/* Read a whole file by path, allocating for it. Returns 0 and fills *out and
 * *size, or a negative errno. The caller frees with fs_free_file.
 *
 * execve's shape, and it lives here rather than in syscall.c so the loader
 * does not have to know what a filesystem is. */
int   fs_read_whole(const char *abs_path, uint8 **out, uint32 *size);
void  fs_free_file(uint8 *buf);

/* --- mutation ------------------------------------------------------------
 *
 * The path-taking front ends for the four vtable slots above. Each resolves
 * the mount, hands the volume-relative path down, and returns 0 or a
 * negative errno.
 *
 * fs_rename refuses to cross volumes with -EXDEV rather than silently
 * copying: POSIX says rename(2) does not move data between filesystems, and
 * a caller told otherwise would skip the copy-and-delete it actually needs.
 */
/* This object's ACL, from the filesystem if it has one and projected from the
 * node's mode if it has not. Never fails for want of an ACL - see
 * fs_ops_t::getacl. Returns 0 or a negative errno. */
int fs_getacl(const fs_node_t *n, struct acl *out);

/* May this credential do `wanted` (an NFSv4 mask) to this object? 0 or
 * -EACCES. The whole point of routing every check through one function is
 * that there is one place where the answer is decided, rather than an open()
 * and an access(2) that can drift apart - which is what the old faccessat
 * comment about "a check that consults its own answer" was describing. */
int fs_access(const fs_node_t *n, const struct cred *c, uint32 wanted);

/* Replace `n`'s ACL with `a`, after checking the caller holds ACE_WRITE_ACL -
 * see the definition in kernel/fs/vfs.c for why this is the one place that
 * check and the one place fs_ops_t::setacl is called. */
int fs_setacl(fs_node_t *n, const struct cred *c, const struct acl *a);

/* chown: make (uid, gid) `n`'s owner and group, either of which may be
 * ACL_CHOWN_KEEP. -EROFS on a read-only volume, -EPERM when
 * acl_chown_permitted refuses. The one place fs_ops_t::setowner is called. */
int fs_setowner(fs_node_t *n, const struct cred *c, uint32 uid, uint32 gid);

int fs_mkdir(const char *abs_path, const struct cred *c);
int fs_rmdir(const char *abs_path);
int fs_unlink(const char *abs_path);
int fs_rename(const char *old_path, const char *new_path);


#endif
