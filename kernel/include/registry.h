#ifndef REGISTRY_H
#define REGISTRY_H

#include "object.h"
#include "typesk.h"

/* The Configuration Manager - the registry, which on NT lives in the
 * kernel (ROADMAP item 16(p)). advapi32's Reg* calls are a thin client of
 * the NtCreateKey family (kernel/exec/nt.c), and drivers read their service
 * keys from it, so it has to exist before either.
 *
 * A tree of KEYS under \Registry, each with a name, subkeys kept sorted
 * case-insensitively (the order NtEnumerateKey reports, as on NT), values
 * kept in the order they were first set, an optional class string and a
 * last-write time. Names are UTF-16 and compared ignoring ASCII case.
 *
 * WHAT IT IS NOT YET: persistent. The tree is built at boot -
 * \Registry\Machine (HKLM) with SOFTWARE, SYSTEM, HARDWARE, SAM and
 * SECURITY, and \Registry\User\.DEFAULT (HKU, and HKCU until there are
 * user tokens) - seeded with the keys programs read to learn what they are
 * running on (the Windows NT CurrentVersion values, the session-manager
 * environment, the computer name, the processor), and everything written
 * after that lasts until reboot. Loading hives in the regf format from
 * files, and writing them back, is the next step; change notification and
 * security on keys come after it. */

#define REG_NONE                 0
#define REG_SZ                   1
#define REG_EXPAND_SZ            2
#define REG_BINARY               3
#define REG_DWORD                4
#define REG_DWORD_BIG_ENDIAN     5
#define REG_LINK                 6
#define REG_MULTI_SZ             7
#define REG_QWORD                11

#define REG_OPTION_VOLATILE      0x1u
#define REG_CREATED_NEW_KEY      1
#define REG_OPENED_EXISTING_KEY  2

struct reg_key;

/* Build the tree. Once, at boot, after the heap. */
void registry_init(void);

/* Resolve `name` (UTF-16, `chars` long) - absolute "\Registry\..." when
 * `base` is NULL, otherwise relative to `base` - and, with `create`, make
 * whatever is missing (the class given to the LAST component only). An
 * empty relative name names `base` itself. Returns 0 with the key and
 * whether it was made, or an NTSTATUS. The key comes back referenced. */
uint32 registry_lookup(struct reg_key *base, const uint16 *name, uint32 chars,
                       int create, const uint16 *klass, uint32 class_chars,
                       struct reg_key **out, int *created);

/* A handle's view of a key: an object holding one reference on it. */
object_t *registry_key_object(struct reg_key *k);
struct reg_key *registry_key_of(object_t *obj);   /* NULL: not a key */
void registry_key_deref(struct reg_key *k);

uint32 registry_delete_key(struct reg_key *k);
uint32 registry_set_value(struct reg_key *k, const uint16 *name, uint32 chars,
                          uint32 type, const void *data, uint32 size);
uint32 registry_delete_value(struct reg_key *k, const uint16 *name,
                             uint32 chars);

/* The query and enumerate calls, writing NT's information structures into
 * `buf` (a kernel buffer of `len` bytes) and the full size wanted into
 * *result. STATUS_BUFFER_TOO_SMALL when not even the fixed part fits,
 * STATUS_BUFFER_OVERFLOW when it does but the rest does not (the fixed part
 * is written), STATUS_NO_MORE_ENTRIES past the last one. */
uint32 registry_query_value(struct reg_key *k, const uint16 *name,
                            uint32 chars, uint32 klass, void *buf, uint32 len,
                            uint32 *result);
uint32 registry_enumerate_value(struct reg_key *k, uint32 index, uint32 klass,
                                void *buf, uint32 len, uint32 *result);
uint32 registry_enumerate_key(struct reg_key *k, uint32 index, uint32 klass,
                              void *buf, uint32 len, uint32 *result);
uint32 registry_query_key(struct reg_key *k, uint32 klass, void *buf,
                          uint32 len, uint32 *result);

#endif
