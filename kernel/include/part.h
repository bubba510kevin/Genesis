#ifndef PART_H
#define PART_H

#include "device.h"
#include "typesk.h"

/* The partition table, whichever kind it turns out to be.
 *
 * --- Why this is one file and not "gpt.c beside disk.c's MBR parse" -------
 * disk.c already parsed an MBR into a disk_partition_t with 32-bit LBAs and a
 * type BYTE. GPT has 64-bit LBAs and a type GUID, and neither fits. The two
 * obvious moves are both wrong:
 *
 *   Add a second scanner and a second struct, and volume.c asks each disk
 *   twice and merges. That is the shape of bug this tree has hit before -
 *   two live paths doing overlapping work, disagreeing about a disk that has
 *   both a protective MBR and a real GPT, which is EVERY GPT disk.
 *
 *   Widen disk_partition_t in place and leave the MBR parse where it is, and
 *   the caller still has to know which of the two ran to interpret `type`.
 *
 * So: one entry type wide enough for both, one entry point that decides which
 * table is on the disk, and disk.c's MBR parse moved in here beside the GPT
 * one. There is exactly one answer to "what partitions are on this disk", and
 * the decision of which table to believe is made once, in part_scan.
 *
 * --- Reading through the device layer -------------------------------------
 * Every read here goes through dev_read rather than ata_read. That is the
 * first thing device.h buys: a partition table on a USB stick, on a virtual
 * disk, or on a file-backed loop device is parsed by this same code, because
 * none of it knows what is underneath. It is also what makes the removable
 * case work - a disk pulled mid-scan returns -ENODEV from dev_read and the
 * scan reports a short table rather than reading a freed driver struct.
 */

#define PART_MAX 32             /* GPT usually declares 128; a from-scratch
                                 * kernel that finds more than 32 partitions
                                 * on one disk is looking at a corrupt table,
                                 * and 32 entries is 1KB of stack in the
                                 * scanner rather than 4KB. */

typedef enum {
    PART_TABLE_NONE = 0,        /* no signature: a whole-disk filesystem   */
    PART_TABLE_MBR,
    PART_TABLE_GPT
} part_table_t;

/* A GUID as it sits on disk: three little-endian integers then eight bytes
 * in order. NOT sixteen opaque bytes, because that is exactly the mistake
 * that makes a type GUID compare unequal to itself between a tool that byte-
 * swapped and one that did not. Stored here in the on-disk form and compared
 * bytewise; nothing in the kernel prints one yet. */
typedef struct {
    uint8 b[16];
} part_guid_t;

typedef struct part_entry {
    uint64 start_lba;
    uint64 sectors;

    /* Both are filled, and which one is meaningful depends on `table`. Both
     * present rather than a union: a union means every reader has to check
     * the tag before touching either field, and the one that forgets reads a
     * GUID as a type byte and gets 0xEE. */
    uint8       mbr_type;       /* PART_TABLE_MBR: 0x83, 0x06, 0x07 ...    */
    part_guid_t type_guid;      /* PART_TABLE_GPT                          */
    part_guid_t unique_guid;    /* PART_TABLE_GPT: stable across reboots   */

    uint8  bootable;
    uint32 index;               /* 1-based, as the partition is numbered   */
} part_entry_t;

/* Scan `disk` and fill `out` with up to `max` entries.
 *
 * Returns the number found (possibly zero), or a negative errno. `table` is
 * set to which kind was believed, and it is set even when the count is zero -
 * "no signature at all" and "a valid GPT with no used entries" are different
 * facts about the disk and a caller sizing a volume list wants both.
 *
 * A disk with a protective MBR (one entry of type 0xEE) and a valid GPT
 * reports the GPT. A protective MBR with an UNREADABLE GPT reports zero
 * partitions rather than the 0xEE entry, because handing back the protective
 * entry as a real partition means offering to mount the whole disk including
 * its own partition table. */
int part_scan(device_t *disk, part_entry_t *out, int max, part_table_t *table);

/* Non-zero if this GPT type GUID is one a filesystem could be on - as opposed
 * to a reserved, EFI-system or protective entry. What volume.c consults
 * before offering a partition a drive letter. */
int part_type_is_data(const part_entry_t *e);

/* Human-readable name of the table kind, for the boot report. */
const char *part_table_name(part_table_t t);

/* CRC-32 as GPT specifies it: the ordinary reflected polynomial 0xEDB88320,
 * init and final xor 0xFFFFFFFF. Exported because verification.c checks it
 * against a known vector - a CRC that is wrong in a consistent way validates
 * every table it computes itself and rejects every real one, and there is no
 * way to notice that from inside. */
uint32 part_crc32(const void *data, uint64 len);

#endif
