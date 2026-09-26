#ifndef DEVICES_H
#define DEVICES_H

#include "ns.h"
#include "object.h"
#include "typesk.h"

/* Pseudo-devices: objects with behaviour and no hardware.
 *
 * /dev/null is the first one, and it is worth being clear about what makes it
 * a device rather than a special case in open(2). It is an object_t with a
 * type whose read returns end-of-file and whose write accepts everything -
 * nothing else in the kernel knows it exists. It gets a name in \Device\, a
 * link in \??\, and from there /dev/null works because the POSIX rewrite in
 * sys_openat already resolves through the namespace. No syscall changed.
 *
 * Adding another is the same three steps: a type, an ob_create, a link. */
void devices_register(void);

/* Open a namespace DIRECTORY as a listable object.
 *
 * \??\ is a directory, not a device, so ns_lookup answers -EISDIR for it -
 * correct, because a directory of devices is not something you read bytes
 * from. But `ls /dev` still has to work, and what it needs is a handle it can
 * call getdents64 on. That is what this returns: an object whose getdents
 * walks the namespace directly, so the listing is the namespace rather than a
 * copy of it that can drift.
 *
 * NULL if the path is not a namespace directory, or if the object pool is
 * full. The caller owns the reference. */
object_t *nsdir_open(const char *ns_path);

/* Resolve a POSIX /dev path - "/dev", "/dev/null", "/dev/C:" - to an object.
 *
 * This is the whole of the /dev policy, in one place, because it used to be
 * two: sys_openat and sys_stat_path each rewrote /dev/<name> into \??\<name>
 * themselves, which meant a device reachable by open() and a device visible
 * to stat() were two separate claims that could disagree.
 *
 * /dev is a view onto \??\ and \Device\ both, searched in that order, so a
 * device that was only ever ns_insert()ed is reachable without anyone
 * remembering to write a DOS link for it. See devices.c for why that ordering
 * and not the other one.
 *
 * On success *out holds a REFERENCED object the caller must ob_deref, and
 * `remainder` holds whatever followed the component that named it - "/dev/C:"
 * with nothing after it gives an empty remainder, "/dev/C:/etc/motd" gives
 * "\etc\motd". Errors are the errno the syscall should return, negated:
 * -ENOENT, -ENOTDIR, -ELOOP, -ENAMETOOLONG, -ENFILE. */
int dev_lookup(const char *posix_path, object_t **out,
               char *remainder, uint64 remainder_size);

/* Is this one of the objects dev_lookup hands back for a DIRECTORY - /dev
 * itself, or a namespace directory below it? Asked by stat, which reports a
 * different mode for one. */
int dev_is_directory(const object_t *obj);

#endif
