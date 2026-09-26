#ifndef FATFS_H
#define FATFS_H

#include "device.h"
#include "fs.h"

/* Mount the FAT16 filesystem on `dev` and return it as a generic volume, or
 * NULL if the device holds no FAT16 signature.
 *
 * The only entry point to this filesystem from above. Everything else about
 * FAT reaches the rest of the kernel through the fs_ops vtable, which is the
 * point - flk.c calls this and hands the result to fs_set_root, and nothing
 * else in the kernel names FAT at all. */
fs_volume_t *fatfs_mount(device_t *dev);

/* The prober volume.c calls: is this volume FAT16, and if so mount it.
 *
 * Returns NULL for "not mine", which is a normal answer rather than an error -
 * that is the whole contract of a probe table, and it is why this cannot just
 * be fatfs_mount under another name: fatfs_mount on a non-FAT volume should
 * say so loudly, and a prober should say so quietly. */
fs_volume_t *fatfs_probe(device_t *dev);

#endif
