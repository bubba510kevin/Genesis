#include "e820.h"
#include "paging.h"
#include "screen.h"
#include "typesk.h"

/* The buffer boot.asm wrote: a 64-bit count followed by E820_MAX_ENTRIES
 * slots. Reached through the kernel window rather than a bare pointer,
 * because by the time this runs paging is on and physical 0x5000 is not a
 * valid virtual address. */
typedef struct {
    uint64       count;
    e820_entry_t entries[E820_MAX_ENTRIES];
} e820_buffer_t;

/* Used only if the BIOS call failed outright. Deliberately small and
 * pessimistic: 16MB is what the kernel assumed before E820 existed, so a
 * machine that lands here behaves exactly as it did previously rather than
 * inventing memory that may not be there. */
static const e820_entry_t fallback_entries[] = {
    { 0x00000000ULL, 0x0009FC00ULL, E820_TYPE_USABLE,   1 },
    { 0x0009FC00ULL, 0x00000400ULL, E820_TYPE_RESERVED, 1 },  /* EBDA        */
    { 0x000A0000ULL, 0x00060000ULL, E820_TYPE_RESERVED, 1 },  /* VGA + ROMs  */
    { 0x00100000ULL, 0x00F00000ULL, E820_TYPE_USABLE,   1 },  /* 1MB - 16MB  */
};

static e820_map_t map;

static const char *type_name(uint32 type) {
    switch (type) {
        case E820_TYPE_USABLE:   return "usable";
        case E820_TYPE_RESERVED: return "reserved";
        case E820_TYPE_ACPI:     return "ACPI reclaimable";
        case E820_TYPE_NVS:      return "ACPI NVS";
        case E820_TYPE_BAD:      return "bad memory";
        default:                 return "unknown";
    }
}

const e820_map_t *e820_load(void) {
    const e820_buffer_t *buf = (const e820_buffer_t *)(KERNEL_VMA + E820_BUFFER);
    uint64 count = buf->count;

    if (count == 0 || count > E820_MAX_ENTRIES) {
        map.count     = sizeof(fallback_entries) / sizeof(fallback_entries[0]);
        map.entries   = fallback_entries;
        map.from_bios = 0;
        return &map;
    }

    map.count     = count;
    map.entries   = buf->entries;
    map.from_bios = 1;
    return &map;
}

void e820_report(const e820_map_t *map, uint8 color) {
    uint64 i;

    if (!map->from_bios) {
        print_string("e820: BIOS call FAILED - using the built-in fallback map\n", 0x0C);
    }

    for (i = 0; i < map->count; i++) {
        const e820_entry_t *e = &map->entries[i];

        print_string("  ", color);
        print_hex64(e->base, color);
        print_string(" + ", color);
        print_hex64(e->length, color);
        print_string("  ", color);
        print_string(type_name(e->type), color);
        print_string("\n", color);
    }
}

phys_addr_t e820_highest_usable(const e820_map_t *map) {
    phys_addr_t top = 0;
    uint64 i;

    for (i = 0; i < map->count; i++) {
        const e820_entry_t *e = &map->entries[i];
        phys_addr_t end;

        if (e->type != E820_TYPE_USABLE || e->length == 0) {
            continue;
        }
        end = (phys_addr_t)(e->base + e->length);
        if (end < e->base) {
            continue;   /* firmware arithmetic wrapped; ignore the entry */
        }
        if (end > top) {
            top = end;
        }
    }
    return top;
}

uint64 e820_usable_bytes(const e820_map_t *map) {
    uint64 total = 0;
    uint64 i;

    for (i = 0; i < map->count; i++) {
        if (map->entries[i].type == E820_TYPE_USABLE) {
            total += map->entries[i].length;
        }
    }
    return total;
}
