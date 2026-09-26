#ifndef DISK_H
#define DISK_H

#include "device.h"
#include "object.h"
#include "typesk.h"

/* Raw disks and the partitions on them, as objects.
 *
 * --- Why a disk and a volume are two objects ------------------------------
 * They describe the same bytes and they are not the same thing, and
 * collapsing them costs you one of two capabilities:
 *
 *   \Device\Harddisk0\DR0   the whole disk, read and written at a byte
 *                           offset. This is what `dd if=/dev/sda` reads and
 *                           what a partition editor writes. It has no idea
 *                           what a filesystem is.
 *
 *   \Device\HarddiskVolume1 one partition, which PARSES the rest of the path
 *                           and hands back the file named by it.
 *
 * Collapse them into one object and either you cannot image the disk (because
 * the object insists on interpreting a path) or you cannot open a file on it
 * (because the object only knows how to hand you bytes at an offset). NT
 * keeps them separate for exactly this reason and so does POSIX - /dev/sda
 * and /dev/sda1 are different device nodes, and /dev/sda1 mounted somewhere
 * is a third thing again.
 *
 * --- What is here and what is not -----------------------------------------
 * The raw disk object and MBR parsing are here. The volume object that parses
 * a remainder into a file is NOT: that is the I/O manager's job, it needs the
 * filesystem vtable underneath it, and doing it now would mean a second path
 * resolver alongside the one in fs.h. The partition table is what a second
 * filesystem needs to be findable at all, which is why it is the half that
 * landed first. */

/* The partition types and the table parse moved to part.h, where the MBR and
 * GPT scanners sit together. A disk_partition_t with a 32-bit LBA and a type
 * BYTE could not describe a GPT entry, and keeping it beside a wider one
 * meant every caller knowing which scanner had run to interpret the type
 * field. See part.h. */

/* Register every ATA device as a raw block object in the namespace, plus a
 * \??\ link under the POSIX name. Called once at boot, after ata_init. */
void disk_register(void);

/* Non-zero if this object is a raw block device. stat needs it: a block
 * device reports S_IFBLK and a character device S_IFCHR, and a program that
 * gets the wrong one draws real conclusions from it - `dd` picks a block size
 * from it, and a partition tool refuses to touch a character device. */
int disk_is_block(const object_t *obj);

/* Size in bytes, or 0 if this is not a disk object.
 *
 * Its own accessor rather than reusing fileobj_size, which answers by reading
 * the object body as an fs_node_t - correct for a file and meaningless for
 * anything else. stat needs a real st_size here: a caller sizing a transfer
 * from stat concludes there is nothing to read when it gets zero. */
uint64 disk_size(const object_t *obj);

#endif
