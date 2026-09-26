#ifndef FILEOBJ_H
#define FILEOBJ_H

#include "fs.h"
#include "object.h"

/* Files and directories as objects.
 *
 * The second object type, and the one that shows whether the first was worth
 * building. read() and write() were already calling through a type pointer,
 * so nothing in the syscall layer changes to support these - a descriptor
 * pointing at a file works because the syscall never knew it was pointing at
 * a console.
 *
 * Read-only for as long as the mounted filesystem is, and no longer: the
 * write path asks the volume rather than refusing unconditionally. A write to
 * a read-only volume returns -EROFS rather than -EBADF, because the
 * descriptor is fine and the operation is legitimate - the filesystem simply
 * cannot do it, and those are different enough that a caller should be able
 * to tell them apart.
 *
 * Nothing in this file names FAT. It holds fs_node_t values and calls through
 * the fs_ops vtable, which is what makes a second filesystem a matter of
 * mounting one rather than of editing this. */

/* Open a path as an object. Returns NULL and sets *err to a negative errno on
 * failure. The caller owns the reference. */
object_t *fileobj_open(const char *abs_path, uint32 access, int *err);

/* The same, on a volume already in hand, with `rel` relative to that volume's
 * root. What a volume device's parse op calls - see fs_lookup_on. */
object_t *fileobj_open_on(fs_volume_t *v, const char *rel, uint32 access,
                          int *err);

/* Non-zero if this object is a directory - what getdents64 checks before
 * walking, and what read() checks before refusing with -EISDIR. */
int fileobj_is_dir(const object_t *obj);

/* Size in bytes, or 0 for a directory. Only meaningful for an object
 * fileobj_is_file() accepts - it reads obj->body as an fs_node_t. */
/* The node behind a file object, or NULL. Valid while the caller holds a
 * reference to the object. See the definition for why this one hands back the
 * whole node where every other accessor here answers a single question. */
const fs_node_t *fileobj_node(const object_t *obj);

uint64 fileobj_size(const object_t *obj);

/* Non-zero if `obj` is a file or directory object from THIS file, answered
 * from its type pointer rather than by trusting what obj->body points at.
 * What a caller must ask before using fileobj_size or fileobj_truncate on an
 * object it did not open itself. */
int fileobj_is_file(const object_t *obj);

/* Set the file's length. -EINVAL if `obj` is not a file object, -EROFS if the
 * filesystem has no truncate operation. */
int fileobj_truncate(object_t *obj, uint64 size);

/* The filesystem's stable identity for this file, for st_ino. */
uint64 fileobj_ino(const object_t *obj);

/* Fill a getdents64 buffer starting from the *index*th entry, advancing
 * *pos past what was emitted. Returns bytes written, 0 at the end of the
 * directory, or a negative errno. */
int64 fileobj_getdents(object_t *obj, void *buf, uint64 max, uint64 *pos);

#endif
