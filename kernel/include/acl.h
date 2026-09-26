#ifndef ACL_H
#define ACL_H

#include "acl_abi.h"
#include "typesk.h"

/* One access control list, two views of it.
 *
 * --- the decision this header encodes -------------------------------------
 * Genesis has to present POSIX permissions to a Linux process and Windows
 * permissions to an NT process, on the same file, at the same time, without
 * the two disagreeing. There are three ways to arrange that and only one of
 * them works:
 *
 *   (a) store mode bits, synthesise an ACL for NT. Loses everything an ACL
 *       can say that a mode word cannot, which is most of the reason anyone
 *       wants one - a named user, a deny, an inheritance rule.
 *   (b) store both and keep them in step. Two sources of truth. They drift,
 *       and the drift is silent until it is a security hole.
 *   (c) store the ACL, and treat the mode word as a PROJECTION of it.
 *
 * (c), which is also what ZFS does, and not by coincidence: ZFS's native ACL
 * is the NFSv4 ACL, NFSv4's model was taken from NT's, and every bit in
 * acl_abi.h has an NT name in its comment. The Windows view is not a
 * translation of anything - it is the stored form. The POSIX mode is the
 * derived one.
 *
 * The practical consequence is that chmod is lossy and open() is not. A
 * chmod has to rewrite the owner@/group@/everyone@ entries and cannot express
 * the rest, which is exactly why ZFS has an `aclmode` property arguing about
 * what chmod should do to an ACL it cannot represent. An access check reads
 * the ACL and never consults the mode at all.
 *
 * --- what a filesystem without ACLs does ----------------------------------
 * FAT16 has no owner and no permission bits. It does not get a special case:
 * acl_from_mode projects its fixed mode into a three-entry trivial ACL, and
 * every check above this line runs identically on both filesystems. A
 * filesystem gains real ACLs by filling in fs_ops_t::getacl, and nothing else
 * changes.
 */

/* --- mode bits ------------------------------------------------------------
 *
 * Spelled out here because the kernel had no S_* macros at all - syscall.c
 * wrote 0100755 and 0040755 as literals into a stat buffer. Those literals
 * were honest at the time: there was no owner, no ACL and nothing to report.
 * Now there is. */
#define S_IFMT   0170000u
#define S_IFDIR  0040000u
#define S_IFREG  0100000u
#define S_IFLNK  0120000u
#define S_IFCHR  0020000u
#define S_IFIFO  0010000u
#define S_IFSOCK 0140000u
#define S_IFBLK  0060000u

#define S_ISUID  0004000u
#define S_ISGID  0002000u
#define S_ISVTX  0001000u

#define S_IRUSR  0000400u
#define S_IWUSR  0000200u
#define S_IXUSR  0000100u
#define S_IRGRP  0000040u
#define S_IWGRP  0000020u
#define S_IXGRP  0000010u
#define S_IROTH  0000004u
#define S_IWOTH  0000002u
#define S_IXOTH  0000001u

#define S_IRWXU  (S_IRUSR | S_IWUSR | S_IXUSR)
#define S_IRWXG  (S_IRGRP | S_IWGRP | S_IXGRP)
#define S_IRWXO  (S_IROTH | S_IWOTH | S_IXOTH)

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

/* --- an identity ----------------------------------------------------------
 *
 * Small and by value. A credential is read on every access check and copied
 * on every fork; making it a pointer to a refcounted object would buy sharing
 * that nothing here needs and cost a lifetime rule that everything here would
 * have to obey.
 *
 * euid/egid are what access checks use; uid/gid are what a setuid program
 * needs to be able to drop back to. They are separate because a program that
 * cannot tell them apart cannot implement setuid correctly, not because
 * anything in this kernel currently sets them differently. */
#define CRED_NGROUPS 16

typedef struct cred {
    uint32 uid;
    uint32 gid;
    uint32 euid;
    uint32 egid;
    uint32 ngroups;
    uint32 groups[CRED_NGROUPS];

    /* Set only for the one uid currently holding the "supreme" privilege -
     * see genesis_supreme_uid_get/set in process.h and PR_GENESIS_* in
     * syscall.c. Not a second identity: it is assembled here the same way
     * euid is, from process_t and the one kernel-global that names the
     * current holder, so there is nowhere for it to go stale. */
    uint32 supreme;
} cred_t;

/* uid 0. Named because "is this root" is asked in several places and
 * `c->euid == 0` in each of them is a rule nobody can find later. */
int cred_is_root(const cred_t *c);

/* Root, OR the one uid a root process has designated supreme (see
 * genesis_supreme_uid_set). This is the privilege check acl_access's root
 * bypass predicted in its own comment before there was a second identity to
 * hold it - root and supreme are exempted the same way and for the same
 * reason: an access check that a deny ACE could lock either of them out of
 * would make cred_is_root's exemption pointless the day a file's owner
 * denies root explicitly. */
int cred_is_supreme(const cred_t *c);

/* Does this credential hold `gid`, either as its effective group or in its
 * supplementary list? */
int cred_in_group(const cred_t *c, uint32 gid);

/* A credential with no privilege at all, for a process that has not been
 * given one. Not root: defaulting to root would make every access check pass
 * and the whole mechanism decorative. */
void cred_init_nobody(cred_t *c);

/* --- an ACL ---------------------------------------------------------------
 *
 * `trivial` records that this ACL was PROJECTED from a mode word rather than
 * read from disk - either because the filesystem has no ACLs, or because the
 * object's ZFS_ACL_TRIVIAL flag said the stored one adds nothing. It changes
 * no decision; it is carried so that a caller reporting an ACL to userland
 * can say where it came from, and so that a test can tell the two apart. */
typedef struct acl_ace {
    uint32 type;    /* ACE_ACCESS_ALLOWED_ACE_TYPE & co  */
    uint32 flags;   /* ACE_OWNER, inheritance bits, ...  */
    uint32 mask;    /* ACE_READ_DATA & co                */
    uint64 who;     /* uid, or gid if ACE_IDENTIFIER_GROUP; unused for the
                     * three special ACEs, whose who is in `flags`        */
} acl_ace_t;

typedef struct acl {
    uint32    count;
    uint32    trivial;
    uint32    owner;     /* the file's uid, needed to resolve owner@      */
    uint32    group;     /* the file's gid, needed to resolve group@      */
    acl_ace_t ace[ACL_ACE_MAX];
} acl_t;

/* --- the two projections --------------------------------------------------
 *
 * These are inverses only in the trivial case, and deliberately so. Mode to
 * ACL is total; ACL to mode is lossy, and what it loses is every entry a mode
 * word has nowhere to put. */

/* Project a mode word into the three-entry ACL it is equivalent to. Sets
 * `trivial`. */
void acl_from_mode(uint32 mode, uint32 uid, uint32 gid, acl_t *out);

/* Project an ACL back onto mode bits, preserving the file-type and setuid
 * bits of `type_bits` (which is the object's existing mode, or just its
 * S_IFMT part).
 *
 * This is OpenZFS's zfs_mode_compute, and it is reproduced rather than
 * invented because "what mode does this ACL correspond to" is a question
 * with a wrong answer that looks fine: the first entry to mention a bit
 * decides that bit, and entries naming a specific user or group contribute
 * NOTHING, because there is nowhere in a mode word for them to go. That last
 * rule is why the fixture's secret.txt reports 0600 while granting uid 1001
 * read access, and why that is correct rather than a discrepancy. */
uint32 acl_to_mode(const acl_t *a, uint32 type_bits);

/* --- evaluation -----------------------------------------------------------
 *
 * Returns 0 if every bit in `wanted` is granted, or -EACCES.
 *
 * NFSv4 order semantics: entries are walked in order and the FIRST entry to
 * mention a bit decides it. A deny early in the list beats an allow later; an
 * allow early beats a deny later. This is not POSIX-ACL semantics and cannot
 * be reordered into them - the order of the entries is part of the data. */
int acl_access(const acl_t *a, const cred_t *c, uint32 wanted);

/* Does this ACE apply to this credential, given the file's owner and group?
 * Exposed because the evaluator and the mode projection both need it and
 * because it is the single trickiest rule in the file - see the note in
 * acl.c about ACE_GROUP alone not meaning group@. */
int acl_ace_applies(const acl_ace_t *e, const cred_t *c, uint32 owner,
                    uint32 group);

/* --- translating the POSIX questions --------------------------------------
 *
 * access(2) asks in R_OK/W_OK/X_OK; open(2) asks in O_RDONLY/O_WRONLY/O_RDWR.
 * Both have to become an NFSv4 mask before anything can be decided, and doing
 * that conversion at each call site is how the three of them end up
 * disagreeing. */
uint32 acl_mask_for_posix(int r, int w, int x, int is_dir);

/* --- the write half: inheritance and chmod --------------------------------
 *
 * Reading an ACL has been real since item 7's read half landed; these two are
 * what a filesystem's setacl slot (fs_ops_t::setacl, fs_setacl) actually
 * calls to PRODUCE the new ACL a create(2) or a chmod(2) writes. Both are
 * pure - no I/O, no filesystem - for the same reason the rest of this file
 * is: it is what lets the interesting rule be checked with no disk. */

/* Build a new object's initial ACL from its parent directory's, the NT/NFSv4
 * way: every entry in `parent` marked inheritable for this kind of child
 * (ACE_FILE_INHERIT_ACE for a file, ACE_DIRECTORY_INHERIT_ACE for a
 * directory) is copied in, with ACE_INHERIT_ONLY_ACE cleared - it is now
 * EFFECTIVE, not a template - and ACE_INHERITED_ACE set, so a caller can
 * always tell an explicit grant from an inherited one. A child that cannot
 * itself have descendants (a file), or a source entry marked
 * ACE_NO_PROPAGATE_INHERIT_ACE, gets its OWN inherit bits cleared on the
 * copy, so inheritance does not propagate a second time.
 *
 * The base is always a plain acl_from_mode(default_mode, ...) - inherited
 * entries are APPENDED after it, not a replacement for it, so the owner can
 * always reach and re-secure something it just created regardless of what
 * the parent's ACL does or does not grant. If nothing in the parent turns
 * out to be inheritable (including `parent` being NULL, for a filesystem
 * root with no parent at all), the result is exactly that base - the same
 * default a filesystem with no ACL concept produces already. */
void acl_inherit(const acl_t *parent, int child_is_dir, uint32 default_mode,
                 uint32 uid, uint32 gid, acl_t *out);

/* chmod's own half of the lossy relationship this header's own top comment
 * describes: rewrite exactly the owner@/group@/everyone@ ALLOW entries to
 * match `mode`, leaving every other entry - a named user or group grant, a
 * deny, anything inherited - untouched and in place. Nothing else in an ACL
 * has anywhere a mode word could tell it what to become.
 *
 * Requires `old` to already carry all three special entries as effective
 * (non-inherit-only) ALLOWs - which every ACL this codebase itself produces
 * does, via acl_from_mode or acl_inherit above. An ACL missing one - read
 * from somewhere else entirely - is not a shape this function was written to
 * splice, and it falls back to a fresh acl_from_mode(mode, ...) rather than
 * guess at preserving entries it cannot find a safe place to keep.
 *
 * `mode` must carry the object's S_IFMT type bits as well as the new
 * permissions: on a directory, w also grants ACE_DELETE_CHILD, exactly as
 * acl_from_mode does. */
void acl_apply_chmod(const acl_t *old, uint32 mode, acl_t *out);

/* The full mode a chmod(2) to `requested` should actually store, for an
 * object of type `type_bits` whose effective ACL is `a`: the type, plus
 * `requested`'s permission AND special bits (S_ISUID/S_ISGID/S_ISVTX) - with
 * one silent adjustment, Linux's: a caller that is neither supreme nor a
 * member of the object's group has S_ISGID dropped, with no error. Without
 * that, anyone could chmod g+s their own file and have it run with the
 * privileges of a group they do not belong to. Pure; see acl_apply_chmod
 * for what then becomes of the rwx half. */
uint32 acl_chmod_mode(const acl_t *a, const cred_t *c, uint32 type_bits,
                      uint32 requested);

/* chown(2)'s "leave this one alone" - the (uid_t)-1 / (gid_t)-1 POSIX
 * callers pass for the half of the pair they are not changing. */
#define ACL_CHOWN_KEEP 0xFFFFFFFFu

/* May `c` make (new_uid, new_gid) the owner and owning group of the object
 * whose effective ACL is `a`? `a->owner`/`a->group` are the CURRENT pair;
 * either new id may be ACL_CHOWN_KEEP. Returns 0 or -EPERM - POSIX's errno
 * for chown, not the -EACCES an ordinary access check answers.
 *
 * This is deliberately NOT fs_access(ACE_WRITE_OWNER) and nothing more,
 * because WRITE_OWNER alone is the wrong shape for both halves:
 *
 *   uid - an ordinary caller may never GIVE a file away. Holding
 *         ACE_WRITE_OWNER (which acl_from_mode never grants, so only an
 *         explicit ACE can) lets a caller TAKE ownership - set the owner to
 *         its own euid - and nothing else. That is NT's rule for WRITE_OWNER
 *         without SeRestorePrivilege, and it is the only reading under which
 *         a grant of WRITE_OWNER cannot be used to plant a file on someone
 *         else's quota or under someone else's name.
 *   gid - the owner may move its own file to any group it is itself a
 *         member of (POSIX chgrp), with no WRITE_OWNER at all; a non-owner
 *         holding WRITE_OWNER may do the same. Moving a file into a group
 *         the caller is NOT in is refused either way.
 *
 * A supreme caller (cred_is_supreme) may set any pair. A request that
 * changes nothing is permitted to anyone - it grants nothing. */
int acl_chown_permitted(const acl_t *a, const cred_t *c, uint32 new_uid,
                        uint32 new_gid);

/* Boot-time check of cred_is_supreme and acl_access's bypass. See
 * kernel/fs/acl_selftest.c for why this is a boot selftest for half of the
 * feature and systest.c's job for the other (ring-3-reachable) half. */
void acl_selftest(void);

#endif /* ACL_H */
