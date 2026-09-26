#ifndef PATH_H
#define PATH_H

#include "typesk.h"

/* Turning what a user typed into an absolute path.
 *
 * Deliberately separate from fat.c. The FAT driver takes absolute paths and
 * says so - "a cwd belongs to a process and this layer has no idea one
 * exists". This is the layer that knows, and it does its work entirely in
 * strings: no disk access, no volume, nothing to fail except running out of
 * buffer.
 *
 * That split is not just tidiness. ".." is resolved TEXTUALLY here rather than
 * by following the ".." entry on disk, which is what shells do and what the
 * POSIX logical-path rules describe. It also happens to be the only thing that
 * works everywhere: the root directory of a FAT16 volume has no "." or ".."
 * entries at all, so "/.." cannot be resolved by lookup. Textually it is just
 * "/", which is the right answer. */

/* Longest absolute path the kernel will construct. Sized for 8.3 components
 * a few levels deep, which is all a FAT16 volume can hold anyway. */
#define PATH_MAX_LEN 128

#define PATH_OK          0
#define PATH_ERR_TOOLONG -1   /* the result does not fit in outsz  */
#define PATH_ERR_INVAL   -2   /* null argument, or outsz too small */

/* Resolve `in` against `cwd` and write the normalized absolute result to
 * `out`.
 *
 * `cwd` must be absolute; an absolute `in` ignores it entirely. The result
 * always begins with '/', never ends with one except for the root itself,
 * contains no empty components, and has every "." dropped and every ".."
 * applied. ".." at the root is the root - climbing above it is not an error,
 * it just does not go anywhere, which is what every real system does.
 *
 * Component length is NOT checked here. Whether "verylongname.text" can exist
 * is a property of the filesystem, not of path syntax, and fat_lookup rejects
 * it with its own error. Enforcing 8.3 in both places would mean changing two
 * files the day something other than FAT is mounted. */
int path_normalize(const char *cwd, const char *in, char *out, uint64 outsz);

#endif
