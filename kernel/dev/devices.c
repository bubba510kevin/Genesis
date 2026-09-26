#include "devices.h"
#include "ns.h"
#include "object.h"
#include "screen.h"
#include "typesk.h"

/* --- /dev/null -----------------------------------------------------------
 *
 * The two operations are the whole device. A read returns zero, which is end
 * of input, so `cat /dev/null` prints nothing and stops rather than blocking
 * forever. A write claims to have written everything and keeps none of it,
 * which is what makes `command > /dev/null` finish instead of failing with a
 * short write partway through.
 *
 * Returning n rather than 0 from write matters more than it looks: a caller
 * that gets a short write is obliged to retry the remainder, and a device
 * that always short-writes turns that into an infinite loop. */

static int64 null_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    (void)obj;
    (void)buf;
    (void)n;
    (void)offset;
    return 0;                       /* always EOF */
}

static int64 null_write(object_t *obj, const void *buf, uint64 n,
                        uint64 *offset) {
    (void)obj;
    (void)buf;
    (void)offset;
    return (int64)n;                /* accepted in full, kept none of it */
}

/* OBJ_CONSOLE rather than OBJ_FILE: the class is what syscall.c consults to
 * decide whether a descriptor is seekable, and lseek on /dev/null should be
 * -ESPIPE for the same reason it is on a terminal. There is no position to
 * move. */
static const object_type_t null_type = {
    .name  = "null",
    .klass = OBJ_CONSOLE,
    .read  = null_read,
    .write = null_write
};

/* --- /dev as a view onto the namespace ----------------------------------
 *
 * /dev used to be a plain synonym for \??\, and that was the bug. \??\ holds
 * DOS device NAMES, and every one of them is a link somebody wrote by hand.
 * A device object inserted at \Device\Serial0 was therefore invisible under
 * /dev and could not be opened through it until someone remembered to add
 * \??\serial0 as well - which is exactly the "second list that drifts" the
 * namespace exists to prevent. systest's own comment claimed a registered
 * device shows up in /dev with nothing maintaining a second list; \??\ WAS
 * that second list, and it only looked correct because every device present
 * happened to have been given a link by hand on the same line it was created.
 *
 * So /dev is a view onto BOTH directories, searched in this order:
 *
 *   \??\      first, so an alias (tty, stdin, stdout) or a DOS name (C:, NUL)
 *             wins where one exists
 *   \Device\  second, so a device that was only ever ns_insert()ed is still
 *             reachable under its own name, with no link at all
 *
 * Registering a device is one call again. A link in \??\ now gives it a
 * SECOND name; it is no longer what makes it exist. */

static const char *const dev_dirs[] = { "\\??", "\\Device" };
#define DEV_DIR_COUNT ((int)(sizeof(dev_dirs) / sizeof(dev_dirs[0])))

/* --- listing a namespace directory --------------------------------------
 *
 * The records are linux_dirent64, identical in layout to the ones fileobj.c
 * emits for a FAT directory, because the caller is the same getdents64 and
 * must not be able to tell which kind of directory it asked about.
 *
 *    0  d_ino     u64
 *    8  d_off     u64      resume point
 *   16  d_reclen  u16      8-aligned, including the name and terminator
 *   18  d_type    u8
 *   19  d_name    char[]
 */

#define DT_DIR   4
#define DT_LNK  10
#define DT_CHR   2

struct dents_ctx {
    uint8 *buf;
    uint64 max;
    uint64 used;
    uint64 index;
    uint64 skip;
    int    stopped;                  /* the buffer filled mid-directory */

    /* Directories already walked in this listing. An entry whose name one of
     * them already claimed is skipped, so /dev shows \Device\Console once
     * rather than once per name that reaches it. Empty for an ordinary
     * single-directory listing. */
    const ns_entry_t *earlier[DEV_DIR_COUNT];
    int               earlier_count;
};

static uint64 name_len(const char *s) {
    uint64 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static uint8 kind_to_dtype(ns_kind_t kind) {
    switch (kind) {
        case NS_DIRECTORY: return DT_DIR;
        /* A link is reported as a link rather than as the thing it points at.
         * ls -l /dev showing `null -> \Device\Null` is the honest answer, and
         * resolving it here would make \??\C: indistinguishable from the
         * volume it names. */
        case NS_LINK:      return DT_LNK;
        default:           return DT_CHR;
    }
}

static int dents_visit(const ns_entry_t *e, void *ctx) {
    struct dents_ctx *dc = (struct dents_ctx *)ctx;
    uint64 nlen, reclen, i;
    uint8 *rec;
    int    k;

    /* Deduplication comes before the position counter, so that the resume
     * point counts entries that are actually emitted. Counting a skipped
     * duplicate would make the position mean something different on the
     * second call than it did on the first. */
    for (k = 0; k < dc->earlier_count; k++) {
        if (ns_child(dc->earlier[k], e->name) != NULL) {
            return 0;
        }
    }

    if (dc->index++ < dc->skip) {
        return 0;                        /* already handed to the caller */
    }

    nlen   = name_len(e->name);
    reclen = (19 + nlen + 1 + 7) & ~7ULL;

    if (dc->used + reclen > dc->max) {
        /* Stop without emitting a partial record, and leave the index where
         * it is so the next call starts with this entry. Rolling back the
         * increment is the part that is easy to miss: without it, running out
         * of buffer silently skips an entry. */
        dc->index--;
        dc->stopped = 1;
        return 1;
    }

    rec = dc->buf + dc->used;
    for (i = 0; i < reclen; i++) {
        rec[i] = 0;
    }
    *(uint64 *)(rec + 0)  = dc->index;   /* no inode numbers here */
    *(uint64 *)(rec + 8)  = dc->index;
    *(uint16 *)(rec + 16) = (uint16)reclen;
    rec[18] = kind_to_dtype(e->kind);
    for (i = 0; i < nlen; i++) {
        rec[19 + i] = (uint8)e->name[i];
    }

    dc->used += reclen;
    return 0;
}

static void dents_begin(struct dents_ctx *dc, void *buf, uint64 max, uint64 pos) {
    dc->buf           = (uint8 *)buf;
    dc->max           = max;
    dc->used          = 0;
    dc->index         = 0;
    dc->skip          = pos;
    dc->stopped       = 0;
    dc->earlier_count = 0;
}

/* Zero bytes because nothing fit is NOT the same answer as zero bytes because
 * the directory ended, and getdents64 has only one way to say each: 0 means
 * end of directory. A caller handed 0 for a buffer that was simply too small
 * concludes the directory is empty. */
static int64 dents_finish(struct dents_ctx *dc, uint64 *pos) {
    if (dc->used == 0 && dc->stopped) {
        return -22;                      /* -EINVAL */
    }
    *pos = dc->index;
    return (int64)dc->used;
}

static int64 nsdir_getdents(object_t *obj, void *buf, uint64 max,
                            uint64 *pos) {
    const ns_entry_t *dir = (const ns_entry_t *)obj->body;
    struct dents_ctx dc;
    int rc;

    if (dir == NULL) {
        return -5;                       /* -EIO */
    }
    dents_begin(&dc, buf, max, *pos);
    rc = ns_iterate_entry(dir, dents_visit, &dc);
    if (rc < 0) {
        return rc;
    }
    return dents_finish(&dc, pos);
}

/* No destroy: the body is a namespace entry, which belongs to the namespace
 * and outlives every handle onto it. */
static const object_type_t nsdir_type = {
    .name     = "device-directory",
    .klass    = OBJ_DIRECTORY,
    .getdents = nsdir_getdents
};

/* The merged listing. Same records, walked across every directory /dev is a
 * view onto, in resolution order, with names an earlier directory already
 * claimed left out.
 *
 * The body is NULL rather than a directory entry, because there is no single
 * entry this is a listing of - which is the whole point. */
static int64 devdir_getdents(object_t *obj, void *buf, uint64 max,
                             uint64 *pos) {
    struct dents_ctx dc;
    int k, j, rc;

    (void)obj;
    dents_begin(&dc, buf, max, *pos);

    for (k = 0; k < DEV_DIR_COUNT; k++) {
        ns_entry_t *dir = ns_lookup_entry(dev_dirs[k]);

        if (dir == NULL) {
            continue;
        }
        dc.earlier_count = 0;
        for (j = 0; j < k; j++) {
            ns_entry_t *prev = ns_lookup_entry(dev_dirs[j]);

            if (prev != NULL) {
                dc.earlier[dc.earlier_count++] = prev;
            }
        }
        rc = ns_iterate_entry(dir, dents_visit, &dc);
        if (rc < 0) {
            return rc;
        }
        if (rc > 0) {
            break;                       /* the buffer filled */
        }
    }
    return dents_finish(&dc, pos);
}

static const object_type_t devdir_type = {
    .name     = "dev",
    .klass    = OBJ_DIRECTORY,
    .getdents = devdir_getdents
};

object_t *nsdir_open(const char *ns_path) {
    ns_entry_t *e = ns_lookup_entry(ns_path);

    if (e == NULL || e->kind != NS_DIRECTORY) {
        return NULL;
    }
    return ob_create(&nsdir_type, e);
}

/* --- resolving a POSIX /dev path ---------------------------------------- */

/* "/dev/console" -> "\??\console" or "\Device\console", depending on which
 * directory is being tried. Component separators flip; the names themselves
 * are passed through untouched, because the namespace compares them
 * case-insensitively and /dev/CON, /dev/con and /dev/Con all have to find the
 * same entry. */
static int dev_ns_path(const char *dir, const char *tail,
                       char *out, uint64 cap) {
    uint64 n = 0;

    while (*dir != '\0') {
        if (n + 1 >= cap) return 0;
        out[n++] = *dir++;
    }
    while (*tail != '\0') {
        if (n + 1 >= cap) return 0;
        out[n++] = (*tail == '/') ? '\\' : *tail;
        tail++;
    }
    out[n] = '\0';
    return 1;
}

int dev_lookup(const char *posix_path, object_t **out,
               char *remainder, uint64 remainder_size) {
    const char *tail = posix_path + 4;   /* past "/dev" */
    int k;
    int last = -2;                       /* -ENOENT unless something worse */

    if (out == NULL) {
        return -22;
    }
    *out = NULL;
    if (remainder != NULL && remainder_size > 0) {
        remainder[0] = '\0';
    }

    /* Bare "/dev". Not a lookup at all: there is no single namespace entry
     * that is /dev, only the merged view of the directories it presents. */
    if (*tail == '\0' || (tail[0] == '/' && tail[1] == '\0')) {
        object_t *obj = ob_create(&devdir_type, NULL);

        if (obj == NULL) {
            return -23;                  /* -ENFILE */
        }
        *out = obj;
        return 0;
    }

    for (k = 0; k < DEV_DIR_COUNT; k++) {
        char nspath[NS_PATH_MAX];
        object_t *obj = NULL;
        int rc;

        if (!dev_ns_path(dev_dirs[k], tail, nspath, sizeof(nspath))) {
            return -36;                  /* -ENAMETOOLONG */
        }
        rc = ns_lookup(nspath, &obj, remainder, remainder_size);
        if (rc == 0) {
            *out = obj;
            return 0;
        }
        if (rc == -21) {
            /* A namespace DIRECTORY below /dev. Not something to read bytes
             * from, but it still has to open so that a listing can ask, and
             * the object it gets walks the namespace live rather than a
             * snapshot that can drift. */
            obj = nsdir_open(nspath);
            if (obj != NULL) {
                if (remainder != NULL && remainder_size > 0) {
                    remainder[0] = '\0';
                }
                *out = obj;
                return 0;
            }
            return -21;
        }
        /* -ENOENT means "not under this directory" and the next one still
         * gets a turn. Anything else - a loop, a name too long - is a real
         * answer about this path and is reported as it is. */
        if (rc != -2) {
            return rc;
        }
        last = rc;
        if (remainder != NULL && remainder_size > 0) {
            remainder[0] = '\0';
        }
    }
    return last;
}

int dev_is_directory(const object_t *obj) {
    return obj != NULL &&
           (obj->type == &nsdir_type || obj->type == &devdir_type);
}

/* --- registration -------------------------------------------------------- */

void devices_register(void) {
    object_t *nul = ob_create(&null_type, NULL);

    if (nul == NULL) {
        print_string("dev: no object slot for null\n", 0x0C);
        return;
    }
    if (ns_insert("\\Device\\Null", nul) != 0) {
        print_string("dev: could not name \\Device\\Null\n", 0x0C);
    }
    ob_deref(nul);                       /* the namespace holds it now */

    /* The name in \Device\ is now enough on its own - /dev/null resolves to
     * it through the \Device\ arm of the view, and would even if these two
     * lines were deleted. They stay because \??\ is where a PE binary looks
     * and NUL is the spelling it uses; \??\null keeps the POSIX name in the
     * directory that is listed first, so `ls /dev` shows `null` rather than
     * both `NUL` and `Null`. */
    ns_link("\\??\\null", "\\Device\\Null");
    ns_link("\\??\\NUL",  "\\Device\\Null");
}
