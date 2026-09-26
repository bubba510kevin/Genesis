/* Host tests for the object namespace.
 *
 * The interesting behaviour here is not "can it store a name" - it is what
 * happens at the seams: a link that lands mid-path, a path that continues
 * past the object it named, a link that points at itself. Those are the cases
 * that are cheap to test here and very expensive to debug on the machine,
 * because a namespace bug looks like an unrelated device failing to open.
 */

#include <stdio.h>

#include "devices.h"
#include "ns.h"
#include "object.h"
#include "typesk.h"

static int failures;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

static void check_eq(long got, long want, const char *what) {
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s  (got %ld, wanted %ld)\n", what, got, want);
        failures++;
    }
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Two distinguishable stand-in device types. Nothing calls read or write;
 * what matters is that lookup returns the object that was inserted and not
 * some other one. */
static const object_type_t fake_console = {
    .name = "console", .klass = OBJ_CONSOLE
};
static const object_type_t fake_volume = {
    .name = "volume", .klass = OBJ_DIRECTORY
};

/* The namespace has no teardown and is not supposed to: names live for the
 * life of the kernel. So these tests build ONE namespace between them rather
 * than one each, in the order the boot path would - devices first, then the
 * links that point at them. */
static object_t *console;
static object_t *volume;

static void test_basic_names(void) {
    object_t *found = NULL;
    char rem[NS_PATH_MAX];

    printf("\nns: names and lookup\n");

    ns_init();
    console = ob_create(&fake_console, NULL);
    volume  = ob_create(&fake_volume, NULL);
    check(console != NULL && volume != NULL, "two objects exist to be named");

    check_eq(ns_insert("\\Device\\Console", console), 0, "insert \\Device\\Console");
    check_eq(ns_insert("\\Device\\HarddiskVolume1", volume), 0,
             "insert \\Device\\HarddiskVolume1");
    check_eq(ns_insert("\\Device\\Console", console), -17,
             "a duplicate name is -EEXIST, not a silent replace");
    check_eq(ns_insert("\\NoSuchDir\\Thing", console), -2,
             "insert under a missing directory is -ENOENT");

    check_eq(ns_lookup("\\Device\\Console", &found, rem, sizeof(rem)), 0,
             "lookup finds it");
    check(found == console, "and returns the object that was inserted");
    check(rem[0] == '\0', "with no remainder");
    ob_deref(found);

    /* The reference the namespace holds is the point of that ob_ref: without
     * it, the object would be destroyed while the name still resolved. */
    check(console->refcount >= 2, "the namespace holds its own reference");

    check_eq(ns_lookup("\\Device\\Nope", &found, rem, sizeof(rem)), -2,
             "a missing name is -ENOENT");
    check_eq(ns_lookup("\\Device", &found, rem, sizeof(rem)), -21,
             "a directory is -EISDIR - it is not something to open");
    check_eq(ns_lookup("Device\\Console", &found, rem, sizeof(rem)), -22,
             "a relative path is -EINVAL: there is no working directory here");
}

static void test_links(void) {
    object_t *found = NULL;
    char rem[NS_PATH_MAX];

    printf("\nns: symbolic links\n");

    check_eq(ns_link("\\??\\CON", "\\Device\\Console"), 0, "link \\??\\CON");
    check_eq(ns_link("\\??\\C:", "\\Device\\HarddiskVolume1"), 0, "link \\??\\C:");

    check_eq(ns_lookup("\\??\\CON", &found, rem, sizeof(rem)), 0,
             "a link resolves to its target's object");
    check(found == console, "and it is the right object");
    ob_deref(found);

    /* Case-insensitivity is not cosmetic: \\??\\c: and \\??\\C: naming
     * different drives is the bug this prevents. */
    check_eq(ns_lookup("\\??\\con", &found, rem, sizeof(rem)), 0,
             "lookup is case-insensitive");
    ob_deref(found);

    check_eq(ns_link("\\??\\Dangling", "\\Device\\NotYetCreated"), 0,
             "a link to something that does not exist is allowed");
    check_eq(ns_lookup("\\??\\Dangling", &found, rem, sizeof(rem)), -2,
             "and resolving it is -ENOENT until the target appears");

    /* Late binding: the link was made before the device. This is the whole
     * reason ns_link does not resolve its target. */
    ns_insert("\\Device\\NotYetCreated", console);
    check_eq(ns_lookup("\\??\\Dangling", &found, rem, sizeof(rem)), 0,
             "creating the target makes the existing link work");
    ob_deref(found);

    check_eq(ns_link("\\??\\Loop", "\\??\\Loop"), 0, "a self-referential link");
    check_eq(ns_lookup("\\??\\Loop", &found, rem, sizeof(rem)), -40,
             "resolves to -ELOOP rather than hanging the kernel");
}

static void test_unparsed_remainder(void) {
    object_t *found = NULL;
    char rem[NS_PATH_MAX];

    printf("\nns: the unparsed remainder\n");

    check_eq(ns_lookup("\\Device\\HarddiskVolume1\\etc\\motd", &found,
                       rem, sizeof(rem)), 0,
             "a path continuing past an object still resolves");
    check(found == volume, "to that object");
    /* The separator stays on the front: what the device receives is an
     * absolute path within itself, not a fragment it has to guess the
     * rooting of. */
    check(str_eq(rem, "\\etc\\motd"),
          "with everything after it handed back unparsed");
    ob_deref(found);

    /* Through a link, which is the case that actually matters: the link
     * target and the remainder have to be spliced in the right order. */
    check_eq(ns_lookup("\\??\\C:\\bin\\busybox", &found, rem, sizeof(rem)), 0,
             "and the same through \\??\\C:");
    check(found == volume, "reaching the volume the letter points at");
    check(str_eq(rem, "\\bin\\busybox"), "with the path on the volume intact");
    ob_deref(found);

    /* A caller with nowhere to put a remainder must be refused rather than
     * handed the device and a silently discarded path. */
    check_eq(ns_lookup("\\??\\C:\\bin\\busybox", &found, NULL, 0), -2,
             "a caller that cannot take a remainder is refused, not truncated");
}

static int count_cb(const ns_entry_t *e, void *ctx) {
    int *n = (int *)ctx;
    (void)e;
    (*n)++;
    return 0;
}

static int stop_cb(const ns_entry_t *e, void *ctx) {
    (void)e;
    (void)ctx;
    return 5;
}

static void test_iterate(void) {
    int n = 0;

    printf("\nns: iteration\n");

    check_eq(ns_iterate("\\Device", count_cb, &n), 0, "iterating \\Device works");
    check(n >= 1, "and it has at least the console in it");

    check_eq(ns_iterate("\\Device", stop_cb, NULL), 5,
             "a callback returning non-zero stops the walk and is returned");
    check_eq(ns_iterate("\\Device\\Console", count_cb, &n), -20,
             "iterating an object rather than a directory is -ENOTDIR");
    check_eq(ns_iterate("\\Nope", count_cb, &n), -2, "and a missing path is -ENOENT");

    check(ns_lookup_entry("\\??\\CON") != NULL, "lookup_entry finds a link");
    check(ns_lookup_entry("\\??\\CON")->kind == NS_LINK,
          "and does NOT follow it - a listing must show the link itself");
    check(ns_lookup_entry("\\Device")->kind == NS_DIRECTORY,
          "and reports a directory as a directory");
}

static void test_mkdir(void) {
    printf("\nns: directories\n");

    check(ns_mkdir("\\Device\\Storage\\Volumes") != NULL,
          "mkdir creates every missing level");
    check(ns_lookup_entry("\\Device\\Storage") != NULL, "the middle one exists");
    check(ns_mkdir("\\Device\\Storage") != NULL, "mkdir of an existing directory is fine");
    check(ns_root() != NULL, "the root exists");
}

/* The device directory, listed through the object it is opened as. The record
 * packing is fiddly - 8-aligned reclen, a resume index that must NOT advance
 * past an entry that did not fit - and it is much cheaper to get wrong here
 * than on the machine, where it shows up as ls silently skipping a device. */
static void test_device_directory(void) {
    object_t *dir;
    uint8     buf[512];
    uint64    pos = 0;
    int64     n;
    int       seen_console = 0, seen_link = 0;
    uint64    off;

    printf("\nns: listing the device directory\n");

    dir = nsdir_open("\\Device");
    check(dir != NULL, "a namespace directory opens as an object");
    check(nsdir_open("\\Device\\Console") == NULL,
          "but an object in it does not - that is not a directory");
    check(nsdir_open("\\Nope") == NULL, "and neither does a missing path");
    if (dir == NULL) {
        return;
    }

    n = dir->type->getdents(dir, buf, sizeof(buf), &pos);
    check(n > 0, "getdents returns records");

    for (off = 0; off < (uint64)n; ) {
        uint16      reclen = *(uint16 *)(buf + off + 16);
        const char *name   = (const char *)(buf + off + 19);

        check(reclen % 8 == 0, "every record length is 8-aligned");
        if (str_eq(name, "Console")) seen_console = 1;
        if (reclen == 0) break;
        off += reclen;
    }
    check(off == (uint64)n, "and the records exactly fill the returned length");
    check(seen_console, "the console is listed");

    /* A second call with the same position returns nothing more: the walk is
     * finished, not restarted. */
    n = dir->type->getdents(dir, buf, sizeof(buf), &pos);
    check(n == 0, "a second call returns nothing - the listing is complete");

    /* A buffer too small for everything must hand back a resume point that
     * loses nothing. */
    pos = 0;
    n = dir->type->getdents(dir, buf, 40, &pos);
    check(n > 0 && n <= 40, "a short buffer returns what fits");
    check(pos >= 1, "and leaves a resume point past what it emitted");

    (void)seen_link;
    ob_deref(dir);
}


/* /dev as a view onto BOTH \??\ and \Device\.
 *
 * This is the regression test for the bug that made a device registered in
 * \Device\ invisible under /dev: /dev used to be a plain synonym for \??\,
 * so a name only existed there if somebody had also written a link by hand.
 * The namespace in this file deliberately has \Device\ entries with no link
 * (HarddiskVolume1, and Console under its own name) which is exactly the case
 * that used to fail.
 *
 * Runs last, because it adds a link of its own to check deduplication. */
static void test_dev_view(void) {
    object_t *obj = NULL;
    char      rem[NS_PATH_MAX];
    uint8     buf[512];
    uint64    pos = 0;
    int64     n;
    uint64    off;
    int       seen_con = 0, seen_device_only = 0, console_count = 0;

    printf("\nns: /dev as a view onto the namespace\n");

    check_eq(dev_lookup("/dev/CON", &obj, rem, sizeof(rem)), 0,
             "a DOS name in \\??\\ resolves");
    check(obj == console, "to the object behind it");
    ob_deref(obj);

    /* The fix. \Device\HarddiskVolume1 has no \??\ link in this namespace,
     * and used to be unreachable through /dev for that reason alone. */
    check_eq(dev_lookup("/dev/HarddiskVolume1", &obj, rem, sizeof(rem)), 0,
             "a device with NO \\??\\ link still resolves under /dev");
    check(obj == volume, "to the device that was registered");
    ob_deref(obj);

    check_eq(dev_lookup("/dev/harddiskvolume1", &obj, rem, sizeof(rem)), 0,
             "and the fallback is case-insensitive like everything else");
    ob_deref(obj);

    /* \??\ is searched first, so an alias wins where one exists. */
    check_eq(dev_lookup("/dev/C:", &obj, rem, sizeof(rem)), 0,
             "\\??\\C: resolves through /dev too");
    check(obj == volume, "to the same volume the letter names");
    ob_deref(obj);

    check_eq(dev_lookup("/dev/C:/bin/busybox", &obj, rem, sizeof(rem)), 0,
             "a path continuing past the device resolves");
    check(str_eq(rem, "\\bin\\busybox"), "and hands back the unparsed remainder");
    ob_deref(obj);

    check_eq(dev_lookup("/dev/nosuchdevice", &obj, rem, sizeof(rem)), -2,
             "a name in neither directory is -ENOENT");
    check_eq(dev_lookup("/dev/Loop", &obj, rem, sizeof(rem)), -40,
             "and a real error is reported, not retried as a miss");

    /* /dev itself. It is not an entry in the namespace at all - there is no
     * single directory it names - so it opens as the merged view. */
    check_eq(dev_lookup("/dev", &obj, rem, sizeof(rem)), 0, "/dev itself opens");
    check(ob_is_directory(obj), "and reports as a directory");
    check(dev_is_directory(obj), "which is what stat asks");
    if (obj == NULL) {
        return;
    }

    n = obj->type->getdents(obj, buf, sizeof(buf), &pos);
    check(n > 0, "listing it returns records");
    for (off = 0; off < (uint64)n; ) {
        uint16      reclen = *(uint16 *)(buf + off + 16);
        const char *name   = (const char *)(buf + off + 19);

        if (str_eq(name, "CON"))             seen_con = 1;
        if (str_eq(name, "HarddiskVolume1")) seen_device_only = 1;
        if (reclen == 0) break;
        off += reclen;
    }
    check(seen_con, "a \\??\\ name is listed");
    check(seen_device_only,
          "and so is a \\Device\\ name with no link - one list, not two");
    ob_deref(obj);

    /* Deduplication: give the console a \??\ name matching its \Device\ one
     * and it must still appear exactly once. */
    check_eq(ns_link("\\??\\Console", "\\Device\\Console"), 0,
             "add a \\??\\ alias spelled like the device itself");
    check_eq(dev_lookup("/dev", &obj, rem, sizeof(rem)), 0, "/dev reopens");
    if (obj == NULL) {
        return;
    }
    pos = 0;
    n = obj->type->getdents(obj, buf, sizeof(buf), &pos);
    for (off = 0; off < (uint64)n; ) {
        uint16      reclen = *(uint16 *)(buf + off + 16);
        const char *name   = (const char *)(buf + off + 19);

        if (str_eq(name, "Console")) console_count++;
        if (reclen == 0) break;
        off += reclen;
    }
    check_eq(console_count, 1, "the device is listed once, not once per name");
    ob_deref(obj);
}

/* Paging the merged listing. The resume position counts entries EMITTED, and
 * it has to keep meaning that across the seam between the two directories -
 * a buffer that fills mid-listing must lose nothing and repeat nothing. */
static void test_dev_paging(void) {
    object_t *whole = NULL, *paged = NULL;
    char      rem[NS_PATH_MAX];
    uint8     big[512], small[64];
    uint64    pos = 0;
    int64     n;
    uint64    off;
    int       total = 0, walked = 0, rounds = 0;

    printf("\nns: paging the merged /dev listing\n");

    check_eq(dev_lookup("/dev", &whole, rem, sizeof(rem)), 0, "/dev opens");
    if (whole == NULL) return;
    n = whole->type->getdents(whole, big, sizeof(big), &pos);
    for (off = 0; off < (uint64)n; ) {
        uint16 reclen = *(uint16 *)(big + off + 16);
        total++;
        if (reclen == 0) break;
        off += reclen;
    }
    ob_deref(whole);
    check(total > 0, "a large buffer lists everything in one call");

    check_eq(dev_lookup("/dev", &paged, rem, sizeof(rem)), 0, "and reopens");
    if (paged == NULL) return;
    pos = 0;
    for (;;) {
        n = paged->type->getdents(paged, small, sizeof(small), &pos);
        if (n <= 0) break;
        for (off = 0; off < (uint64)n; ) {
            uint16 reclen = *(uint16 *)(small + off + 16);
            walked++;
            if (reclen == 0) break;
            off += reclen;
        }
        if (++rounds > 64) break;        /* a stuck position, not a listing */
    }
    check_eq(walked, total,
             "a small buffer sees exactly the same entries, none lost or doubled");

    /* A buffer too small for even one record must not answer 0: getdents64
     * has only one meaning for 0, and it is end of directory. */
    pos = 0;
    check_eq((long)paged->type->getdents(paged, small, 8, &pos), -22,
             "a buffer too small for one record is -EINVAL, not \"empty\"");
    ob_deref(paged);
}

int ns_run_tests(void) {
    failures = 0;
    test_basic_names();
    test_links();
    test_unparsed_remainder();
    test_iterate();
    test_mkdir();
    test_device_directory();
    test_dev_view();
    test_dev_paging();
    return failures;
}
