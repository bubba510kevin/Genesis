#include "fs.h"
#include "acl.h"
#include "pcache.h"
#include "kheap.h"
#include "path.h"
#include "typesk.h"

/* The generic half of the filesystem layer: one mounted volume, and the
 * wrappers every caller goes through. See fs.h for why the wrappers exist.
 *
 * There is deliberately no filesystem knowledge in this file. If something
 * here ever needs to know what FAT is, the abstraction has failed and this is
 * where it will be visible. */

/* --- the mount table -----------------------------------------------------
 *
 * See fs.h for the rules. What follows is how, and the two things worth
 * saying about the implementation.
 *
 * The table is a fixed array scanned linearly. Eight entries, scanned on
 * every path resolution: that is at most eight string compares against a path
 * already in a register-warm buffer, and it happens once per open, not once
 * per read. A tree would be faster and would need an allocator, an ordering
 * invariant and a rebalance - none of which can be justified before there is
 * a second mount at all.
 *
 * Entries are never reordered. fs_volume_for finds the longest match by
 * comparing lengths as it goes rather than by keeping the table sorted, so
 * mounting /mnt/usb before /mnt does not change any answer. Sorting on insert
 * is the version where the invariant is one unmount away from being wrong. */

typedef struct {
    char         point[64];
    uint64       len;              /* length of point, without a trailing / */
    fs_volume_t *vol;
    int          in_use;
} mount_t;

static mount_t mounts[FS_MOUNT_MAX];

static uint64 str_len(const char *s) {
    uint64 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int str_eq(const char *a, const char *b) {
    uint64 i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return a[i] == b[i];
}

/* Does `path` lie at or under `point`?
 *
 * The component-boundary test is the whole function. "/mnt" covers
 * "/mnt/usb" and does NOT cover "/mntstuff", and a plain prefix compare gets
 * the second one wrong - it hands /mntstuff to the volume mounted at /mnt and
 * then asks that volume for a file called "stuff". */
static int covers(const mount_t *m, const char *path, uint64 path_len) {
    uint64 i;

    if (m->len == 1) {
        return 1;                  /* "/" covers everything */
    }
    if (path_len < m->len) {
        return 0;
    }
    for (i = 0; i < m->len; i++) {
        if (path[i] != m->point[i]) {
            return 0;
        }
    }
    return path[m->len] == '\0' || path[m->len] == '/';
}

fs_volume_t *fs_volume_for(const char *abs_path, const char **rel) {
    static const char root_rel[] = "/";
    uint64  path_len;
    mount_t *best = NULL;
    int i;

    if (abs_path == NULL || abs_path[0] != '/') {
        return NULL;
    }
    path_len = str_len(abs_path);

    for (i = 0; i < FS_MOUNT_MAX; i++) {
        if (!mounts[i].in_use || mounts[i].vol == NULL) {
            continue;
        }
        if (!covers(&mounts[i], abs_path, path_len)) {
            continue;
        }
        if (best == NULL || mounts[i].len > best->len) {
            best = &mounts[i];
        }
    }
    if (best == NULL) {
        return NULL;
    }

    if (rel != NULL) {
        /* Exactly the mount point resolves to that volume's root. Returning
         * the empty string here would hand the filesystem "" and every
         * lookup() in the tree treats that as malformed rather than as the
         * root - which turns `ls /mnt/usb` into -EINVAL. */
        if (best->len == 1) {
            *rel = abs_path;
        } else if (abs_path[best->len] == '\0') {
            *rel = root_rel;
        } else {
            *rel = abs_path + best->len;
        }
    }
    return best->vol;
}

int fs_mount_at(const char *mount_point, fs_volume_t *v) {
    uint64 len;
    int    free_slot = -1;
    int    i;

    if (mount_point == NULL || v == NULL || mount_point[0] != '/') {
        return -22;                     /* -EINVAL */
    }
    len = str_len(mount_point);
    if (len + 1 > sizeof(mounts[0].point)) {
        return -36;                     /* -ENAMETOOLONG */
    }
    /* A trailing slash on anything but the root is rejected rather than
     * trimmed. Trimming means "/mnt" and "/mnt/" are one mount that unmounts
     * under either spelling, which is friendlier and also means the table can
     * hold two entries that look different and are the same - and then
     * fs_unmount_at removes whichever it finds first. The caller normalizes;
     * path_normalize already does exactly this. */
    if (len > 1 && mount_point[len - 1] == '/') {
        return -22;
    }

    for (i = 0; i < FS_MOUNT_MAX; i++) {
        if (!mounts[i].in_use) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (str_eq(mounts[i].point, mount_point)) {
            return -16;                 /* -EBUSY */
        }
    }
    if (free_slot < 0) {
        return -28;                     /* -ENOSPC */
    }

    for (i = 0; i < (int)len; i++) {
        mounts[free_slot].point[i] = mount_point[i];
    }
    mounts[free_slot].point[len] = '\0';
    mounts[free_slot].len        = len;
    mounts[free_slot].vol        = v;
    mounts[free_slot].in_use     = 1;
    return 0;
}

/* Defined below, next to fs_volume_is_mounted which it consults. Declared
 * here because both unmount paths call it and the first of them comes
 * first. */
static void release_if_last(fs_volume_t *v);

int fs_unmount_at(const char *mount_point) {
    int found = -1;
    int i;

    if (mount_point == NULL) {
        return -22;
    }
    for (i = 0; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].in_use && str_eq(mounts[i].point, mount_point)) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        return -2;                      /* -ENOENT */
    }
    /* A mount below this one keeps it busy. Removing /mnt while /mnt/usb is
     * mounted leaves an entry whose parent path resolves through nothing -
     * lookups on /mnt/usb would still work, which sounds harmless and means
     * the mount table no longer describes a tree. */
    for (i = 0; i < FS_MOUNT_MAX; i++) {
        if (i == found || !mounts[i].in_use) {
            continue;
        }
        if (covers(&mounts[found], mounts[i].point,
                   str_len(mounts[i].point))) {
            return -16;                 /* -EBUSY */
        }
    }
    {
        fs_volume_t *v = mounts[found].vol;

        mounts[found].in_use = 0;
        mounts[found].vol    = NULL;

        /* Only when this was the LAST reference. A volume mounted at two
         * points is still reachable through the other one, and releasing the
         * filesystem here would leave that mount routing into a volume whose
         * state had been handed back. */
        release_if_last(v);
    }
    return 0;
}

int fs_volume_is_mounted(const fs_volume_t *v) {
    int i;

    if (v == NULL) {
        return 0;
    }
    for (i = 0; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].in_use && mounts[i].vol == v) {
            return 1;
        }
    }
    return 0;
}

/* Tell the filesystem it is gone, once nothing routes to it any more.
 *
 * `mounted` is cleared HERE rather than left to each filesystem, so that the
 * flag every stale fs_node_t is protected by has exactly one writer. See
 * fs_ops_t::unmount in fs.h for why the filesystem must retire its slot
 * rather than free it. */
static void release_if_last(fs_volume_t *v) {
    if (v == NULL || fs_volume_is_mounted(v)) {
        return;
    }
    v->mounted = 0;
    if (v->ops != NULL && v->ops->unmount != NULL) {
        v->ops->unmount(v);
    }
}

int fs_unmount_volume(fs_volume_t *v) {
    int removed = 0;
    int i;

    if (v == NULL) {
        return -22;
    }
    /* Every cached page of this volume, first. The fs_volume_t is about to be
     * retired and its pages describe bytes on a medium nobody can name any
     * more - and volume.c's retire-rather-than-free policy means the SAME
     * pointer can later describe a different disk, which would turn those
     * pages into a successful read of somebody else's filesystem. That is the
     * exact hazard volume.c's comment already warns about one layer down. */
    pcache_invalidate_volume(v);

    /* No -EBUSY here, on purpose, and it is the one place in this file that
     * ignores the tree invariant above. The caller is surprise removal: the
     * medium is already gone, and refusing to take down a mount because
     * something is mounted underneath it would leave an entry pointing at a
     * volume whose device answers -ENODEV. Better a mount table that briefly
     * describes less than a tree than one that describes hardware that is not
     * there. */
    for (i = 0; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].in_use && mounts[i].vol == v) {
            mounts[i].in_use = 0;
            mounts[i].vol    = NULL;
            removed++;
        }
    }

    /* Unconditionally, not only when something was removed. A volume that was
     * probed but never mounted anywhere still holds a filesystem slot, and
     * that is precisely the case the leak was made of: a probe that succeeded
     * against a disk nobody mounted took a slot for the life of the machine.
     * The return value still reports whether the mount table changed, which
     * is what the caller asked. */
    release_if_last(v);
    return removed > 0 ? 0 : -2;
}

int fs_iterate_mounts(int (*cb)(const char *mount_point, fs_volume_t *v,
                                void *ctx), void *ctx) {
    int i;

    if (cb == NULL) {
        return -22;
    }
    for (i = 0; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].in_use && mounts[i].vol != NULL) {
            int rc = cb(mounts[i].point, mounts[i].vol, ctx);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

fs_volume_t *fs_root(void) {
    int i;

    for (i = 0; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].in_use && mounts[i].len == 1) {
            return mounts[i].vol;
        }
    }
    return NULL;
}

void fs_set_root(fs_volume_t *v) {
    /* Replace rather than refuse. This is what boot calls, and a second call
     * means the root is being changed - which is what "then flip root" in the
     * ZFS plan is, and it must not need an unmount of a filesystem that is
     * still executing the code doing the flipping. */
    int i;

    for (i = 0; i < FS_MOUNT_MAX; i++) {
        if (mounts[i].in_use && mounts[i].len == 1) {
            mounts[i].vol = v;
            return;
        }
    }
    fs_mount_at("/", v);
}

/* The one place a missing or unmounted volume becomes an errno.
 *
 * -ENODEV rather than -ENOENT, and the distinction is worth carrying: a path
 * that does not exist on a mounted volume is a different problem from a
 * volume that was never mounted, and a boot with a missing disk should not
 * present as every file being absent. */
static int volume_ok(const char *abs_path, fs_volume_t **out,
                     const char **rel) {
    fs_volume_t *v = fs_volume_for(abs_path, rel);

    if (v == NULL || !v->mounted || v->ops == NULL) {
        return -19;                     /* -ENODEV */
    }
    *out = v;
    return 0;
}

int fs_lookup(const char *abs_path, fs_node_t *out) {
    fs_volume_t *v;
    const char *rel = NULL;
    int rc;

    if (abs_path == NULL || out == NULL) {
        return -22;                     /* -EINVAL */
    }
    rc = volume_ok(abs_path, &v, &rel);
    if (rc != 0) {
        return rc;
    }
    if (v->ops->lookup == NULL) {
        return -22;
    }
    /* `rel`, not `abs_path`. The volume is asked for the path AS IT SEES IT -
     * a stick mounted at /mnt/usb is asked for "/notes.txt", never for
     * "/mnt/usb/notes.txt". The alternative is every filesystem knowing where
     * it is mounted, and then a volume mounted at two points has to be two
     * objects. */
    rc = v->ops->lookup(v, rel, out);
    if (rc == 0) {
        /* Stamped here rather than by each filesystem. A filesystem that
         * forgot would produce a node that reads correctly through whichever
         * volume happens to be current, which is right today and silently
         * wrong the first time there are two. */
        out->vol = v;
    }
    return rc;
}

/* Is `abs_path` exactly a mount point? Used to refuse rmdir on one - a mount
 * point is a directory belonging to the PARENT volume, and removing it while
 * something is mounted there leaves the mount table naming a directory that
 * no longer exists. Exact match, not prefix: /mnt/usb being a mount point
 * says nothing about /mnt/usb/sub. */
static int fs_is_mount_point(const char *abs_path) {
    uint64 len = str_len(abs_path);
    int i;

    for (i = 0; i < FS_MOUNT_MAX; i++) {
        uint64 k;
        int same = 1;

        if (!mounts[i].in_use || mounts[i].vol == NULL) {
            continue;
        }
        if (mounts[i].len != len) {
            continue;
        }
        for (k = 0; k < len; k++) {
            if (mounts[i].point[k] != abs_path[k]) {
                same = 0;
                break;
            }
        }
        if (same) {
            return 1;
        }
    }
    return 0;
}

/* --- mutation ------------------------------------------------------------
 *
 * The same volume_ok / hand-down-the-relative-path shape fs_lookup uses, so
 * a filesystem sees mkdir("/x") whether it is mounted at / or at /mnt/usb.
 */

/* The directory that holds (or would hold) `abs_path`. "/x" -> "/",
 * "/a/b" -> "/a"; a trailing slash names the same entry ("mkdir /a/b/"),
 * not a child of it. Resolved through fs_lookup rather than through the
 * volume the child lives on, so a mount point is judged by the directory
 * actually being written into. */
static int fs_lookup_parent(const char *abs_path, fs_node_t *out) {
    char parent[PATH_MAX_LEN];
    uint64 len = 0, slash = 0, i;
    int rc;

    while (abs_path[len] != '\0') {
        len++;
    }
    if (len == 0 || len >= sizeof(parent)) {
        return -36;                            /* -ENAMETOOLONG */
    }
    while (len > 1 && abs_path[len - 1] == '/') {
        len--;
    }
    for (i = 0; i < len; i++) {
        parent[i] = abs_path[i];
        if (abs_path[i] == '/') {
            slash = i;
        }
    }
    parent[slash == 0 ? 1 : slash] = '\0';

    rc = fs_lookup(parent, out);
    if (rc != 0) {
        return rc;
    }
    if (!out->is_dir) {
        return -20;                            /* -ENOTDIR */
    }
    return 0;
}

/* May `c` add a new name to the directory that would hold `abs_path`?
 * `wanted` is ACE_ADD_FILE for create, ACE_ADD_SUBDIRECTORY for mkdir, and
 * ACE_EXECUTE is always added to it: POSIX asks for write AND search on the
 * parent, and NFSv4/NT reach the same answer (a directory you cannot
 * traverse is one you cannot name a child of). Both are the same bits
 * acl_from_mode derives from a directory's w and x, so a plain mode-only
 * directory answers exactly as POSIX would. NULL `c` is the kernel and is
 * not asked.
 *
 * Callers check for an existing name FIRST. open(O_CREAT) on a file that
 * already exists must not need write access to its directory - that is how
 * a user opens their own file in a directory they cannot add to - and
 * mkdir of an existing name answers -EEXIST on Linux regardless. */
static int fs_may_add_entry(const char *abs_path, const struct cred *c,
                            uint32 wanted) {
    fs_node_t pn;
    int rc;

    if (c == NULL) {
        return 0;
    }
    rc = fs_lookup_parent(abs_path, &pn);
    if (rc != 0) {
        return rc;
    }
    return fs_access(&pn, c, wanted | ACE_EXECUTE);
}

/* May `c` remove the existing name `abs_path`, already resolved to
 * `victim`? The NFSv4 rule (RFC 5661 6.2.1.3.2), which is ZFS's and, in
 * spirit, NT's: ACE_DELETE on the object itself, OR ACE_DELETE_CHILD and
 * ACE_EXECUTE on the directory holding it. Either suffices. For a mode-only
 * object the first half never fires (acl_from_mode grants no ACE_DELETE),
 * so the answer is exactly POSIX's "write and search on the parent".
 *
 * When both refuse, the parent's answer is the one returned - it is the one
 * a mode-only world can act on.
 *
 * A STICKY parent (S_ISVTX - /tmp) narrows the parent half: even with
 * DELETE_CHILD on the directory, only the entry's owner, the directory's
 * owner, or a supreme caller may remove it, and anyone else gets -EPERM -
 * Linux's errno here, because it is not the directory's permission bits
 * that refused. The object half is NOT narrowed: an explicit ACE_DELETE on
 * the object was put there by someone allowed to write its ACL, which is a
 * deliberate grant the sticky bit exists to protect against the absence
 * of, not to override. */
static int fs_may_remove_entry(const char *abs_path, const fs_node_t *victim,
                               const struct cred *c) {
    const cred_t *cr = (const cred_t *)c;
    fs_node_t pn;
    int rc;

    if (c == NULL) {
        return 0;
    }
    if (fs_access(victim, c, ACE_DELETE) == 0) {
        return 0;
    }
    rc = fs_lookup_parent(abs_path, &pn);
    if (rc != 0) {
        return rc;
    }
    rc = fs_access(&pn, c, ACE_DELETE_CHILD | ACE_EXECUTE);
    if (rc != 0) {
        return rc;
    }
    if ((pn.mode & S_ISVTX) && !cred_is_supreme(cr) &&
        cr->euid != victim->uid && cr->euid != pn.uid) {
        return -1;                             /* -EPERM */
    }
    return 0;
}

int fs_mkdir(const char *abs_path, const struct cred *c, uint32 mode) {
    fs_volume_t *v;
    const char *rel = NULL;
    int rc;

    if (abs_path == NULL) {
        return -22;
    }
    rc = volume_ok(abs_path, &v, &rel);
    if (rc != 0) {
        return rc;
    }
    if (v->ops->mkdir == NULL) {
        return -30;                     /* -EROFS */
    }
    if (c != NULL) {
        fs_node_t existing;

        if (fs_lookup(abs_path, &existing) == 0) {
            return -17;                 /* -EEXIST, before any permission
                                         * question - see fs_may_add_entry */
        }
        rc = fs_may_add_entry(abs_path, c, ACE_ADD_SUBDIRECTORY);
        if (rc != 0) {
            return rc;
        }
    }
    return v->ops->mkdir(v, rel, c, mode & (0777u | S_ISVTX));
}

int fs_statfs(const char *abs_path, fs_statfs_t *out) {
    fs_volume_t *v;
    const char *rel = NULL;
    fs_node_t node;
    int rc;

    if (abs_path == NULL || out == NULL) {
        return -22;
    }
    rc = volume_ok(abs_path, &v, &rel);
    if (rc != 0) {
        return rc;
    }

    /* The path has to EXIST, and checking the volume is not the same thing.
     *
     * volume_ok answers "which volume owns this name", and with one volume
     * mounted at "/" that is every name - including names of files that are
     * not there. So statfs("/no/such/path") described the root volume
     * perfectly and returned success, which is wrong in the way that matters:
     * statfs names a file in order to identify the filesystem it is on, and a
     * name that resolves to nothing identifies nothing. POSIX says ENOENT and
     * a caller probing for a mount point relies on it. */
    rc = fs_lookup(abs_path, &node);
    if (rc != 0) {
        return rc;
    }
    if (v->ops->statfs == NULL) {
        return -38;                     /* -ENOSYS */
    }
    out->block_size  = 0;
    out->blocks      = 0;
    out->blocks_free = 0;
    out->name_max    = 0;
    return v->ops->statfs(v, out);
}

int fs_create(const char *abs_path, const struct cred *c, uint32 mode) {
    fs_volume_t *v;
    const char *rel = NULL;
    int rc;

    if (abs_path == NULL) {
        return -22;
    }
    rc = volume_ok(abs_path, &v, &rel);
    if (rc != 0) {
        return rc;
    }
    if (v->ops->create == NULL) {
        return -30;                     /* -EROFS */
    }
    if (c != NULL) {
        fs_node_t existing;

        if (fs_lookup(abs_path, &existing) == 0) {
            return -17;                 /* -EEXIST, before any permission
                                         * question - see fs_may_add_entry */
        }
        rc = fs_may_add_entry(abs_path, c, ACE_ADD_FILE);
        if (rc != 0) {
            return rc;
        }
    }
    return v->ops->create(v, rel, c, mode & 0777u);
}

int fs_truncate(fs_node_t *n, uint64 size) {
    if (n == NULL || n->vol == NULL || n->vol->ops == NULL) {
        return -5;
    }
    if (n->is_dir) {
        return -21;                     /* -EISDIR */
    }
    if (n->vol->ops->truncate == NULL) {
        return -30;                     /* -EROFS */
    }
    /* The WHOLE file, not the truncated range. A shrink frees clusters and a
     * grow zero-fills, so the mapping from offset to disk block can change
     * anywhere - and this call does not know how big the file was, which is
     * precisely the range that has to go. pcache_invalidate takes max = 0 to
     * mean exactly that. */
    {
        int rc = n->vol->ops->truncate(n->vol, n, size);

        if (rc == 0) {
            pcache_invalidate(n, 0, 0);
        }
        return rc;
    }
}

int fs_truncate_at(const char *abs_path, uint64 size) {
    fs_node_t node;
    int rc;

    rc = fs_lookup(abs_path, &node);
    if (rc != 0) {
        return rc;
    }
    return fs_truncate(&node, size);
}

int fs_rmdir(const char *abs_path, const struct cred *c) {
    fs_volume_t *v;
    const char *rel = NULL;
    int rc;

    if (abs_path == NULL) {
        return -22;
    }
    rc = volume_ok(abs_path, &v, &rel);
    if (rc != 0) {
        return rc;
    }
    if (v->ops->rmdir == NULL) {
        return -30;
    }
    /* Refusing to unmount-by-rmdir. A mount point is a directory belonging
     * to the PARENT volume, and removing it while something is mounted there
     * leaves the mount table pointing at a name that no longer exists. */
    if (fs_is_mount_point(abs_path)) {
        return -16;                     /* -EBUSY */
    }
    if (c != NULL) {
        fs_node_t victim;

        rc = fs_lookup(abs_path, &victim);
        if (rc != 0) {
            return rc;
        }
        rc = fs_may_remove_entry(abs_path, &victim, c);
        if (rc != 0) {
            return rc;
        }
    }
    return v->ops->rmdir(v, rel);
}

int fs_unlink(const char *abs_path, const struct cred *c) {
    fs_volume_t *v;
    const char *rel = NULL;
    int rc;

    if (abs_path == NULL) {
        return -22;
    }
    rc = volume_ok(abs_path, &v, &rel);
    if (rc != 0) {
        return rc;
    }
    if (v->ops->unlink == NULL) {
        return -30;
    }
    /* Resolve BEFORE unlinking, purely to learn the inode whose pages have to
     * go. After the unlink there is nothing left to look up, and pages keyed
     * on an inode number that has been freed are worse than stale - FAT
     * reuses directory slots, so the next file created can inherit them and
     * read the deleted file's contents. */
    {
        fs_node_t doomed;
        int found = (fs_lookup(abs_path, &doomed) == 0);
        int have  = (found && !doomed.is_dir);
        int urc;

        if (c != NULL) {
            if (!found) {
                return -2;              /* -ENOENT: nothing there to be
                                         * allowed to remove */
            }
            rc = fs_may_remove_entry(abs_path, &doomed, c);
            if (rc != 0) {
                return rc;
            }
        }
        urc = v->ops->unlink(v, rel);

        if (urc == 0 && have) {
            pcache_invalidate(&doomed, 0, 0);
        }
        return urc;
    }
}

int fs_rename(const char *old_path, const char *new_path,
              const struct cred *c) {
    fs_volume_t *vo, *vn;
    const char *rel_old = NULL, *rel_new = NULL;
    int rc;

    if (old_path == NULL || new_path == NULL) {
        return -22;
    }
    rc = volume_ok(old_path, &vo, &rel_old);
    if (rc != 0) {
        return rc;
    }
    rc = volume_ok(new_path, &vn, &rel_new);
    if (rc != 0) {
        return rc;
    }
    /* -EXDEV, not a silent copy. POSIX says rename does not move data
     * between filesystems, and a caller told it succeeded would skip the
     * copy-and-delete it actually has to do. */
    if (vo != vn) {
        return -18;                     /* -EXDEV */
    }
    if (vo->ops->rename == NULL) {
        return -30;
    }
    /* A rename is a removal from one directory and an addition to another,
     * and is gated as both - plus a removal of whatever it replaces, since
     * replacing a file the caller may not delete must not be a way to
     * delete it. */
    if (c != NULL) {
        fs_node_t src, dst;

        rc = fs_lookup(old_path, &src);
        if (rc != 0) {
            return rc;
        }
        rc = fs_may_remove_entry(old_path, &src, c);
        if (rc != 0) {
            return rc;
        }
        rc = fs_may_add_entry(new_path, c, src.is_dir ? ACE_ADD_SUBDIRECTORY
                                                      : ACE_ADD_FILE);
        if (rc != 0) {
            return rc;
        }
        if (fs_lookup(new_path, &dst) == 0) {
            rc = fs_may_remove_entry(new_path, &dst, c);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return vo->ops->rename(vo, rel_old, rel_new);
}

int fs_lookup_on(fs_volume_t *v, const char *rel, fs_node_t *out) {
    int rc;

    if (v == NULL || rel == NULL || out == NULL) {
        return -22;
    }
    if (!v->mounted || v->ops == NULL || v->ops->lookup == NULL) {
        return -19;                     /* -ENODEV */
    }
    rc = v->ops->lookup(v, rel, out);
    if (rc == 0) {
        /* Stamped here too, and this is the reason it is stamped in the
         * wrapper rather than by each filesystem: there are two entry points
         * now and a filesystem that set it itself would have to be right in
         * both. */
        out->vol = v;
    }
    return rc;
}

/* --- access control -------------------------------------------------------
 *
 * Two functions, and the split between them is the whole design: fs_getacl
 * answers "what does this object say", fs_access answers "may this caller do
 * this". Nothing above the VFS reads an ACL in order to decide something -
 * they call fs_access - so there is exactly one implementation of the
 * decision and it cannot drift between open(2) and access(2).
 */

int fs_getacl(const fs_node_t *n, struct acl *out) {
    acl_t *a = (acl_t *)out;
    int rc;

    if (n == NULL || a == NULL) {
        return -22;                            /* -EINVAL */
    }
    /* The same guard fs_read uses, and deliberately not a mount-table check.
     * fs_volume_is_mounted asks whether the volume appears in the mount
     * table, which is a different question: a volume can be legitimately in
     * use through fs_lookup_on before it is ever mounted at a path, and the
     * host suite does exactly that. Staleness after an unmount is caught the
     * way every other operation catches it, by the volume's own `mounted`
     * flag turning reads into -ENODEV. */
    if (n->vol->ops == NULL) {
        return -5;                             /* -EIO */
    }

    if (n->vol->ops != NULL && n->vol->ops->getacl != NULL) {
        rc = n->vol->ops->getacl((fs_volume_t *)n->vol, n, out);
        if (rc == 0) {
            return 0;
        }
        /* -ENOENT is "this object has no stored ACL", which is an ordinary
         * answer and not a failure - a pre-v5 ZFS filesystem, or an object
         * whose ZFS_ACL_TRIVIAL flag says the stored ACL adds nothing to the
         * mode. Fall through to the projection.
         *
         * Every OTHER error is returned. That distinction is load-bearing: a
         * corrupt ACL (-EIO), one too large to hold (-E2BIG), or a layout
         * this reader cannot size (-ENOTSUP) must NOT quietly become "use the
         * mode bits instead". The mode bits on such a file are exactly the
         * thing that is known not to tell the whole story, so projecting them
         * would answer a security question with the data the ACL was there to
         * override - and it would grant, not deny. */
        if (rc != -2) {
            return rc;
        }
    }

    acl_from_mode(n->mode, n->uid, n->gid, a);
    return 0;
}

int fs_access(const fs_node_t *n, const struct cred *c, uint32 wanted) {
    acl_t a;
    int rc;

    if (n == NULL || c == NULL) {
        return -22;
    }
    rc = fs_getacl(n, (struct acl *)&a);
    if (rc != 0) {
        return rc;
    }

    /* The volume being read-only is a property of the MEDIUM, not of the
     * ACL, and an ACL cannot express it. Checked here so that both callers
     * get it: an ACL granting write on a read-only mount must still refuse,
     * or access(W_OK) says yes and the write then fails with -EROFS, which is
     * the exact "check before writing" pattern faccessat exists to serve. */
    if ((wanted & (ACE_WRITE_DATA | ACE_APPEND_DATA | ACE_DELETE_CHILD |
                   ACE_WRITE_ATTRIBUTES | ACE_WRITE_ACL | ACE_WRITE_OWNER)) &&
        !fs_writable_vol(n->vol)) {
        return -13;                            /* -EACCES */
    }

    return acl_access(&a, (const cred_t *)c, wanted);
}

/* Replace `n`'s ACL with `a`, gated the same way any other mutation is: the
 * caller must hold ACE_WRITE_ACL (fs_access already refuses that bit outright
 * on a read-only volume, which is exactly right here too - an ACL nobody can
 * persist is not a real one), and the filesystem must implement the slot.
 *
 * Deliberately not the entry point that computes a NEW acl_t - a chmod(2)
 * caller builds one with acl_apply_chmod first, a create(2) path with
 * acl_inherit, and both call this to make it durable. Keeping that split
 * means this function never has to know which shape of change is being
 * made, only that the caller was allowed to make it. */
static int fs_setacl_special(fs_node_t *n, const struct cred *c,
                             const struct acl *a, uint32 special) {
    int rc;

    if (n == NULL || c == NULL || a == NULL) {
        return -22;                            /* -EINVAL */
    }
    rc = fs_access(n, c, ACE_WRITE_ACL);
    if (rc != 0) {
        return rc;
    }
    if (n->vol == NULL || n->vol->ops == NULL || n->vol->ops->setacl == NULL) {
        return -30;                            /* -EROFS: the same errno a
                                               * filesystem with no write
                                               * slot at all already answers */
    }
    return n->vol->ops->setacl(n->vol, n, a, special);
}

int fs_setacl(fs_node_t *n, const struct cred *c, const struct acl *a) {
    return fs_setacl_special(n, c, a, FS_SPECIAL_KEEP);
}

/* chmod(2) in full: the rwx half through acl_apply_chmod (rewriting only
 * owner@/group@/everyone@, leaving named and inherited entries alone), the
 * special half through acl_chmod_mode, and both in ONE setacl call so a
 * chmod is never half-applied. Before this, the special bits could not be
 * changed at all - the filesystem carried them over from the old mode. */
int fs_chmod(fs_node_t *n, const struct cred *c, uint32 mode) {
    acl_t old_acl, new_acl;
    uint32 full;
    int rc;

    if (n == NULL || c == NULL) {
        return -22;                            /* -EINVAL */
    }
    rc = fs_getacl(n, (struct acl *)&old_acl);
    if (rc != 0) {
        return rc;
    }
    full = acl_chmod_mode(&old_acl, (const cred_t *)c, n->mode, mode);
    acl_apply_chmod(&old_acl, full, &new_acl);
    return fs_setacl_special(n, c, (const struct acl *)&new_acl,
                             full & (S_ISUID | S_ISGID | S_ISVTX));
}

/* chown's counterpart to fs_setacl, and deliberately NOT built on it:
 * fs_setacl's gate is ACE_WRITE_ACL, which every owner holds, and chown must
 * not be something every owner can do. The decision is acl_chown_permitted's
 * (kernel/fs/acl.c), made against the same effective ACL fs_access reads.
 *
 * The read-only check comes first and answers -EROFS, not -EPERM: that is
 * both what Linux answers and the more useful thing to be told - no caller,
 * however privileged, could have made this change on this medium. */
int fs_setowner(fs_node_t *n, const struct cred *c, uint32 uid, uint32 gid) {
    acl_t a;
    int rc;

    if (n == NULL || c == NULL) {
        return -22;                            /* -EINVAL */
    }
    rc = fs_getacl(n, (struct acl *)&a);
    if (rc != 0) {
        return rc;
    }
    if (!fs_writable_vol(n->vol)) {
        return -30;                            /* -EROFS */
    }
    rc = acl_chown_permitted(&a, (const cred_t *)c, uid, gid);
    if (rc != 0) {
        return rc;
    }
    if (uid == ACL_CHOWN_KEEP) {
        uid = a.owner;
    }
    if (gid == ACL_CHOWN_KEEP) {
        gid = a.group;
    }
    if (uid == a.owner && gid == a.group) {
        return 0;                              /* nothing to write */
    }
    if (n->vol->ops->setowner == NULL) {
        return -30;                            /* -EROFS, as fs_setacl */
    }
    return n->vol->ops->setowner(n->vol, n, uid, gid);
}

int fs_writable_vol(const fs_volume_t *v) {
    if (v == NULL || !v->mounted || v->ops == NULL) {
        return 0;
    }
    return v->ops->write != NULL;
}

int64 fs_read(const fs_node_t *n, uint64 offset, void *buf, uint64 max) {
    if (n == NULL || n->vol == NULL || n->vol->ops == NULL) {
        return -5;                      /* -EIO */
    }
    if (n->is_dir) {
        /* -EISDIR is a specific answer a libc uses to tell "you opened a
         * directory" apart from "this cannot be read at all". Answered here
         * so every filesystem does not have to remember to. */
        return -21;
    }
    if (n->vol->ops->read == NULL) {
        return -22;
    }
    /* Through the node's OWN volume, not the root. That is the whole reason
     * fs_node_t carries a volume pointer, and it is what makes a second
     * mounted volume work without this function changing.
     *
     * And through the PAGE CACHE, which calls that same slot on a miss.
     * Placed here rather than in each filesystem for the reason the vtable
     * exists at all: a second filesystem should not have to remember to
     * cache, and two caches would be two things to invalidate. */
    return pcache_read(n, offset, buf, max);
}

int64 fs_write(fs_node_t *n, uint64 offset, const void *buf, uint64 max) {
    if (n == NULL || n->vol == NULL || n->vol->ops == NULL) {
        return -5;
    }
    if (n->is_dir) {
        return -21;
    }
    if (n->vol->ops->write == NULL) {
        /* -EROFS, not -EBADF or -EINVAL. The descriptor is valid and the
         * request is meaningful; the filesystem is read-only. A caller that
         * gets -EBADF concludes it has a bug, which sends you looking in the
         * wrong place. */
        return -30;
    }
    /* WRITE-THROUGH, then drop the pages this touched.
     *
     * Invalidate AFTER the write rather than before, and the order is the
     * bug that order prevents: dropping first leaves a window in which a
     * concurrent read re-populates the page from the OLD disk contents and
     * the write then lands underneath it, so the cache holds stale bytes for
     * a file that was written successfully. Dropping after means the worst
     * case is a re-read.
     *
     * The range is the range WRITTEN, not the whole file - a large file
     * appended to one line at a time should not lose its head every time. */
    {
        int64 got = n->vol->ops->write(n->vol, n, offset, buf, max);

        if (got > 0) {
            pcache_invalidate(n, offset, (uint64)got);
        }
        return got;
    }
}

int fs_iterate(const fs_node_t *dir, fs_dir_cb cb, void *ctx) {
    if (dir == NULL || dir->vol == NULL || dir->vol->ops == NULL) {
        return -5;
    }
    if (!dir->is_dir) {
        return -20;                     /* -ENOTDIR */
    }
    if (dir->vol->ops->iterate == NULL) {
        return -22;
    }
    return dir->vol->ops->iterate(dir->vol, dir, cb, ctx);
}

int fs_writable(const char *abs_path) {
    fs_volume_t *v;
    const char *rel = NULL;

    if (volume_ok(abs_path, &v, &rel) != 0) {
        return 0;
    }
    return v->ops->write != NULL;
}

/* --- whole-file reads ----------------------------------------------------
 *
 * Two passes: size it, allocate, fill it. The size comes from the node rather
 * than from a first read, because a read that returns short at end of file is
 * indistinguishable from one that was truncated by the buffer - and sizing by
 * reading until short means reading the file twice for no reason. */
int fs_read_whole(const char *abs_path, uint8 **out, uint32 *size) {
    fs_node_t node;
    uint8 *buf;
    int64 got;
    int rc;

    if (out == NULL || size == NULL) {
        return -22;
    }
    rc = fs_lookup(abs_path, &node);
    if (rc != 0) {
        return rc;
    }
    if (node.is_dir) {
        return -21;                     /* -EISDIR */
    }
    if (node.size == 0) {
        /* An empty file is not an error, but there is nothing to execute in
         * it either, and every caller of this wants an image. -ENOEXEC says
         * which of the two it is; returning a zero-length buffer would push
         * the decision onto a caller that has less to go on. */
        return -8;
    }
    if (node.size > 0x7FFFFFFFULL) {
        return -27;                     /* -EFBIG */
    }

    buf = (uint8 *)kmalloc((uint32)node.size);
    if (buf == NULL) {
        return -12;                     /* -ENOMEM */
    }
    got = fs_read(&node, 0, buf, node.size);
    if (got < 0) {
        kfree(buf);
        return (int)got;
    }
    if ((uint64)got != node.size) {
        /* A short read of a file whose size the directory entry just stated
         * is a corrupt volume, not a normal end of file. Reporting it as
         * success with a truncated image would hand the ELF loader half a
         * binary, and the failure would surface as a bad program header. */
        kfree(buf);
        return -5;                      /* -EIO */
    }

    *out  = buf;
    *size = (uint32)node.size;
    return 0;
}

void fs_free_file(uint8 *buf) {
    kfree(buf);
}
