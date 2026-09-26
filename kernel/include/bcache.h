#ifndef BCACHE_H
#define BCACHE_H

#include "device.h"
#include "typesk.h"

/* The block cache: one copy of a block, above the driver.
 *
 * --- Where it sits, and why that is the only defensible place -------------
 * The TODO asked whether this subsumes disk.c's bounce-buffer
 * read-modify-write or wraps it. Neither, exactly, and the distinction is
 * worth being precise about because "two live paths doing overlapping
 * buffering" is the failure the question was pointing at:
 *
 *   The cache sits in dev_read/dev_write, ABOVE the driver, and every request
 *   it passes down is block-aligned and block-sized. So disk.c's partial-
 *   sector path still exists and is still correct, and for cached devices it
 *   is never taken - the cache is the only thing that issues those reads and
 *   it never issues an unaligned one. The bounce buffer stops being a
 *   buffering layer and becomes what it always should have been: the driver's
 *   own handling of a request that does not line up with its transfer unit,
 *   for the callers that reach it with the cache off or absent.
 *
 * The alternative placements were both worse:
 *
 *   In the driver (disk.c calls a cache instead of ata_read). Then every
 *   driver has its own cache, a ramdisk gets one it does not want, and the
 *   volume layer above still sees uncached bytes.
 *
 *   In the filesystem (fat.c caches sectors). That is where the FAT-specific
 *   version would have gone, and it caches ONE filesystem's blocks. ZFS
 *   arriving next would have wanted its own, which is two caches for one
 *   disk - and the two would disagree the first time anything wrote.
 *
 * --- Cached at the DISK, not at the volume --------------------------------
 * Only the bottom device in a stack sets DEVICE_CACHED. A volume forwards to
 * its parent disk with an offset added (vol_read), so a volume read is a disk
 * read and is cached there. Caching at both layers would keep two copies of
 * the same physical block under two keys, and a raw write to \Device\Harddisk0
 * would leave the volume's copy stale with nothing to notice - the same
 * two-views-of-one-thing shape as the \??\-vs-\Device\ merge, which has
 * already produced one real bug in this tree.
 *
 * --- Write-through, deliberately, and what it costs -----------------------
 * Writes update the cached block AND go to the device before the call
 * returns. That is a decision, not an oversight, and here is the argument
 * against write-back TODAY:
 *
 *   There is no sync(2), no unmount that flushes, no shutdown path, and no
 *   flusher thread - the kernel is uniprocessor and doing PIO from the timer
 *   interrupt is not a flusher, it is a way to take a fault in an ISR. A
 *   write-back cache in that machine is a cache that loses every dirty block
 *   on every reset, and the only writer today is a raw write to a device from
 *   ring 3, where the caller has every reason to believe the byte is on the
 *   platter when write() returns.
 *
 *   The whole measured win here is on the READ side anyway: fat.c re-reads
 *   the same FAT sectors and the same directory block for every path
 *   component, and the boot goes from hundreds of ATA commands to a handful.
 *   Write-back would add crash-consistency risk to buy back writes that
 *   nothing currently issues in bulk.
 *
 * When fs write lands, or the ZFS side needs a transaction group flushed in
 * order, the change is a dirty bit here plus a bcache_flush() at the sync
 * points - one file, and the callers do not move. That is the reason the
 * cache is a layer with its own key space rather than a buffer bolted to
 * disk.c: the layer is what makes the later change local.
 *
 * --- Presence, and why invalidation is not optional -----------------------
 * device.c recycles device_t slots (dev_free puts one back in the pool). A
 * cached block keyed on a device_t POINTER therefore outlives the device it
 * came from, and the next device to take that slot would find the previous
 * medium's bytes under its own key - a read that returns plausible data from
 * the wrong disk, with no error anywhere. dev_detach calls
 * bcache_invalidate_dev for exactly this reason, and the call is in device.c
 * beside the DEVICE_GONE flag rather than in each driver's detach, on the
 * same argument that put the presence check there.
 */

/* The cache's block size. Eight ATA sectors, and a page: a block is a
 * plausible unit to hand to the VMM later without a second size existing.
 *
 * Bigger than a sector on purpose - a sector-granular cache spends its whole
 * hit rate on the FAT and none of it on readahead, because a filesystem that
 * reads a 4KB cluster in eight calls gets eight misses. */
#define BCACHE_BLOCK_SHIFT 12
#define BCACHE_BLOCK_SIZE  (1u << BCACHE_BLOCK_SHIFT)

/* How many blocks the cache will use if it is given the memory for them.
 * 64 blocks is 256KB, which is what flk.c asks kmalloc for. */
#define BCACHE_MAX_BLOCKS  64

typedef struct bcache_stats {
    uint64 hits;         /* a request found the block already resident     */
    uint64 misses;       /* a request had to fill a block from the device  */
    uint64 fills;        /* device reads issued by the cache               */
    uint64 evictions;    /* resident blocks dropped to make room           */
    uint64 writes;       /* device writes issued by the cache              */
    uint64 bypasses;     /* requests that could not use the cache at all   */
    uint64 blocks;       /* how many blocks the cache actually has         */
    uint64 resident;     /* how many of them currently hold something      */
} bcache_stats_t;

/* Hand the cache its memory. `mem` must be BCACHE_BLOCK_SIZE-aligned and is
 * carved into as many blocks as fit, up to BCACHE_MAX_BLOCKS.
 *
 * Taking the arena from the caller rather than calling kmalloc here is the
 * same arrangement pmm_init has with its bitmap, and for the same two
 * reasons: this file has no opinion about which allocator is appropriate at
 * the point it runs, and the host tests can hand it malloc'd memory and
 * exercise the real code instead of a copy of it.
 *
 * Calling this with NULL, or never calling it, leaves the cache DISABLED and
 * every request passes straight through. That is the honest failure mode for
 * an out-of-memory boot: slower, never wrong. */
void bcache_init(void *mem, uint64 bytes);

/* Non-zero if the cache has blocks. dev_read consults this so that an
 * allocation failure at boot degrades to the uncached path rather than to a
 * kernel that cannot read its own root. */
int bcache_enabled(void);

/* The cached forms of dev_read and dev_write. Called by device.c for a device
 * that set DEVICE_CACHED; not called directly by anything else.
 *
 * Both take and return exactly what dev_read/dev_write do, including the
 * short-read-at-the-end and -ENOSPC-past-the-end conventions, because a cache
 * that changes the contract of the thing it is caching is a cache that makes
 * every caller conditional on whether it is on. */
int64 bcache_read(device_t *dev, uint64 offset, void *buf, uint64 n);
int64 bcache_write(device_t *dev, uint64 offset, const void *buf, uint64 n);

/* Drop every block belonging to this device. Called from dev_detach, and safe
 * to call for a device that has nothing cached. Nothing is written back -
 * with write-through there is nothing to write back, and if there ever is,
 * the medium being gone is precisely the case where a flush cannot happen. */
void bcache_invalidate_dev(device_t *dev);

/* Drop everything. Only for the tests, which need a clean cache between
 * cases; the kernel never has a reason to throw away a correct cache. */
void bcache_invalidate_all(void);

/* Counters since boot. The point of them is the control measurement: a test
 * that asserts a second read was a hit is only meaningful next to one that
 * shows the first was a miss, and a boot report that shows zero hits is a
 * cache that is installed and not working. */
void bcache_get_stats(bcache_stats_t *out);

/* One line, behind the same verbosity as the other boot reports. */
void bcache_report(void);

#endif
