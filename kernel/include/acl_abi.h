#ifndef ACL_ABI_H
#define ACL_ABI_H

/* The NFSv4 access control model, as constants and nothing else.
 *
 * --- why this header is only #defines -------------------------------------
 * It is included from BOTH sides of the compile wall described in
 * kernel/zfs/zfs_genesis.h: from kernel/fs/acl.c, which is built against
 * typesk.h where size_t is `unsigned long long`, and from
 * kernel/zfs/zfs_sa.inc, which is built against kernel/zfs/compat/ where it
 * is `unsigned long`. A header with a typedef in it could not be included
 * from both. A header with no types, no includes and no declarations can,
 * and that is the whole design constraint here.
 *
 * The alternative was to write these numbers twice and add a test that the
 * two copies agree. One copy and no test is better.
 *
 * --- why one set of constants serves both personalities -------------------
 * These are the NFSv4 ACL bits, which is what ZFS stores natively. They are
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

/* --- the znode pflags this code cares about -------------------------------
 *
 * ZFS_ACL_TRIVIAL means "the ACL says exactly what the mode bits say". It is
 * a fast path with teeth: real ZFS checks it and does not load the ACL at
 * all. A reader that ignores it does needless work; a WRITER that leaves it
 * set after storing a non-trivial ACL has stored an ACL nothing will ever
 * read. tests/host/zplsetacl.c hit exactly that and the fixture README
 * records it. */
/* Guarded one by one because kernel/zfs/vendor/zfsimpl.h defines the same
 * five, with the same values, and the vendored translation unit includes
 * both. The guards are not papering over a disagreement - if the two ever
 * disagreed the guard would silently pick the vendored one, so the ZFS
 * selftest asserts the values it actually reads. Genesis needs its own copy
 * because kernel/zfs/zfs_vfs.c is on the far side of the wall from
 * zfsimpl.h and cannot include it. */
#ifndef ZFS_ACL_TRIVIAL
#define ZFS_ACL_TRIVIAL          0x00000004u
#endif
#ifndef ZFS_ACL_PROTECTED
#define ZFS_ACL_PROTECTED        0x00000010u
#endif
#ifndef ZFS_ACL_DEFAULTED
#define ZFS_ACL_DEFAULTED        0x00000020u
#endif
#ifndef ZFS_ACL_AUTO_INHERIT
#define ZFS_ACL_AUTO_INHERIT     0x00000040u
#endif
#ifndef ZFS_NO_EXECS_DENIED
#define ZFS_NO_EXECS_DENIED      0x00000100u
#endif

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


/* --- and the check that the guards above are not hiding a disagreement --
 *
 * Every ACE_ constant here is #ifndef-guarded, because kernel/zfs/vendor/
 * zfsimpl.h defines the same names and the vendored translation unit reads
 * both headers. A guard makes the warning go away - and would also make a
 * genuine mismatch go away, silently, by letting whichever header came
 * first decide a security-relevant bit pattern.
 *
 * So each one is verified against the value it MUST have. This is #if and
 * #error rather than a static assertion on purpose: acl_abi.h declares no
 * types at all (that is what lets the vendored side include it across the
 * size_t wall), and a static assertion is a type.
 */
#if ACE_READ_DATA != 0x00000001
#error "ACE_READ_DATA disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_LIST_DIRECTORY != 0x00000001
#error "ACE_LIST_DIRECTORY disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_WRITE_DATA != 0x00000002
#error "ACE_WRITE_DATA disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_ADD_FILE != 0x00000002
#error "ACE_ADD_FILE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_APPEND_DATA != 0x00000004
#error "ACE_APPEND_DATA disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_ADD_SUBDIRECTORY != 0x00000004
#error "ACE_ADD_SUBDIRECTORY disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_READ_NAMED_ATTRS != 0x00000008
#error "ACE_READ_NAMED_ATTRS disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_WRITE_NAMED_ATTRS != 0x00000010
#error "ACE_WRITE_NAMED_ATTRS disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_EXECUTE != 0x00000020
#error "ACE_EXECUTE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_DELETE_CHILD != 0x00000040
#error "ACE_DELETE_CHILD disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_READ_ATTRIBUTES != 0x00000080
#error "ACE_READ_ATTRIBUTES disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_WRITE_ATTRIBUTES != 0x00000100
#error "ACE_WRITE_ATTRIBUTES disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_DELETE != 0x00010000
#error "ACE_DELETE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_READ_ACL != 0x00020000
#error "ACE_READ_ACL disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_WRITE_ACL != 0x00040000
#error "ACE_WRITE_ACL disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_WRITE_OWNER != 0x00080000
#error "ACE_WRITE_OWNER disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_SYNCHRONIZE != 0x00100000
#error "ACE_SYNCHRONIZE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_ACCESS_ALLOWED_ACE_TYPE != 0x0000
#error "ACE_ACCESS_ALLOWED_ACE_TYPE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_ACCESS_DENIED_ACE_TYPE != 0x0001
#error "ACE_ACCESS_DENIED_ACE_TYPE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_SYSTEM_AUDIT_ACE_TYPE != 0x0002
#error "ACE_SYSTEM_AUDIT_ACE_TYPE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_SYSTEM_ALARM_ACE_TYPE != 0x0003
#error "ACE_SYSTEM_ALARM_ACE_TYPE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_ACCESS_ALLOWED_OBJECT_ACE_TYPE != 0x0005
#error "ACE_ACCESS_ALLOWED_OBJECT_ACE_TYPE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_ACCESS_DENIED_OBJECT_ACE_TYPE != 0x0006
#error "ACE_ACCESS_DENIED_OBJECT_ACE_TYPE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_SYSTEM_AUDIT_OBJECT_ACE_TYPE != 0x0007
#error "ACE_SYSTEM_AUDIT_OBJECT_ACE_TYPE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_SYSTEM_ALARM_OBJECT_ACE_TYPE != 0x0008
#error "ACE_SYSTEM_ALARM_OBJECT_ACE_TYPE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_FILE_INHERIT_ACE != 0x0001
#error "ACE_FILE_INHERIT_ACE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_DIRECTORY_INHERIT_ACE != 0x0002
#error "ACE_DIRECTORY_INHERIT_ACE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_NO_PROPAGATE_INHERIT_ACE != 0x0004
#error "ACE_NO_PROPAGATE_INHERIT_ACE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_INHERIT_ONLY_ACE != 0x0008
#error "ACE_INHERIT_ONLY_ACE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_SUCCESSFUL_ACCESS_ACE_FLAG != 0x0010
#error "ACE_SUCCESSFUL_ACCESS_ACE_FLAG disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_FAILED_ACCESS_ACE_FLAG != 0x0020
#error "ACE_FAILED_ACCESS_ACE_FLAG disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_IDENTIFIER_GROUP != 0x0040
#error "ACE_IDENTIFIER_GROUP disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_INHERITED_ACE != 0x0080
#error "ACE_INHERITED_ACE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_OWNER != 0x1000
#error "ACE_OWNER disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_GROUP != 0x2000
#error "ACE_GROUP disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif
#if ACE_EVERYONE != 0x4000
#error "ACE_EVERYONE disagrees with kernel/zfs/vendor/zfsimpl.h - one of the two is wrong about an on-disk bit"
#endif

#endif /* ACL_ABI_H */
