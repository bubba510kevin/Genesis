/* tmpfs - see tmpfs.h.
 *
 * A tree of nodes. A file's bytes are in whole pages from the PMM, found
 * through a growable array of page pointers - no large contiguous heap
 * allocation, and a sparse file's untouched pages are never allocated (they
 * read as zeros). The fs_node_t handed to the VFS carries the node pointer
 * in its private area; `opens` counts the open objects holding it
 * (node_hold/node_release), so an unlinked node is freed by whichever comes
 * last, the unlink or the close. */

#include "tmpfs.h"
#include "acl.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "paging.h"
#include "pmm.h"
#include "timer.h"

#define TMP_NAME_MAX  255

typedef struct tnode {
    struct tnode *parent;
    struct tnode *child;         /* first entry, directories only */
    struct tnode *next;          /* sibling */
    char          name[TMP_NAME_MAX + 1];
    int           is_dir;
    uint32        mode;          /* type and permission bits */
    uint32        uid, gid;
    uint64        ino;
    uint64        size;
    uint8       **pages;         /* page i holds bytes [i*4096, ...) */
    uint64        npages;        /* entries in `pages` */
    int           opens;
    int           unlinked;
} tnode_t;

typedef struct {
    tnode_t *root;
    uint64   next_ino;
    uint64   bytes;              /* pages in use, in bytes */
    uint64   limit;
} tmpfs_t;

static const fs_ops_t tmpfs_ops;

static tnode_t *node_of(const fs_node_t *n) {
    return *(tnode_t *const *)n->priv;
}

static void fill_node(fs_volume_t *v, tnode_t *t, fs_node_t *out) {
    out->vol = v;
    out->size = t->is_dir ? 0 : t->size;
    out->ino = t->ino;
    out->is_dir = t->is_dir;
    out->readonly = 0;
    out->mode = t->mode;
    out->uid = t->uid;
    out->gid = t->gid;
    *(tnode_t **)out->priv = t;
}

static int name_eq(const char *a, const char *b, uint64 n) {
    uint64 i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return a[n] == '\0';
}

/* Walk an absolute, normalized path. With `parent_out`, stop one short:
 * the parent directory and the last component. */
static int walk(tmpfs_t *fs, const char *path, tnode_t **out,
                const char **last, uint64 *last_len) {
    tnode_t *cur = fs->root;
    const char *p = path;

    while (*p == '/') {
        p++;
    }
    while (*p != '\0') {
        const char *e = p;
        tnode_t *c;
        uint64 n;

        while (*e != '\0' && *e != '/') {
            e++;
        }
        n = (uint64)(e - p);
        if (last != NULL) {
            const char *q = e;

            while (*q == '/') {
                q++;
            }
            if (*q == '\0') {           /* p..e is the last component */
                *last = p;
                *last_len = n;
                *out = cur;
                return 0;
            }
        }
        if (!cur->is_dir) {
            return -20;                 /* -ENOTDIR */
        }
        for (c = cur->child; c != NULL; c = c->next) {
            if (name_eq(c->name, p, n)) {
                break;
            }
        }
        if (c == NULL) {
            return -2;
        }
        cur = c;
        p = e;
        while (*p == '/') {
            p++;
        }
    }
    if (last != NULL) {
        return -22;                     /* the root has no parent */
    }
    *out = cur;
    return 0;
}

static tnode_t *find_child(tnode_t *dir, const char *name, uint64 n) {
    tnode_t *c;

    for (c = dir->child; c != NULL; c = c->next) {
        if (name_eq(c->name, name, n)) {
            return c;
        }
    }
    return NULL;
}

static void free_pages(tmpfs_t *fs, tnode_t *t, uint64 from_page) {
    uint64 i;

    for (i = from_page; i < t->npages; i++) {
        if (t->pages[i] != NULL) {
            pmm_free_frame((phys_addr_t)((uint64)t->pages[i] - PHYSMAP_BASE));
            t->pages[i] = NULL;
            fs->bytes -= 4096;
        }
    }
}

static void node_free(tmpfs_t *fs, tnode_t *t) {
    free_pages(fs, t, 0);
    if (t->pages != NULL) {
        kfree(t->pages);
    }
    kfree(t);
}

/* Off the tree; freed now unless an open object still holds it. */
static void detach(tmpfs_t *fs, tnode_t *t) {
    tnode_t **at;

    for (at = &t->parent->child; *at != NULL; at = &(*at)->next) {
        if (*at == t) {
            *at = t->next;
            break;
        }
    }
    t->parent = NULL;
    t->next = NULL;
    t->unlinked = 1;
    if (t->opens == 0) {
        node_free(fs, t);
    }
}

static tnode_t *make_node(tmpfs_t *fs, tnode_t *dir, const char *name,
                          uint64 n, int is_dir, const struct cred *c,
                          uint32 mode) {
    tnode_t *t;
    uint64 i;

    if (n == 0 || n > TMP_NAME_MAX) {
        return NULL;
    }
    t = (tnode_t *)kcalloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        t->name[i] = name[i];
    }
    t->name[n] = '\0';
    t->is_dir = is_dir;
    t->mode = (is_dir ? S_IFDIR : S_IFREG) | (mode & 07777u);
    t->uid = c != NULL ? ((const cred_t *)c)->euid : 0;
    t->gid = c != NULL ? ((const cred_t *)c)->egid : 0;
    t->ino = fs->next_ino++;
    t->parent = dir;
    t->next = dir->child;
    dir->child = t;
    return t;
}

/* --- the ops ------------------------------------------------------------- */

static int tmp_lookup(fs_volume_t *v, const char *path, fs_node_t *out) {
    tnode_t *t;
    int rc = walk((tmpfs_t *)v->body, path, &t, NULL, NULL);

    if (rc != 0) {
        return rc;
    }
    fill_node(v, t, out);
    return 0;
}

static int64 tmp_read(fs_volume_t *v, const fs_node_t *n, uint64 off,
                      void *buf, uint64 max) {
    tnode_t *t = node_of(n);
    uint8 *dst = (uint8 *)buf;
    uint64 done = 0;

    (void)v;
    if (t->is_dir) {
        return -21;
    }
    if (off >= t->size) {
        return 0;
    }
    if (max > t->size - off) {
        max = t->size - off;
    }
    while (done < max) {
        uint64 pos = off + done, pg = pos / 4096, po = pos % 4096;
        uint64 chunk = 4096 - po, i;
        const uint8 *src = (pg < t->npages) ? t->pages[pg] : NULL;

        if (chunk > max - done) {
            chunk = max - done;
        }
        for (i = 0; i < chunk; i++) {
            dst[done + i] = src != NULL ? src[po + i] : 0;   /* a hole */
        }
        done += chunk;
    }
    return (int64)done;
}

static int grow_index(tnode_t *t, uint64 pages) {
    uint8 **np;
    uint64 cap, i;

    if (pages <= t->npages) {
        return 0;
    }
    cap = t->npages ? t->npages : 4;
    while (cap < pages) {
        cap *= 2;
    }
    np = (uint8 **)kcalloc(cap, sizeof(*np));
    if (np == NULL) {
        return -28;
    }
    for (i = 0; i < t->npages; i++) {
        np[i] = t->pages[i];
    }
    if (t->pages != NULL) {
        kfree(t->pages);
    }
    t->pages = np;
    t->npages = cap;
    return 0;
}

static int64 tmp_write(fs_volume_t *v, fs_node_t *n, uint64 off,
                       const void *buf, uint64 max) {
    tmpfs_t *fs = (tmpfs_t *)v->body;
    tnode_t *t = node_of(n);
    const uint8 *src = (const uint8 *)buf;
    uint64 done = 0;

    if (t->is_dir) {
        return -21;
    }
    if (max == 0) {
        return 0;
    }
    if (grow_index(t, (off + max + 4095) / 4096) != 0) {
        return -28;
    }
    while (done < max) {
        uint64 pos = off + done, pg = pos / 4096, po = pos % 4096;
        uint64 chunk = 4096 - po, i;

        if (chunk > max - done) {
            chunk = max - done;
        }
        if (t->pages[pg] == NULL) {
            phys_addr_t f;

            if (fs->bytes + 4096 > fs->limit ||
                (f = pmm_alloc_frame()) == 0) {
                break;                   /* -ENOSPC, or a short write */
            }
            t->pages[pg] = (uint8 *)phys_to_virt(f);
            for (i = 0; i < 4096; i++) {
                t->pages[pg][i] = 0;
            }
            fs->bytes += 4096;
        }
        for (i = 0; i < chunk; i++) {
            t->pages[pg][po + i] = src[done + i];
        }
        done += chunk;
    }
    if (done == 0) {
        return -28;                      /* -ENOSPC */
    }
    if (off + done > t->size) {
        t->size = off + done;
    }
    n->size = t->size;
    return (int64)done;
}

static int tmp_iterate(fs_volume_t *v, const fs_node_t *dir, fs_dir_cb cb,
                       void *ctx) {
    tnode_t *d = node_of(dir), *c;

    (void)v;
    if (!d->is_dir) {
        return -20;
    }
    for (c = d->child; c != NULL; c = c->next) {
        fs_dirent_t e;
        uint64 i;
        int rc;

        for (i = 0; c->name[i] != '\0'; i++) {
            e.name[i] = c->name[i];
        }
        e.name[i] = '\0';
        e.ino = c->ino;
        e.is_dir = c->is_dir;
        rc = cb(&e, ctx);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int tmp_statfs(fs_volume_t *v, fs_statfs_t *out) {
    tmpfs_t *fs = (tmpfs_t *)v->body;

    out->block_size = 4096;
    out->blocks = fs->limit / 4096;
    out->blocks_free = (fs->limit - fs->bytes) / 4096;
    out->name_max = TMP_NAME_MAX;
    return 0;
}

static int tmp_create_common(fs_volume_t *v, const char *path,
                             const struct cred *c, uint32 mode, int is_dir) {
    tmpfs_t *fs = (tmpfs_t *)v->body;
    tnode_t *dir;
    const char *name;
    uint64 n;
    int rc = walk(fs, path, &dir, &name, &n);

    if (rc != 0) {
        return rc;
    }
    if (!dir->is_dir) {
        return -20;
    }
    if (n > TMP_NAME_MAX) {
        return -36;
    }
    if (find_child(dir, name, n) != NULL) {
        return -17;
    }
    return make_node(fs, dir, name, n, is_dir, c, mode) != NULL ? 0 : -28;
}

static int tmp_create(fs_volume_t *v, const char *path, const struct cred *c,
                      uint32 mode) {
    return tmp_create_common(v, path, c, mode, 0);
}

static int tmp_mkdir(fs_volume_t *v, const char *path, const struct cred *c,
                     uint32 mode) {
    return tmp_create_common(v, path, c, mode, 1);
}

static int tmp_truncate(fs_volume_t *v, fs_node_t *n, uint64 size) {
    tmpfs_t *fs = (tmpfs_t *)v->body;
    tnode_t *t = node_of(n);

    if (t->is_dir) {
        return -21;
    }
    if (size < t->size) {
        uint64 keep = (size + 4095) / 4096;

        free_pages(fs, t, keep);
        /* The tail of the last kept page must read as zero if the file
         * grows again. */
        if (size % 4096 != 0 && keep - 1 < t->npages &&
            t->pages[keep - 1] != NULL) {
            uint64 i;

            for (i = size % 4096; i < 4096; i++) {
                t->pages[keep - 1][i] = 0;
            }
        }
    } else if (size / 4096 > fs->limit / 4096) {
        return -27;                      /* -EFBIG */
    }
    /* Growing: the pages stay unallocated, a hole that reads as zeros. */
    t->size = size;
    n->size = size;
    return 0;
}

static int tmp_remove(fs_volume_t *v, const char *path, int want_dir) {
    tmpfs_t *fs = (tmpfs_t *)v->body;
    tnode_t *dir, *t;
    const char *name;
    uint64 n;
    int rc = walk(fs, path, &dir, &name, &n);

    if (rc != 0) {
        return rc;
    }
    t = dir->is_dir ? find_child(dir, name, n) : NULL;
    if (t == NULL) {
        return -2;
    }
    if (want_dir && !t->is_dir) {
        return -20;
    }
    if (!want_dir && t->is_dir) {
        return -21;
    }
    if (want_dir && t->child != NULL) {
        return -39;                      /* -ENOTEMPTY */
    }
    detach(fs, t);
    return 0;
}

static int tmp_rmdir(fs_volume_t *v, const char *path) {
    return tmp_remove(v, path, 1);
}

static int tmp_unlink(fs_volume_t *v, const char *path) {
    return tmp_remove(v, path, 0);
}

static int tmp_rename(fs_volume_t *v, const char *from, const char *to) {
    tmpfs_t *fs = (tmpfs_t *)v->body;
    tnode_t *fdir, *tdir, *t, *victim, *a, **at;
    const char *fname, *tname;
    uint64 fn, tn, i;
    int rc;

    rc = walk(fs, from, &fdir, &fname, &fn);
    if (rc != 0) {
        return rc;
    }
    rc = walk(fs, to, &tdir, &tname, &tn);
    if (rc != 0) {
        return rc;
    }
    t = fdir->is_dir ? find_child(fdir, fname, fn) : NULL;
    if (t == NULL) {
        return -2;
    }
    if (!tdir->is_dir) {
        return -20;
    }
    if (tn > TMP_NAME_MAX) {
        return -36;
    }
    /* A directory may not move inside itself. */
    for (a = tdir; a != NULL; a = a->parent) {
        if (a == t) {
            return -22;
        }
    }
    victim = find_child(tdir, tname, tn);
    if (victim == t) {
        return 0;
    }
    if (victim != NULL) {
        if (victim->is_dir != t->is_dir) {
            return victim->is_dir ? -21 : -20;
        }
        if (victim->is_dir && victim->child != NULL) {
            return -39;
        }
        detach(fs, victim);              /* replaced, as rename(2) does */
    }
    for (at = &fdir->child; *at != NULL; at = &(*at)->next) {
        if (*at == t) {
            *at = t->next;
            break;
        }
    }
    for (i = 0; i < tn; i++) {
        t->name[i] = tname[i];
    }
    t->name[tn] = '\0';
    t->parent = tdir;
    t->next = tdir->child;
    tdir->child = t;
    return 0;
}

static int tmp_setacl(fs_volume_t *v, fs_node_t *n, const struct acl *a,
                      uint32 special) {
    tnode_t *t = node_of(n);
    uint32 m;

    (void)v;
    m = acl_to_mode((const acl_t *)a, t->mode & S_IFMT);
    if (special != FS_SPECIAL_KEEP) {
        m = (m & ~07000u) | (special & 07000u);
    } else {
        m = (m & ~07000u) | (t->mode & 07000u);
    }
    t->mode = m;
    n->mode = m;
    return 0;
}

static int tmp_setowner(fs_volume_t *v, fs_node_t *n, uint32 uid,
                        uint32 gid) {
    tnode_t *t = node_of(n);

    (void)v;
    t->uid = uid;
    t->gid = gid;
    n->uid = uid;
    n->gid = gid;
    return 0;
}

static void tmp_hold(fs_volume_t *v, fs_node_t *n) {
    (void)v;
    node_of(n)->opens++;
}

static void tmp_release(fs_volume_t *v, fs_node_t *n) {
    tnode_t *t = node_of(n);

    if (--t->opens == 0 && t->unlinked) {
        node_free((tmpfs_t *)v->body, t);
    }
}

static const fs_ops_t tmpfs_ops = {
    .name         = "tmpfs",
    .lookup       = tmp_lookup,
    .read         = tmp_read,
    .write        = tmp_write,
    .iterate      = tmp_iterate,
    .statfs       = tmp_statfs,
    .create       = tmp_create,
    .truncate     = tmp_truncate,
    .mkdir        = tmp_mkdir,
    .rmdir        = tmp_rmdir,
    .unlink       = tmp_unlink,
    .rename       = tmp_rename,
    .setacl       = tmp_setacl,
    .setowner     = tmp_setowner,
    .node_hold    = tmp_hold,
    .node_release = tmp_release,
};

fs_volume_t *tmpfs_create(void) {
    fs_volume_t *v = (fs_volume_t *)kcalloc(1, sizeof(*v));
    tmpfs_t *fs = (tmpfs_t *)kcalloc(1, sizeof(*fs));
    tnode_t *root = (tnode_t *)kcalloc(1, sizeof(*root));

    if (v == NULL || fs == NULL || root == NULL) {
        return NULL;
    }
    root->is_dir = 1;
    root->mode = S_IFDIR | 01777u;       /* sticky, world-writable: /tmp */
    root->ino = 1;
    fs->root = root;
    fs->next_ino = 2;
    fs->limit = pmm_total_frames() * 4096ULL / 4;
    v->ops = &tmpfs_ops;
    v->body = fs;
    v->block_size = 4096;
    v->mounted = 1;                      /* live: the VFS checks this */
    return v;
}

void tmpfs_init(void) {
    fs_volume_t *v = tmpfs_create();
    int rc;

    if (v == NULL) {
        kprintf_c(0x0C, "tmpfs: out of memory\n");
        return;
    }
    /* The mount point exists on the root volume too, so `ls /` shows it -
     * best effort, since the root may be read-only. */
    (void)fs_mkdir("/tmp", NULL, 01777u);
    rc = fs_mount_at("/tmp", v);
    if (rc != 0) {
        kprintf_c(0x0C, "tmpfs: mounting /tmp failed (%d)\n", rc);
    }
}
