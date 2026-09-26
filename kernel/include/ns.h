#ifndef NS_H
#define NS_H

#include "object.h"
#include "typesk.h"

/* The object namespace: names for objects, arranged in a tree.
 *
 * --- What this is for ----------------------------------------------------
 * object.c answers "what can I do with this handle". It has no answer to
 * "give me the console" that is not a direct C call to tty_console(). That
 * works while the kernel is the only thing naming objects, and stops working
 * the moment a program does - which is the whole content of an open(2) or an
 * NtCreateFile.
 *
 * NT's answer is a single hierarchical namespace holding everything nameable:
 * devices, drivers, links, events, sections. POSIX's answer is the filesystem
 * with device nodes grafted into it. These are not as different as they look
 * - /dev/console and \Device\Console are the same lookup - and the design
 * here is NT's, with the POSIX spelling layered on top rather than beside it.
 * That ordering matters for the PE personality later: two namespaces would
 * mean two sets of names for one console.
 *
 * --- The shape -----------------------------------------------------------
 *   \                     the root
 *   \Device\              real device objects live here
 *   \Device\Console
 *   \Device\Keyboard
 *   \Device\HarddiskVolume1
 *   \??\                  DOS-device names: links INTO \Device\
 *   \??\CON       -> \Device\Console
 *   \??\C:        -> \Device\HarddiskVolume1
 *
 * \??\ is not decoration. It is the indirection that lets "C:" mean a
 * different volume per session in NT, and here it is what lets a name a
 * program knows resolve to a device the kernel chose - the drive letter is
 * policy, the device object is mechanism, and a link is the seam between
 * them.
 *
 * --- The unparsed remainder ----------------------------------------------
 * This is the part worth getting right now rather than later. Resolution
 * stops at the first component that names an OBJECT rather than a directory,
 * and hands back everything after it untouched:
 *
 *   \??\C:\bin\busybox   ->  object = \Device\HarddiskVolume1
 *                            remainder = "\bin\busybox"
 *
 * The namespace does not know what "\bin\busybox" means and must not try -
 * that string belongs to the device, which parses it as a path on its volume.
 * That single rule is what lets one namespace address filesystems, serial
 * ports and pipes without knowing anything about any of them, and it is what
 * the I/O manager in a later phase is built around.
 *
 * Separator is backslash, and lookup is case-insensitive, because both are
 * true of the namespace this imitates. The POSIX shim in sys_openat converts.
 */

#define NS_NAME_MAX      32
#define NS_PATH_MAX     128
/* Raised from 64. Eleven entries was the whole namespace for a long time -
 * the devices, the volumes and the \??\ links - and 64 was generous against
 * that. ROADMAP item 14 flagged it as "fine for eleven entries and not fine
 * once every named event is one", and that is now the case: \ObjectTypes
 * publishes one entry per registered type, and \BaseNamedObjects takes one
 * per named dispatcher object, which is a number userspace chooses.
 *
 * An ns_entry_t is a name, a kind, two pointers and a path, so this costs
 * bytes of .bss. Running out is not graceful - ns_insert answers -ENOSPC and
 * the caller usually cannot do anything useful with that - so the number
 * should be comfortable rather than tight. */
#define NS_MAX_ENTRIES  256

/* A link that resolves to another link is normal (\??\C: could point at a
 * link). A link that eventually reaches itself is not, and without a cap it
 * is a kernel hang rather than an error. */
#define NS_MAX_LINK_DEPTH 8

typedef enum {
    NS_NONE = 0,
    NS_DIRECTORY,      /* contains other entries                        */
    NS_LINK,           /* a symbolic name for another path              */
    NS_OBJECT          /* names an object_t; resolution stops here      */
} ns_kind_t;

typedef struct ns_entry {
    char             name[NS_NAME_MAX];
    ns_kind_t        kind;
    struct ns_entry *parent;
    object_t        *object;               /* NS_OBJECT only */
    char             target[NS_PATH_MAX];  /* NS_LINK only   */
} ns_entry_t;

/* Build the root and the two standard directories. Safe to call twice; the
 * second call is a no-op. Nothing else in this file works before it. */
void ns_init(void);

/* The root directory entry. Mostly for tests and for iteration. */
ns_entry_t *ns_root(void);

/* Create a directory, and every directory above it that does not yet exist -
 * so ns_mkdir("\\Device\\Storage") works without three calls. Returns the
 * entry, or NULL if the pool is full or a component is already something
 * other than a directory. */
ns_entry_t *ns_mkdir(const char *path);

/* Name an object. Takes a reference, held until ns_remove or forever -
 * whichever comes first, and today it is always forever.
 *
 * Returns 0, -EEXIST if the name is taken, -ENOENT if a parent directory is
 * missing, -ENOSPC if the pool is full. */
int ns_insert(const char *path, object_t *obj);

/* Create a symbolic link at `path` naming `target`. The target is NOT
 * resolved now and need not exist yet, which is what lets \??\ be populated
 * before the devices it points at are created. */
int ns_link(const char *path, const char *target);

/* Remove one entry, without following a trailing link.
 *
 * ns_remove("\\??\\D:") removes the drive LETTER; the volume it named is
 * untouched. Following the link would make deleting a name delete the device,
 * which is the one thing nobody expects from unlinking a symlink.
 *
 * Drops the object reference the namespace held, so an object with no other
 * holder is destroyed here and one with open handles survives - which is what
 * makes surprise removal work: the name goes, the object lives until the last
 * descriptor closes, and every operation on it in between answers -ENODEV.
 *
 * Returns 0, -ENOENT, -EINVAL for the root, or -ENOTEMPTY for a directory
 * that still has children. */
int ns_remove(const char *path);

/* Remove EVERY entry naming this object, links included.
 *
 * The distinction from ns_remove matters for the merged /dev view: a device
 * named at \Device\HarddiskVolume2 and aliased at \??\D: has two entries, and
 * leaving either behind is a name that resolves to something gone. dev_detach
 * removes exactly the two names it inserted; this is for a caller that has
 * lost track, and for the verification check that asserts nothing was.
 *
 * Returns 0 if anything was removed, -ENOENT if nothing named it. */
int ns_remove_object(const object_t *obj);

/* The full path of an entry, built by walking parents to the root. What
 * ns_remove_object needs to recognise a link that points at an object it is
 * about to remove, and what a "list the namespace" tool wants for output. */
int ns_path_of(const ns_entry_t *e, char *out, uint64 cap);

/* Resolve a path to an object.
 *
 * On success *out holds a REFERENCED object - the caller owns that reference
 * and must ob_deref it - and `remainder` holds whatever followed the
 * component that named it, empty if nothing did. Pass remainder_size 0 and
 * remainder NULL if the caller cannot accept a remainder; a non-empty
 * remainder is then -ENOENT rather than silently discarded, because
 * discarding it is how "\??\C:\bin\sh" quietly becomes "the whole volume".
 *
 * Errors: -ENOENT, -ENOTDIR, -EISDIR (the path named a directory, which is
 * not an object), -ELOOP, -ENAMETOOLONG, -EINVAL for a relative path. */
int ns_lookup(const char *path, object_t **out,
              char *remainder, uint64 remainder_size);

/* Resolve to an entry rather than an object, without following a trailing
 * link. This is what a "list the namespace" tool wants; ns_lookup is what an
 * open wants. NULL if not found. */
ns_entry_t *ns_lookup_entry(const char *path);

/* The child of `dir` with this name, or NULL. Case-insensitive, like every
 * other comparison here.
 *
 * Exported because a caller that presents SEVERAL directories as one view -
 * /dev is the only one today - has to be able to ask "has an earlier
 * directory already claimed this name" without rebuilding a path and walking
 * from the root to answer it. */
ns_entry_t *ns_child(const ns_entry_t *dir, const char *name);

/* The namespace's name comparison, so nothing outside this file has to write
 * a second one. Two implementations of "are these the same name" that
 * disagree about case is the bug that makes \??\c: and \??\C: different
 * drives. */
int ns_name_eq(const char *a, const char *b);

/* Call `cb` for every child of the directory at `path`, in creation order.
 * A non-zero return from `cb` stops the walk and is returned. -ENOENT or
 * -ENOTDIR if the path is not a directory. */
int ns_iterate(const char *path,
               int (*cb)(const ns_entry_t *entry, void *ctx), void *ctx);

/* The same, on a directory already in hand. Something holding an open handle
 * on a directory has already resolved it, and resolving the path again on
 * every getdents64 would let the answer change halfway through a listing. */
int ns_iterate_entry(const ns_entry_t *dir,
                     int (*cb)(const ns_entry_t *entry, void *ctx), void *ctx);

/* Print the tree. Called at boot behind the same verbosity flag as the E820
 * dump - a namespace you cannot see is a namespace you cannot debug. */
void ns_report(void);

#endif
