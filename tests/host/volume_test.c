/* Host tests for the storage chain: a disk device, a partition scan, a volume,
 * a filesystem probe, a drive letter, and a mounted root.
 *
 * --- Why this exists ------------------------------------------------------
 * part_test.c tests the scanner in isolation and passed while the machine
 * booted with nothing mounted, twice. Both failures were in the JOINTS rather
 * than in any one piece:
 *
 *   part_scan called a FAT boot sector a partition table, because a FAT boot
 *   sector carries the same 0xAA55 signature an MBR does. Caught only once
 *   the fixture came from tools/fatfs.py instead of from a synthetic image
 *   this file's author wrote - a fixture written to match the code's
 *   assumptions cannot contradict them.
 *
 *   dev_attach could not name a disk at all, because ns_insert does not
 *   create parent directories and \Device\Harddisk0\DR0 needs one. Every disk
 *   failed to register, so the scan never ran on anything.
 *
 * Neither is visible from a unit test of the part in question, and neither is
 * visible from ring 3: verification.c can only ask what happened on the one
 * machine it is running on, and if no disk registered there is nothing to ask
 * about. So the chain gets tested end to end, on the host, against an image
 * built by the real writer.
 *
 * The fixture is $OUT/test.img - the same FAT16 image fat_test.c reads, built
 * by tools/fatfs.py. Using the real producer is the entire point.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_stub.h"
#include "device.h"
#include "fatfs.h"
#include "fs.h"
#include "ns.h"
#include "object.h"
#include "part.h"
#include "volume.h"

static int volume_failures;

static void check(int cond, const char *what) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        volume_failures++;
    }
}

static device_t   disk;
static dev_stub_t backing;

int volume_run_tests(const char *image) {
    part_entry_t parts[PART_MAX];
    part_table_t table;
    int n;
    int made;

    printf("\nstorage: disk to mounted root\n");

    if (dev_stub_load(&disk, &backing, image) != 0) {
        printf("  FAIL  could not open %s\n", image);
        return 1;
    }

    /* The namespace has to exist before a device can be named. In the kernel
     * this is namespace_init running before storage_init, and the ordering is
     * load bearing: volumes name themselves as they are discovered, so
     * \Device\ must be there first. */
    ns_init();

    disk.kind = DEVICE_KIND_DISK;

    /* --- naming, which is where the second boot failure was ---------------
     *
     * \Device\Harddisk0\DR0 is TWO levels below \Device. ns_insert does not
     * create parents, so before dev_attach did, this returned -ENOENT and
     * every disk silently failed to register - and a scan over no disks finds
     * no volumes, which is what "volumes:" printing nothing meant.
     *
     * The check is the return value rather than "did anything print",
     * because that is exactly the distinction the boot log could not make. */
    {
        int rc = dev_attach(&disk, "\\Device\\Harddisk0\\DR0", "sda");

        check(rc == 0, "a two-level device name registers (needs its parent "
                       "directory created)");
        if (rc != 0) {
            return volume_failures;   /* nothing below can mean anything */
        }
    }
    check(ns_lookup_entry("\\Device\\Harddisk0") != NULL,
          "and the intermediate directory really was created");
    check(ns_lookup_entry("\\??\\sda") != NULL,
          "and the POSIX alias points at it");

    /* --- the scan, against an image from the real writer ------------------ */
    n = part_scan(&disk, parts, PART_MAX, &table);
    check(n == 0, "a fatfs.py image reports no partitions");
    check(table == PART_TABLE_NONE,
          "and NO TABLE - a FAT boot sector's 0xAA55 is not a partition table");

    /* --- the volume ------------------------------------------------------ */
    made = volume_scan_disk(&disk);
    check(made == 1, "which becomes exactly one whole-disk volume");

    volume_register_fs("fat16", fatfs_probe);
    volume_init();

    check(fs_root() != NULL, "the filesystem probe mounts it as the root");

    /* --- and the root is really readable ---------------------------------
     *
     * The check that would have caught both boot failures in one line, and
     * the reason it is last: everything above can succeed in a way that still
     * leaves nothing to read. This is what start_init_process does. */
    {
        fs_node_t node;
        check(fs_lookup("/etc/motd", &node) == 0,
              "and a file resolves through the mount table");
    }
    {
        unsigned char *buf = NULL;
        unsigned int   size = 0;

        check(fs_read_whole("/bin/busybox", &buf, &size) == 0 && size > 0,
              "and the init binary reads back - the boot path, end to end");
        if (buf != NULL) {
            fs_free_file(buf);
        }
    }

    /* --- removal, which is also the cleanup -----------------------------
     *
     * This suite has to leave no trace: the suites share one namespace, one
     * object pool and one mount table, and leaving \Device\HarddiskVolume1
     * and \??\C: behind made ns_test's own inserts come back -EEXIST - a
     * failure that reads like a broken ns_insert and is really a test that
     * did not clean up. ns_init() is not the way to do it; it early-returns
     * once a root exists, so it is a no-op rather than a reset.
     *
     * dev_detach is, and using it here is not just convenient. Surprise
     * removal has been sitting in verification.c's PART B as a HUMAN item -
     * "needs a removable device to pull", of which this machine has none - and
     * a file-backed disk is exactly the removable device that was missing.
     * The cleanup and the test are the same operation.
     *
     * What has to hold after a pull, and none of it is obvious:
     *   the volume on the disk goes too, depth first
     *   BOTH names of each device go - a stale \??\ link in the arm /dev
     *     searches first would shadow the next device given that name
     *   the mount comes down, so the root is unmounted
     *   the device object SURVIVES and answers -ENODEV, because handles may
     *     still be open on it */
    printf("\nstorage: surprise removal\n");
    dev_detach(&disk);

    check(ns_lookup_entry("\\Device\\Harddisk0\\DR0") == NULL,
          "pulling the disk removes its \\Device\\ name");
    check(ns_lookup_entry("\\??\\sda") == NULL,
          "and its \\??\\ alias, so no stale link shadows the next device");
    check(ns_lookup_entry("\\Device\\HarddiskVolume1") == NULL,
          "and the volume on it went too, without being named directly");
    check(ns_lookup_entry("\\??\\C:") == NULL,
          "including the drive letter it had been given");
    check(fs_root() == NULL,
          "and the root is unmounted rather than pointing at a gone device");

    /* The object outlives the name. That is the whole reason dev_detach does
     * not free anything: a descriptor held across the removal has to fail,
     * not fault. */
    {
        unsigned char buf[512];
        check(dev_read(&disk, 0, buf, sizeof(buf)) == -19,
              "and a read on the pulled device is -ENODEV, not a fault");
    }

    /* The intermediate directory dev_attach created is not a device and
     * nothing removes it; take it out by hand so the namespace is exactly as
     * it was found. */
    ns_remove("\\Device\\Harddisk0");

    /* --- the slot leak, which is what fs_ops_t::unmount is for -----------
     *
     * ROADMAP item 7 called this the one item on its list that was a bug
     * rather than a missing feature: a filesystem's per-volume slot was taken
     * by a successful probe and never released, because fs_unmount_volume
     * removed mount-table entries and never told the filesystem. The pools
     * are four deep on both filesystems, so "a machine that has seen four
     * pools stops recognising the fifth".
     *
     * The test is the sentence: insert and pull the same disk more times than
     * the pool is deep, and the last one must still mount.
     *
     * SEVEN rounds against pools of four (FATFS_MAX) and eight (VOLUME_MAX),
     * so it clears both by a margin rather than landing exactly on one - a
     * test that passes at exactly the boundary cannot tell "the slot was
     * reclaimed" from "there was one spare".
     *
     * MEASURED against the bug rather than assumed to catch it: with
     * fat_ops.unmount forced back to NULL, this fails and reports "first
     * cycle that failed to mount: 4". Four and not five because the mount at
     * the top of this file already spent one of FATFS_MAX's four slots -
     * which is itself the point, since the leak is per-machine and not
     * per-test.
     *
     * It did not fail loudly before: fatfs_mount returned NULL, volume_probe
     * found no filesystem, and fs_root() simply stayed NULL. A disk that
     * silently stops being mountable after a few insertions. */
    printf("\nstorage: a filesystem slot is reclaimed on unmount\n");
    {
        static device_t   d2;
        static dev_stub_t b2;
        int round;
        int mounted = 0;
        int first_failed = 0;

        for (round = 1; round <= 7; round++) {
            if (dev_stub_load(&d2, &b2, image) != 0) {
                break;
            }
            d2.kind = DEVICE_KIND_DISK;
            if (dev_attach(&d2, "\\Device\\Harddisk1\\DR0", "sdb") != 0) {
                break;
            }
            volume_scan_disk(&d2);
            volume_init();

            if (fs_root() != NULL) {
                mounted++;
            } else if (first_failed == 0) {
                first_failed = round;
            }

            dev_detach(&d2);
            ns_remove("\\Device\\Harddisk1");
        }

        check(mounted == 7,
              "the same disk mounts on every one of seven insert/pull cycles "
              "- more than either pool is deep");
        if (mounted != 7) {
            printf("        first cycle that failed to mount: %d\n",
                   first_failed);
        }
        check(fs_root() == NULL,
              "and the last pull left nothing mounted behind it");
    }

    return volume_failures;
}
