/* The FILE page cache.
 *
 * --- what this is for -----------------------------------------------------
 *
 * ROADMAP item 7's third blocker: "A PAGE CACHE WITH WRITEBACK. ZFS's read
 * path is built on ARC buffers handed to the VM; the write path is dirty
 * pages flushed at txg commit. Genesis has a BLOCK cache (kernel/fs/bcache.c)
 * which is a different thing at a lower layer."
 *
 * The distinction that entry is drawing is the whole reason this file is not
 * bcache.c with a bigger number. bcache caches DISK BLOCKS by (device, LBA).
 * That helps a filesystem re-reading the same sector, and it does nothing at
 * all for the question "have I already read this part of this FILE" - because
 * answering that means walking the filesystem's own structures to turn a file
 * offset into an LBA, which is most of the cost of the read. Item 7(d) names
 * the consequence: "exec of a large binary off ZFS re-reads it through the
 * block cache every time".
 *
 * So this caches by (volume, inode, page index), above the filesystem rather
 * than below it, and a hit costs a hash lookup and a memcpy.
 *
 * --- WHAT IT DOES NOT DO YET, and why that half is separable --------------
 *
 * WRITES ARE WRITE-THROUGH AND INVALIDATING, not written back. A write goes
 * to the filesystem exactly as before and the affected pages are dropped from
 * the cache.
 *
 * That is not the finished article - item 7 wants dirty pages flushed at a
 * transaction boundary - and stopping here is a decision rather than fatigue.
 * Deferring a write means deferring the ALLOCATION behind it, and on FAT the
 * two are the same act: fat_write_entry_at extends the chain, links clusters
 * and rewrites the directory record as it goes. A dirty page for an offset
 * past EOF describes bytes that have no cluster yet, so flushing it later has
 * to allocate later - and until it does, the file's size on disk disagrees
 * with the size every reader is being told. Getting that wrong does not
 * produce a slow filesystem, it produces a file whose length is a lie.
 *
 * ZFS does not have this problem, which is exactly why its write path is
 * built the way it is: a txg commit allocates and writes together, and
 * nothing is visible until it does. The writeback half therefore belongs with
 * the DMU rather than here, and building it against FAT first would mean
 * building it twice.
 *
 * What IS here is the half both paths need and the half item 7(d) asks for.
 *
 * --- eviction, and the tie to the other blocker ---------------------------
 *
 * Clock/second-chance over a fixed pool, plus a vm_lowmem handler so the
 * cache shrinks when the machine is short - which only became possible when
 * blocker (4) was built (kernel/bsd/kern_lowmem.c). A cache with no way to be
 * told to shrink is the thing that entry says the ARC must not be.
 */

#include "pcache.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"

/* 64 pages, 256KB. Sized against what it is for rather than against the
 * machine: the working set that matters is the binary being exec'd and the
 * directory it came from, and 256KB covers busybox twice over. It is a fixed
 * pool rather than a growing one because the reclaim path has to be able to
 * bound it, and a cache that can grow without limit under memory pressure is
 * the failure this exists next to. */
#define PC_PAGES    64
#define PC_PAGESZ   4096
#define PC_BUCKETS  64

typedef struct {
    const fs_volume_t *vol;      /* NULL = free slot */
    uint64 ino;
    uint64 page;                 /* index, not offset */
    uint32 valid;                /* bytes of this page that are real */
    int    ref;                  /* second-chance bit */
    int    next;                 /* hash chain: index, or -1 */
    uint8 *data;
} pc_page_t;

static pc_page_t pages[PC_PAGES];
static int       buckets[PC_BUCKETS];
static int       ready;
static int       clock_hand;

static uint64 pc_hits, pc_misses, pc_evictions;

static uint32 hash_of(const fs_volume_t *vol, uint64 ino, uint64 page) {
    uint64 h = (uint64)(uintptr)vol;

    h ^= ino * 0x9E3779B97F4A7C15ULL;
    h ^= page * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 29;
    return (uint32)(h % PC_BUCKETS);
}

static void pc_init(void) {
    int i;

    if (ready) {
        return;
    }
    for (i = 0; i < PC_BUCKETS; i++) {
        buckets[i] = -1;
    }
    for (i = 0; i < PC_PAGES; i++) {
        pages[i].vol  = NULL;
        pages[i].next = -1;
        pages[i].data = NULL;
    }
    ready = 1;
}

static void unlink_page(int idx) {
    uint32 b;
    int    cur, prev = -1;

    if (pages[idx].vol == NULL) {
        return;
    }
    b = hash_of(pages[idx].vol, pages[idx].ino, pages[idx].page);
    for (cur = buckets[b]; cur >= 0; cur = pages[cur].next) {
        if (cur == idx) {
            if (prev < 0) {
                buckets[b] = pages[cur].next;
            } else {
                pages[prev].next = pages[cur].next;
            }
            break;
        }
        prev = cur;
    }
    pages[idx].vol  = NULL;
    pages[idx].next = -1;
}

static int lookup(const fs_volume_t *vol, uint64 ino, uint64 page) {
    int cur;

    for (cur = buckets[hash_of(vol, ino, page)]; cur >= 0;
         cur = pages[cur].next) {
        if (pages[cur].vol == vol && pages[cur].ino == ino &&
            pages[cur].page == page) {
            return cur;
        }
    }
    return -1;
}

/* Second chance: sweep, clearing the reference bit, and take the first slot
 * whose bit was already clear. Plain LRU would need a list touched on every
 * hit; this needs one bit and a hand, and gives the same answer for the case
 * that matters - a page read once and never again loses to one read twice. */
static int evict_one(void) {
    int spins;

    for (spins = 0; spins < PC_PAGES * 2; spins++) {
        int idx = clock_hand;

        clock_hand = (clock_hand + 1) % PC_PAGES;
        if (pages[idx].vol == NULL) {
            return idx;                  /* free already */
        }
        if (pages[idx].ref) {
            pages[idx].ref = 0;
            continue;
        }
        unlink_page(idx);
        pc_evictions++;
        return idx;
    }
    /* Everything referenced twice round. Take the hand's slot rather than
     * failing: a cache that refuses to evict stops being a cache. */
    unlink_page(clock_hand);
    pc_evictions++;
    return clock_hand;
}

static int page_for(const fs_node_t *n, uint64 page, int *out_hit) {
    int idx = lookup(n->vol, n->ino, page);
    int64 got;

    if (idx >= 0) {
        pages[idx].ref = 1;
        *out_hit = 1;
        return idx;
    }
    *out_hit = 0;

    idx = evict_one();
    if (pages[idx].data == NULL) {
        pages[idx].data = (uint8 *)kmalloc(PC_PAGESZ);
        if (pages[idx].data == NULL) {
            return -1;
        }
    }

    got = n->vol->ops->read(n->vol, n, page * PC_PAGESZ, pages[idx].data,
                            PC_PAGESZ);
    if (got < 0) {
        return -1;
    }

    {
        uint32 b = hash_of(n->vol, n->ino, page);

        pages[idx].vol   = n->vol;
        pages[idx].ino   = n->ino;
        pages[idx].page  = page;
        pages[idx].valid = (uint32)got;
        pages[idx].ref   = 1;
        pages[idx].next  = buckets[b];
        buckets[b]       = idx;
    }
    return idx;
}

int64 pcache_read(const fs_node_t *n, uint64 offset, void *buf, uint64 max) {
    uint8 *dst = (uint8 *)buf;
    uint64 done = 0;

    if (n == NULL || n->vol == NULL || n->vol->ops == NULL ||
        n->vol->ops->read == NULL) {
        return -5;
    }
    pc_init();

    while (done < max) {
        uint64 pos  = offset + done;
        uint64 page = pos / PC_PAGESZ;
        uint32 off  = (uint32)(pos % PC_PAGESZ);
        uint64 want = max - done;
        int    hit  = 0;
        int    idx;

        if (want > PC_PAGESZ - off) {
            want = PC_PAGESZ - off;
        }

        idx = page_for(n, page, &hit);
        if (idx < 0) {
            /* The cache could not take it. Fall through to the filesystem for
             * the rest rather than failing the read: a cache that turns into
             * an error when it is full is worse than no cache. */
            int64 direct = n->vol->ops->read(n->vol, n, pos, dst + done,
                                             max - done);

            if (direct < 0) {
                return done > 0 ? (int64)done : direct;
            }
            return (int64)(done + (uint64)direct);
        }
        if (hit) {
            pc_hits++;
        } else {
            pc_misses++;
        }

        if (off >= pages[idx].valid) {
            break;                       /* past end of file */
        }
        if (want > pages[idx].valid - off) {
            want = pages[idx].valid - off;
        }
        {
            uint64 i;
            for (i = 0; i < want; i++) {
                dst[done + i] = pages[idx].data[off + i];
            }
        }
        done += want;
        if (pages[idx].valid < PC_PAGESZ) {
            break;                       /* short page: end of file */
        }
    }
    return (int64)done;
}

void pcache_invalidate(const fs_node_t *n, uint64 offset, uint64 max) {
    uint64 first, last, p;

    if (n == NULL || n->vol == NULL || !ready) {
        return;
    }
    first = offset / PC_PAGESZ;
    /* max == 0 means the whole file, and it has to be spelled that way rather
     * than as a huge length: a caller that has just truncated does not know
     * how big the file WAS, which is exactly the range that has to go. */
    if (max == 0) {
        int i;
        for (i = 0; i < PC_PAGES; i++) {
            if (pages[i].vol == n->vol && pages[i].ino == n->ino) {
                unlink_page(i);
            }
        }
        return;
    }
    last = (offset + max - 1) / PC_PAGESZ;
    for (p = first; p <= last; p++) {
        int idx = lookup(n->vol, n->ino, p);
        if (idx >= 0) {
            unlink_page(idx);
        }
    }
}

void pcache_invalidate_volume(const fs_volume_t *vol) {
    int i;

    if (!ready) {
        return;
    }
    for (i = 0; i < PC_PAGES; i++) {
        if (pages[i].vol == vol) {
            unlink_page(i);
        }
    }
}

int pcache_reclaim(void) {
    int i, freed = 0;

    if (!ready) {
        return 0;
    }
    /* Every page here is clean by construction - see the file comment - so
     * reclaim is unconditional. The day writeback lands, this grows a "flush
     * it first" branch and the distinction starts to matter. */
    for (i = 0; i < PC_PAGES; i++) {
        if (pages[i].vol != NULL) {
            unlink_page(i);
            freed++;
        }
        if (pages[i].data != NULL) {
            kfree(pages[i].data);
            pages[i].data = NULL;
        }
    }
    return freed;
}

void pcache_stats(uint64 *hits, uint64 *misses, uint64 *evictions) {
    if (hits != NULL)      *hits = pc_hits;
    if (misses != NULL)    *misses = pc_misses;
    if (evictions != NULL) *evictions = pc_evictions;
}

void pcache_report(uint8 color) {
    int i, used = 0;

    for (i = 0; i < PC_PAGES; i++) {
        if (pages[i].vol != NULL) {
            used++;
        }
    }
    kprintf_c(color, "pcache: %d of %d pages, hits %lx misses %lx "
                     "evictions %lx\n", used, PC_PAGES, pc_hits, pc_misses,
              pc_evictions);
}

/* --- is it a cache? -------------------------------------------------------
 *
 * One question, and it is not "does it return the right bytes" - a cache that
 * ignored itself and called the filesystem every time returns the right bytes
 * too. The question is whether the SECOND read of the same page reaches the
 * filesystem, and the only way to see that is to count.
 *
 * The control is the miss count. Asserting hits rise proves nothing on its
 * own: a counter incremented in the wrong branch would do that. Both are
 * checked, and the miss count must NOT move on the second pass.
 */
int pcache_selftest(void) {
    int failures = 0;
    fs_node_t n;
    uint64 h0, m0, h1, m1, h2, m2;
    char buf[64];
    int64 r1, r2;

#define PCT(cond, what)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            kprintf_c(0x0C, "pcache: %s\n", (what));                        \
            failures++;                                                     \
        }                                                                   \
    } while (0)

    if (fs_lookup("/etc/motd", &n) != 0 || n.is_dir) {
        kprintf_c(0x0E, "pcache: /etc/motd not found - cache NOT exercised\n");
        return 0;
    }

    /* Start from a known-cold page rather than from whatever boot left
     * behind. /etc/motd is PRINTED at boot, so its first page is already
     * cached by the time this runs and the "first read misses" assertion was
     * failing against a working cache - the test's assumption, not the
     * cache's behaviour. */
    pcache_invalidate(&n, 0, 0);

    pcache_stats(&h0, &m0, NULL);
    r1 = pcache_read(&n, 0, buf, sizeof(buf));
    pcache_stats(&h1, &m1, NULL);

    PCT(r1 > 0, "the first read returned nothing");
    PCT(m1 > m0, "the first read did not MISS - it never reached the disk");

    r2 = pcache_read(&n, 0, buf, sizeof(buf));
    pcache_stats(&h2, &m2, NULL);

    PCT(r2 == r1, "the second read returned a different length");
    PCT(h2 > h1, "the second read did not HIT");
    PCT(m2 == m1,
        "the second read MISSED as well - the cache is not caching");

    /* And invalidation really drops it, or a write would be invisible to
     * every subsequent read. The control is the pair: a miss here, having
     * just had a hit above, is the whole proof. */
    pcache_invalidate(&n, 0, 0);
    (void)pcache_read(&n, 0, buf, sizeof(buf));
    {
        uint64 h3, m3;
        pcache_stats(&h3, &m3, NULL);
        PCT(m3 > m2, "a read after invalidate still hit the cache");
    }

    if (failures == 0) {
        kprintf("pcache: selftest passed\n");
    } else {
        kprintf_c(0x0C, "pcache: selftest FAILED (%d)\n", failures);
    }
    return failures;
#undef PCT
}
