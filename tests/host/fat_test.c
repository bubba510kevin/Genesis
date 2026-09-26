/* Host tests for fat.c.
 *
 * The real driver, reading a real FAT16 image, with the disk replaced by a
 * file and the kernel heap by malloc. Path resolution is the part worth
 * testing this way: an error in it produces "file not found" for a file that
 * is plainly there, which on target tells you nothing about which of the four
 * layers - ATA, cluster chain, directory walk, name matching - was wrong.
 *
 * A caveat about what this proves. The fixture is written by tools/fatfs.py,
 * which is mine, so a shared misunderstanding of the format would pass here.
 * On a machine with mkfs.fat, build.py formats with that instead and the
 * cross-check is real; here it is only self-consistency. Where the format is
 * subtle - the "-2" in cluster addressing, the root's fixed array, end-of-
 * directory markers - the tests below assert against values computed by hand
 * rather than against whatever the writer happened to produce.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ata.h"
#include "fat.h"
#include "kheap.h"
#include "dev_stub.h"
#include "typesk.h"

/* --- the disk ------------------------------------------------------------ */

/* The disk is a device_t now, not an ata_device_t.
 *
 * fat.c reads through dev_read, which is what lets a FAT filesystem sit on a
 * PARTITION rather than only at sector 0 - a driver holding an ata_device_t
 * can only ever address the whole disk. The stub moved to dev_stub.c and is
 * shared with part_test.c; one stub for both is possible precisely because
 * they now read through the same interface.
 *
 * ata_read and ata_write are still defined because ata.h is still included for
 * ATA_SECTOR_SIZE and other translation units may reference them. Nothing in
 * fat.c calls them any more; if something does, this abort says so at once
 * rather than letting a stale path work by accident. */
int ata_read(ata_device_t *dev, uint32 lba, uint8 count, void *buffer) {
    (void)dev; (void)lba; (void)count; (void)buffer;
    printf("  FAIL  fat.c reached ata_read directly\n");
    abort();
}

int ata_write(ata_device_t *dev, uint32 lba, uint8 count, const void *buffer) {
    (void)dev; (void)lba; (void)count; (void)buffer;
    return ATA_ERR_TIMEOUT;   /* the driver is read-only; nothing should call this */
}

/* --- the heap ------------------------------------------------------------ */

void *kmalloc(kh_size size)  { return malloc(size); }
void  kfree(void *p)         { free(p); }

/* --- harness -------------------------------------------------------------- */

static int fat_failures;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL  %s\n", what);
        fat_failures++;
    } else {
        printf("  ok    %s\n", what);
    }
}

static fat_volume_t vol;
static device_t     dev;
static dev_stub_t   dev_backing;

/* --- listing capture ------------------------------------------------------ */

static char listed[32][16];
static int  listed_n;

static void collect(const char *name, uint32 size, uint8 attr) {
    (void)size; (void)attr;
    if (listed_n < 32) {
        snprintf(listed[listed_n], sizeof(listed[0]), "%s", name);
        listed_n++;
    }
}

static int listed_has(const char *name) {
    for (int i = 0; i < listed_n; i++) {
        if (strcmp(listed[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* --- tests ---------------------------------------------------------------- */

static void test_mount(void) {
    printf("\nfat: mount\n");
    check(fat_mount(&vol, &dev) == FAT_OK, "the image is recognised as FAT16");
    check(vol.bytes_per_sector == 512, "512-byte sectors");
    check(vol.root_entries > 0, "the root directory has a capacity");
    /* Cluster 2 is the first data cluster, so its LBA must be data_start
     * exactly - the "- 2" in the address calculation is the classic place a
     * FAT driver is off by two clusters and reads plausible-looking garbage. */
    check(vol.data_start > vol.root_start, "the data area follows the root");
}

static void test_root_listing(void) {
    printf("\nfat: the root lists its directories\n");
    listed_n = 0;
    check(fat_list(&vol, "/", collect) == FAT_OK, "listing the root succeeds");
    check(listed_has("BIN"), "BIN is present");
    check(listed_has("USR"), "USR is present");
    check(listed_has("WSR"), "WSR is present");
    check(!listed_has("BUSYBOX"),
          "a file one level down does not appear in the root");
}

static void test_subdirectory_listing(void) {
    printf("\nfat: a subdirectory is a cluster chain, not a fixed array\n");
    listed_n = 0;
    check(fat_list(&vol, "/usr/bin", collect) == FAT_OK, "listing /usr/bin succeeds");
    check(listed_has("TINY"), "the file in it is found");
    check(listed_has("."), "the . entry is there");
    check(listed_has(".."), "the .. entry is there");
}

static void test_lookup(void) {
    fat_entry_t e;

    printf("\nfat: path resolution\n");

    check(fat_lookup(&vol, "/", &e) == FAT_OK && (e.attr & FAT_ATTR_DIRECTORY),
          "\"/\" resolves to a directory");
    check(fat_lookup(&vol, "/bin/busybox", &e) == FAT_OK,
          "/bin/busybox resolves");
    check(e.size == 153600, "and reports the size it was written with");
    check(!(e.attr & FAT_ATTR_DIRECTORY), "and is not a directory");

    check(fat_lookup(&vol, "/wsr/system32/kernel32.dll", &e) == FAT_OK,
          "a three-deep path resolves");
    check(e.size == 9000, "with the right size");

    check(fat_lookup(&vol, "/BIN/BUSYBOX", &e) == FAT_OK,
          "matching is case-insensitive");
    check(fat_lookup(&vol, "//bin//busybox", &e) == FAT_OK,
          "repeated slashes are skipped");
    check(fat_lookup(&vol, "/bin/", &e) == FAT_OK,
          "a trailing slash names the directory");
}

static void test_lookup_failures(void) {
    fat_entry_t e;

    printf("\nfat: lookups that should fail, and how\n");

    check(fat_lookup(&vol, "/bin/nope", &e) == FAT_ERR_NOTFOUND,
          "a missing name is NOTFOUND");
    check(fat_lookup(&vol, "/nosuchdir/x", &e) == FAT_ERR_NOTFOUND,
          "a missing directory is NOTFOUND");
    /* The distinction that matters to a shell: "/bin/busybox/x" is not a
     * missing file, it is a path that runs through a file. */
    check(fat_lookup(&vol, "/bin/busybox/x", &e) == FAT_ERR_NOTDIR,
          "descending through a file is NOTDIR, not NOTFOUND");
    check(fat_lookup(&vol, "bin/busybox", &e) == FAT_ERR_BADPATH,
          "a relative path is rejected - there is no cwd at this layer");
    check(fat_lookup(&vol, "/averyverylongname.text", &e) == FAT_ERR_BADPATH,
          "a component too long for 8.3 is rejected, not truncated");

    /* --- the truncation gap the 12-character bound does not cover --------
     *
     * The check above catches a component longer than 8.3-plus-a-dot. It
     * cannot catch one that is short enough to pass and still has a stem
     * longer than eight - and truncating that produces a WRONG ANSWER rather
     * than an error.
     *
     * "system321" is nine characters, so it passes the bound. Truncated to
     * eight it is "SYSTEM32", which is a directory that really exists in the
     * fixture. Before fat_name_to_entry checked its bounds, this lookup
     * SUCCEEDED and handed back that directory.
     *
     * It needs a target with no extension to collide - with one, the key
     * needs 9+1+3 = 13 characters and the bound catches it first - which is
     * precisely the case a directory name hits. */
    check(fat_lookup(&vol, "/wsr/system321", &e) == FAT_ERR_NOTFOUND,
          "a 9-character stem is not truncated onto a real 8-character name");
    check(fat_lookup(&vol, "/wsr/system32", &e) == FAT_OK,
          "and the real name still resolves (the control that makes the "
          "check above mean something)");
    check(fat_lookup(&vol, "/bin/a.b.c", &e) == FAT_ERR_NOTFOUND,
          "a second dot is not representable, so not found");
}

static void test_read(void) {
    static uint8 buf[200000];
    uint32 size = 0;
    int rc;

    printf("\nfat: reading file contents\n");

    rc = fat_read_path(&vol, "/etc/motd", buf, sizeof(buf), &size);
    check(rc == FAT_OK, "reading /etc/motd succeeds");
    check(size == 21, "the size is right");
    check(memcmp(buf, "hello from /etc/motd\n", 21) == 0, "the contents are right");

    /* 153600 bytes spans many clusters, so this exercises the chain rather
     * than a single-cluster read. The pattern repeats every 256 bytes, so a
     * cluster followed out of order shows up immediately. */
    rc = fat_read_path(&vol, "/bin/busybox", buf, sizeof(buf), &size);
    check(rc == FAT_OK && size == 153600, "a multi-cluster file reads back");
    int pattern_ok = 1;
    for (uint32 i = 0; i < size; i++) {
        if (buf[i] != (uint8)(i % 256)) {
            pattern_ok = 0;
            break;
        }
    }
    check(pattern_ok, "every byte is in the right place across the chain");

    size = 12345;
    rc = fat_read_path(&vol, "/etc/empty", buf, sizeof(buf), &size);
    check(rc == FAT_OK && size == 0,
          "an empty file reads as zero bytes rather than following cluster 0");

    rc = fat_read_path(&vol, "/etc", buf, sizeof(buf), &size);
    check(rc == FAT_ERR_ISDIR, "reading a directory as a file is refused");

    rc = fat_read_path(&vol, "/bin/busybox", buf, 100, &size);
    check(rc == FAT_ERR_TOOBIG && size == 153600,
          "a short buffer is refused but still reports the size needed");
}

static void test_size_probe(void) {
    uint32 size = 0;

    printf("\nfat: the size-then-allocate pattern flk.c uses\n");
    check(fat_read_path(&vol, "/bin/busybox", NULL, 0, &size) == FAT_ERR_TOOBIG,
          "asking with a zero buffer is refused");
    check(size == 153600, "but fills in the size, which is the point");
}

static int count_visit(const fat_entry_t *e, void *ctx) {
    int *n = (int *)ctx;
    (void)e;
    (*n)++;
    return 0;
}

static int stop_visit(const fat_entry_t *e, void *ctx) {
    int *n = (int *)ctx;
    (void)e;
    (*n)++;
    return 1;      /* stop after the first */
}

static void test_partial_reads(void) {
    static uint8 buf[4096];
    fat_entry_t ent;
    uint32 got = 0;

    printf("\nfat: reading at an offset, which is what read(2) needs\n");

    check(fat_lookup(&vol, "/bin/busybox", &ent) == FAT_OK, "the file resolves");

    check(fat_read_entry_at(&vol, &ent, 0, buf, 16, &got) == FAT_OK && got == 16,
          "a 16-byte read from the start returns 16 bytes");
    check(buf[0] == 0 && buf[15] == 15, "and the right ones");

    /* Offset 5000 is inside the third cluster of a 4KB-cluster volume, so
     * this exercises the chain walk rather than a single-cluster read. */
    check(fat_read_entry_at(&vol, &ent, 5000, buf, 8, &got) == FAT_OK && got == 8,
          "a read from the middle of the chain succeeds");
    {
        int ok = 1;
        for (uint32 i = 0; i < 8; i++) {
            if (buf[i] != (uint8)((5000 + i) % 256)) {
                ok = 0;
            }
        }
        check(ok, "and lands on exactly the right bytes");
    }

    /* A read straddling a cluster boundary is the case an off-by-one in the
     * within-cluster arithmetic breaks, and only that case. */
    check(fat_read_entry_at(&vol, &ent, 4090, buf, 12, &got) == FAT_OK && got == 12,
          "a read spanning a cluster boundary returns everything asked for");
    {
        int ok = 1;
        for (uint32 i = 0; i < 12; i++) {
            if (buf[i] != (uint8)((4090 + i) % 256)) {
                ok = 0;
            }
        }
        check(ok, "with no bytes lost or repeated at the seam");
    }

    check(fat_read_entry_at(&vol, &ent, 153590, buf, 100, &got) == FAT_OK && got == 10,
          "a read past the end is short, not an error");
    check(fat_read_entry_at(&vol, &ent, 153600, buf, 100, &got) == FAT_OK && got == 0,
          "a read at exactly EOF returns zero bytes");
    check(fat_read_entry_at(&vol, &ent, 999999, buf, 100, &got) == FAT_OK && got == 0,
          "and one far past it does too");

    check(fat_lookup(&vol, "/etc", &ent) == FAT_OK, "a directory resolves");
    check(fat_read_entry_at(&vol, &ent, 0, buf, 16, &got) == FAT_ERR_ISDIR,
          "reading a directory through the file path is refused");
}

static void test_iteration(void) {
    int n = 0;

    printf("\nfat: iteration with a context\n");

    check(fat_iterate(&vol, "/usr/bin", count_visit, &n) == FAT_OK,
          "iterating a subdirectory succeeds");
    check(n == 3, "it saw ., .. and the one file");

    n = 0;
    fat_iterate(&vol, "/usr/bin", stop_visit, &n);
    check(n == 1, "a callback returning non-zero stops the walk");

    n = 0;
    check(fat_iterate(&vol, "/bin/busybox", count_visit, &n) == FAT_ERR_NOTDIR,
          "iterating a file is -ENOTDIR");
}

int fat_run_tests(const char *image) {
    if (dev_stub_load(&dev, &dev_backing, image) != 0) {
        printf("  FAIL  could not open %s\n", image);
        return 1;
    }

    test_mount();
    test_root_listing();
    test_subdirectory_listing();
    test_lookup();
    test_lookup_failures();
    test_read();
    test_size_probe();
    test_partial_reads();
    test_iteration();

    return fat_failures;
}
