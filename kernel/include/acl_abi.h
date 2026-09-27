#ifndef ACL_ABI_H
#define ACL_ABI_H

/* The NFSv4 access control model, as constants and nothing else.
 *
 * --- why this header is only #defines -------------------------------------
 * It was written to be includable from both sides of a compile wall - the
 * vendored ZFS reader was built against a libc where size_t was a different
 * type from typesk.h's - and a header with no types, no includes and no
 * declarations can sit on both sides of any such wall. ZFS has left the tree
 * (2026-09-26); the property is kept because it costs nothing and is exactly
 * what a future vendored filesystem would need.
 *
 * --- why one set of constants serves both personalities -------------------
 * These are the NFSv4 ACL bits, which is what ZFS and gnfs store. They are
 * also, bit for bit, the Windows access mask - the correspondence is not an
 * approximation and it is not Genesis's invention. OpenZFS's own
 * <sys/acl.h> annotates ACE_READ_NAMED_ATTRS with "FILE_READ_EA" and
 * ACE_WRITE_NAMED_ATTRS with "FILE_WRITE_EA", because NFSv4's ACL model was
 * taken from NT's in the first place.
 *
 * That is the fact this kernel's dual permission story rests on. A file's
 * ACL does not need translating between a POSIX view and a Windows view; the
 * Windows view IS the stored form, and the POSIX mode is a projection of it.
 * The names below are the NFSv4 spellings, with the NT spelling in the
 * comment where the two differ.
 */

/* --- access mask ---------------------------------------------------------- */

#ifndef ACE_READ_DATA
#define ACE_READ_DATA            0x00000001u  /* NT FILE_READ_DATA          */
#endif
#ifndef ACE_LIST_DIRECTORY
#define ACE_LIST_DIRECTORY       0x00000001u  /* same bit, on a directory   */
#endif
#ifndef ACE_WRITE_DATA
#define ACE_WRITE_DATA           0x00000002u  /* NT FILE_WRITE_DATA         */
#endif
#ifndef ACE_ADD_FILE
#define ACE_ADD_FILE             0x00000002u  /* same bit, on a directory   */
#endif
#ifndef ACE_APPEND_DATA
#define ACE_APPEND_DATA          0x00000004u  /* NT FILE_APPEND_DATA        */
#endif
#ifndef ACE_ADD_SUBDIRECTORY
#define ACE_ADD_SUBDIRECTORY     0x00000004u  /* same bit, on a directory   */
#endif
#ifndef ACE_READ_NAMED_ATTRS
#define ACE_READ_NAMED_ATTRS     0x00000008u  /* NT FILE_READ_EA            */
#endif
#ifndef ACE_WRITE_NAMED_ATTRS
#define ACE_WRITE_NAMED_ATTRS    0x00000010u  /* NT FILE_WRITE_EA           */
#endif
#ifndef ACE_EXECUTE
#define ACE_EXECUTE              0x00000020u  /* NT FILE_EXECUTE            */
#endif
#ifndef ACE_DELETE_CHILD
#define ACE_DELETE_CHILD         0x00000040u  /* NT FILE_DELETE_CHILD       */
#endif
#ifndef ACE_READ_ATTRIBUTES
#define ACE_READ_ATTRIBUTES      0x00000080u  /* NT FILE_READ_ATTRIBUTES    */
#endif
#ifndef ACE_WRITE_ATTRIBUTES
#define ACE_WRITE_ATTRIBUTES     0x00000100u  /* NT FILE_WRITE_ATTRIBUTES   */
#endif
#ifndef ACE_DELETE
#define ACE_DELETE               0x00010000u  /* NT DELETE                  */
#endif
#ifndef ACE_READ_ACL
#define ACE_READ_ACL             0x00020000u  /* NT READ_CONTROL            */
#endif
#ifndef ACE_WRITE_ACL
#define ACE_WRITE_ACL            0x00040000u  /* NT WRITE_DAC               */
#endif
#ifndef ACE_WRITE_OWNER
#define ACE_WRITE_OWNER          0x00080000u  /* NT WRITE_OWNER             */
#endif
#ifndef ACE_SYNCHRONIZE
#define ACE_SYNCHRONIZE          0x00100000u  /* NT SYNCHRONIZE             */
#endif

/* --- ACE type ------------------------------------------------------------- */

#ifndef ACE_ACCESS_ALLOWED_ACE_TYPE
#define ACE_ACCESS_ALLOWED_ACE_TYPE  0x0000u
#endif
#ifndef ACE_ACCESS_DENIED_ACE_TYPE
#define ACE_ACCESS_DENIED_ACE_TYPE   0x0001u
#endif
#ifndef ACE_SYSTEM_AUDIT_ACE_TYPE
#define ACE_SYSTEM_AUDIT_ACE_TYPE    0x0002u
#endif
#ifndef ACE_SYSTEM_ALARM_ACE_TYPE
#define ACE_SYSTEM_ALARM_ACE_TYPE    0x0003u
#endif

/* The four object-ACE types. Genesis does not interpret them - they carry a
 * GUID that only a directory service gives meaning to - but they must be
 * RECOGNISED, because they are a different size on disk and a walk that does
 * not know that will desynchronise and read the rest of the ACL as garbage.
 * See zfs_ace_fuid_size in OpenZFS. */
#ifndef ACE_ACCESS_ALLOWED_OBJECT_ACE_TYPE
#define ACE_ACCESS_ALLOWED_OBJECT_ACE_TYPE  0x0005u
#endif
#ifndef ACE_ACCESS_DENIED_OBJECT_ACE_TYPE
#define ACE_ACCESS_DENIED_OBJECT_ACE_TYPE   0x0006u
#endif
#ifndef ACE_SYSTEM_AUDIT_OBJECT_ACE_TYPE
#define ACE_SYSTEM_AUDIT_OBJECT_ACE_TYPE    0x0007u
#endif
#ifndef ACE_SYSTEM_ALARM_OBJECT_ACE_TYPE
#define ACE_SYSTEM_ALARM_OBJECT_ACE_TYPE    0x0008u
#endif

/* --- ACE flags ------------------------------------------------------------ */

#ifndef ACE_FILE_INHERIT_ACE
#define ACE_FILE_INHERIT_ACE         0x0001u  /* NT OBJECT_INHERIT_ACE      */
#endif
#ifndef ACE_DIRECTORY_INHERIT_ACE
#define ACE_DIRECTORY_INHERIT_ACE    0x0002u  /* NT CONTAINER_INHERIT_ACE   */
#endif
#ifndef ACE_NO_PROPAGATE_INHERIT_ACE
#define ACE_NO_PROPAGATE_INHERIT_ACE 0x0004u  /* NT NO_PROPAGATE_INHERIT    */
#endif
#ifndef ACE_INHERIT_ONLY_ACE
#define ACE_INHERIT_ONLY_ACE         0x0008u  /* NT INHERIT_ONLY_ACE        */
#endif
#ifndef ACE_SUCCESSFUL_ACCESS_ACE_FLAG
#define ACE_SUCCESSFUL_ACCESS_ACE_FLAG 0x0010u
#endif
#ifndef ACE_FAILED_ACCESS_ACE_FLAG
#define ACE_FAILED_ACCESS_ACE_FLAG   0x0020u
#endif
#ifndef ACE_IDENTIFIER_GROUP
#define ACE_IDENTIFIER_GROUP         0x0040u  /* the who is a gid, not a uid */
#endif
#ifndef ACE_INHERITED_ACE
#define ACE_INHERITED_ACE            0x0080u
#endif

/* The three "special" whos. An ACE carrying one of these needs no id: the
 * flags ARE the identity, resolved against the file's own owner and group at
 * evaluation time. This is also what makes such an ACE eight bytes on disk
 * instead of sixteen. */
#ifndef ACE_OWNER
#define ACE_OWNER                    0x1000u
#endif
#ifndef ACE_GROUP
#define ACE_GROUP                    0x2000u
#endif
#ifndef ACE_EVERYONE
#define ACE_EVERYONE                 0x4000u
#endif

/* group@ is BOTH bits. Testing for ACE_GROUP alone matches an ordinary
 * named-group ACE too, which is a different thing entirely. */
#define ACE_OWNING_GROUP             (ACE_GROUP | ACE_IDENTIFIER_GROUP)

/* The bits that say who an ACE is about, as opposed to how it inherits. Mask
 * with this before comparing against ACE_OWNER and friends. */
#define ACE_TYPE_FLAGS  (ACE_OWNER | ACE_GROUP | ACE_EVERYONE | \
                         ACE_IDENTIFIER_GROUP)

/* --- how many ACEs Genesis will carry for one object ----------------------
 *
 * A bound is required: the ACE stream is variable width, so its length in
 * bytes says nothing about its length in entries, and it comes off a disk
 * that may be lying. Real ACLs are small - the fixture's largest is four -
 * and OpenZFS's own default inherited ACL is six.
 *
 * Thirty-two, because an acl_t is passed on the stack and kernel stacks here
 * are 16KB (KSTACK_SIZE): at 24 bytes an entry this is 768 bytes, which is a
 * lot to spend but not a lot to lose. Raising it means reconsidering that,
 * not just editing the number.
 *
 * An object whose ACL does not fit is REFUSED rather than truncated, because
 * a truncated ACL is a different ACL - and specifically it is missing its
 * tail, which in NFSv4 convention is where the deny entries that qualify the
 * earlier allows are written. Truncation grants more than the file says. */
#define ACL_ACE_MAX 32


/* --- and the check that every value is the ABI's -------------------------
 *
 * These bits are not Genesis's to choose. The access mask is the SAME field
 * in NFSv4 and in NT - ACE_READ_DATA and FILE_READ_DATA are both 0x1,
 * ACE_WRITE_ACL and WRITE_DAC are both 0x40000 - and that shared layout is
 * what lets one stored ACL be shown as a POSIX mode and as a Windows
 * SECURITY_DESCRIPTOR without a translation table (kernel/fs/ntsec.c). A
 * constant typed one bit out would not fail anything; it would grant or
 * deny the wrong right. So each one is pinned here to the value it MUST
 * have, as #if/#error so a mistake is a build failure rather than a wrong
 * answer.
 *
 * (These used to be cross-checked against the vendored ZFS reader's own
 * copy, zfsimpl.h. ZFS left the tree 2026-09-26; the published values are
 * the reference now, and they are the same numbers.)
 */
#if ACE_READ_DATA != 0x00000001
#error "ACE_READ_DATA is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_LIST_DIRECTORY != 0x00000001
#error "ACE_LIST_DIRECTORY is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_WRITE_DATA != 0x00000002
#error "ACE_WRITE_DATA is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_ADD_FILE != 0x00000002
#error "ACE_ADD_FILE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_APPEND_DATA != 0x00000004
#error "ACE_APPEND_DATA is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_ADD_SUBDIRECTORY != 0x00000004
#error "ACE_ADD_SUBDIRECTORY is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_READ_NAMED_ATTRS != 0x00000008
#error "ACE_READ_NAMED_ATTRS is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_WRITE_NAMED_ATTRS != 0x00000010
#error "ACE_WRITE_NAMED_ATTRS is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_EXECUTE != 0x00000020
#error "ACE_EXECUTE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_DELETE_CHILD != 0x00000040
#error "ACE_DELETE_CHILD is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_READ_ATTRIBUTES != 0x00000080
#error "ACE_READ_ATTRIBUTES is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_WRITE_ATTRIBUTES != 0x00000100
#error "ACE_WRITE_ATTRIBUTES is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_DELETE != 0x00010000
#error "ACE_DELETE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_READ_ACL != 0x00020000
#error "ACE_READ_ACL is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_WRITE_ACL != 0x00040000
#error "ACE_WRITE_ACL is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_WRITE_OWNER != 0x00080000
#error "ACE_WRITE_OWNER is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_SYNCHRONIZE != 0x00100000
#error "ACE_SYNCHRONIZE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_ACCESS_ALLOWED_ACE_TYPE != 0x0000
#error "ACE_ACCESS_ALLOWED_ACE_TYPE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_ACCESS_DENIED_ACE_TYPE != 0x0001
#error "ACE_ACCESS_DENIED_ACE_TYPE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_SYSTEM_AUDIT_ACE_TYPE != 0x0002
#error "ACE_SYSTEM_AUDIT_ACE_TYPE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_SYSTEM_ALARM_ACE_TYPE != 0x0003
#error "ACE_SYSTEM_ALARM_ACE_TYPE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_ACCESS_ALLOWED_OBJECT_ACE_TYPE != 0x0005
#error "ACE_ACCESS_ALLOWED_OBJECT_ACE_TYPE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_ACCESS_DENIED_OBJECT_ACE_TYPE != 0x0006
#error "ACE_ACCESS_DENIED_OBJECT_ACE_TYPE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_SYSTEM_AUDIT_OBJECT_ACE_TYPE != 0x0007
#error "ACE_SYSTEM_AUDIT_OBJECT_ACE_TYPE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_SYSTEM_ALARM_OBJECT_ACE_TYPE != 0x0008
#error "ACE_SYSTEM_ALARM_OBJECT_ACE_TYPE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_FILE_INHERIT_ACE != 0x0001
#error "ACE_FILE_INHERIT_ACE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_DIRECTORY_INHERIT_ACE != 0x0002
#error "ACE_DIRECTORY_INHERIT_ACE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_NO_PROPAGATE_INHERIT_ACE != 0x0004
#error "ACE_NO_PROPAGATE_INHERIT_ACE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_INHERIT_ONLY_ACE != 0x0008
#error "ACE_INHERIT_ONLY_ACE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_SUCCESSFUL_ACCESS_ACE_FLAG != 0x0010
#error "ACE_SUCCESSFUL_ACCESS_ACE_FLAG is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_FAILED_ACCESS_ACE_FLAG != 0x0020
#error "ACE_FAILED_ACCESS_ACE_FLAG is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_IDENTIFIER_GROUP != 0x0040
#error "ACE_IDENTIFIER_GROUP is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_INHERITED_ACE != 0x0080
#error "ACE_INHERITED_ACE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_OWNER != 0x1000
#error "ACE_OWNER is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_GROUP != 0x2000
#error "ACE_GROUP is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif
#if ACE_EVERYONE != 0x4000
#error "ACE_EVERYONE is not the NFSv4/NT value - an access right would be granted or denied wrongly"
#endif

#endif /* ACL_ABI_H */
