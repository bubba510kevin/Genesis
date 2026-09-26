/* Host tests for file CONTENT writing - fat_write_entry_at.
 *
 * --- why this is a host test and not a systest section --------------------
 * Nothing in ring 3 can create a file yet: sys_openat still refuses O_CREAT,
 * so a userspace test can only write to files that already exist on the
 * staged image, and it cannot check what actually landed on the medium
 * afterwards except by reading it back through the same code that wrote it.
 * Here the image is an ordinary host file, so a write can be checked against
 * the BYTES ON DISK rather than against the writer's own opinion of them.
 *
 * --- what a broken implementation would still pass ------------------------
 * Writing and reading back through the same functions is the weakest possible
 * check: a "filesystem" that kept the data in RAM and never touched the image
 * would pass it. So the tests here do three things it cannot:
 *
 *   REMOUNT.       Every assertion about persistence is made after tearing
 *                  the volume down and mounting the image again, so the
 *                  answer comes off the medium and not out of a cache.
 *   GROW THE FILE. Extending across a cluster boundary is where the chain
 *                  has to be allocated and linked; a write that only ever
 *                  overwrites in place never exercises it.
 *   CHECK THE SIZE. The directory record is a separate 32 bytes from the
 *                  data, and forgetting to update it is the failure that
 *                  reads back as a perfectly good file that is too short.
 */

#include <stdio.h>
#include <string.h>

#include "dev_stub.h"
#include "device.h"
#include "fat.h"

static int fw_failures;

static void check(int cond, const char *what) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        fw_failures++;
    }
}

static device_t     disk;
static dev_stub_t   backing;
static fat_volume_t vol;

/* Mount the image fresh. Every persistence claim in this file is made across
 * one of these, which is the only thing that distinguishes "written" from
 * "remembered". */
static int remount(const char *image) {
    if (dev_stub_load_rw(&disk, &backing, image) != 0) {
        return -1;
    }
    disk.kind = DEVICE_KIND_DISK;
    return fat_mount(&vol, &disk) == FAT_OK ? 0 : -1;
}

int fat_write_run_tests(const char *image) {
    fat_entry_t ent;
    char buf[64];
    uint32 got = 0, wrote = 0;
    const char *replacement = "GOODBYE, DISK!\n";
    uint32 replacement_len = (uint32)strlen(replacement);
    uint32 original_size;

    printf("\nfat: writing file content\n");

    if (remount(image) != 0) {
        printf("  FAIL  could not mount %s\n", image);
        return 1;
    }

    /* --- 1. overwrite in place, within one cluster --------------------- */
    if (fat_lookup(&vol, "/etc/motd", &ent) != FAT_OK) {
        printf("  FAIL  /etc/motd is not on the fixture\n");
        return 1;
    }
    original_size = ent.size;
    check(original_size > 0, "the file has content to start with");

    check(fat_write_entry_at(&vol, &ent, 0, replacement, replacement_len,
                             &wrote) == FAT_OK && wrote == replacement_len,
          "a write at offset 0 reports every byte written");

    /* Across a remount, so this is the medium answering and not the entry
     * still sitting in memory. */
    check(remount(image) == 0, "and the volume remounts afterwards");
    check(fat_lookup(&vol, "/etc/motd", &ent) == FAT_OK,
          "and the file is still there");
    memset(buf, 0, sizeof(buf));
    check(fat_read_entry_at(&vol, &ent, 0, buf, replacement_len, &got)
              == FAT_OK && got == replacement_len,
          "and reads back the same number of bytes");
    check(memcmp(buf, replacement, replacement_len) == 0,
          "and they are the bytes that were written");

    /* The tail beyond what was overwritten must be UNTOUCHED. A write
     * implemented as "zero the cluster then copy" would pass everything above
     * and fail here, and that is a data-loss bug rather than a wrong read. */
    check(ent.size == original_size,
          "a write that did not extend the file left the size alone");
    if (original_size > replacement_len) {
        memset(buf, 0, sizeof(buf));
        check(fat_read_entry_at(&vol, &ent, replacement_len, buf,
                                original_size - replacement_len, &got)
                  == FAT_OK && got == original_size - replacement_len,
              "and the bytes past the write are still readable");
        check(buf[0] != '\0',
              "and were not clobbered - a read-modify-write, not a rewrite");
    }

    /* --- 2. extend past the end, across a cluster boundary -------------
     *
     * The fixture's clusters are small enough that a few KB crosses several,
     * which is the case that has to allocate and LINK. A write that only ever
     * lands inside the first cluster never touches fat_next_or_grow. */
    {
        static char big[9000];
        static char back[9000];
        uint32 i;
        uint32 target = (uint32)sizeof(big);

        for (i = 0; i < target; i++) {
            /* A position-dependent pattern, so a chain that is linked in the
             * wrong ORDER fails. A constant fill would read back correctly
             * from a scrambled chain. */
            big[i] = (char)((i * 7u + (i >> 8)) & 0xFF);
        }

        check(fat_lookup(&vol, "/usr/bin/tiny", &ent) == FAT_OK,
              "a second file to grow");
        check(fat_write_entry_at(&vol, &ent, 0, big, target, &wrote)
                  == FAT_OK && wrote == target,
              "a multi-cluster write reports every byte written");
        check(ent.size == target,
              "and the entry's size grew to match");

        check(remount(image) == 0, "the volume remounts after growing a file");
        check(fat_lookup(&vol, "/usr/bin/tiny", &ent) == FAT_OK,
              "and the grown file resolves");
        check(ent.size == target,
              "and the DIRECTORY RECORD carries the new size - the half that "
              "is not the data");

        memset(back, 0, sizeof(back));
        check(fat_read_entry_at(&vol, &ent, 0, back, target, &got) == FAT_OK &&
                  got == target,
              "and it reads back at full length");
        check(memcmp(back, big, target) == 0,
              "with every byte in the right place - the cluster chain is "
              "linked in order");
    }

    /* --- 3. a write PAST the end fills the gap with zeroes --------------
     *
     * FAT has no holes, so the skipped clusters are really allocated and
     * really zeroed. Checking the gap is what separates that from a chain
     * with a gap in it, which reads back as whatever those clusters held -
     * somebody's deleted data. */
    {
        static char back[9000];
        const char *tail = "TAIL";
        uint64 gap_at;
        uint32 i;
        int gap_is_zero = 1;

        uint32 gap_start;

        check(fat_lookup(&vol, "/etc/motd", &ent) == FAT_OK,
              "back to the first file");
        /* The gap starts at the OLD END OF FILE, not at the end of what test
         * 1 wrote. Bytes between the two are the file's original content and
         * are supposed to still be there - reading them as part of the gap
         * was this test asserting that valid data had been destroyed. */
        gap_start = ent.size;
        gap_at    = ent.size + 4096;

        /* POISON the bytes just past the end of file, inside the cluster this
         * file already owns.
         *
         * Without this the gap check proves nothing on a freshly formatted
         * image: every byte past every EOF is already zero, so an
         * implementation that zeroed only the clusters it newly allocated -
         * which is what the first version of fat_write_entry_at did - passes
         * by luck. The bytes that matter are the ones in the LAST ALREADY-
         * ALLOCATED cluster, and the only way to have stale data there on a
         * clean image is to put it there.
         *
         * Located by searching the raw image for the marker test 1 wrote,
         * rather than by computing a cluster LBA - which would mean this test
         * carrying its own copy of fat.c's geometry arithmetic, and agreeing
         * with a bug in it. */
        {
            unsigned char poison[256];
            uint64 at = 0;
            uint64 i;
            int found = 0;

            memset(poison, 0xEE, sizeof(poison));
            for (i = 0; i + replacement_len <= backing.len; i++) {
                if (memcmp(backing.data + i, replacement, replacement_len)
                        == 0) {
                    at = i;
                    found = 1;
                    break;
                }
            }
            check(found, "the file's data is findable in the raw image");
            if (found) {
                check(dev_write(&disk, at + gap_start, poison, sizeof(poison))
                          == (int64)sizeof(poison),
                      "and the bytes past its end can be poisoned");
            }
        }

        check(fat_write_entry_at(&vol, &ent, gap_at, tail, 4, &wrote)
                  == FAT_OK && wrote == 4,
              "a write past the end succeeds");
        check(ent.size == (uint32)gap_at + 4,
              "and the size covers the gap plus what was written");

        check(remount(image) == 0, "the volume remounts after the gap write");
        check(fat_lookup(&vol, "/etc/motd", &ent) == FAT_OK,
              "and the file resolves");

        memset(back, 0xAA, sizeof(back));
        check(fat_read_entry_at(&vol, &ent, gap_start,
                                back, 4000, &got) == FAT_OK,
              "and the gap reads");
        check(got == 4000, "and the gap is really there to be read");
        for (i = 0; i < got; i++) {
            if (back[i] != 0) {
                gap_is_zero = 0;
                break;
            }
        }
        check(gap_is_zero,
              "and every byte of the gap is ZERO, not whatever those clusters "
              "held before");

        memset(back, 0, sizeof(back));
        check(fat_read_entry_at(&vol, &ent, gap_at, back, 4, &got) == FAT_OK &&
                  got == 4 && memcmp(back, tail, 4) == 0,
              "and the bytes written past the gap are where they were put");
    }

    /* --- 4. creating a file ---------------------------------------------
     *
     * A created file must be EMPTY and must have no cluster: FAT spells that
     * as first-cluster 0, size 0, and a create that allocated one would give
     * every touched-and-never-written file a cluster it does not use. */
    {
        check(fat_create(&vol, "/newfile.txt") == FAT_OK,
              "a file can be created");
        check(fat_create(&vol, "/newfile.txt") == FAT_ERR_EXISTS,
              "and creating it again is refused, not silently reopened");

        check(remount(image) == 0, "the volume remounts after a create");
        check(fat_lookup(&vol, "/newfile.txt", &ent) == FAT_OK,
              "and the new file is really on the medium");
        check(ent.size == 0 && ent.cluster == 0,
              "and it is empty with no cluster allocated");

        /* And it is writable, which is the point of creating it - this is the
         * path O_CREAT|O_WRONLY takes, and the first write is the one that
         * has to allocate the first cluster. */
        check(fat_write_entry_at(&vol, &ent, 0, "created and written\n", 20,
                                 &wrote) == FAT_OK && wrote == 20,
              "and writing to it allocates its first cluster");
        check(ent.cluster != 0, "which the entry now records");

        check(remount(image) == 0, "the volume remounts again");
        check(fat_lookup(&vol, "/newfile.txt", &ent) == FAT_OK &&
                  ent.size == 20,
              "and the created file has the size it was written to");
        memset(buf, 0, sizeof(buf));
        check(fat_read_entry_at(&vol, &ent, 0, buf, 20, &got) == FAT_OK &&
                  memcmp(buf, "created and written\n", 20) == 0,
              "and reads back what was put in it");
    }

    /* --- 5. truncating ---------------------------------------------------
     *
     * Both directions. Shrinking has to cut the chain and free the tail;
     * growing has to zero-fill, which is the same gap problem as a write past
     * the end and is why fat_truncate delegates to it rather than repeating
     * it. */
    {
        static char back[9000];
        uint32 i;
        int all_zero = 1;

        check(fat_lookup(&vol, "/newfile.txt", &ent) == FAT_OK, "the file");
        check(fat_truncate(&vol, &ent, 5) == FAT_OK, "truncates shorter");
        check(ent.size == 5, "and the entry says so");

        check(remount(image) == 0, "the volume remounts after shrinking");
        check(fat_lookup(&vol, "/newfile.txt", &ent) == FAT_OK &&
                  ent.size == 5,
              "and the shorter size is on the medium");
        memset(buf, 0, sizeof(buf));
        check(fat_read_entry_at(&vol, &ent, 0, buf, sizeof(buf), &got)
                  == FAT_OK && got == 5,
              "and a read stops at the new end rather than the old one");
        check(memcmp(buf, "creat", 5) == 0, "with the surviving bytes intact");

        /* To zero: the chain goes entirely and the first cluster with it. */
        check(fat_truncate(&vol, &ent, 0) == FAT_OK, "truncates to nothing");
        check(ent.size == 0 && ent.cluster == 0,
              "and the chain is released - no cluster left attached");

        /* And GROWING, which must produce zeroes rather than whatever those
         * clusters last held. The bytes come from clusters just freed by the
         * truncate above, which is exactly the case that would leak old
         * content if growth did not zero-fill. */
        check(fat_truncate(&vol, &ent, 6000) == FAT_OK, "and grows again");
        check(ent.size == 6000, "to the requested length");

        check(remount(image) == 0, "the volume remounts after growing");
        check(fat_lookup(&vol, "/newfile.txt", &ent) == FAT_OK &&
                  ent.size == 6000,
              "and the grown size is on the medium");
        memset(back, 0x5A, sizeof(back));
        check(fat_read_entry_at(&vol, &ent, 0, back, 6000, &got) == FAT_OK &&
                  got == 6000,
              "and it reads back at full length");
        for (i = 0; i < 6000; i++) {
            if (back[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        check(all_zero,
              "and every byte of it is ZERO - a grown file must not expose "
              "what those clusters held before");
    }

    /* --- 6. a directory is refused ------------------------------------- */
    check(fat_lookup(&vol, "/etc", &ent) == FAT_OK, "a directory resolves");
    check(fat_write_entry_at(&vol, &ent, 0, "x", 1, &wrote) == FAT_ERR_ISDIR,
          "and writing to it is -EISDIR, not a corrupted directory");

    return fw_failures;
}
