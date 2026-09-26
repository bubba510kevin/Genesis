#include "ns.h"
#include "object.h"
#include "screen.h"
#include "typesk.h"

/* See ns.h for what this is and why it is shaped like NT's rather than like a
 * flat table of device names. What follows is how.
 *
 * Storage is a fixed pool, same reasoning as the address-space and process
 * pools: the count is small and bounded by the hardware present, and a fixed
 * pool turns exhaustion into "cannot create device" at boot rather than into
 * heap fragmentation at hour six. Entries are never reordered, so a pointer
 * to one stays valid for the life of the kernel. */

static ns_entry_t pool[NS_MAX_ENTRIES];
static int        pool_used;
static ns_entry_t *root;

#define E_NOENT        -2
#define E_NOTDIR      -20
#define E_ISDIR       -21
#define E_INVAL       -22
#define E_NOSPC       -28
#define E_NAMETOOLONG -36
#define E_LOOP        -40
#define E_EXIST       -17
#define E_NOTEMPTY    -39

static int is_sep(char c) {
    return c == '\\';
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive, because the namespace this imitates is. Getting this
 * wrong makes \??\c: and \??\C: two different drives, which is the kind of
 * bug that only shows up once something lower-cases a name in passing. */
int ns_name_eq(const char *a, const char *b) {
    while (*a != '\0' && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static uint64 str_len(const char *s) {
    uint64 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int str_copy(char *dst, uint64 cap, const char *src) {
    uint64 i = 0;

    if (cap == 0) {
        return E_NAMETOOLONG;
    }
    while (src[i] != '\0') {
        if (i + 1 >= cap) {
            dst[0] = '\0';
            return E_NAMETOOLONG;
        }
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return 0;
}

static ns_entry_t *entry_alloc(ns_entry_t *parent, const char *name,
                               ns_kind_t kind) {
    ns_entry_t *e;

    int i;

    /* A freed slot before a fresh one. Removal exists now (see ns_remove), and
     * a bump allocator that never reuses turns a machine that mounts and
     * unmounts the same USB stick sixty-four times into one with no room left
     * for a console. Scanning first costs a loop over at most sixty-four
     * entries on a call that happens once per device.
     *
     * pool_used stays a high-water mark rather than a live count: every other
     * loop in this file walks [0, pool_used) and tests kind != NS_NONE, so
     * decrementing it on a free would hide live entries above the hole. */
    e = NULL;
    for (i = 0; i < pool_used; i++) {
        if (pool[i].kind == NS_NONE) {
            e = &pool[i];
            break;
        }
    }
    if (e == NULL) {
        if (pool_used >= NS_MAX_ENTRIES) {
            return NULL;
        }
        e = &pool[pool_used];
        pool_used++;
    }

    if (str_copy(e->name, sizeof(e->name), name) != 0) {
        /* Leave the slot free rather than half-initialised. A name that did
         * not fit must not leave an entry whose kind says it is live. */
        e->kind = NS_NONE;
        return NULL;
    }
    e->kind      = kind;
    e->parent    = parent;
    e->object    = NULL;
    e->target[0] = '\0';
    return e;
}

static ns_entry_t *find_child(const ns_entry_t *dir, const char *name) {
    int i;

    for (i = 0; i < pool_used; i++) {
        if (pool[i].parent == dir && pool[i].kind != NS_NONE &&
            ns_name_eq(pool[i].name, name)) {
            return &pool[i];
        }
    }
    return NULL;
}

void ns_init(void) {
    if (root != NULL) {
        return;
    }
    pool_used = 0;
    root = entry_alloc(NULL, "\\", NS_DIRECTORY);

    /* The two directories every name in the system hangs off. Created here
     * rather than lazily so that a lookup failure is always "no such name"
     * and never "the directory did not exist yet", which are the same errno
     * and very different bugs. */
    ns_mkdir("\\Device");
    ns_mkdir("\\??");
}

ns_entry_t *ns_root(void) {
    return root;
}

/* --- path walking --------------------------------------------------------
 *
 * One component at a time. `p` is advanced past the component and any
 * separators after it, so the caller can hand what is left to a device as the
 * unparsed remainder without any further bookkeeping. */
static int next_component(const char **p, char *out, uint64 cap) {
    const char *s = *p;
    uint64 n = 0;

    while (is_sep(*s)) {
        s++;
    }
    if (*s == '\0') {
        *p = s;
        return 0;                    /* no component left */
    }
    while (*s != '\0' && !is_sep(*s)) {
        if (n + 1 >= cap) {
            return E_NAMETOOLONG;
        }
        out[n++] = *s++;
    }
    out[n] = '\0';
    *p = s;
    return 1;
}

/* The whole resolver. `stop_at_object` is what separates a lookup that wants
 * an object (and a remainder) from one that wants an entry.
 *
 * follow_last controls whether a link named by the FINAL component is
 * followed. An open must follow it; something listing the namespace must not,
 * or \??\C: is indistinguishable from the volume it points at. */
static int resolve(const char *path, int follow_last,
                   ns_entry_t **out_entry,
                   char *remainder, uint64 remainder_size) {
    char work[NS_PATH_MAX];
    char name[NS_NAME_MAX];
    int  depth = 0;
    int  rc;

    if (root == NULL) {
        return E_NOENT;
    }
    if (path == NULL || !is_sep(path[0])) {
        return E_INVAL;              /* the namespace has no working directory */
    }
    rc = str_copy(work, sizeof(work), path);
    if (rc != 0) {
        return rc;
    }

    for (;;) {
        const char *p   = work;
        ns_entry_t *cur = root;

        for (;;) {
            ns_entry_t *child;

            rc = next_component(&p, name, sizeof(name));
            if (rc < 0) {
                return rc;
            }
            if (rc == 0) {
                /* The path ended on `cur`. */
                if (remainder != NULL && remainder_size > 0) {
                    remainder[0] = '\0';
                }
                *out_entry = cur;
                return 0;
            }
            if (cur->kind != NS_DIRECTORY) {
                return E_NOTDIR;
            }

            child = find_child(cur, name);
            if (child == NULL) {
                return E_NOENT;
            }

            if (child->kind == NS_LINK) {
                const char *rest = p;
                char        next[NS_PATH_MAX];
                uint64      tlen, rlen;

                /* A link named by the last component, when the caller asked
                 * not to follow one, is the answer itself. */
                if (*rest == '\0' && !follow_last) {
                    if (remainder != NULL && remainder_size > 0) {
                        remainder[0] = '\0';
                    }
                    *out_entry = child;
                    return 0;
                }
                if (++depth > NS_MAX_LINK_DEPTH) {
                    return E_LOOP;
                }

                /* Splice: the link's target replaces everything consumed so
                 * far, and the unconsumed tail rides along. This is the step
                 * that makes \??\C:\bin\sh become
                 * \Device\HarddiskVolume1\bin\sh before anything looks at
                 * the volume. */
                tlen = str_len(child->target);
                rlen = str_len(rest);
                if (tlen + 1 + rlen + 1 > sizeof(next)) {
                    return E_NAMETOOLONG;
                }
                {
                    uint64 i;
                    for (i = 0; i < tlen; i++) {
                        next[i] = child->target[i];
                    }
                    if (rlen > 0) {
                        if (!is_sep(rest[0])) {
                            next[tlen++] = '\\';
                        }
                        for (i = 0; i < rlen; i++) {
                            next[tlen + i] = rest[i];
                        }
                        tlen += rlen;
                    }
                    next[tlen] = '\0';
                }
                rc = str_copy(work, sizeof(work), next);
                if (rc != 0) {
                    return rc;
                }
                break;               /* restart the walk on the new path */
            }

            if (child->kind == NS_OBJECT) {
                /* Resolution stops at an object. Everything left belongs to
                 * whatever that object is - see the header. */
                if (remainder != NULL && remainder_size > 0) {
                    rc = str_copy(remainder, remainder_size, p);
                    if (rc != 0) {
                        return rc;
                    }
                } else if (*p != '\0') {
                    /* A caller that cannot take a remainder must not be
                     * handed a truncated answer. */
                    return E_NOENT;
                }
                *out_entry = child;
                return 0;
            }

            cur = child;             /* a directory: keep walking */
        }
    }
}

/* --- construction ------------------------------------------------------- */

/* Split a path into its parent directory and its final component. The parent
 * is created on demand when `create_parents` is set, which is what makes
 * ns_mkdir("\\Device\\Storage\\Volume") one call rather than three. */
static int split(const char *path, int create_parents,
                 ns_entry_t **out_dir, char *leaf, uint64 leaf_cap) {
    char        name[NS_NAME_MAX];
    const char *p = path;
    ns_entry_t *cur = root;
    int         rc;
    int         have = 0;

    if (root == NULL || path == NULL || !is_sep(path[0])) {
        return E_INVAL;
    }

    for (;;) {
        rc = next_component(&p, name, sizeof(name));
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            break;
        }
        if (have) {
            /* The component held from last time is a directory after all. */
            ns_entry_t *child = find_child(cur, leaf);

            if (child == NULL) {
                if (!create_parents) {
                    return E_NOENT;
                }
                child = entry_alloc(cur, leaf, NS_DIRECTORY);
                if (child == NULL) {
                    return E_NOSPC;
                }
            }
            if (child->kind != NS_DIRECTORY) {
                return E_NOTDIR;
            }
            cur = child;
        }
        rc = str_copy(leaf, leaf_cap, name);
        if (rc != 0) {
            return rc;
        }
        have = 1;
    }

    if (!have) {
        return E_INVAL;              /* the path was just "\" */
    }
    *out_dir = cur;
    return 0;
}

ns_entry_t *ns_mkdir(const char *path) {
    ns_entry_t *dir;
    ns_entry_t *existing;
    char        leaf[NS_NAME_MAX];

    if (split(path, 1, &dir, leaf, sizeof(leaf)) != 0) {
        return NULL;
    }
    existing = find_child(dir, leaf);
    if (existing != NULL) {
        return existing->kind == NS_DIRECTORY ? existing : NULL;
    }
    return entry_alloc(dir, leaf, NS_DIRECTORY);
}

int ns_insert(const char *path, object_t *obj) {
    ns_entry_t *dir;
    ns_entry_t *e;
    char        leaf[NS_NAME_MAX];
    int         rc;

    if (obj == NULL) {
        return E_INVAL;
    }
    rc = split(path, 0, &dir, leaf, sizeof(leaf));
    if (rc != 0) {
        return rc;
    }
    if (find_child(dir, leaf) != NULL) {
        return E_EXIST;
    }
    e = entry_alloc(dir, leaf, NS_OBJECT);
    if (e == NULL) {
        return E_NOSPC;
    }
    /* The namespace is a reference holder like any other. Without this an
     * object could be destroyed while its name still resolves, and the next
     * lookup hands out a pointer to a reused pool slot. */
    ob_ref(obj);
    e->object = obj;
    return 0;
}

int ns_link(const char *path, const char *target) {
    ns_entry_t *dir;
    ns_entry_t *e;
    char        leaf[NS_NAME_MAX];
    int         rc;

    rc = split(path, 0, &dir, leaf, sizeof(leaf));
    if (rc != 0) {
        return rc;
    }
    if (find_child(dir, leaf) != NULL) {
        return E_EXIST;
    }
    e = entry_alloc(dir, leaf, NS_LINK);
    if (e == NULL) {
        return E_NOSPC;
    }
    /* Deliberately not resolved here. \??\ is populated before the devices it
     * names exist, and a link that had to point at something already created
     * would force an ordering between two subsystems that do not otherwise
     * care about each other. */
    rc = str_copy(e->target, sizeof(e->target), target);
    if (rc != 0) {
        e->kind = NS_NONE;
        return rc;
    }
    return 0;
}

/* --- removal -------------------------------------------------------------
 *
 * The half that did not exist while every device was permanent. Three things
 * make it more than "clear the slot".
 *
 * --- One name, or every name that reaches the object ---------------------
 * A device registered at \Device\HarddiskVolume2 and aliased at \??\D: has
 * TWO entries. Removing only the one the caller named leaves the other
 * resolving to an object that is about to answer -ENODEV forever - a drive
 * letter that exists, opens, and fails. Worse, /dev is the merged view of
 * \??\ and \Device\ (see devices.c), and a stale link in the arm that is
 * searched FIRST shadows a device later given the same name.
 *
 * So ns_remove(path) removes that entry, and ns_remove_object(obj) removes
 * every entry that names it. dev_detach uses the first, twice, because it
 * knows both of its names and removing exactly what it inserted is a
 * narrower claim than removing everything that happens to point here. The
 * second exists for the case where a driver has lost track - and for the
 * verification check that asserts nothing is left behind.
 *
 * --- A link is not followed ----------------------------------------------
 * ns_remove("\\??\\D:") removes the LINK, not the volume it names. Following
 * it would make deleting a drive letter delete the drive, which is the one
 * behaviour nobody expects from unlinking a symlink.
 *
 * --- The slot is recycled -------------------------------------------------
 * entry_alloc was a bump allocator, correct for a namespace that only ever
 * grew. With removal it has to reuse, or a machine that mounts and unmounts a
 * USB stick sixty-four times runs out of namespace and no longer has room for
 * a console. find_child already skips NS_NONE, so a freed entry is invisible
 * the moment its kind is cleared - the reuse below is safe precisely because
 * the "is this slot live" test was already written that way. */

/* Detach one entry: drop the object reference and free the slot. Does not
 * touch children; the callers below decide what that means. */
static void entry_release(ns_entry_t *e) {
    if (e->kind == NS_OBJECT && e->object != NULL) {
        ob_deref(e->object);
    }
    e->object    = NULL;
    e->target[0] = '\0';
    e->name[0]   = '\0';
    e->parent    = NULL;
    e->kind      = NS_NONE;
}

int ns_remove(const char *path) {
    ns_entry_t *e;
    int i;

    /* Not following a trailing link is the whole point - see above. */
    e = ns_lookup_entry(path);
    if (e == NULL) {
        return E_NOENT;
    }
    if (e == root) {
        return E_INVAL;
    }
    if (e->kind == NS_DIRECTORY) {
        /* A non-empty directory is -ENOTEMPTY, matching rmdir(2). Removing it
         * anyway would orphan every child: find_child matches on the parent
         * POINTER, so the children would still be live entries whose parent
         * slot has been recycled into some other device's entry - and they
         * would then appear as that device's children. */
        for (i = 0; i < pool_used; i++) {
            if (pool[i].parent == e && pool[i].kind != NS_NONE) {
                return E_NOTEMPTY;
            }
        }
    }
    entry_release(e);
    return 0;
}

int ns_remove_object(const object_t *obj) {
    int removed = 0;
    int i;

    if (obj == NULL) {
        return E_INVAL;
    }
    /* Objects first, then links, and the order matters. A link whose target
     * names an object still in the tree is a link this pass can resolve; once
     * the object entry is gone the target string is just a string, and the
     * only way to tell whether it pointed here is to have looked before. So
     * the link scan below compares against the NAMES collected while the
     * object entries were still present. */
    for (i = 0; i < pool_used; i++) {
        if (pool[i].kind == NS_OBJECT && pool[i].object == obj) {
            char full[NS_PATH_MAX];
            int  k;

            if (ns_path_of(&pool[i], full, sizeof(full)) == 0) {
                for (k = 0; k < pool_used; k++) {
                    if (pool[k].kind == NS_LINK &&
                        ns_name_eq(pool[k].target, full)) {
                        entry_release(&pool[k]);
                        removed++;
                    }
                }
            }
            entry_release(&pool[i]);
            removed++;
        }
    }
    return removed > 0 ? 0 : E_NOENT;
}

int ns_path_of(const ns_entry_t *e, char *out, uint64 cap) {
    const ns_entry_t *chain[16];
    int    n = 0;
    uint64 len = 0;
    int    i;

    if (e == NULL || out == NULL || cap < 2) {
        return E_INVAL;
    }
    while (e != NULL && e != root) {
        if (n >= (int)(sizeof(chain) / sizeof(chain[0]))) {
            return E_NAMETOOLONG;
        }
        chain[n++] = e;
        e = e->parent;
    }
    if (n == 0) {
        out[0] = '\\';
        out[1] = '\0';
        return 0;
    }
    for (i = n - 1; i >= 0; i--) {
        uint64 nl = str_len(chain[i]->name);

        if (len + 1 + nl + 1 > cap) {
            return E_NAMETOOLONG;
        }
        out[len++] = '\\';
        {
            uint64 j;
            for (j = 0; j < nl; j++) {
                out[len + j] = chain[i]->name[j];
            }
        }
        len += nl;
    }
    out[len] = '\0';
    return 0;
}

/* --- lookup ------------------------------------------------------------- */

int ns_lookup(const char *path, object_t **out,
              char *remainder, uint64 remainder_size) {
    ns_entry_t *e = NULL;
    int rc;

    if (out == NULL) {
        return E_INVAL;
    }
    rc = resolve(path, 1, &e, remainder, remainder_size);
    if (rc != 0) {
        return rc;
    }
    if (e->kind != NS_OBJECT) {
        return E_ISDIR;              /* a directory is not something to open */
    }
    ob_ref(e->object);               /* the caller owns this reference */
    *out = e->object;
    return 0;
}

/* find_child, under a name callers outside this file can use. Taking the
 * directory as an entry rather than as a path matters: something merging two
 * directories into one view asks this question once per candidate name, and
 * re-resolving a path from the root each time would make a listing O(n^2) in
 * the size of the namespace for no gain. */
ns_entry_t *ns_child(const ns_entry_t *dir, const char *name) {
    return find_child(dir, name);
}

ns_entry_t *ns_lookup_entry(const char *path) {
    ns_entry_t *e = NULL;

    if (resolve(path, 0, &e, NULL, 0) != 0) {
        return NULL;
    }
    return e;
}

int ns_iterate_entry(const ns_entry_t *dir,
                     int (*cb)(const ns_entry_t *entry, void *ctx), void *ctx) {
    int rc, i;

    if (dir == NULL) {
        return E_NOENT;
    }
    if (dir->kind != NS_DIRECTORY) {
        return E_NOTDIR;
    }
    for (i = 0; i < pool_used; i++) {
        if (pool[i].parent == dir && pool[i].kind != NS_NONE) {
            rc = cb(&pool[i], ctx);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

int ns_iterate(const char *path,
               int (*cb)(const ns_entry_t *entry, void *ctx), void *ctx) {
    ns_entry_t *dir = NULL;
    int rc;

    rc = resolve(path, 0, &dir, NULL, 0);
    if (rc != 0) {
        return rc;
    }
    return ns_iterate_entry(dir, cb, ctx);
}

/* --- reporting ---------------------------------------------------------- */

static void report_dir(const ns_entry_t *dir, int depth) {
    int i, d;

    for (i = 0; i < pool_used; i++) {
        const ns_entry_t *e = &pool[i];

        if (e->parent != dir || e->kind == NS_NONE) {
            continue;
        }
        for (d = 0; d <= depth; d++) {
            print_string("  ", 0x07);
        }
        print_string(e->name, 0x0F);

        switch (e->kind) {
            case NS_DIRECTORY:
                print_string("\\\n", 0x07);
                report_dir(e, depth + 1);
                break;
            case NS_LINK:
                print_string(" -> ", 0x08);
                print_string(e->target, 0x0B);
                print_string("\n", 0x07);
                break;
            case NS_OBJECT:
                print_string("  [", 0x08);
                print_string(e->object != NULL && e->object->type != NULL
                                 ? e->object->type->name : "?", 0x0A);
                print_string("]\n", 0x08);
                break;
            default:
                print_string("\n", 0x07);
                break;
        }
    }
}

void ns_report(void) {
    if (root == NULL) {
        print_string("namespace: not initialised\n", 0x0C);
        return;
    }
    print_string("Object namespace:\n", 0x0F);
    report_dir(root, 0);
}
