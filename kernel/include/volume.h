#ifndef VOLUME_H
#define VOLUME_H

#include "device.h"
#include "fs.h"
#include "part.h"
#include "typesk.h"

/* One partition, as a device that PARSES a path.
 *
 * disk.h drew the line: \Device\Harddisk0\DR0 hands you bytes at an offset and
 * has no idea what a filesystem is, and \Device\HarddiskVolume1 parses the
 * rest of the path and hands back the file named by it. The raw half landed
 * first. This is the other half, and it is the reason device.h had to exist:
 * "parse a remainder into an object" is not something an object_t can do.
 *
 * --- The two spellings, and why neither is built on the other ------------
 *
 *   \??\D:\bin\sh  -> ns_lookup follows the link to \Device\HarddiskVolume2,
 *                     stops at the object, hands back "\bin\sh"
 *                  -> vol_parse converts to "/bin/sh" and asks the volume's
 *                     filesystem directly
 *
 *   /mnt/usb/bin/sh -> fs_volume_for finds the longest mount point, /mnt/usb,
 *                     and hands "/bin/sh" to the same filesystem
 *
 * Two resolvers, one filesystem, and the paths they hand down are identical.
 * The alternative - implement the NT side by synthesising a POSIX path and
 * calling openat - fails for a volume with no mount point at all, which is
 * the normal state of a stick you have not mounted. The alternative the other
 * way round - implement POSIX by synthesising \??\ paths - fails as soon as
 * two mounts of one volume exist, because there is only one drive letter.
 *
 * --- Drive letters -------------------------------------------------------
 * The rule, decided here rather than when a second volume lands:
 *
 *   A: and B: are never assigned. They meant floppies, and every DOS-era
 *       program that special-cases a drive letter special-cases these two.
 *       Two letters is a cheap price for not finding out which ones.
 *   C: is the volume mounted at "/" - the system volume, which is what C:
 *       means on Windows and what makes \??\C:\wsr\Windows\System32 resolve.
 *   D: onward, in the order volumes are discovered, to every volume that
 *       part_type_is_data accepts.
 *
 * Letters ARE reused after removal. The alternative - never reuse, so a stale
 * handle can never see a different medium - runs out after twenty-three
 * insertions, and it does not buy what it looks like it buys: a handle holds
 * a reference to the volume OBJECT, and that object stays alive and answers
 * -ENODEV whatever the letter is later given to. The name is reused; the
 * object is not.
 */

typedef struct volume volume_t;

/* Discover the partitions on `disk` and create a volume device for each.
 *
 * Returns the number of volumes created, or a negative errno. A disk with no
 * partition table gets ONE volume covering the whole disk - which is the
 * current build's data disk, and treating it as "no volumes" would mean the
 * kernel that boots today stops booting. */
int volume_scan_disk(device_t *disk);

/* Try each registered filesystem against `vol` and mount the first that
 * recognises it. Returns 0, or -ENODEV if none did.
 *
 * Separate from volume_scan_disk on purpose: a volume that exists and cannot
 * be identified is still a volume, still has a device object, and can still
 * be read raw. Folding the two together means a filesystem this kernel does
 * not implement makes the partition disappear. */
int volume_probe(volume_t *vol);

/* Mount an identified volume at an absolute POSIX path. Thin over
 * fs_mount_at; it exists so the volume remembers where it went and surprise
 * removal can take it down without searching the table. */
int volume_mount(volume_t *vol, const char *mount_point);

/* Unmount, drop the drive letter, and detach the device. What both eject and
 * surprise removal end at - they differ in what happens BEFORE, not after. */
void volume_remove(volume_t *vol);

/* The volume behind a device, or NULL if that device is not a volume. */
volume_t *volume_from_device(const device_t *dev);

/* The volume's filesystem, or NULL if nothing is mounted on it. */
fs_volume_t *volume_fs(const volume_t *vol);

/* Assigned drive letter, or 0 if it has none. */
char volume_letter(const volume_t *vol);

/* --- filesystem probes ---------------------------------------------------
 *
 * A filesystem registers a prober: given a volume device, decide whether this
 * is your format and if so mount it. Returning NULL is a normal answer and
 * means "not mine", not an error.
 *
 * A table rather than an if-chain in volume.c, because the whole point of the
 * fs vtable was that adding ext2 or ZFS touches no file that already works.
 * An if-chain naming every filesystem is that file. */
typedef fs_volume_t *(*fs_probe_fn)(device_t *dev);

/* Register a prober. Called from each filesystem's own init, before the first
 * volume scan. Returns 0 or -ENOSPC. */
int volume_register_fs(const char *name, fs_probe_fn probe);

/* Bring up every disk device: scan, probe, letter, and mount the root if one
 * is found. Called once at boot, after disks are registered. */
void volume_init(void);

/* Print one line per volume. */
void volume_report(void);

#endif
