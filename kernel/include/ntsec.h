#ifndef NTSEC_H
#define NTSEC_H

#include "acl.h"
#include "typesk.h"

/* The Windows view of a file's permissions: a real SECURITY_DESCRIPTOR.
 *
 * --- what is actually being converted --------------------------------------
 * Less than it looks. ZFS stores an NFSv4 ACL, NFSv4's ACL model was taken
 * from NT's, and the ACCESS MASK is identical bit for bit - ACE_READ_DATA and
 * FILE_READ_DATA are both 0x1, ACE_WRITE_ACL and WRITE_DAC are both 0x40000,
 * all the way up. kernel/include/acl_abi.h gives every constant its NT name
 * in a comment for this reason. So the mask crosses unchanged, and that is
 * not a shortcut - it is the same field.
 *
 * Two things genuinely differ and both are easy to miss:
 *
 *   THE ACE FLAGS AGREE ONLY UP TO 0x8. Inherit-object (0x1), inherit-
 *   container (0x2), no-propagate (0x4) and inherit-only (0x8) are the same
 *   in both. Above that they diverge: NFSv4 says 0x10 successful-access,
 *   0x20 failed-access, 0x40 identifier-group, 0x80 inherited; NT says 0x10
 *   inherited, 0x40 successful-access, 0x80 failed-access, and has no
 *   identifier-group bit at all because the "is this a group" question is
 *   answered by the SID. Copying the byte across would mark an inherited ACE
 *   as an audit ACE and a group ACE as inherited.
 *
 *   THE "WHO" IS A SID, NOT AN ID. That is the mapping below.
 *
 * --- mapping identities ----------------------------------------------------
 * Samba's scheme, because it is the one an interoperating system already
 * uses and inventing a different one would mean a file's owner reads
 * differently depending on which machine looked at it:
 *
 *   uid N          S-1-22-1-N       "Unix User"
 *   gid N          S-1-22-2-N       "Unix Group"
 *   everyone@      S-1-1-0          Everyone / World
 *
 * owner@ and group@ are the interesting case and get TWO answers depending
 * on the ACE, which is correct rather than inconsistent:
 *
 *   an EFFECTIVE ACE      owner@ -> the file's actual owner SID
 *   an INHERIT_ONLY ACE   owner@ -> S-1-3-0 CREATOR_OWNER
 *
 * An effective owner@ entry says "whoever owns this file may do X", and the
 * descriptor a caller reads should name that person. An inherit-only entry
 * is a TEMPLATE to be stamped onto things created later, whose owner is not
 * known yet - which is exactly what CREATOR_OWNER means and the only thing
 * it means. Collapsing the two loses the distinction that makes an
 * inheritable ACL work.
 */

/* --- SECURITY_DESCRIPTOR control bits ------------------------------------- */
#define SE_OWNER_DEFAULTED  0x0001u
#define SE_GROUP_DEFAULTED  0x0002u
#define SE_DACL_PRESENT     0x0004u
#define SE_DACL_DEFAULTED   0x0008u
#define SE_SACL_PRESENT     0x0010u
#define SE_SELF_RELATIVE    0x8000u

/* --- NT ACE flags, which are NOT the NFSv4 ones above 0x8 ----------------- */
#define NT_OBJECT_INHERIT_ACE       0x01u
#define NT_CONTAINER_INHERIT_ACE    0x02u
#define NT_NO_PROPAGATE_INHERIT_ACE 0x04u
#define NT_INHERIT_ONLY_ACE         0x08u
#define NT_INHERITED_ACE            0x10u
#define NT_SUCCESSFUL_ACCESS_FLAG   0x40u
#define NT_FAILED_ACCESS_FLAG       0x80u

/* ACL_REVISION. 2 is the value for a DACL containing only the ACE types this
 * kernel produces; 4 (ACL_REVISION_DS) is required only for object ACEs, and
 * those are recognised but never emitted. */
#define NT_ACL_REVISION  2

/* --- how big a descriptor can get -----------------------------------------
 *
 * 20 bytes of header, two SIDs, an 8-byte ACL header and up to ACL_ACE_MAX
 * entries of (4-byte header + 4-byte mask + a SID of at most 20 bytes). The
 * bound is stated rather than computed at each call site so that a caller
 * can size a buffer without reproducing the arithmetic - and get it wrong. */
#define NTSEC_MAX_SD  (20 + 2 * 20 + 8 + ACL_ACE_MAX * (4 + 4 + 20))

/* Build a self-relative SECURITY_DESCRIPTOR for `a` into `buf`.
 *
 * Self-relative rather than absolute: every internal reference is a byte
 * offset from the start, so the whole thing is one contiguous block that can
 * be copied to userland with a single write. An absolute descriptor is a
 * struct full of kernel pointers, which is exactly what must not cross that
 * boundary.
 *
 * Returns the number of bytes written, or a negative errno. -ENOSPC if `cap`
 * is too small; ask with a NULL buf and a cap of 0 to get the size. */
int64 ntsec_build(const acl_t *a, void *buf, uint64 cap);

/* Write the SID for a uid or gid into `buf` (at least 20 bytes). Returns the
 * SID's length in bytes. Exposed because the descriptor's Owner and Group
 * fields need it and so do tests. */
int ntsec_sid_for_uid(uint32 uid, void *buf);
int ntsec_sid_for_gid(uint32 gid, void *buf);

/* Translate one NFSv4 ACE flag byte into NT's. See the header comment: this
 * is not a copy, and the bits that move are the ones nothing notices until an
 * inherited ACE starts auditing. */
uint8 ntsec_flags_to_nt(uint32 nfsv4_flags);

#endif /* NTSEC_H */
