/* The access control core: one ACL, a POSIX view and an NT view of it.
 *
 * See kernel/include/acl.h for why the ACL is the stored form and the mode
 * word is the projection. This file is the arithmetic.
 *
 * Nothing here does I/O, allocates, or touches a filesystem. That is
 * deliberate - every rule in this file is a pure function of its arguments,
 * which is what lets verif.c check the interesting ones at boot with no disk
 * and no process, and what lets the host suite check them with neither.
 */

#include "acl.h"
#include "typesk.h"

#define EPERM   1
#define EACCES 13

/* --- credentials ---------------------------------------------------------- */

int cred_is_root(const cred_t *c) {
    return c != NULL && c->euid == 0;
}

int cred_is_supreme(const cred_t *c) {
    return c != NULL && (c->euid == 0 || c->supreme != 0);
}

int cred_in_group(const cred_t *c, uint32 gid) {
    uint32 i;

    if (c == NULL) {
        return 0;
    }
    if (c->egid == gid) {
        return 1;
    }
    for (i = 0; i < c->ngroups && i < CRED_NGROUPS; i++) {
        if (c->groups[i] == gid) {
            return 1;
        }
    }
    return 0;
}

void cred_init_nobody(cred_t *c) {
    uint32 i;

    if (c == NULL) {
        return;
    }
    /* 65534, the conventional "nobody". Not 0, and not a fresh zeroed struct
     * either - a zeroed cred_t IS root, which is the failure mode where
     * forgetting to initialise a credential grants everything. Any new
     * process that reaches an access check without having been given an
     * identity should fail it, not pass it. */
    c->uid = c->euid = 65534;
    c->gid = c->egid = 65534;
    c->ngroups = 0;
    /* Explicitly, not left to whatever the caller's stack held - the same
     * reasoning as the rest of this function. An uninitialised `supreme`
     * that happened to be nonzero would grant everything just as surely as
     * a zeroed struct's euid 0 does. */
    c->supreme = 0;
    for (i = 0; i < CRED_NGROUPS; i++) {
        c->groups[i] = 0;
    }
}

/* --- who an ACE is about --------------------------------------------------
 *
 * The three special entry types resolve against the FILE's owner and group,
 * not against anything stored in the ACE. Everything else carries an id.
 *
 * The trap is ACE_GROUP. group@ is ACE_GROUP|ACE_IDENTIFIER_GROUP, and
 * ACE_IDENTIFIER_GROUP on its own means "this ACE names a group by gid".
 * Testing `flags & ACE_GROUP` matches both, so a named-group ACE would be
 * treated as the owning group and would apply to the wrong people. Hence the
 * comparison is against the masked value, never a bit test. */
int acl_ace_applies(const acl_ace_t *e, const cred_t *c, uint32 owner,
                    uint32 group) {
    uint32 entry;

    if (e == NULL || c == NULL) {
        return 0;
    }

    /* An inherit-only ACE says nothing about this object; it exists to be
     * copied onto things created inside it. Evaluating it here would apply a
     * rule meant for children to the parent. */
    if (e->flags & ACE_INHERIT_ONLY_ACE) {
        return 0;
    }

    entry = e->flags & ACE_TYPE_FLAGS;

    if (entry == ACE_OWNER) {
        return c->euid == owner;
    }
    if (entry == ACE_OWNING_GROUP) {
        return cred_in_group(c, group);
    }
    if (entry == ACE_EVERYONE) {
        return 1;
    }
    if (entry == ACE_IDENTIFIER_GROUP) {
        return cred_in_group(c, (uint32)e->who);
    }
    /* entry == 0: a named user. */
    return c->euid == (uint32)e->who;
}

/* --- evaluation ----------------------------------------------------------- */

int acl_access(const acl_t *a, const cred_t *c, uint32 wanted) {
    uint32 allowed = 0;
    uint32 denied = 0;
    uint32 i;

    if (a == NULL || c == NULL) {
        return -EACCES;
    }
    if (wanted == 0) {
        return 0;
    }

    /* Root, or the one uid a root process has designated supreme. Placed
     * before the walk rather than after it, because after it would mean
     * "root is allowed unless an ACE denies root", and a deny ACE that locks
     * the administrator out of a file is not something this kernel should be
     * able to produce.
     *
     * This is a policy choice and it is the POSIX one. NT's is different -
     * there, an explicit deny binds Administrator too, and the way back in is
     * SeTakeOwnershipPrivilege rather than an exemption from the check. This
     * used to be a uid test (`cred_is_root`) with a comment saying that the
     * day this kernel grew privileges as a real concept, this line would
     * become a privilege test instead - it now is one. cred_is_supreme is
     * root OR the designated holder, and both bypass the same way: `wanted`
     * is whatever the caller asked for, which is how chmod's WRITE_ACL check
     * and chown's WRITE_OWNER check (fs_setacl, acl_chown_permitted) are
     * exempted here too, with no second bypass to keep in sync. That is also what
     * makes the designated holder more than Administrator-equivalent - an
     * explicit deny does not bind it, the same as it does not bind root, which
     * is closer to how TrustedInstaller and SYSTEM actually behave than to
     * how a merely-Administrator account does. */
    if (cred_is_supreme(c)) {
        return 0;
    }

    /* NFSv4 order: walk once, and the FIRST entry to mention a bit decides
     * it. `& ~denied` and `& ~allowed` are what implement "first" - a bit
     * already settled is not revisited, so a later entry saying the opposite
     * has no effect.
     *
     * Accumulating into two words rather than returning on the first match is
     * required because a request is usually several bits at once: a deny of
     * write and an allow of read can both be relevant to one O_RDWR, and the
     * answer depends on all of them. */
    for (i = 0; i < a->count && i < ACL_ACE_MAX; i++) {
        const acl_ace_t *e = &a->ace[i];

        /* Audit and alarm entries record; they do not decide. Skipping them
         * matters: an ACE_SYSTEM_AUDIT_ACE_TYPE with a read mask means "log
         * reads", and treating it as an allow would grant them. */
        if (e->type != ACE_ACCESS_ALLOWED_ACE_TYPE &&
            e->type != ACE_ACCESS_DENIED_ACE_TYPE) {
            continue;
        }
        if (!acl_ace_applies(e, c, a->owner, a->group)) {
            continue;
        }
        if (e->type == ACE_ACCESS_DENIED_ACE_TYPE) {
            denied |= (e->mask & ~allowed);
        } else {
            allowed |= (e->mask & ~denied);
        }
    }

    if (wanted & denied) {
        return -EACCES;
    }
    /* Not "nothing denied it" - "something allowed it". An ACL that never
     * mentions a bit does not grant that bit. Getting this backwards turns an
     * empty ACL from "deny everything" into "allow everything", which is the
     * single worst bug this file could have. */
    if ((wanted & allowed) != wanted) {
        return -EACCES;
    }
    return 0;
}

/* --- mode -> ACL ---------------------------------------------------------- */

void acl_from_mode(uint32 mode, uint32 uid, uint32 gid, acl_t *out) {
    uint32 i = 0;
    uint32 write_bits;

    if (out == NULL) {
        return;
    }
    for (i = 0; i < ACL_ACE_MAX; i++) {
        out->ace[i].type = 0;
        out->ace[i].flags = 0;
        out->ace[i].mask = 0;
        out->ace[i].who = 0;
    }
    out->owner = uid;
    out->group = gid;
    out->trivial = 1;

    /* On a directory, w also means "may remove entries" - ACE_DELETE_CHILD,
     * which is a separate NFSv4 bit, and is exactly what OpenZFS's own
     * zfs_acl_chmod puts in a directory's write mask. Leaving it out made
     * access(dir, W_OK) - which asks for it, see acl_mask_for_posix - fail
     * for the non-root owner of their own 0755 directory, and would make
     * every mode-only directory refuse unlink to everyone but root. */
    write_bits = ACE_WRITE_DATA | ACE_APPEND_DATA;
    if ((mode & S_IFMT) == S_IFDIR) {
        write_bits |= ACE_DELETE_CHILD;
    }

    /* Three entries, in the order ZFS writes them. Order matters even here:
     * these are all allows, so no bit is contested, but a reader that
     * projects them back to a mode expects owner, group, everyone. */
    out->ace[0].type = ACE_ACCESS_ALLOWED_ACE_TYPE;
    out->ace[0].flags = ACE_OWNER;
    out->ace[0].mask = 0;
    if (mode & S_IRUSR) { out->ace[0].mask |= ACE_READ_DATA; }
    if (mode & S_IWUSR) { out->ace[0].mask |= write_bits; }
    if (mode & S_IXUSR) { out->ace[0].mask |= ACE_EXECUTE; }
    /* The owner can always read and rewrite the security of its own file, and
     * can always stat it. These bits are not in the mode word and they are
     * not invented: they are what ZFS puts in a trivial ACL's owner@ entry,
     * because POSIX gives the owner chmod unconditionally. */
    out->ace[0].mask |= ACE_READ_ATTRIBUTES | ACE_WRITE_ATTRIBUTES |
                        ACE_READ_ACL | ACE_WRITE_ACL | ACE_SYNCHRONIZE;

    out->ace[1].type = ACE_ACCESS_ALLOWED_ACE_TYPE;
    out->ace[1].flags = ACE_OWNING_GROUP;
    out->ace[1].mask = 0;
    if (mode & S_IRGRP) { out->ace[1].mask |= ACE_READ_DATA; }
    if (mode & S_IWGRP) { out->ace[1].mask |= write_bits; }
    if (mode & S_IXGRP) { out->ace[1].mask |= ACE_EXECUTE; }
    out->ace[1].mask |= ACE_READ_ATTRIBUTES | ACE_READ_ACL | ACE_SYNCHRONIZE;

    out->ace[2].type = ACE_ACCESS_ALLOWED_ACE_TYPE;
    out->ace[2].flags = ACE_EVERYONE;
    out->ace[2].mask = 0;
    if (mode & S_IROTH) { out->ace[2].mask |= ACE_READ_DATA; }
    if (mode & S_IWOTH) { out->ace[2].mask |= write_bits; }
    if (mode & S_IXOTH) { out->ace[2].mask |= ACE_EXECUTE; }
    out->ace[2].mask |= ACE_READ_ATTRIBUTES | ACE_READ_ACL | ACE_SYNCHRONIZE;

    out->count = 3;
}

/* --- ACL -> mode ----------------------------------------------------------
 *
 * OpenZFS's zfs_mode_compute, reproduced. The `seen` word is the whole
 * algorithm: the first entry to mention a permission for a class settles that
 * class, whether it allowed or denied it. A deny leaves the bit clear and
 * marks it seen, so a later allow cannot turn it back on - which is what
 * makes the projection agree with what an evaluation would decide.
 */
uint32 acl_to_mode(const acl_t *a, uint32 type_bits) {
    uint32 mode;
    uint32 seen = 0;
    uint32 i;

    if (a == NULL) {
        return 0;
    }

    /* The file type and the setuid/setgid/sticky bits survive untouched: no
     * ACE says anything about them, so projecting would clear them. */
    mode = type_bits & (S_IFMT | S_ISUID | S_ISGID | S_ISVTX);

    for (i = 0; i < a->count && i < ACL_ACE_MAX; i++) {
        const acl_ace_t *e = &a->ace[i];
        uint32 entry;
        int allow;

        if (e->type != ACE_ACCESS_ALLOWED_ACE_TYPE &&
            e->type != ACE_ACCESS_DENIED_ACE_TYPE) {
            continue;
        }
        if (e->flags & ACE_INHERIT_ONLY_ACE) {
            continue;
        }
        allow = (e->type == ACE_ACCESS_ALLOWED_ACE_TYPE);
        entry = e->flags & ACE_TYPE_FLAGS;

        /* A named-user ACE that happens to name the file's own owner counts
         * as owner@. ZFS does this and it matters: chown can leave an ACL
         * whose owner entry is spelled as an id rather than as owner@, and
         * projecting that to a mode of zero would be wrong. */
        if (entry == ACE_OWNER || (entry == 0 && (uint32)e->who == a->owner)) {
            if ((e->mask & ACE_READ_DATA) && !(seen & S_IRUSR)) {
                seen |= S_IRUSR;
                if (allow) { mode |= S_IRUSR; }
            }
            if ((e->mask & ACE_WRITE_DATA) && !(seen & S_IWUSR)) {
                seen |= S_IWUSR;
                if (allow) { mode |= S_IWUSR; }
            }
            if ((e->mask & ACE_EXECUTE) && !(seen & S_IXUSR)) {
                seen |= S_IXUSR;
                if (allow) { mode |= S_IXUSR; }
            }
        } else if (entry == ACE_OWNING_GROUP ||
                   (entry == ACE_IDENTIFIER_GROUP &&
                    (uint32)e->who == a->group)) {
            if ((e->mask & ACE_READ_DATA) && !(seen & S_IRGRP)) {
                seen |= S_IRGRP;
                if (allow) { mode |= S_IRGRP; }
            }
            if ((e->mask & ACE_WRITE_DATA) && !(seen & S_IWGRP)) {
                seen |= S_IWGRP;
                if (allow) { mode |= S_IWGRP; }
            }
            if ((e->mask & ACE_EXECUTE) && !(seen & S_IXGRP)) {
                seen |= S_IXGRP;
                if (allow) { mode |= S_IXGRP; }
            }
        } else if (entry == ACE_EVERYONE) {
            /* everyone@ touches all three classes, but only the ones not
             * already settled by an earlier owner@ or group@ entry. */
            if (e->mask & ACE_READ_DATA) {
                if (!(seen & S_IRUSR)) { seen |= S_IRUSR; if (allow) { mode |= S_IRUSR; } }
                if (!(seen & S_IRGRP)) { seen |= S_IRGRP; if (allow) { mode |= S_IRGRP; } }
                if (!(seen & S_IROTH)) { seen |= S_IROTH; if (allow) { mode |= S_IROTH; } }
            }
            if (e->mask & ACE_WRITE_DATA) {
                if (!(seen & S_IWUSR)) { seen |= S_IWUSR; if (allow) { mode |= S_IWUSR; } }
                if (!(seen & S_IWGRP)) { seen |= S_IWGRP; if (allow) { mode |= S_IWGRP; } }
                if (!(seen & S_IWOTH)) { seen |= S_IWOTH; if (allow) { mode |= S_IWOTH; } }
            }
            if (e->mask & ACE_EXECUTE) {
                if (!(seen & S_IXUSR)) { seen |= S_IXUSR; if (allow) { mode |= S_IXUSR; } }
                if (!(seen & S_IXGRP)) { seen |= S_IXGRP; if (allow) { mode |= S_IXGRP; } }
                if (!(seen & S_IXOTH)) { seen |= S_IXOTH; if (allow) { mode |= S_IXOTH; } }
            }
        }
        /* Anything else - a named user who is not the owner, a named group
         * that is not the owning group - contributes NOTHING. Not because it
         * is unimportant, but because a mode word has three classes and this
         * entry is about none of them. This is the loss in the projection,
         * and it is the reason the fixture's secret.txt reads 0600 while
         * granting uid 1001. */
    }

    return mode;
}

/* --- the POSIX questions, in NFSv4 terms ---------------------------------- */

uint32 acl_mask_for_posix(int r, int w, int x, int is_dir) {
    uint32 m = 0;

    if (r) {
        /* On a directory, "read" is the right to list it. Same bit, and the
         * name in acl_abi.h is ACE_LIST_DIRECTORY for exactly this reason. */
        m |= ACE_READ_DATA;
    }
    if (w) {
        m |= ACE_WRITE_DATA | ACE_APPEND_DATA;
        if (is_dir) {
            /* Writing a directory means creating and removing entries in it.
             * ACE_ADD_FILE is ACE_WRITE_DATA's bit and ACE_ADD_SUBDIRECTORY
             * is ACE_APPEND_DATA's, so those are already covered; deleting a
             * child is a separate bit and is genuinely part of what W_OK
             * promises on a directory. */
            m |= ACE_DELETE_CHILD;
        }
    }
    if (x) {
        m |= ACE_EXECUTE;
    }
    return m;
}

/* --- the write half: inheritance and chmod --------------------------------- */

void acl_inherit(const acl_t *parent, int child_is_dir, uint32 default_mode,
                 uint32 uid, uint32 gid, acl_t *out) {
    uint32 want_bit = child_is_dir ? ACE_DIRECTORY_INHERIT_ACE
                                   : ACE_FILE_INHERIT_ACE;
    uint32 n;
    uint32 i;

    /* Always the base, whatever the parent turns out to grant - see the
     * header comment on why this is a floor rather than something inherited
     * entries replace. */
    acl_from_mode(default_mode, uid, gid, out);
    if (parent == NULL) {
        return;
    }

    n = out->count;         /* 3, from acl_from_mode */
    for (i = 0; i < parent->count && i < ACL_ACE_MAX; i++) {
        const acl_ace_t *src = &parent->ace[i];
        acl_ace_t *dst;

        if (!(src->flags & want_bit)) {
            continue;
        }
        if (n >= ACL_ACE_MAX) {
            /* Refused, not truncated - same policy ACL_ACE_MAX's own comment
             * states for an ACL read off a disk that has more entries than
             * this kernel will carry. Silently dropping the entry here would
             * be silently dropping a GRANT (or a deny), and either is worse
             * than stopping early. */
            break;
        }

        dst = &out->ace[n];
        *dst = *src;
        /* No longer a template for something not yet created - it applies to
         * THIS object now - and no longer silently explicit: a caller asking
         * "was this granted here or inherited" needs the answer to survive
         * the copy. */
        dst->flags &= ~ACE_INHERIT_ONLY_ACE;
        dst->flags |= ACE_INHERITED_ACE;
        /* A file has no descendants to inherit to again, and an entry marked
         * NO_PROPAGATE said explicitly that one level is all it grants -
         * both cases clear the inherit bits on the COPY so this does not
         * keep propagating past where it was told to stop. */
        if (!child_is_dir || (src->flags & ACE_NO_PROPAGATE_INHERIT_ACE)) {
            dst->flags &= ~(ACE_FILE_INHERIT_ACE | ACE_DIRECTORY_INHERIT_ACE |
                           ACE_NO_PROPAGATE_INHERIT_ACE);
        }
        n++;
    }

    if (n > out->count) {
        out->trivial = 0;   /* real grants now, beyond the mode projection */
        out->count = n;
    }
    /* n == out->count (3): nothing in the parent was inheritable, and out is
     * exactly acl_from_mode's own result already - trivial stays set, which
     * is correct. */
}

/* The class-mask half of acl_from_mode's owner/group/everyone construction,
 * factored out here rather than shared with it: acl_from_mode is read-path
 * code that already works and is tested, and this function exists to let
 * acl_apply_chmod rewrite three entries without touching it. */
static uint32 chmod_class_mask(uint32 mode, uint32 r_bit, uint32 w_bit,
                              uint32 x_bit) {
    uint32 m = 0;

    if (mode & r_bit) { m |= ACE_READ_DATA; }
    if (mode & w_bit) {
        m |= ACE_WRITE_DATA | ACE_APPEND_DATA;
        if ((mode & S_IFMT) == S_IFDIR) {
            m |= ACE_DELETE_CHILD;       /* as acl_from_mode, and why */
        }
    }
    if (mode & x_bit) { m |= ACE_EXECUTE; }
    return m;
}

/* The first non-inherit-only ALLOW entry naming `special`
 * (ACE_OWNER/ACE_OWNING_GROUP/ACE_EVERYONE), or -1. Masked against
 * ACE_TYPE_FLAGS for the same reason acl_ace_applies does - group@ is BOTH
 * ACE_GROUP and ACE_IDENTIFIER_GROUP, and a bare bit test would also match a
 * named-group entry that happens to share one bit with it. */
static int find_special_allow(const acl_t *a, uint32 special) {
    uint32 i;

    for (i = 0; i < a->count && i < ACL_ACE_MAX; i++) {
        const acl_ace_t *e = &a->ace[i];

        if (e->type != ACE_ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        if (e->flags & ACE_INHERIT_ONLY_ACE) {
            continue;
        }
        if ((e->flags & ACE_TYPE_FLAGS) == special) {
            return (int)i;
        }
    }
    return -1;
}

void acl_apply_chmod(const acl_t *old, uint32 mode, acl_t *out) {
    int io, ig, ie;

    io = find_special_allow(old, ACE_OWNER);
    ig = find_special_allow(old, ACE_OWNING_GROUP);
    ie = find_special_allow(old, ACE_EVERYONE);
    if (io < 0 || ig < 0 || ie < 0) {
        acl_from_mode(mode, old->owner, old->group, out);
        return;
    }

    *out = *old;

    /* The owner's extra bits - read/write attributes, read/write the ACL
     * itself, synchronize - are not in the mode word and are not invented
     * here either: they are exactly what acl_from_mode already grants
     * owner@ unconditionally, kept unconditionally for the same reason
     * chmod cannot take chmod away from the owner. */
    out->ace[io].mask = chmod_class_mask(mode, S_IRUSR, S_IWUSR, S_IXUSR) |
                        ACE_READ_ATTRIBUTES | ACE_WRITE_ATTRIBUTES |
                        ACE_READ_ACL | ACE_WRITE_ACL | ACE_SYNCHRONIZE;
    out->ace[ig].mask = chmod_class_mask(mode, S_IRGRP, S_IWGRP, S_IXGRP) |
                        ACE_READ_ATTRIBUTES | ACE_READ_ACL | ACE_SYNCHRONIZE;
    out->ace[ie].mask = chmod_class_mask(mode, S_IROTH, S_IWOTH, S_IXOTH) |
                        ACE_READ_ATTRIBUTES | ACE_READ_ACL | ACE_SYNCHRONIZE;

    /* Still trivial only if there was nothing here chmod could not already
     * speak for - i.e. exactly the three entries just rewritten and nothing
     * else. Informational only (see this field's own comment in acl.h); no
     * decision anywhere reads it. */
    out->trivial = (old->count == 3) ? 1u : 0u;
}

/* --- chown ----------------------------------------------------------------
 *
 * See acl.h for the rule. The one subtlety worth stating here: the WRITE_OWNER
 * test goes through acl_access, not a hand-rolled walk, so a DENY of
 * WRITE_OWNER placed ahead of an allow binds exactly the way it binds every
 * other bit - and so does the supreme bypass, which is why the explicit
 * cred_is_supreme test below is only there to let supreme choose an
 * ARBITRARY uid, a thing no ACE can grant anybody else. */
int acl_chown_permitted(const acl_t *a, const cred_t *c, uint32 new_uid,
                        uint32 new_gid) {
    int uid_changes, gid_changes, may_write_owner;

    if (a == NULL || c == NULL) {
        return -EPERM;
    }
    if (new_uid == ACL_CHOWN_KEEP) {
        new_uid = a->owner;
    }
    if (new_gid == ACL_CHOWN_KEEP) {
        new_gid = a->group;
    }
    uid_changes = (new_uid != a->owner);
    gid_changes = (new_gid != a->group);

    if (!uid_changes && !gid_changes) {
        return 0;
    }
    if (cred_is_supreme(c)) {
        return 0;
    }

    may_write_owner = (acl_access(a, c, ACE_WRITE_OWNER) == 0);

    /* Taking, never giving: WRITE_OWNER lets the caller name ITSELF. */
    if (uid_changes && !(may_write_owner && new_uid == c->euid)) {
        return -EPERM;
    }
    /* Into a group the caller belongs to, and only as the owner or as a
     * holder of WRITE_OWNER. "The owner" is the CURRENT owner - a caller
     * taking ownership in the same call already passed the WRITE_OWNER
     * test above, so it qualifies through that half instead. */
    if (gid_changes &&
        !(cred_in_group(c, new_gid) &&
          (c->euid == a->owner || may_write_owner))) {
        return -EPERM;
    }
    return 0;
}

/* --- chmod's special bits -------------------------------------------------
 *
 * See acl.h. The group tested is the OBJECT's (a->group), not the caller's
 * egid alone: cred_in_group covers the supplementary list too, which is what
 * Linux's in_group_p does. */
uint32 acl_chmod_mode(const acl_t *a, const cred_t *c, uint32 type_bits,
                      uint32 requested) {
    uint32 mode = (type_bits & S_IFMT) |
                  (requested & (S_ISUID | S_ISGID | S_ISVTX | 0777u));

    if ((mode & S_ISGID) && !cred_is_supreme(c) &&
        (a == NULL || !cred_in_group(c, a->group))) {
        mode &= ~S_ISGID;
    }
    return mode;
}
