/* Host tests for object.c.
 *
 * Pure bookkeeping - static pools, no heap, no I/O - so all of it runs here.
 * The cases worth the most are the refcount ones: a leaked reference is
 * invisible until the pool runs dry hours later, and a reference dropped once
 * too often hands the same object to two unrelated callers. Neither has a
 * symptom at the point of the mistake.
 */

#include <stdio.h>
#include <string.h>

#include "ns.h"
#include "object.h"
#include "typesk.h"

static int obj_failures;
static int destroy_calls;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL  %s\n", what);
        obj_failures++;
    } else {
        printf("  ok    %s\n", what);
    }
}

/* A type that records what happened to it. */
static char sink[256];
static uint64 sink_len;

static int64 test_write(object_t *obj, const void *buf, uint64 n, uint64 *offset) {
    const char *s = (const char *)buf;
    uint64 i;

    (void)obj;
    for (i = 0; i < n && sink_len < sizeof(sink) - 1; i++) {
        sink[sink_len++] = s[i];
    }
    sink[sink_len] = '\0';
    *offset += n;          /* a seekable type advances the shared position */
    return (int64)n;
}

static void test_destroy(object_t *obj) {
    (void)obj;
    destroy_calls++;
}

static const object_type_t test_type = {
    .name    = "test",
    .klass   = OBJ_FILE,
    .write   = test_write,
    .destroy = test_destroy
};

/* --- tests --------------------------------------------------------------- */

static void test_object_refcounts(void) {
    object_t *o;

    printf("\nobject: reference counting\n");
    destroy_calls = 0;

    o = ob_create(&test_type, NULL);
    check(o != NULL, "an object is created");
    check(o->refcount == 1, "and starts with one reference");

    ob_ref(o);
    check(o->refcount == 2, "a second reference is counted");

    ob_deref(o);
    check(o->refcount == 1 && destroy_calls == 0,
          "dropping one of two does not destroy it");

    ob_deref(o);
    check(destroy_calls == 1, "dropping the last one destroys it");

    /* Dropping past zero must not run destroy again. The pool would otherwise
     * hand a live slot to the next ob_create while a caller still held it. */
    ob_deref(o);
    check(destroy_calls == 1, "an extra deref is ignored, not a double free");
}

static void test_open_instance_holds_object(void) {
    object_t    *o;
    open_file_t *f;

    printf("\nobject: an open instance keeps its object alive\n");
    destroy_calls = 0;

    o = ob_create(&test_type, NULL);
    f = of_open(o, ACCESS_READ | ACCESS_WRITE);
    check(f != NULL, "the instance opens");
    check(o->refcount == 2, "and takes a reference on the object");

    ob_deref(o);   /* the creator lets go */
    check(destroy_calls == 0, "the object survives its creator releasing it");

    of_deref(f);
    check(destroy_calls == 1, "and dies when the last instance closes");
}

static void test_lowest_free_index(void) {
    handle_t table[MAX_HANDLES];
    object_t *o = ob_create(&test_type, NULL);
    int a, b, c, reused;

    printf("\nhandle: allocation always takes the lowest free index\n");
    handle_table_init(table);

    a = handle_alloc(table, of_open(o, ACCESS_WRITE), 0);
    b = handle_alloc(table, of_open(o, ACCESS_WRITE), 0);
    c = handle_alloc(table, of_open(o, ACCESS_WRITE), 0);
    check(a == 0 && b == 1 && c == 2, "indices are handed out in order");

    handle_close(table, 1);
    reused = handle_alloc(table, of_open(o, ACCESS_WRITE), 0);
    /* This is not tidiness. `cmd 2>&1` closes a descriptor and expects the
     * next open to land in the hole it left. */
    check(reused == 1, "a closed index is reused before a fresh one");

    handle_close_all(table);
    ob_deref(o);
}

static void test_dup_shares_the_offset(void) {
    handle_t table[MAX_HANDLES];
    object_t *o = ob_create(&test_type, NULL);
    open_file_t *f;
    int fd, dupfd;

    printf("\nhandle: dup shares a file offset, which is the whole design\n");
    handle_table_init(table);
    sink_len = 0;
    sink[0] = '\0';

    fd = handle_alloc(table, of_open(o, ACCESS_WRITE), 0);
    f  = handle_get(table, fd);

    /* dup: a second index onto the SAME open instance. of_ref because
     * handle_alloc takes the reference it is given. */
    of_ref(f);
    dupfd = handle_alloc(table, f, 0);
    check(dupfd != fd, "dup produces a different index");
    check(handle_get(table, dupfd) == handle_get(table, fd),
          "but both indices reach one open instance");

    /* Writing through one advances the position the other sees. Two levels
     * instead of three would give each descriptor its own offset here, and a
     * shell redirecting the same file twice would overwrite its own output. */
    {
        open_file_t *a = handle_get(table, fd);
        open_file_t *b = handle_get(table, dupfd);
        a->obj->type->write(a->obj, "abc", 3, &a->offset);
        check(b->offset == 3, "a write through one moves the other's offset");
        b->obj->type->write(b->obj, "de", 2, &b->offset);
        check(a->offset == 5, "and back again");
    }

    /* Closing one must not disturb the other. */
    handle_close(table, fd);
    check(handle_get(table, dupfd) != NULL,
          "closing one index leaves the other open");
    check(handle_get(table, dupfd)->refcount == 1,
          "and the instance is down to its last reference");

    handle_close_all(table);
    ob_deref(o);
}

static void test_install_at(void) {
    handle_t table[MAX_HANDLES];
    object_t *o = ob_create(&test_type, NULL);
    open_file_t *first, *second;

    printf("\nhandle: dup2 semantics\n");
    handle_table_init(table);

    first  = of_open(o, ACCESS_WRITE);
    second = of_open(o, ACCESS_WRITE);
    of_ref(first);    /* the test keeps its own reference to both */
    of_ref(second);
    handle_alloc(table, first, 0);
    handle_alloc(table, second, 0);

    of_ref(first);
    check(handle_install_at(table, 1, first, 0) == 1,
          "installing over an open index succeeds");
    check(handle_get(table, 1) == first, "and the index now names the new one");
    check(second->refcount == 1,
          "the displaced instance lost the table's reference");

    /* dup2(fd, fd): defined as a no-op returning fd. Close-then-install would
     * destroy the instance between the two steps. */
    of_ref(first);
    check(handle_install_at(table, 1, first, 0) == 1, "dup2(fd, fd) returns fd");
    check(handle_get(table, 1) == first, "and leaves it open");

    handle_close_all(table);
    of_deref(first);
    of_deref(second);
    ob_deref(o);
}

static void test_close_on_exec(void) {
    handle_t table[MAX_HANDLES];
    object_t *o = ob_create(&test_type, NULL);

    printf("\nhandle: close-on-exec\n");
    handle_table_init(table);

    handle_alloc(table, of_open(o, ACCESS_WRITE), 0);
    handle_alloc(table, of_open(o, ACCESS_WRITE), HANDLE_CLOEXEC);
    handle_alloc(table, of_open(o, ACCESS_WRITE), 0);

    handle_close_on_exec(table);
    check(handle_get(table, 0) != NULL, "an ordinary descriptor survives exec");
    check(handle_get(table, 1) == NULL, "a close-on-exec one does not");
    check(handle_get(table, 2) != NULL, "and the ones after it are untouched");

    handle_close_all(table);
    ob_deref(o);
}

static void test_clone_semantics(void) {
    handle_t parent[MAX_HANDLES], child[MAX_HANDLES];
    object_t *o = ob_create(&test_type, NULL);
    open_file_t *shared;

    printf("\nhandle: cloning a table for a child process\n");
    handle_table_init(parent);

    shared = of_open(o, ACCESS_WRITE);
    handle_alloc(parent, shared, 0);
    handle_alloc(parent, of_open(o, ACCESS_WRITE), HANDLE_INHERITABLE);

    /* POSIX fork: everything comes across. */
    handle_table_clone(child, parent, 0);
    check(handle_get(child, 0) == handle_get(parent, 0),
          "fork shares the open instance, so parent and child share an offset");
    /* Two: one reference per table. The of_open reference was transferred to
     * the parent's table by handle_alloc, so there is no third holder. */
    check(shared->refcount == 2,
          "and the reference count is exactly one per table holding it");
    handle_close_all(child);

    /* NT CreateProcess: only handles marked inheritable. */
    handle_table_clone(child, parent, 1);
    check(handle_get(child, 0) == NULL,
          "an NT-style clone skips a non-inheritable handle");
    check(handle_get(child, 1) != NULL,
          "and keeps the one marked inheritable");
    handle_close_all(child);

    handle_close_all(parent);
    ob_deref(o);
}

static void test_bad_indices(void) {
    handle_t table[MAX_HANDLES];

    printf("\nhandle: out of range and closed indices\n");
    handle_table_init(table);

    check(handle_get(table, -1) == NULL, "a negative index is NULL");
    check(handle_get(table, MAX_HANDLES) == NULL, "past the end is NULL");
    check(handle_get(table, 0) == NULL, "an unopened index is NULL");
    check(handle_close(table, 0) == -9, "closing an unopened index is -EBADF");
    check(handle_close(table, 9999) == -9, "closing garbage is -EBADF");
}

static void test_exhaustion(void) {
    handle_t table[MAX_HANDLES];
    object_t *o = ob_create(&test_type, NULL);
    int i, last = 0;

    printf("\nhandle: a full table reports EMFILE\n");
    handle_table_init(table);

    for (i = 0; i < MAX_HANDLES + 2; i++) {
        open_file_t *f = of_open(o, ACCESS_WRITE);
        if (f == NULL) {
            break;
        }
        last = handle_alloc(table, f, 0);   /* consumes f either way */
        if (last < 0) {
            break;
        }
    }
    check(last == -24, "filling the table returns -EMFILE rather than wrapping");

    handle_close_all(table);
    ob_deref(o);
}

/* A temporary name (OB_FLAG_TEMPORARY, NT's default for a named object a
 * program creates) lasts exactly as long as an open instance of the object:
 * not the creator's reference, not a kernel reference, an OPEN. */
static void test_temporary_name(void) {
    const char *name = "\\BaseNamedObjects\\HostTempTest";
    const char *keep = "\\BaseNamedObjects\\HostKeepTest";
    object_t *o, *k, *found = NULL;
    open_file_t *a, *b, *kf;

    printf("\nobject: a temporary name lasts as long as an open instance\n");
    ns_init();
    (void)ns_mkdir("\\BaseNamedObjects");
    destroy_calls = 0;

    o = ob_create(&test_type, NULL);
    check(ns_insert(name, o) == 0, "the name goes in");
    ob_make_temporary(o);
    a = of_open(o, ACCESS_READ);
    ob_deref(o);                       /* the creator's handle holds it now */
    check(o->handle_count == 1, "one open instance is counted");

    /* A kernel reference - a waiter, say - is not a handle. */
    ob_ref(o);
    b = of_open(o, ACCESS_READ);       /* a second program opens it by name */
    check(o->handle_count == 2, "a second open is counted");

    of_deref(a);
    check(ns_lookup(name, &found, NULL, 0) == 0 && found == o,
          "the name survives while one instance is still open");
    ob_deref(found);                   /* ns_lookup's reference */

    of_deref(b);
    check(ns_lookup_entry(name) == NULL,
          "the last close removes the name");
    check(destroy_calls == 0,
          "and the object outlives it while a kernel reference remains");
    ob_deref(o);
    check(destroy_calls == 1, "then dies with that reference");

    /* The same name is free for a new object, which is the point. */
    o = ob_create(&test_type, NULL);
    check(ns_insert(name, o) == 0, "the freed name can be taken again");
    ns_remove(name);
    ob_deref(o);

    /* Without the flag a name is permanent - devices, \\ObjectTypes. */
    k = ob_create(&test_type, NULL);
    check(ns_insert(keep, k) == 0, "a permanent name goes in");
    kf = of_open(k, ACCESS_READ);
    ob_deref(k);
    of_deref(kf);
    check(ns_lookup_entry(keep) != NULL,
          "and stays when its last instance closes");
    ns_remove(keep);
}

int object_run_tests(void) {
    test_object_refcounts();
    test_open_instance_holds_object();
    test_lowest_free_index();
    test_dup_shares_the_offset();
    test_install_at();
    test_close_on_exec();
    test_clone_semantics();
    test_bad_indices();
    test_exhaustion();
    test_temporary_name();
    return obj_failures;
}
