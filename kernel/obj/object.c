#include "ns.h"
#include "object.h"
#include "typesk.h"

/* Static pools rather than the heap.
 *
 * Three reasons, in order of how much they matter. Object teardown runs on
 * paths where the heap is not necessarily safe to touch - eventually from an
 * interrupt, once a device can complete I/O asynchronously. Exhaustion
 * becomes "too many open files" at a specific limit rather than heap pressure
 * discovered somewhere unrelated. And nothing in this file allocates, which
 * is what lets the whole object manager be tested on the host without a
 * kernel heap behind it. */

static object_t    object_pool[MAX_OBJECTS];
static open_file_t open_file_pool[MAX_OPEN_FILES];

/* --- objects ------------------------------------------------------------ */

object_t *ob_create(const object_type_t *type, void *body) {
    int i;

    if (type == NULL) {
        return NULL;
    }
    /* Every type that is ever instantiated registers itself, here, once.
     *
     * The alternative was a hand-written ob_register_type call in each of the
     * eight or so files that define a type, and that list is one somebody
     * forgets to add to - a new type would then be missing from
     * \ObjectTypes, which is the one directory whose entire job is to say
     * what types exist. A list that can silently be incomplete is worse than
     * no list, because it looks complete.
     *
     * The consequence is worth stating rather than hiding: \ObjectTypes
     * holds the types that have been INSTANTIATED, not the types that are
     * compiled in. A type nobody has created an object of does not appear.
     * That is a slightly different question from the one NT answers, and it
     * is the more useful one here - "what is on this machine" rather than
     * "what could be".
     *
     * Cheap: ob_register_type walks a table of at most sixteen pointers and
     * returns immediately on a hit, and object creation is not a hot path. */
    (void)ob_register_type(type);

    for (i = 0; i < MAX_OBJECTS; i++) {
        if (object_pool[i].refcount == 0) {
            object_pool[i].type     = type;
            object_pool[i].body     = body;
            object_pool[i].refcount = 1;
            return &object_pool[i];
        }
    }
    return NULL;
}

void ob_ref(object_t *obj) {
    if (obj != NULL && obj->refcount != 0) {
        obj->refcount++;
    }
}

void ob_deref(object_t *obj) {
    if (obj == NULL || obj->refcount == 0) {
        return;
    }
    obj->refcount--;
    if (obj->refcount == 0) {
        if (obj->type != NULL && obj->type->destroy != NULL) {
            obj->type->destroy(obj);
        }
        obj->type = NULL;
        obj->body = NULL;
    }
}

int ob_is_directory(const object_t *obj) {
    return obj != NULL && obj->type != NULL && obj->type->klass == OBJ_DIRECTORY;
}

int ob_poll(object_t *obj, int events) {
    int ready;

    if (obj == NULL || obj->type == NULL || obj->refcount == 0) {
        return OB_POLLNVAL;
    }
    if (obj->type->poll == NULL) {
        ready = OB_POLLIN | OB_POLLOUT;
    } else {
        ready = obj->type->poll(obj, events);
    }
    /* Masked with events PLUS the always-reported bits, never with events
     * alone. A caller that asks only about POLLIN and gets a hangup must be
     * told about the hangup - that is the whole reason those three bits are
     * output-only. Masking with `events` here is the bug that makes a poll on
     * a dead pipe spin: it reports nothing ready, forever, on a descriptor
     * that will never be ready again. */
    return ready & (events | OB_POLL_ALWAYS);
}

int ob_wait(object_t *obj, uint64 deadline) {
    if (obj == NULL || obj->type == NULL || obj->refcount == 0) {
        return -9;                          /* -EBADF */
    }
    if (obj->type->wait == NULL) {
        /* Not waitable. -EINVAL and not a block, and the difference matters
         * enough that this item's check asserts it: a "not waitable" path
         * that hangs is indistinguishable from a type nobody has got to yet,
         * and the caller finds out by never coming back. */
        return -22;                         /* -EINVAL */
    }
    return obj->type->wait(obj, deadline);
}

int ob_signal(object_t *obj, int op, int64 count, int64 *prev) {
    if (obj == NULL || obj->type == NULL || obj->refcount == 0) {
        return -9;
    }
    if (obj->type->signal == NULL) {
        return -22;
    }
    return obj->type->signal(obj, op, count, prev);
}

/* --- the type registry ---------------------------------------------------
 *
 * A flat table of the types anybody has registered, published into
 * \ObjectTypes as objects of the "Type" type - which is itself registered,
 * so the list describes its own membership the way NT's does.
 *
 * Registration is separated from PUBLICATION because of boot order: a type
 * is a file-scope constant and registers as early as its file's init runs,
 * which can be before the namespace exists. Registering into a namespace that
 * is not there yet would silently lose the type, so registration only records
 * it and ob_publish_types() does the inserting once. */
#define OB_TYPE_MAX 16

static void ob_publish_one(const object_type_t *type);

static const object_type_t *type_registry[OB_TYPE_MAX];
static int                  type_count;
static int                  types_published;

/* The type of a type. Its body is the object_type_t it describes, so a
 * walker that opens \ObjectTypes\Event gets something it can ask. */
static const object_type_t objtype_type = {
    .name  = "Type",
    .klass = OBJ_TYPE
};

/* Insert one type as \ObjectTypes\<name>.
 *
 * Failures are SILENT and deliberate: this is a diagnostic surface, and a
 * namespace pool that is one entry short must not stop a driver registering
 * its type or - worse - abort a boot. The absence shows up as a missing entry
 * in a directory whose whole purpose is to be listed. */
static void ob_publish_one(const object_type_t *type) {
    char path[NS_PATH_MAX];
    const char *prefix = "\\ObjectTypes\\";
    object_t *obj;
    int i = 0, k;

    if (type == NULL || type->name == NULL) {
        return;
    }
    while (prefix[i] != '\0' && i < NS_PATH_MAX - 1) {
        path[i] = prefix[i];
        i++;
    }
    for (k = 0; type->name[k] != '\0' && i < NS_PATH_MAX - 1; k++) {
        path[i++] = type->name[k];
    }
    path[i] = '\0';

    if (ns_lookup_entry(path) != NULL) {
        return;                             /* already there */
    }
    obj = ob_create(&objtype_type, (void *)(uintptr)type);
    if (obj == NULL) {
        return;
    }
    /* ns_insert takes its own reference, so this one is dropped whether the
     * insert worked or not - keeping it would leak an object per failed
     * publish, on the path taken when the pool is exactly full. */
    (void)ns_insert(path, obj);
    ob_deref(obj);
}

int ob_register_type(const object_type_t *type) {
    int i;

    if (type == NULL || type->name == NULL) {
        return -22;
    }
    for (i = 0; i < type_count; i++) {
        if (type_registry[i] == type) {
            return 0;                       /* already registered */
        }
    }
    if (type_count >= OB_TYPE_MAX) {
        return -28;                         /* -ENOSPC */
    }
    type_registry[type_count++] = type;

    /* Registered after publication has already happened - a driver loaded at
     * runtime, say. Publish this one now rather than making it wait for a
     * second sweep that never comes. */
    if (types_published) {
        ob_publish_one(type);
    }
    return 0;
}

void ob_publish_types(void) {
    int i;

    /* The Type type first, so \ObjectTypes\Type exists and the list is
     * self-describing rather than describing everything except itself. */
    (void)ob_register_type(&objtype_type);

    types_published = 1;
    for (i = 0; i < type_count; i++) {
        ob_publish_one(type_registry[i]);
    }
}

/* --- open instances ----------------------------------------------------- */

open_file_t *of_open(object_t *obj, uint32 access) {
    int i;

    if (obj == NULL || obj->refcount == 0) {
        return NULL;
    }
    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_file_pool[i].refcount == 0) {
            open_file_pool[i].obj      = obj;
            open_file_pool[i].offset   = 0;
            open_file_pool[i].access   = access;
            open_file_pool[i].status   = 0;
            open_file_pool[i].refcount = 1;
            ob_ref(obj);
            return &open_file_pool[i];
        }
    }
    return NULL;
}

void of_ref(open_file_t *f) {
    if (f != NULL && f->refcount != 0) {
        f->refcount++;
    }
}

void of_deref(open_file_t *f) {
    if (f == NULL || f->refcount == 0) {
        return;
    }
    f->refcount--;
    if (f->refcount == 0) {
        ob_deref(f->obj);
        f->obj    = NULL;
        f->offset = 0;
        f->access = 0;
        f->status = 0;
    }
}

/* --- handle tables ------------------------------------------------------ */

void handle_table_init(handle_t *table) {
    int i;

    for (i = 0; i < MAX_HANDLES; i++) {
        table[i].file  = NULL;
        table[i].flags = 0;
    }
}

int handle_alloc(handle_t *table, open_file_t *file, uint32 flags) {
    return handle_alloc_from(table, 0, file, flags);
}

int handle_alloc_from(handle_t *table, int min, open_file_t *file, uint32 flags) {
    int i, limit;

    if (file == NULL) {
        return -9;    /* -EBADF */
    }
    if (min < 0) {
        min = 0;
    }
    limit = handle_table_limit(table);
    if (min >= limit) {
        of_deref(file);
        return -24;   /* -EMFILE: the floor is already past the table */
    }
    /* Lowest free index at or above the floor. Not an optimisation - POSIX
     * guarantees it, and `cmd 2>&1` works by closing 1 and expecting the next
     * open to land there. */
    for (i = min; i < limit; i++) {
        if (table[i].file == NULL) {
            table[i].file  = file;   /* the caller's reference, transferred */
            table[i].flags = flags;
            return i;
        }
    }
    /* Consume on failure too. Otherwise every caller needs an error path
     * whose only job is to undo an allocation it never explicitly made, and
     * the one that forgets leaks a pool slot per failed open. */
    of_deref(file);
    return -24;       /* -EMFILE */
}

int handle_install_at(handle_t *table, int index, open_file_t *file, uint32 flags) {
    if (index < 0 || index >= handle_table_limit(table) || file == NULL) {
        if (file != NULL) {
            of_deref(file);   /* ownership is taken on failure too */
        }
        return -9;
    }
    /* dup2(fd, fd) is defined to be a no-op returning fd. Closing first and
     * reinstalling would work by accident here because the caller holds a
     * reference, but it would drop the flags and it stops working the moment
     * close does anything real. */
    if (table[index].file == file) {
        table[index].flags = flags;
        of_deref(file);          /* the transferred reference is redundant */
        return index;
    }
    if (table[index].file != NULL) {
        of_deref(table[index].file);
    }
    table[index].file  = file;   /* transferred */
    table[index].flags = flags;
    return index;
}

int handle_flags(const handle_t *table, int index) {
    if (index < 0 || index >= MAX_HANDLES || table[index].file == NULL) {
        return -9;    /* -EBADF */
    }
    return (int)table[index].flags;
}

int handle_set_flags(handle_t *table, int index, uint32 flags) {
    if (index < 0 || index >= MAX_HANDLES || table[index].file == NULL) {
        return -9;
    }
    table[index].flags = flags;
    return 0;
}

open_file_t *handle_get(const handle_t *table, int index) {
    if (index < 0 || index >= MAX_HANDLES) {
        return NULL;
    }
    return table[index].file;
}

int handle_close(handle_t *table, int index) {
    if (index < 0 || index >= MAX_HANDLES || table[index].file == NULL) {
        return -9;
    }
    of_deref(table[index].file);
    table[index].file  = NULL;
    table[index].flags = 0;
    return 0;
}

void handle_close_all(handle_t *table) {
    int i;

    for (i = 0; i < MAX_HANDLES; i++) {
        if (table[i].file != NULL) {
            of_deref(table[i].file);
            table[i].file  = NULL;
            table[i].flags = 0;
        }
    }
}

void handle_close_on_exec(handle_t *table) {
    int i;

    for (i = 0; i < MAX_HANDLES; i++) {
        if (table[i].file != NULL && (table[i].flags & HANDLE_CLOEXEC)) {
            of_deref(table[i].file);
            table[i].file  = NULL;
            table[i].flags = 0;
        }
    }
}

void handle_table_clone(handle_t *dst, const handle_t *src, int inheritable_only) {
    int i;

    handle_table_init(dst);
    for (i = 0; i < MAX_HANDLES; i++) {
        if (src[i].file == NULL) {
            continue;
        }
        if (inheritable_only && !(src[i].flags & HANDLE_INHERITABLE)) {
            continue;
        }
        /* Reference the same open instance rather than copying it. Two
         * processes sharing a descriptor share a file offset - that is what
         * makes `(cmd1; cmd2) > file` append rather than overwrite. */
        dst[i].file  = src[i].file;
        dst[i].flags = src[i].flags;
        of_ref(src[i].file);
    }
}
