#include "vmalloc.h"
#include "pmm.h"

/* See vmalloc.h for why this is a flat sorted array rather than kheap.c's
 * boundary-tag design: this file has to work before kheap_init() runs, so it
 * cannot allocate its own bookkeeping. Ranges are kept sorted by base so
 * kvm_alloc_range's gap scan is a single linear pass. */

typedef struct {
    uint64 base;
    uint64 size;
} kvm_range_t;

static kvm_range_t ranges[KVM_MAX_RANGES];
static int range_count;

void kvm_init(void) {
    range_count = 0;
}

static uint64 align_up(uint64 value, uint64 align) {
    return (value + (align - 1)) & ~(align - 1);
}

uint64 kvm_alloc_range(uint64 size, uint64 align) {
    uint64 gap_start, gap_end, candidate;
    int i, insert_at;

    if (size == 0 || align == 0 || (align & (align - 1)) != 0) {
        return 0;
    }
    if (align < PMM_PAGE_SIZE) {
        align = PMM_PAGE_SIZE;
    }
    size = align_up(size, PMM_PAGE_SIZE);

    if (range_count >= KVM_MAX_RANGES) {
        return 0;
    }

    gap_start = KVM_REGION_START;
    for (i = 0; i <= range_count; i++) {
        gap_end = (i < range_count) ? ranges[i].base : KVM_REGION_END;

        candidate = align_up(gap_start, align);
        if (candidate < gap_end && (gap_end - candidate) >= size) {
            insert_at = i;
            for (int j = range_count; j > insert_at; j--) {
                ranges[j] = ranges[j - 1];
            }
            ranges[insert_at].base = candidate;
            ranges[insert_at].size = size;
            range_count++;
            return candidate;
        }

        if (i < range_count) {
            gap_start = ranges[i].base + ranges[i].size;
        }
    }

    return 0;
}

void kvm_free_range(uint64 base) {
    int i, j;

    for (i = 0; i < range_count; i++) {
        if (ranges[i].base == base) {
            for (j = i; j < range_count - 1; j++) {
                ranges[j] = ranges[j + 1];
            }
            range_count--;
            return;
        }
    }
}
