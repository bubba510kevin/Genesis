#ifndef PCACHE_H
#define PCACHE_H

#include "typesk.h"

struct fs_node;
struct fs_volume;

/* The FILE page cache - ROADMAP item 7's third blocker, first half.
 *
 * See kernel/fs/pcache.c for what it does, what it deliberately does not do
 * yet (writeback of extending writes), and why that half is separable. */

/* Read through the cache. Same contract as fs_read: bytes transferred, or a
 * negative errno. Falls through to the filesystem for anything not cached. */
int64 pcache_read(const struct fs_node *n, uint64 offset, void *buf,
                  uint64 max);

/* Tell the cache that these bytes of this file have changed underneath it.
 * Called by every path that writes, truncates or removes a file. Passing
 * max = 0 means "the whole file". */
void pcache_invalidate(const struct fs_node *n, uint64 offset, uint64 max);

/* Drop every page belonging to a volume. Called on unmount: the fs_volume_t
 * is about to be retired and its pages describe bytes nobody can name. */
void pcache_invalidate_volume(const struct fs_volume *vol);

/* Drop clean pages under memory pressure. Wired to vm_lowmem - see
 * kernel/bsd/kern_lowmem.c. Returns how many pages were released. */
int pcache_reclaim(void);

/* Hits, misses and device reads, for the report and for the check that the
 * cache is a cache. */
void pcache_stats(uint64 *hits, uint64 *misses, uint64 *evictions);
void pcache_report(uint8 color);
int  pcache_selftest(void);

#endif
