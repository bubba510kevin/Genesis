/* Host tests for the block cache.
 *
 * --- What a cache test has to prove, and what it usually proves instead ---
 * A cache is invisible when it works: every read returns the same bytes it
 * would have returned without it. So a test that only checks the DATA passes
 * identically whether the cache is caching or has quietly disabled itself -
 * which is the same failure as a suite that cannot link printing no failures.
 *
 * So every case here asserts two things: the bytes, and the number of device
 * reads that produced them. dev_stub_t counts both directions, and the first
 * case is a CONTROL - the same access pattern with the cache off, showing the
 * device read count that the cached case is being measured against. Without
 * it, "one read" is a number with nothing to compare to.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bcache.h"
#include "dev_stub.h"
#include "device.h"

static int bcache_failures;

static void check(int cond, const char *what) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) {
        bcache_failures++;
    }
}

/* A disk whose contents are a function of the offset, so a read that returns
 * the right COUNT and the wrong BYTES is visible. A buffer of zeroes would
 * make a cache that hands back an empty block look correct. */
#define DISK_BYTES (64u * 1024u)

static unsigned char disk_image[DISK_BYTES];
static device_t      dev;
static dev_stub_t    backing;

static unsigned char byte_at(unsigned long off) {
    return (unsigned char)((off * 31u + (off >> 8) * 7u) & 0xFF);
}

static void fill_image(void) {
    unsigned long i;

    for (i = 0; i < DISK_BYTES; i++) {
        disk_image[i] = byte_at(i);
    }
}

static int matches_image(const unsigned char *buf, unsigned long off,
                         unsigned long len) {
    unsigned long i;

    for (i = 0; i < len; i++) {
        if (buf[i] != disk_image[off + i]) {
            return 0;
        }
    }
    return 1;
}

/* A fresh device over the same image, with the cache flag on and the counters
 * back at zero. Deliberately re-attaches the SAME device_t: that is what the
 * kernel does when dev_free recycles a slot, and it is the case the
 * invalidation test is about. */
static void attach(uint64 size) {
    dev_stub_attach(&dev, &backing, disk_image, size);
    dev.flags = DEVICE_CACHED;
}

/* --- the control --------------------------------------------------------- */

static void test_uncached_reads_the_device_every_time(void) {
    unsigned char buf[512];

    printf("\nbcache: the control - no cache\n");

    bcache_init(NULL, 0);
    check(!bcache_enabled(), "a cache with no arena is disabled, not empty");

    attach(DISK_BYTES);
    dev.flags = 0;                    /* not DEVICE_CACHED: the raw path */

    check(dev_read(&dev, 0, buf, sizeof(buf)) == (int64)sizeof(buf),
          "a read of 512 bytes returns 512");
    check(matches_image(buf, 0, sizeof(buf)), "and the right bytes");
    check(backing.reads == 1, "which took one device read");

    check(dev_read(&dev, 0, buf, sizeof(buf)) == (int64)sizeof(buf),
          "reading the same 512 bytes again returns them again");
    check(backing.reads == 2,
          "and took a SECOND device read - this is the number the cached "
          "case is measured against");
}

/* --- hits --------------------------------------------------------------- */

static void test_a_second_read_is_a_hit(void) {
    unsigned char buf[512];
    bcache_stats_t s;

    printf("\nbcache: a resident block is not read twice\n");

    attach(DISK_BYTES);

    check(dev_read(&dev, 0, buf, sizeof(buf)) == (int64)sizeof(buf),
          "the first read succeeds");
    check(matches_image(buf, 0, sizeof(buf)), "with the right bytes");
    check(backing.reads == 1, "and took exactly one device read");

    bcache_get_stats(&s);
    check(s.misses >= 1, "counted as a miss");

    check(dev_read(&dev, 0, buf, sizeof(buf)) == (int64)sizeof(buf),
          "the second read succeeds");
    check(matches_image(buf, 0, sizeof(buf)), "with the same bytes");
    check(backing.reads == 1,
          "and took NO device read - two in the control, one here");

    /* A different 512 bytes inside the same 4KB block. The point of a block
     * bigger than a sector: the neighbouring sector was already fetched. */
    check(dev_read(&dev, 3584, buf, sizeof(buf)) == (int64)sizeof(buf),
          "a different sector of the same block reads");
    check(matches_image(buf, 3584, sizeof(buf)), "with the right bytes");
    check(backing.reads == 1, "and still no second device read");
}

static void test_the_fill_is_one_block_wide(void) {
    unsigned char buf[8192];

    printf("\nbcache: a request spanning blocks fills each once\n");

    attach(DISK_BYTES);

    check(dev_read(&dev, 4090, buf, 12) == 12,
          "a 12-byte read straddling a block boundary succeeds");
    check(matches_image(buf, 4090, 12), "with the right bytes across the seam");
    check(backing.reads == 2, "and filled the two blocks it touched");

    check(dev_read(&dev, 0, buf, 8192) == 8192,
          "a read covering both blocks entirely succeeds");
    check(matches_image(buf, 0, 8192), "with the right bytes");
    check(backing.reads == 2, "out of the cache, with no new device read");
}

/* --- writes -------------------------------------------------------------- */

static void test_writes_reach_the_device_and_the_cache(void) {
    unsigned char buf[64];
    unsigned char pattern[64];
    int i;

    printf("\nbcache: write-through\n");

    attach(DISK_BYTES);
    for (i = 0; i < 64; i++) {
        pattern[i] = (unsigned char)(0xA0 + i);
    }

    check(dev_read(&dev, 0, buf, 64) == 64, "read the block in first");
    check(backing.reads == 1, "one fill");

    check(dev_write(&dev, 16, pattern, 64) == 64, "a 64-byte write succeeds");
    check(backing.writes == 1, "and reached the device immediately");
    check(memcmp(disk_image + 16, pattern, 64) == 0,
          "the device really has the new bytes - write-through, not deferred");

    check(dev_read(&dev, 16, buf, 64) == 64, "reading it back succeeds");
    check(memcmp(buf, pattern, 64) == 0, "with the bytes just written");
    check(backing.reads == 1,
          "out of the cache: the resident copy was updated, not invalidated");

    /* Restore, so later cases still see the generated pattern. */
    memcpy(disk_image + 16, pattern, 0);
    fill_image();
    bcache_invalidate_all();
}

static void test_a_whole_block_write_does_not_read_first(void) {
    unsigned char block[4096];
    unsigned char buf[4096];
    int i;

    printf("\nbcache: a full-block write skips the fill\n");

    attach(DISK_BYTES);
    for (i = 0; i < 4096; i++) {
        block[i] = (unsigned char)(i ^ 0x5A);
    }

    check(dev_write(&dev, 8192, block, 4096) == 4096,
          "writing a whole aligned block succeeds");
    check(backing.reads == 0,
          "with no device read at all - there is nothing to preserve");
    check(backing.writes == 1, "and one device write");
    check(memcmp(disk_image + 8192, block, 4096) == 0,
          "the device has the block");

    check(dev_read(&dev, 8192, buf, 4096) == 4096, "reading it back succeeds");
    check(memcmp(buf, block, 4096) == 0, "with the bytes written");
    check(backing.reads == 0,
          "still no device read: the write left the block resident");

    fill_image();
    bcache_invalidate_all();
}

/* --- eviction ------------------------------------------------------------ */

static void test_eviction_is_least_recently_used(void) {
    unsigned char buf[16];
    bcache_stats_t s;
    unsigned long i;
    unsigned long blocks;

    printf("\nbcache: eviction\n");

    bcache_get_stats(&s);
    blocks = (unsigned long)s.blocks;
    check(blocks > 1, "the cache has more than one block to evict from");

    attach(DISK_BYTES);

    /* Touch block 0, then enough other blocks to push it out. The disk is
     * 16 blocks and the cache is 64, so this needs a cache smaller than the
     * disk to be meaningful - which is why the arena the harness installs is
     * deliberately small. */
    check(dev_read(&dev, 0, buf, 16) == 16, "block 0 is read in");
    for (i = 1; i <= blocks; i++) {
        if (dev_read(&dev, i * 4096, buf, 16) != 16) {
            check(0, "filling the cache with other blocks");
            return;
        }
    }
    check(backing.reads == blocks + 1, "each of those blocks was filled once");

    check(dev_read(&dev, 0, buf, 16) == 16, "block 0 reads again");
    check(matches_image(buf, 0, 16), "with the right bytes");
    check(backing.reads == blocks + 2,
          "and had to be filled again - it was evicted");

    check(dev_read(&dev, blocks * 4096, buf, 16) == 16,
          "the most recently used block reads");
    check(backing.reads == blocks + 2,
          "without a fill - it was not the one evicted");

    bcache_get_stats(&s);
    check(s.evictions > 0, "and the eviction was counted");
}

/* --- presence ------------------------------------------------------------ */

static void test_detach_drops_the_devices_blocks(void) {
    static unsigned char other_image[DISK_BYTES];
    unsigned char buf[64];
    unsigned long i;

    printf("\nbcache: a detached device leaves nothing behind\n");

    attach(DISK_BYTES);
    check(dev_read(&dev, 0, buf, 64) == 64, "a block is cached");
    check(backing.reads == 1, "by one fill");

    dev_detach(&dev);
    check(dev_read(&dev, 0, buf, 64) == -19,
          "after detach the device answers -ENODEV, not cached bytes");

    /* The recycled slot. device.c hands out device_t entries from a fixed
     * pool, so the next device to exist can be at this exact address - and a
     * cache keyed on the pointer would serve it the previous medium's bytes.
     * A different image behind the same device_t is precisely that case. */
    for (i = 0; i < DISK_BYTES; i++) {
        other_image[i] = (unsigned char)(0xE0 ^ (i & 0x1F));
    }
    dev_stub_attach(&dev, &backing, other_image, DISK_BYTES);
    dev.flags = DEVICE_CACHED;

    check(dev_read(&dev, 0, buf, 64) == 64, "the new device reads");
    check(backing.reads == 1, "with a real fill from the new medium");
    check(memcmp(buf, other_image, 64) == 0,
          "returning the NEW medium's bytes, not the old device's");
}

/* --- the edges ----------------------------------------------------------- */

static void test_the_ends_of_the_device(void) {
    unsigned char buf[4096];
    /* Not a multiple of the block size: the last block is a partial one, and
     * a cache that fills whole blocks has to not report the padding. */
    const uint64 odd = 9000;

    printf("\nbcache: the ends of the device\n");

    attach(odd);

    check(dev_read(&dev, odd, buf, 16) == 0,
          "a read starting past the end is EOF, not an error");
    check(dev_write(&dev, odd, buf, 16) == -28,
          "a write starting past the end is -ENOSPC");

    check(dev_read(&dev, 8192, buf, 4096) == (int64)(odd - 8192),
          "a read over the end is short by exactly the overhang");
    check(matches_image(buf, 8192, (unsigned long)(odd - 8192)),
          "and returns the real tail bytes");

    check(dev_read(&dev, 8192, buf, 4096) == (int64)(odd - 8192),
          "the same read again is still short by the same amount");
    check(backing.reads == 1,
          "served from the resident partial block, without a second fill");
}

/* --- the harness's own arena --------------------------------------------- */

int bcache_run_tests(void) {
    /* Small on purpose: eight blocks against a sixteen-block disk, so the
     * eviction case has something to evict. The kernel installs sixty-four. */
    static unsigned char arena[9 * 4096];

    fill_image();

    test_uncached_reads_the_device_every_time();

    bcache_init(arena, sizeof(arena));
    if (!bcache_enabled()) {
        check(0, "the cache took the arena the harness gave it");
        return bcache_failures;
    }

    test_a_second_read_is_a_hit();
    bcache_invalidate_all();
    test_the_fill_is_one_block_wide();
    bcache_invalidate_all();
    test_writes_reach_the_device_and_the_cache();
    test_a_whole_block_write_does_not_read_first();
    bcache_invalidate_all();
    test_eviction_is_least_recently_used();
    bcache_invalidate_all();
    test_detach_drops_the_devices_blocks();
    bcache_invalidate_all();
    test_the_ends_of_the_device();

    return bcache_failures;
}
