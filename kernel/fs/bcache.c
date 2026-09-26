#include "bcache.h"
#include "device.h"
#include "screen.h"
#include "typesk.h"

/* See bcache.h for where this sits and why it is write-through. What follows
 * is how, and the two invariants everything else depends on:
 *
 *   1. A resident block's contents are equal to what the device would return
 *      for that block. Every write goes to the device AND to the resident
 *      copy; a write that fails at the device invalidates the copy rather
 *      than leaving a cache that disagrees with the disk.
 *
 *   2. A (device, block) key names at most one buffer. Insertion looks up
 *      first, so a second fill of a block that arrived while... - and there is
 *      no "while" here, because the kernel is uniprocessor and no device read
 *      can block into another. That is the one place this file is allowed to
 *      be simpler than a real cache, and it is written down so that the day
 *      dev_read can sleep, this comment is what fails.
 */

/* There is no memcpy in this kernel; every file that needs one writes it. */
static void bc_copy(void *dst, const void *src, uint64 n) {
    uint8       *d = (uint8 *)dst;
    const uint8 *s = (const uint8 *)src;
    uint64 i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void bc_zero(void *dst, uint64 n) {
    uint8 *d = (uint8 *)dst;
    uint64 i;

    for (i = 0; i < n; i++) {
        d[i] = 0;
    }
}

/* Power of two, so the mask is an AND. Sized to the block count rather than
 * generously: with 64 blocks resident there can never be more than 64 keys,
 * so 64 buckets is one entry per bucket at full occupancy. */
#define BCACHE_BUCKETS 64

typedef struct bcache_buf {
    device_t *dev;              /* NULL for a buffer that holds nothing   */
    uint64    blkno;            /* block index within that device         */
    uint8    *data;             /* BCACHE_BLOCK_SIZE bytes in the arena   */

    struct bcache_buf *hash_next;
    struct bcache_buf *lru_prev;
    struct bcache_buf *lru_next;
} bcache_buf_t;

/* The headers are static and the DATA comes from the arena. Splitting them
 * that way keeps the header array a fixed, small .bss cost (the kernel image
 * is loaded below the boot stack at 0x90000 and .bss counts against that),
 * while the 256KB of block data comes from the heap where there is room. */
static bcache_buf_t  bufs[BCACHE_MAX_BLOCKS];
static bcache_buf_t *buckets[BCACHE_BUCKETS];
static bcache_buf_t *lru_head;      /* most recently used */
static bcache_buf_t *lru_tail;      /* first to be evicted */
static int           buf_count;

static bcache_stats_t stats;

/* --- the lists ----------------------------------------------------------- */

static uint64 hash_of(const device_t *dev, uint64 blkno) {
    /* The device pointer contributes its middle bits: device_t entries come
     * from one static array, so the low bits are a small multiple of
     * sizeof(device_t) and the high bits are identical for every device. Both
     * ends are useless on their own; shifting by 5 and mixing with the block
     * number is enough to spread 64 keys over 64 buckets. */
    uint64 d = ((uint64)(uintptr)dev) >> 5;

    return (d ^ (blkno * 0x9E3779B1ULL)) & (BCACHE_BUCKETS - 1);
}

static void lru_unlink(bcache_buf_t *b) {
    if (b->lru_prev != NULL) {
        b->lru_prev->lru_next = b->lru_next;
    } else if (lru_head == b) {
        lru_head = b->lru_next;
    }
    if (b->lru_next != NULL) {
        b->lru_next->lru_prev = b->lru_prev;
    } else if (lru_tail == b) {
        lru_tail = b->lru_prev;
    }
    b->lru_prev = NULL;
    b->lru_next = NULL;
}

static void lru_push_front(bcache_buf_t *b) {
    b->lru_prev = NULL;
    b->lru_next = lru_head;
    if (lru_head != NULL) {
        lru_head->lru_prev = b;
    }
    lru_head = b;
    if (lru_tail == NULL) {
        lru_tail = b;
    }
}

static void lru_touch(bcache_buf_t *b) {
    if (lru_head == b) {
        return;
    }
    lru_unlink(b);
    lru_push_front(b);
}

static void hash_insert(bcache_buf_t *b) {
    uint64 h = hash_of(b->dev, b->blkno);

    b->hash_next = buckets[h];
    buckets[h] = b;
}

static void hash_remove(bcache_buf_t *b) {
    uint64 h = hash_of(b->dev, b->blkno);
    bcache_buf_t **pp = &buckets[h];

    while (*pp != NULL) {
        if (*pp == b) {
            *pp = b->hash_next;
            b->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

static bcache_buf_t *lookup(const device_t *dev, uint64 blkno) {
    bcache_buf_t *b = buckets[hash_of(dev, blkno)];

    while (b != NULL) {
        if (b->dev == dev && b->blkno == blkno) {
            return b;
        }
        b = b->hash_next;
    }
    return NULL;
}

/* Detach a buffer from its key, leaving it empty and at the LRU tail so it is
 * the next one reused. Not "free": there is nothing to free, the arena is
 * permanent. */
static void evict(bcache_buf_t *b) {
    if (b->dev == NULL) {
        return;
    }
    hash_remove(b);
    b->dev   = NULL;
    b->blkno = 0;
    lru_unlink(b);
    /* To the TAIL, not the front: an emptied buffer is the best candidate for
     * the next fill, and putting it at the front would make the next eviction
     * throw away a block somebody just read. */
    b->lru_prev = lru_tail;
    b->lru_next = NULL;
    if (lru_tail != NULL) {
        lru_tail->lru_next = b;
    }
    lru_tail = b;
    if (lru_head == NULL) {
        lru_head = b;
    }
}

/* --- setup --------------------------------------------------------------- */

void bcache_init(void *mem, uint64 bytes) {
    uint8 *arena = (uint8 *)mem;
    uint64 count;
    uint64 i;

    lru_head  = NULL;
    lru_tail  = NULL;
    buf_count = 0;
    for (i = 0; i < BCACHE_BUCKETS; i++) {
        buckets[i] = NULL;
    }
    bc_zero(&stats, sizeof(stats));

    if (arena == NULL || bytes < BCACHE_BLOCK_SIZE) {
        /* Disabled. Every request bypasses; see the header on why this is a
         * degradation and not a failure. */
        return;
    }

    /* Align the arena up. A caller that hands us a kmalloc'd pointer gets 16-
     * byte alignment, and a block that straddles a page boundary is harmless
     * today and is exactly the thing that stops being harmless the day these
     * buffers are handed to a DMA engine. Costing one block of the arena to
     * make the alignment true from the start is cheaper than discovering it
     * from a driver. */
    {
        uint64 addr = (uint64)(uintptr)arena;
        uint64 up   = (addr + BCACHE_BLOCK_SIZE - 1) & ~((uint64)BCACHE_BLOCK_SIZE - 1);

        if (up - addr >= bytes) {
            return;
        }
        bytes -= (up - addr);
        arena  = (uint8 *)(uintptr)up;
    }

    count = bytes / BCACHE_BLOCK_SIZE;
    if (count > BCACHE_MAX_BLOCKS) {
        count = BCACHE_MAX_BLOCKS;
    }

    for (i = 0; i < count; i++) {
        bcache_buf_t *b = &bufs[i];

        b->dev       = NULL;
        b->blkno     = 0;
        b->data      = arena + i * BCACHE_BLOCK_SIZE;
        b->hash_next = NULL;
        b->lru_prev  = NULL;
        b->lru_next  = NULL;
        lru_push_front(b);
    }
    buf_count    = (int)count;
    stats.blocks = count;
}

int bcache_enabled(void) {
    return buf_count > 0;
}

/* --- the fill ------------------------------------------------------------ */

/* Take a buffer for (dev, blkno), filling it from the device if it is not
 * already resident. Returns NULL if the fill failed, with *err set - the
 * caller has to distinguish "no buffer" from "the disk said -ENODEV", because
 * one of those is a cache problem and the other is the answer. */
static bcache_buf_t *get_block(device_t *dev, uint64 blkno, int64 *err) {
    bcache_buf_t *b = lookup(dev, blkno);
    int64 got;

    if (b != NULL) {
        stats.hits++;
        lru_touch(b);
        return b;
    }

    stats.misses++;

    b = lru_tail;
    if (b == NULL) {
        *err = -12;                          /* -ENOMEM */
        return NULL;
    }
    if (b->dev != NULL) {
        stats.evictions++;
        hash_remove(b);
        b->dev = NULL;
    }

    /* Fill BEFORE the key is installed. A buffer that is in the hash while its
     * contents are undefined is a buffer another read can find - not possible
     * on this kernel today, and installing the key first would make that a
     * property of the scheduler rather than of this function. */
    stats.fills++;
    got = dev_read_raw(dev, blkno << BCACHE_BLOCK_SHIFT, b->data,
                       BCACHE_BLOCK_SIZE);
    if (got < 0) {
        *err = got;
        return NULL;
    }
    /* A short read is the end of the device, which is not an error: the
     * caller's own bounds check has already refused anything past the end, so
     * the only bytes affected are the tail of the last block. Zero them rather
     * than leaving the evicted block's contents there, or a read that stops
     * one byte short of a block boundary returns another disk's data for the
     * rest of the block the day something reads it. */
    if ((uint64)got < BCACHE_BLOCK_SIZE) {
        bc_zero(b->data + got, BCACHE_BLOCK_SIZE - (uint64)got);
    }

    b->dev   = dev;
    b->blkno = blkno;
    hash_insert(b);
    lru_touch(b);
    return b;
}

/* --- the operations ------------------------------------------------------ */

int64 bcache_read(device_t *dev, uint64 offset, void *buf, uint64 n) {
    uint8 *dst = (uint8 *)buf;
    uint64 done = 0;
    uint64 end;

    if (!bcache_enabled() || dev->size == 0) {
        /* An unsized device has no block count and no end - a serial port, a
         * future ramdisk that grows. Caching it would need a key space this
         * file cannot bound. */
        stats.bypasses++;
        return dev_read_raw(dev, offset, buf, n);
    }

    end = dev->size;
    if (offset >= end) {
        return 0;                            /* past the end is EOF */
    }
    if (n > end - offset) {
        n = end - offset;
    }

    while (done < n) {
        uint64 pos    = offset + done;
        uint64 blkno  = pos >> BCACHE_BLOCK_SHIFT;
        uint64 within = pos & (BCACHE_BLOCK_SIZE - 1);
        uint64 chunk  = BCACHE_BLOCK_SIZE - within;
        int64  err    = -5;
        bcache_buf_t *b;

        if (chunk > n - done) {
            chunk = n - done;
        }
        b = get_block(dev, blkno, &err);
        if (b == NULL) {
            /* Bytes already copied are real and were read successfully.
             * Returning the count matches what a driver does with a transfer
             * that fails partway, and it is what lets a caller reading a whole
             * file in a loop stop at the right place instead of retrying from
             * zero. */
            return done > 0 ? (int64)done : err;
        }
        bc_copy(dst + done, b->data + within, chunk);
        done += chunk;
    }
    return (int64)done;
}

int64 bcache_write(device_t *dev, uint64 offset, const void *buf, uint64 n) {
    const uint8 *src = (const uint8 *)buf;
    uint64 done = 0;
    uint64 end;

    if (!bcache_enabled() || dev->size == 0) {
        stats.bypasses++;
        return dev_write_raw(dev, offset, buf, n);
    }

    end = dev->size;
    /* -ENOSPC past the end, matching disk.c: a device does not grow, and a
     * caller told it wrote fewer bytes than it asked for retries forever. */
    if (offset >= end) {
        return -28;
    }
    if (n > end - offset) {
        n = end - offset;
    }

    while (done < n) {
        uint64 pos    = offset + done;
        uint64 blkno  = pos >> BCACHE_BLOCK_SHIFT;
        uint64 within = pos & (BCACHE_BLOCK_SIZE - 1);
        uint64 chunk  = BCACHE_BLOCK_SIZE - within;
        bcache_buf_t *b;
        int64 put;

        if (chunk > n - done) {
            chunk = n - done;
        }

        b = lookup(dev, blkno);
        if (b == NULL && within == 0 && chunk == BCACHE_BLOCK_SIZE) {
            /* A whole block, not resident. Take a buffer and fill it from the
             * CALLER rather than from the device: the write is about to make
             * every byte of it the caller's anyway, and reading first would be
             * a device read whose result is discarded in full. */
            b = lru_tail;
            if (b != NULL) {
                if (b->dev != NULL) {
                    stats.evictions++;
                    hash_remove(b);
                }
                b->dev   = dev;
                b->blkno = blkno;
                hash_insert(b);
                lru_touch(b);
            }
        } else if (b == NULL) {
            /* A partial block that is not resident. Deliberately NOT filled:
             * caching it would cost a device read to hold data the caller has
             * shown no sign of wanting to read back. The write still happens;
             * it is just not remembered. */
            stats.bypasses++;
        }

        if (b != NULL) {
            bc_copy(b->data + within, src + done, chunk);
        }

        stats.writes++;
        put = dev_write_raw(dev, pos, src + done, chunk);
        if (put != (int64)chunk) {
            /* The device did not take what the cache now holds. Invariant 1
             * has been broken and the only way to restore it without knowing
             * how much of the write landed is to forget the block. Keeping it
             * would mean the next read returns bytes that are not on the
             * disk - a write error that turns into silent corruption on a
             * later read is far worse than the write error itself. */
            if (b != NULL) {
                evict(b);
            }
            if (put < 0) {
                return done > 0 ? (int64)done : put;
            }
            done += (uint64)put;
            return (int64)done;
        }
        done += chunk;
    }
    return (int64)done;
}

void bcache_invalidate_dev(device_t *dev) {
    int i;

    for (i = 0; i < buf_count; i++) {
        if (bufs[i].dev == dev) {
            evict(&bufs[i]);
        }
    }
}

void bcache_invalidate_all(void) {
    int i;

    for (i = 0; i < buf_count; i++) {
        evict(&bufs[i]);
    }
}

void bcache_get_stats(bcache_stats_t *out) {
    int i;
    uint64 resident = 0;

    if (out == NULL) {
        return;
    }
    for (i = 0; i < buf_count; i++) {
        if (bufs[i].dev != NULL) {
            resident++;
        }
    }
    stats.resident = resident;
    *out = stats;
}

void bcache_report(void) {
    bcache_stats_t s;

    bcache_get_stats(&s);
    print_string("bcache: ", 0x0F);
    if (s.blocks == 0) {
        print_string("disabled (no memory)\n", 0x0C);
        return;
    }
    print_hex((uint32)s.blocks, 0x07);
    print_string(" blocks, hits ", 0x07);
    print_hex((uint32)s.hits, 0x0A);
    print_string(" misses ", 0x07);
    print_hex((uint32)s.misses, 0x0A);
    print_string(" evictions ", 0x07);
    print_hex((uint32)s.evictions, 0x07);
    print_string(" writes ", 0x07);
    print_hex((uint32)s.writes, 0x07);
    print_string("\n", 0x07);
}
