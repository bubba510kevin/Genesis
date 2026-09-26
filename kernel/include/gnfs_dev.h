#ifndef GNFS_DEV_H
#define GNFS_DEV_H

#include "device.h"
#include "fs.h"
#include "gnfs_layout.h"

/* The device-facing half of gnfs (kernel/gnfs/gnfs_vfs.c), declared
 * separately from kernel/include/gnfs_layout.h on purpose: that header is
 * pure logic with no notion of a device_t, portable to a host test or the
 * host format tool with no kernel underneath it. This one is not - it is
 * shared by gnfs_vfs.c itself and by tests/host/gnfs_test.c, which drives
 * gnfs_probe and gnfs_txg_commit against tests/host/dev_stub.c's fake
 * device_t the same way tests/host/fat_test.c already does for FAT. */

fs_volume_t *gnfs_probe(device_t *dev);

/* Advance the volume `dev` holds by one txg, writing `bitmap` (exactly
 * `bitmap_bytes_used` bytes of it) and `obj_table` (exactly
 * `obj_table_bytes_used` bytes of it) to whichever of their two on-disk
 * regions the current root does not occupy, then a new root record naming
 * both. `cur` is both the caller's view of the current root and, on
 * success, updated to the new one - see kernel/gnfs/gnfs_vfs.c for the
 * crash-safety argument. Returns 0 or a negative errno; on failure `*cur` is
 * untouched and the volume is unchanged. */
int gnfs_txg_commit(device_t *dev, gnfs_root_t *cur, const uint8 *bitmap,
                    uint64 bitmap_bytes_used, const uint8 *obj_table,
                    uint64 obj_table_bytes_used);

#endif /* GNFS_DEV_H */
