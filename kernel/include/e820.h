#ifndef E820_H
#define E820_H

#include "typesk.h"

/* The BIOS memory map, as reported by int 0x15 / eax=0xE820.
 *
 * This replaces two hardcoded guesses: TOTAL_MEM_BYTES ("how much RAM is
 * there") and the 0xA0000-0xFFFFF reservation ("which parts of it are not
 * RAM"). Both were right on QEMU and would have been wrong on the next
 * machine, in the second case silently - a page table placed in the VGA
 * aperture swallows its own zeroing and reads back as 0xFF.
 *
 * boot.asm collects the map in real mode, because E820 is a BIOS call and
 * there is no BIOS once we leave it. It writes the entries to E820_BUFFER,
 * which is below the kernel load address and inside the region the PMM
 * reserves at boot, so the table survives until it has been parsed.
 *
 * Entries are NOT sorted, NOT deduplicated, and MAY overlap - the firmware
 * makes no promises. Consumers must treat the map as a set of assertions
 * about individual ranges, never as a partition of the address space. The
 * PMM handles this by marking everything unusable first and then clearing
 * only what a type-1 entry vouches for. */

#define E820_BUFFER        0x00005000ULL   /* must match boot.asm            */
#define E820_MAX_ENTRIES   32              /* must match boot.asm            */

#define E820_TYPE_USABLE    1
#define E820_TYPE_RESERVED  2
#define E820_TYPE_ACPI      3   /* reclaimable once the tables are parsed    */
#define E820_TYPE_NVS       4
#define E820_TYPE_BAD       5

/* Exactly the 24-byte layout the BIOS fills in. The `attr` word is the ACPI
 * 3.0 extension; boot.asm presets it to 1 so a BIOS that only writes 20 bytes
 * leaves the "ignore this entry" bit clear rather than leaving us reading
 * whatever was in the buffer. */
typedef struct {
    uint64 base;
    uint64 length;
    uint32 type;
    uint32 attr;
} e820_entry_t;

typedef struct {
    uint64              count;
    const e820_entry_t *entries;
    int                 from_bios;   /* 0 = the fallback map below is in use */
} e820_map_t;

/* Parses the buffer boot.asm filled in. Never returns NULL: if the BIOS call
 * failed or produced nothing usable, this returns a conservative built-in map
 * (low 640KB plus 1MB-16MB) and sets from_bios = 0, so the kernel still boots
 * on hardware where E820 is unavailable. Callers that care should say so
 * loudly - running on the fallback means every memory-size decision above is
 * a guess again. */
const e820_map_t *e820_load(void);

/* One line per entry, with the type spelled out. Worth printing at every boot
 * while this is new: it is the only direct evidence of what the firmware
 * actually said, and every later "out of memory" or stray-write bug wants to
 * be checked against it first. */
void e820_report(const e820_map_t *map, uint8 color);

/* Highest address covered by any usable entry. This is a ceiling for sizing
 * the direct map and the PMM bitmap, NOT a count of available memory - the
 * range below it is full of holes. */
phys_addr_t e820_highest_usable(const e820_map_t *map);

/* Total bytes across usable entries. This one IS the memory you have. */
uint64 e820_usable_bytes(const e820_map_t *map);

#endif
