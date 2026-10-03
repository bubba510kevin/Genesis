#include "fileobj.h"
#include "fs.h"
#include "object.h"
#include "typesk.h"

/* The node a file object describes. A static pool for the same reasons
 * object.c uses one: no heap on the teardown path, and exhaustion arrives as
 * a specific limit rather than as heap pressure somewhere unrelated.
 *
 * fs_node_t rather than fat_entry_t. That single type change is most of what
 * made this file filesystem-independent: everything below asks the node what
 * it is instead of reading an attribute byte out of a FAT directory entry. */
static fs_node_t body_pool[MAX_OBJECTS];
static int       body_used[MAX_OBJECTS];

static fs_node_t *body_alloc(void) {
    int i;
    for (i = 0; i < MAX_OBJECTS; i++) {
        if (!body_used[i]) {
            body_used[i] = 1;
            return &body_pool[i];
        }
    }
    return NULL;
}

static void body_free(fs_node_t *e) {
    int i;
    for (i = 0; i < MAX_OBJECTS; i++) {
        if (&body_pool[i] == e) {
            body_used[i] = 0;
        }
    }
}

static int64 file_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    fs_node_t *node = (fs_node_t *)obj->body;
    int64 got;

    if (node == NULL) {
        return -5;                          /* -EIO */
    }
    /* The -EISDIR check and the volume check both moved into fs_read. This
     * function no longer knows what a directory attribute bit looks like,
     * which is the point of the change that introduced fs_node_t. */
    got = fs_read(node, *offset, buf, n);
    if (got < 0) {
        return got;
    }
    /* Advancing the position here rather than in the caller is what makes
     * two descriptors from dup share progress through the file. */
    *offset += (uint64)got;
    return got;
}

static int64 file_write(object_t *obj, const void *buf, uint64 n, uint64 *offset) {
    fs_node_t *node = (fs_node_t *)obj->body;
    int64 done;

    if (node == NULL) {
        return -5;
    }
    /* Through the vtable, which answers -EROFS when the filesystem has no
     * write operation. That used to be a constant here, and a constant is
     * exactly what would still be here on the day the volume became writable
     * - a file that could be written and a write path that refused. */
    done = fs_write(node, *offset, buf, n);
    if (done > 0) {
        *offset += (uint64)done;
    }
    return done;
}

static void file_destroy(object_t *obj) {
    fs_node_t *n = (fs_node_t *)obj->body;

    if (n != NULL && n->vol != NULL && n->vol->ops != NULL &&
        n->vol->ops->node_release != NULL) {
        n->vol->ops->node_release(n->vol, n);
    }
    body_free(n);
}

/* Designated initialisers throughout, so that adding a slot to
 * object_type_t cannot silently rebind an existing one - which is exactly
 * what happened when getdents was inserted ahead of destroy and every
 * positional initialiser in the kernel started installing its destructor as
 * a directory reader. */
static const object_type_t file_type = {
    .name    = "file",
    .klass   = OBJ_FILE,
    .read    = file_read,
    .write   = file_write,
    .destroy = file_destroy
};

static const object_type_t dir_type = {
    .name     = "directory",
    .klass    = OBJ_DIRECTORY,
    .read     = file_read,
    .write    = file_write,
    .getdents = fileobj_getdents,
    .destroy  = file_destroy
};

/* The half both entry points share: a resolved node becomes an object.
 *
 * Factored out when the second entry point arrived rather than copied, and
 * the reason is the object pool. Two copies of "allocate a body, create an
 * object, free the body if that fails" is two chances to leak a body slot on
 * the failure path - and a leaked body slot presents as the filesystem
 * running out of open files after some number of failed opens, which is
 * about as far from its cause as a bug gets. */
static object_t *node_to_object(const fs_node_t *found, int writable,
                                uint32 access, int *err) {
    fs_node_t *body;
    object_t  *obj;

    /* Refuse write access up front rather than at the first write. open() is
     * where a program expects to learn it cannot modify a file.
     *
     * Asked of the volume rather than assumed. This was an unconditional
     * -EROFS, which is the line that would have kept every file read-only
     * after the filesystem underneath became writable. */
    if ((access & ACCESS_WRITE) != 0 && !writable) {
        *err = -30;                         /* -EROFS */
        return NULL;
    }

    body = body_alloc();
    if (body == NULL) {
        *err = -23;                         /* -ENFILE */
        return NULL;
    }
    *body = *found;

    obj = ob_create(found->is_dir ? &dir_type : &file_type, body);
    if (obj == NULL) {
        body_free(body);
        *err = -23;
        return NULL;
    }
    if (body->vol != NULL && body->vol->ops != NULL &&
        body->vol->ops->node_hold != NULL) {
        body->vol->ops->node_hold(body->vol, body);
    }
    *err = 0;
    return obj;
}

object_t *fileobj_open(const char *abs_path, uint32 access, int *err) {
    fs_node_t found;
    int rc;

    /* One call, one errno. The switch that used to be here translated four
     * FAT_ERR_ values by hand, and it was the second copy of that table -
     * syscall.c had the other. Both are gone; fatfs.c owns the only one. */
    rc = fs_lookup(abs_path, &found);
    if (rc != 0) {
        *err = rc;
        return NULL;
    }
    return node_to_object(&found, fs_writable(abs_path), access, err);
}

/* The same, on a volume already in hand and with no POSIX path at all.
 *
 * What a volume device's parse op calls: by the time \Device\HarddiskVolume2
 * has been resolved and handed "\bin\sh", there is no mount point to route
 * through and there may not be one anywhere. Going back through fileobj_open
 * would mean synthesising a POSIX path for a volume that has none - and if
 * two volumes were both reachable, synthesising one that resolves to the
 * WRONG volume. */
object_t *fileobj_open_on(fs_volume_t *v, const char *rel, uint32 access,
                          int *err) {
    fs_node_t found;
    int rc;

    rc = fs_lookup_on(v, rel, &found);
    if (rc != 0) {
        *err = rc;
        return NULL;
    }
    return node_to_object(&found, fs_writable_vol(v), access, err);
}

int fileobj_is_dir(const object_t *obj) {
    if (obj == NULL || obj->body == NULL) {
        return 0;
    }
    return ((const fs_node_t *)obj->body)->is_dir;
}

/* Is this object one of THIS file's types?
 *
 * The question every accessor here should be asking and mostly does not.
 * fileobj_size below casts obj->body to an fs_node_t on trust, which is
 * correct for a file object and nonsense for a console or a pipe - the same
 * mistake fileobj_is_dir made against the /dev directory object, where it
 * read byte 24 of an ns_entry_t's name and reported -ENOTDIR for `ls /dev`.
 *
 * Answered from the TYPE pointer, which is the only thing that identifies an
 * object without believing what it points at. Exported because do_write needs
 * it: O_APPEND means "seek to the end of the file", and a pipe has no end. */
int fileobj_is_file(const object_t *obj) {
    return obj != NULL && (obj->type == &file_type || obj->type == &dir_type);
}

/* Set the length of the file this object names.
 *
 * Goes through fs_truncate, so a filesystem with no truncate slot answers
 * -EROFS and O_TRUNC on it fails rather than silently opening a file it
 * promised to empty. */
int fileobj_truncate(object_t *obj, uint64 size) {
    fs_node_t *node;

    if (!fileobj_is_file(obj) || obj->body == NULL) {
        return -22;                     /* -EINVAL: not a file */
    }
    node = (fs_node_t *)obj->body;
    return fs_truncate(node, size);
}

/* The node behind a file object, or NULL for anything that is not one.
 *
 * Every other accessor here answers ONE question about the node, which is the
 * right shape while the questions are "how big" and "is it a directory". A
 * security descriptor needs the mode, both ids, the volume and the private
 * area all at once, and adding five more one-question accessors to serve it
 * would be worse than handing back the node.
 *
 * const, and that is the whole safety argument: a caller can read the node
 * but cannot make the object disagree with the filesystem behind it. The
 * pointer is valid only while the caller holds a reference to `obj`. */
const fs_node_t *fileobj_node(const object_t *obj) {
    if (!fileobj_is_file(obj) || obj->body == NULL) {
        return NULL;
    }
    return (const fs_node_t *)obj->body;
}

uint64 fileobj_size(const object_t *obj) {
    const fs_node_t *node;

    if (obj == NULL || obj->body == NULL) {
        return 0;
    }
    node = (const fs_node_t *)obj->body;
    return node->is_dir ? 0 : node->size;
}

/* The inode number, for st_ino. Supplied by the filesystem rather than
 * derived here: FAT uses the first cluster, which is stable for the life of a
 * file with contents, and a filesystem with real inodes will give a real
 * one. */
uint64 fileobj_ino(const object_t *obj) {
    if (obj == NULL || obj->body == NULL) {
        return 0;
    }
    return ((const fs_node_t *)obj->body)->ino;
}

/* --- getdents64 ---------------------------------------------------------
 * The record layout is fixed by the ABI:
 *
 *   0  d_ino     u64
 *   8  d_off     s64   - what to seek to for the NEXT record
 *   16 d_reclen  u16   - including the name and its terminator
 *   18 d_type    u8
 *   19 d_name    char[] NUL-terminated
 *
 * d_reclen must be 8-aligned or a libc that walks the buffer by adding
 * reclen lands mid-record and reads garbage. */

#define DT_DIR  4
#define DT_REG  8

struct dents_ctx {
    uint8 *buf;
    uint64 max;
    uint64 used;
    uint64 index;      /* how many entries walked so far  */
    uint64 skip;       /* how many to ignore before emitting */
    int    stopped;    /* the buffer filled mid-directory */
};

static uint64 name_len(const char *s) {
    uint64 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int dents_visit(const fs_dirent_t *e, void *ctx) {
    struct dents_ctx *dc = (struct dents_ctx *)ctx;
    uint64 nlen, reclen, i;
    uint8 *rec;

    if (dc->index++ < dc->skip) {
        return 0;                            /* already returned to the caller */
    }

    nlen   = name_len(e->name);
    reclen = (19 + nlen + 1 + 7) & ~7ULL;

    if (dc->used + reclen > dc->max) {
        /* Out of room. Stopping without emitting a partial record is half the
         * contract; rolling the index back is the other half, and it was
         * missing. dc->index was already incremented at the top of this
         * function, so the position handed back counted an entry that was
         * never emitted and the next call resumed PAST it - one file silently
         * missing from the listing every time the buffer filled mid-directory.
         * devices.c had this right and said so in a comment; this did not. */
        dc->index--;
        dc->stopped = 1;
        return 1;
    }

    rec = dc->buf + dc->used;
    for (i = 0; i < reclen; i++) {
        rec[i] = 0;
    }
    *(uint64 *)(rec + 0)  = e->ino ? e->ino : dc->index;
    *(uint64 *)(rec + 8)  = (uint64)dc->index;   /* resume point */
    *(uint16 *)(rec + 16) = (uint16)reclen;
    rec[18] = e->is_dir ? DT_DIR : DT_REG;
    for (i = 0; i < nlen; i++) {
        rec[19 + i] = (uint8)e->name[i];
    }

    dc->used += reclen;
    return 0;
}

int64 fileobj_getdents(object_t *obj, void *buf, uint64 max, uint64 *pos) {
    struct dents_ctx dc;
    const fs_node_t *node;
    int rc;

    if (obj == NULL || obj->body == NULL) {
        return -5;
    }
    if (!fileobj_is_dir(obj)) {
        return -20;                          /* -ENOTDIR */
    }
    node = (const fs_node_t *)obj->body;

    dc.buf     = (uint8 *)buf;
    dc.max     = max;
    dc.used    = 0;
    dc.index   = 0;
    dc.skip    = *pos;
    dc.stopped = 0;

    rc = fs_iterate(node, dents_visit, &dc);
    if (rc < 0) {
        return rc;
    }
    /* Zero bytes because nothing fit is not the same answer as zero bytes
     * because the directory ended, and getdents64 has only one way to say
     * each: 0 means end of directory. A caller handed 0 for a buffer too
     * small for even the first record concludes the directory is empty. */
    if (dc.used == 0 && dc.stopped) {
        return -22;                          /* -EINVAL */
    }
    *pos = dc.index;
    return (int64)dc.used;
}
