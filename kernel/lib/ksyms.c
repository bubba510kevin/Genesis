#include "ksyms.h"

/* Defined in kernel/ksyms_data.c and placed by linker.ld's .ksyms output
 * section. Its own compiled-in content (a single non-zero byte, see
 * ksyms_data.c) is never read here - build.py's gen_ksyms() overwrites
 * this whole range of kernel.bin with the real table after the normal
 * link, in the format documented below. */
extern const uint8 __ksyms_reserved[];

/* On-disk format, written by build.py's gen_ksyms() straight into the
 * bytes reserved at __ksyms_reserved:
 *
 *   u32 count            - number of entries
 *   u32 strtab_size      - bytes of name data following the entries
 *   count * ksym_entry_t - sorted ascending by addr (nm -n's own order)
 *   strtab_size bytes    - NUL-terminated names, back to back
 *
 * ksym_entry_t is 16 bytes (8 + 4 + 4 pad) because that's what the C
 * struct below naturally lays out to on x86-64 - build.py packs its
 * Python side to match (struct.pack("<QI4x", ...)) rather than the other
 * way around, so this file never needs an unaligned byte-by-byte read. */
typedef struct {
    uint64 addr;
    uint32 name_offset;
    uint32 reserved;
} ksym_entry_t;

static uint32 sym_count(void) {
    uint32 count;
    const uint32 *p = (const uint32 *)__ksyms_reserved;
    count = p[0];
    return count;
}

static uint32 strtab_size(void) {
    const uint32 *p = (const uint32 *)__ksyms_reserved;
    return p[1];
}

static const ksym_entry_t *entries(void) {
    return (const ksym_entry_t *)(__ksyms_reserved + 8);
}

static const char *strtab(void) {
    return (const char *)(__ksyms_reserved + 8 + (uint64)sym_count() * sizeof(ksym_entry_t));
}

const char *ksym_lookup(uint64 addr, uint64 *out_offset) {
    uint32 count = sym_count();
    const ksym_entry_t *e = entries();
    uint32 lo, hi, mid, best;

    (void)strtab_size();  /* not needed for the lookup itself - bounds the blob only */

    if (count == 0 || addr < e[0].addr) {
        return 0;
    }

    lo = 0;
    hi = count - 1;
    best = 0;
    while (lo <= hi) {
        mid = lo + (hi - lo) / 2;
        if (e[mid].addr <= addr) {
            best = mid;
            if (mid == count - 1) {
                break;
            }
            lo = mid + 1;
        } else {
            if (mid == 0) {
                break;
            }
            hi = mid - 1;
        }
    }

    if (out_offset != 0) {
        *out_offset = addr - e[best].addr;
    }
    return strtab() + e[best].name_offset;
}

/* --- the inverse: NAME -> address (Part 15) -------------------------------
 *
 * ksym_lookup answers "what is at this address", which is what a backtrace
 * needs. A relocatable module needs the other direction: its undefined
 * symbols are names, and linking it means finding the address each one
 * stands for.
 *
 * Linear, not binary: the table is sorted by ADDRESS, which says nothing
 * about name order. Sorting a second copy by name would double the 50KB the
 * table already costs for a search that runs a few dozen times per module
 * load and never again. */
uint64 ksym_resolve(const char *name) {
    uint32 count = sym_count();
    const ksym_entry_t *e = entries();
    const char *strings = strtab();
    uint32 i;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    for (i = 0; i < count; i++) {
        const char *cand = strings + e[i].name_offset;
        uint32 k = 0;

        while (cand[k] != '\0' && cand[k] == name[k]) {
            k++;
        }
        if (cand[k] == '\0' && name[k] == '\0') {
            return e[i].addr;
        }
    }
    /* Zero is unambiguous as "not found": no kernel symbol lives at address
     * zero, and the caller refuses the load rather than relocating to it. */
    return 0;
}
