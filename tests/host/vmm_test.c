/* Host-side test for pmm.c and paging.c.
 *
 * These two files are the ones a mistake in is hardest to diagnose on real
 * hardware: the failure mode is a triple fault with no output, or worse, a
 * page fault a long way from the cause. Running the actual source against a
 * simulated physical address space catches the algorithmic errors on the host,
 * where there is a debugger.
 *
 * What this does NOT test: anything the CPU does. The MMU walk, TLB
 * invalidation, CR3, and the correctness of the entry bit layout are all
 * outside the harness. It tests the bookkeeping - which frame gets handed out,
 * which entry gets written, what is reclaimed on unmap - and that is exactly
 * the layer both of the bugs so far lived in.
 *
 * Build with tests/host/run.sh, which rewrites KERNEL_VMA, PHYSMAP_BASE and
 * KERNEL_MAP_SIZE so that physical addresses are host addresses, and strips
 * the inline asm. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "e820.h"
#include "paging.h"
#include "pmm.h"

#define FAKE_RAM_BASE  0x40000000ULL
#define FAKE_RAM_SIZE  (64ULL * 1024 * 1024)

/* Scratch for the kernel-stack virtual range. Deliberately outside every
 * usable E820 entry below, so the PMM never hands out a frame here and the
 * stack-zeroing writes cannot land on a page table. */
#define KSTACK_SCRATCH_BASE 0x50000000ULL
#define KSTACK_SCRATCH_SIZE (32ULL * 1024 * 1024)

/* A hole in the middle, standing in for the VGA aperture: the allocator must
 * never hand out an address inside it. */
#define HOLE_BASE      (FAKE_RAM_BASE + 0x00A00000ULL)
#define HOLE_SIZE      0x00060000ULL

static e820_entry_t test_entries[] = {
    { FAKE_RAM_BASE, 0x00A00000ULL,                E820_TYPE_USABLE,   1 },
    { HOLE_BASE,     HOLE_SIZE,                    E820_TYPE_RESERVED, 1 },
    { HOLE_BASE + HOLE_SIZE,
      FAKE_RAM_SIZE - 0x00A00000ULL - HOLE_SIZE,   E820_TYPE_USABLE,   1 },
};

static e820_map_t test_map = {
    sizeof(test_entries) / sizeof(test_entries[0]), test_entries, 1
};

static uint8 bitmap[(FAKE_RAM_BASE + FAKE_RAM_SIZE) / PMM_PAGE_SIZE / 8];
static uint8 refcounts[(FAKE_RAM_BASE + FAKE_RAM_SIZE) / PMM_PAGE_SIZE];

/* e820.c reports through the VGA driver; the harness only needs the symbols
 * to exist. Printing them is actually useful - the map the tests run against
 * shows up in the output. */
void print_string(const char *s, uint8 color) { (void)color; fputs(s, stdout); }
void print_hex64(uint64 v, uint8 color)       { (void)color; printf("0x%016llX", (unsigned long long)v); }
void print_hex(uint32 v, uint8 color)         { (void)color; printf("0x%08X", v); }
void clear_screen(uint8 color)                { (void)color; }

/* paging_enable_nx asks cpu.c whether the CPU has NX. cpu.c is not part of
 * this harness - it is CPUID and MSR reads, none of which the bookkeeping
 * under test depends on - so the answer is stubbed to yes, and run.sh strips
 * the EFER write from the scratch copy of paging.c. What is left is exactly
 * the part worth testing on the host: which bits end up in which entry. */
int cpu_has_nx(void) { return 1; }

int kbd_run_tests(void);
int fat_run_tests(const char *image);
int fat_write_run_tests(const char *rw_image);
int path_run_tests(void);
int object_run_tests(void);
int kstack_run_tests(void);
int ns_run_tests(void);
int pipe_run_tests(void);
int pe_run_tests(void);
int ntproc_run_tests(void);
int rtc_run_tests(void);
int part_run_tests(const char *image_dir);
int volume_run_tests(const char *image);
int bcache_run_tests(void);
int gnfs_run_tests(void);
int gnfs_fixture_run_tests(void);
int gnfs_fixture_write(const char *path);

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("  FAIL  %s\n", what);
        failures++;
    } else {
        printf("  ok    %s\n", what);
    }
}

static int in_hole(phys_addr_t p) {
    return p >= HOLE_BASE && p < HOLE_BASE + HOLE_SIZE;
}

static void test_pmm_respects_the_map(void) {
    phys_addr_t frames[4096];
    int i, n = 0;
    int all_aligned = 1, none_in_hole = 1, all_in_ram = 1;
    uint64 baseline;

    printf("\npmm: only usable memory is ever handed out\n");

    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    /* Not zero: every frame outside a usable E820 entry - everything below
     * the fake RAM region, and the hole inside it - is marked used from the
     * start and stays that way. That is the whole design. */
    baseline = pmm_used_frames();

    for (i = 0; i < 4096; i++) {
        phys_addr_t f = pmm_alloc_frame();
        if (f == 0) {
            break;
        }
        if (f & 0xFFF)                                   all_aligned = 0;
        if (in_hole(f))                                  none_in_hole = 0;
        if (f < FAKE_RAM_BASE ||
            f >= FAKE_RAM_BASE + FAKE_RAM_SIZE)          all_in_ram = 0;
        frames[n++] = f;
    }

    check(n == 4096, "allocated 4096 frames without running dry");
    check(all_aligned, "every frame is page aligned");
    check(none_in_hole, "no frame falls inside the reserved hole");
    check(all_in_ram, "no frame falls outside any usable entry");

    for (i = 0; i < n; i++) {
        pmm_free_frame(frames[i]);
    }
    check(pmm_used_frames() == baseline,
          "freeing everything returns the count to its post-init baseline");
}

static void test_pmm_exhaustion(void) {
    uint64 got = 0;

    printf("\npmm: exhaustion is reported, not wrapped around\n");

    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    while (pmm_alloc_frame() != 0) {
        got++;
        if (got > (FAKE_RAM_SIZE / PMM_PAGE_SIZE) + 16) {
            break;   /* would loop forever if the allocator reissued frames */
        }
    }
    check(got == (FAKE_RAM_SIZE - HOLE_SIZE) / PMM_PAGE_SIZE,
          "exactly the usable frames were issued, each exactly once");
    check(pmm_alloc_frame() == 0, "further allocation returns 0");
}

static void test_alloc_below(void) {
    phys_addr_t limit = FAKE_RAM_BASE + 0x00100000ULL;
    int i, all_below = 1;

    printf("\npmm: alloc_frame_below honours its limit\n");

    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    for (i = 0; i < 200; i++) {
        phys_addr_t f = pmm_alloc_frame_below(limit);
        if (f == 0 || f >= limit) {
            all_below = 0;
            break;
        }
    }
    check(all_below, "200 bootstrap frames all landed below the limit");
}

static void test_map_and_read_back(void) {
    address_space_t *as;
    virt_addr_t v = 0x0000000030000000ULL;
    phys_addr_t p;

    printf("\nvmm: a mapping can be read back\n");

    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    paging_init(&test_map);
    check(paging_physmap_ready(), "the direct map was built");

    as = vmm_kernel_space();
    check(vmm_get_phys_in(as, v) == 0, "the address starts unmapped");

    p = vmm_alloc_page_in(as, v, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    check(p != 0, "vmm_alloc_page_in succeeded");
    check(!in_hole(p), "the frame it chose is real memory");
    check(vmm_get_phys_in(as, v) == p, "get_phys returns the frame that was mapped");

    /* The regression that started all this: a frame in a hole read back as
     * all-ones through the mask. Anything other than the frame we were handed
     * means the walk and the write disagree. */
    check((vmm_get_phys_in(as, v) & ~0x000FFFFFFFFFF000ULL) == 0,
          "no reserved bits in the value read back");
}

static void test_unmap_reclaims_tables(void) {
    address_space_t *as = vmm_kernel_space();
    virt_addr_t v = 0x0000000030000000ULL;
    uint64 before, after_map, after_unmap;

    printf("\nvmm: unmapping gives back the frame and the tables\n");

    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    paging_init(&test_map);

    before = pmm_used_frames();
    check(vmm_alloc_page_in(as, v, PAGE_PRESENT | PAGE_RW | PAGE_USER) != 0,
          "mapped one page into empty address space");
    after_map = pmm_used_frames();
    check(after_map - before == 4,
          "cost was 4 frames: one page plus PDPT, PD and PT");

    vmm_unmap_page_in(as, v, VMM_FREE_FRAME);
    after_unmap = pmm_used_frames();
    check(after_unmap == before,
          "unmapping returned all four - no leaked page tables");
    check(vmm_get_phys_in(as, v) == 0, "the address is unmapped again");
}

static void test_shared_table_is_not_reclaimed_early(void) {
    address_space_t *as = vmm_kernel_space();
    virt_addr_t a = 0x0000000030000000ULL;
    virt_addr_t b = 0x0000000030001000ULL;   /* same page table as `a` */
    uint64 before;

    printf("\nvmm: a table still in use survives its neighbour's unmap\n");

    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    paging_init(&test_map);

    before = pmm_used_frames();
    vmm_alloc_page_in(as, a, PAGE_PRESENT | PAGE_RW);
    vmm_alloc_page_in(as, b, PAGE_PRESENT | PAGE_RW);
    check(pmm_used_frames() - before == 5, "two pages shared one set of tables");

    vmm_unmap_page_in(as, a, VMM_FREE_FRAME);
    check(vmm_get_phys_in(as, b) != 0,
          "the surviving mapping is still intact");
    check(pmm_used_frames() - before == 4,
          "only the page was freed, not the table it shared");

    vmm_unmap_page_in(as, b, VMM_FREE_FRAME);
    check(pmm_used_frames() == before, "the last unmap collapsed the whole chain");
}

static void test_out_of_memory_is_clean(void) {
    address_space_t *as = vmm_kernel_space();
    uint64 leaked_at_failure;
    virt_addr_t v = 0x0000000050000000ULL;

    printf("\nvmm: a failed mapping leaves nothing behind\n");

    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    paging_init(&test_map);

    while (pmm_alloc_frame() != 0) {
        /* drain */
    }
    leaked_at_failure = pmm_used_frames();

    check(vmm_alloc_page_in(as, v, PAGE_PRESENT | PAGE_RW) == 0,
          "allocation fails when there is no memory");
    check(pmm_used_frames() == leaked_at_failure,
          "the failed attempt leaked no frames");
    check(vmm_get_phys_in(as, v) == 0,
          "no partial mapping was left in place");
}

/* The kstack tests need a live PMM and page tables; they run after the VMM
 * tests have finished with theirs. */
void kstack_test_prepare(void) {
    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    paging_init(&test_map);
}

static void test_space_lifecycle(void) {
    address_space_t *a, *b;
    uint64 before, after_create, after_map;

    printf("\nvmm: address space create and destroy\n");

    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    paging_init(&test_map);

    before = pmm_used_frames();
    a = vmm_space_create();
    check(a != NULL, "a space is created");
    after_create = pmm_used_frames();
    check(after_create - before == 1, "it costs exactly one frame - the PML4");

    /* The kernel half must be shared, or a syscall taken in this space would
     * fault on the first kernel address it touched. */
    check(vmm_get_phys_in(a, (virt_addr_t)0xFFFFFE0000001000ULL) != 0,
          "the direct map is visible from the new space");

    b = vmm_space_create();
    check(b != NULL && b != a, "a second space is distinct");

    /* Same virtual address, two spaces, two different frames - which is the
     * entire point of having more than one. */
    {
        phys_addr_t pa = vmm_alloc_page_in(a, 0x30000000ULL, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        phys_addr_t pb = vmm_alloc_page_in(b, 0x30000000ULL, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        check(pa != 0 && pb != 0 && pa != pb,
              "the same address maps to different frames in each space");
        check(vmm_get_phys_in(a, 0x30000000ULL) == pa &&
              vmm_get_phys_in(b, 0x30000000ULL) == pb,
              "and each space reads back its own");
    }
    after_map = pmm_used_frames();

    vmm_space_destroy(b);
    check(pmm_used_frames() < after_map, "destroying a space frees its frames");

    vmm_space_destroy(a);
    check(pmm_used_frames() == before,
          "destroying both returns every frame, tables included");

    /* The kernel's own tables must survive - they were only ever borrowed. */
    check(vmm_get_phys_in(vmm_kernel_space(),
                          (virt_addr_t)0xFFFFFE0000001000ULL) != 0,
          "the kernel's direct map was not freed along with them");
}

static void test_destroy_refuses_dangerous_cases(void) {
    printf("\nvmm: destroy refuses what would be fatal\n");

    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    paging_init(&test_map);

    {
        uint64 before = pmm_used_frames();
        vmm_space_destroy(vmm_kernel_space());
        check(pmm_used_frames() == before,
              "destroying the kernel space is a no-op, not a catastrophe");
        vmm_space_destroy(NULL);
        check(pmm_used_frames() == before, "destroying NULL is a no-op");
        vmm_space_destroy(vmm_current_space());
        check(pmm_used_frames() == before,
              "destroying the space you are running on is refused");
    }
}

/* --- refcounting and copy-on-write -------------------------------------- */

static void test_frame_refcounts(void) {
    phys_addr_t f;

    printf("\npmm: reference counts\n");
    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));

    f = pmm_alloc_frame();
    check(f != 0, "a frame was allocated");
    check(pmm_frame_refs(f) == 1, "and starts with one reference");

    pmm_ref_frame(f);
    check(pmm_frame_refs(f) == 2, "taking a reference raises the count");

    pmm_free_frame(f);
    check(pmm_frame_refs(f) == 1, "dropping one leaves it allocated");
    check(pmm_alloc_frame() != f, "and it is not handed out again");

    pmm_free_frame(f);
    check(pmm_frame_refs(f) == 0, "dropping the last one frees it");

    {
        /* The failure this replaced: an unconditional free let a frame that
         * two spaces still mapped be handed to a third. */
        phys_addr_t again = pmm_alloc_frame();
        check(again == f, "the freed frame is available again");
    }
}

static void test_cow_clone_shares_without_copying(void) {
    address_space_t *parent, *child;
    phys_addr_t frame;
    uint64 free_before, free_after;
    virt_addr_t va = 0x400000;

    printf("\nvmm: clone shares pages copy-on-write\n");
    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    paging_init(&test_map);

    parent = vmm_space_create();
    check(parent != NULL, "a parent space was created");

    frame = vmm_alloc_page_in(parent, va, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    check(frame != 0, "with one writable user page in it");
    *(volatile uint64 *)phys_to_virt(frame) = 0xABCDEF;

    free_before = pmm_free_frames();
    child = vmm_space_clone(parent);
    free_after = pmm_free_frames();

    check(child != NULL, "the space cloned");
    check(vmm_get_phys_in(child, va) == frame,
          "the child maps the SAME frame, not a copy");
    check(pmm_frame_refs(frame) == 2, "which now has two references");
    /* Four frames: the child's PML4, and one table at each of the three
     * levels below it to reach a single page. The data page itself is NOT
     * among them - that is the whole claim being tested. */
    check(free_before - free_after == 4,
          "only the four table frames were allocated, no data page");

    vmm_space_destroy(child);
    check(pmm_frame_refs(frame) == 1,
          "destroying the child gives back its reference, not the frame");
    check(vmm_get_phys_in(parent, va) == frame,
          "and the parent still maps it");

    vmm_space_destroy(parent);
    check(pmm_frame_refs(frame) == 0, "the last space out frees it");
}

static void test_cow_write_fault_splits_the_page(void) {
    address_space_t *parent, *child;
    phys_addr_t original, split;
    virt_addr_t va = 0x400000;

    printf("\nvmm: a write fault on a shared page copies it\n");
    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    paging_init(&test_map);

    parent   = vmm_space_create();
    original = vmm_alloc_page_in(parent, va, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    *(volatile uint64 *)phys_to_virt(original) = 0x1234;

    child = vmm_space_clone(parent);
    check(child != NULL, "the space cloned");

    /* The fault handler works on the CURRENT space, so make the child it. */
    vmm_switch_to(child);
    check(vmm_handle_write_fault(va, 0x7) == 1,
          "a user write fault on the shared page is resolved");

    split = vmm_get_phys_in(child, va);
    check(split != original, "the child got a private frame");
    check(*(volatile uint64 *)phys_to_virt(split) == 0x1234,
          "with the contents copied into it");
    check(pmm_frame_refs(original) == 1,
          "and the original is down to one reference");

    /* The parent is now the only sharer, so its own first write must NOT
     * copy again - it should simply get write permission back. */
    vmm_switch_to(parent);
    check(vmm_handle_write_fault(va, 0x7) == 1,
          "the last sharer's write fault is resolved too");
    check(vmm_get_phys_in(parent, va) == original,
          "by keeping the page rather than copying it a second time");

    check(vmm_handle_write_fault(va, 0x7) == 0,
          "a write that is no longer copy-on-write is not claimed");
    check(vmm_handle_write_fault(va, 0x4) == 0,
          "and neither is a not-present fault");
}

/* Walk to the leaf entry for `va`, and to the three entries above it.
 *
 * The walk is done here rather than by asking paging.c for the entry, because
 * the bug this guards against is a flag lost between reading an entry and
 * making the next mapping from it - and an accessor living in the same file
 * could lose it in exactly the same way. The test looks at the page tables
 * themselves, which is the only thing the CPU will look at either. */
#define TEST_ADDR_MASK 0x000FFFFFFFFFF000ULL

static uint64 entry_at_level(address_space_t *as, virt_addr_t va, int level) {
    uint64 *t = (uint64 *)phys_to_virt(as->root);
    uint64  e;
    int     shift;

    /* level 4 = PML4 entry, 3 = PDPT, 2 = PD, 1 = the leaf PT entry. */
    for (shift = 39; shift >= 12; shift -= 9) {
        e = t[(va >> shift) & 0x1FF];
        if ((e & PAGE_PRESENT) == 0) {
            return 0;
        }
        if (level == (shift - 3) / 9) {
            return e;
        }
        t = (uint64 *)phys_to_virt(e & TEST_ADDR_MASK);
    }
    return 0;
}

static uint64 leaf_entry(address_space_t *as, virt_addr_t va) {
    return entry_at_level(as, va, 1);
}

static void test_nx_is_enforced_and_survives_fork(void) {
    address_space_t *as, *parent, *child;
    virt_addr_t va = 0x400000;
    phys_addr_t original, split;
    uint64 flags = PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_NX;

    printf("\nvmm: no-execute is stripped when unavailable, kept when not\n");
    pmm_init(&test_map, bitmap, sizeof(bitmap), refcounts, sizeof(refcounts));
    paging_init(&test_map);

    /* Before EFER.NXE, bit 63 is RESERVED rather than ignored: an entry
     * carrying it faults on every access, not on a fetch, and reports a cause
     * that reads like a corrupt page table. So the flag must be stripped, and
     * a caller must not have to know that - which is what makes PAGE_NX safe
     * to pass unconditionally from a loader. */
    check(paging_nx_enabled() == 0, "NX starts disabled");
    as = vmm_space_create();
    check(vmm_alloc_page_in(as, va, flags) != 0, "a page maps with PAGE_NX");
    check((leaf_entry(as, va) & PAGE_NX) == 0,
          "and bit 63 is STRIPPED, not written, while NX is off");
    vmm_space_destroy(as);

    check(paging_enable_nx() == 1, "NX enables");
    check(paging_nx_enabled() == 1, "and says so");

    parent = vmm_space_create();
    original = vmm_alloc_page_in(parent, va, flags);
    check(original != 0, "a page maps with PAGE_NX again");
    check((leaf_entry(parent, va) & PAGE_NX) != 0, "and now bit 63 is set");
    *(volatile uint64 *)phys_to_virt(original) = 0x1234;

    /* Bit 63 on a parent entry means "nothing below this is executable",
     * which is a far broader statement than the one the caller made about one
     * page - it would take out the image's text along with the data page. */
    check((entry_at_level(parent, va, 4) & PAGE_NX) == 0,
          "the PML4 entry above it is NOT marked no-execute");
    check((entry_at_level(parent, va, 3) & PAGE_NX) == 0,
          "nor the PDPT entry");
    check((entry_at_level(parent, va, 2) & PAGE_NX) == 0,
          "nor the PD entry");

    /* clone_page used to carry flags across with `entry & 0xFFF`, which is
     * every flag except this one. The child would have inherited an
     * executable copy of a page the parent had marked non-executable - a
     * protection that vanished on fork, in the child only. */
    child = vmm_space_clone(parent);
    check(child != NULL, "the space cloned");
    check((leaf_entry(child, va) & PAGE_NX) != 0,
          "and the child's copy is still no-execute");
    check((leaf_entry(parent, va) & PAGE_NX) != 0,
          "as is the parent's");

    /* And the same for the private copy a write fault hands out: the page
     * must not become executable at the moment it is first written to. */
    vmm_switch_to(child);
    check(vmm_handle_write_fault(va, 0x7) == 1, "a write fault splits it");
    split = vmm_get_phys_in(child, va);
    check(split != original, "into a private frame");
    check((leaf_entry(child, va) & PAGE_NX) != 0,
          "which is no-execute too");
    check((leaf_entry(child, va) & PAGE_RW) != 0,
          "and writable, which is the point of the fault");

    vmm_switch_to(vmm_kernel_space());
    vmm_space_destroy(child);
    vmm_space_destroy(parent);
}

int main(int argc, char **argv) {
    void *ram = mmap((void *)FAKE_RAM_BASE, FAKE_RAM_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    void *scratch = mmap((void *)KSTACK_SCRATCH_BASE, KSTACK_SCRATCH_SIZE,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    if (ram != (void *)FAKE_RAM_BASE) {
        printf("could not map fake RAM at the fixed address\n");
        return 2;
    }
    if (scratch != (void *)KSTACK_SCRATCH_BASE) {
        printf("could not map the kernel-stack scratch region\n");
        return 2;
    }

    /* Not a test run: write the gnfs ACL fixture and stop. See
     * tests/host/gnfs_fixture.c - run.sh uses this to prove the committed
     * fixture is exactly what the kernel's own gnfs code produces. */
    if (argc == 3 && strcmp(argv[1], "--gnfs-fixture") == 0) {
        return gnfs_fixture_write(argv[2]) == 0 ? 0 : 1;
    }

    test_pmm_respects_the_map();
    test_pmm_exhaustion();
    test_alloc_below();
    test_map_and_read_back();
    test_unmap_reclaims_tables();
    test_shared_table_is_not_reclaimed_early();
    test_out_of_memory_is_clean();
    test_space_lifecycle();
    test_destroy_refuses_dangerous_cases();
    test_frame_refcounts();
    test_cow_clone_shares_without_copying();
    test_cow_write_fault_splits_the_page();
    test_nx_is_enforced_and_survives_fork();

    /* The storage chain runs FIRST among the suites that allocate objects.
     *
     * object.c's pool is a fixed 64 entries with no reset, and the suites
     * below hold objects for their duration - so a chain test placed after
     * them fails at dev_attach with -ENFILE, which reads exactly like a
     * broken device layer and is nothing of the kind. Ordering is the whole
     * fix; the alternative is a per-suite pool reset that the kernel has no
     * reason to grow. */
    if (argc > 1) {
        failures += volume_run_tests(argv[1]);
    }

    /* Straight after the storage chain and before everything else, for the
     * same pool-ordering reason - and because the cache tests want the device
     * read counters to mean what they say, which they only do while nothing
     * else has been reading. */
    failures += bcache_run_tests();

    /* gnfs keeps its own static mount-slot arrays (kernel/gnfs/gnfs_vfs.c),
     * separate from object.c's pool - no ordering dependency on the storage
     * chain above or the suites below. */
    failures += gnfs_run_tests();

    failures += kbd_run_tests();
    failures += path_run_tests();
    failures += object_run_tests();
    failures += kstack_run_tests();
    failures += ns_run_tests();
    failures += pipe_run_tests();
    failures += pe_run_tests();
    failures += ntproc_run_tests();
    failures += rtc_run_tests();
    failures += part_run_tests(argc > 2 ? argv[2] : "build/hosttest/imgs");


    if (argc > 1) {
        failures += fat_run_tests(argv[1]);
    } else {
        printf("\nfat: skipped (no image argument)\n");
        printf("storage: skipped (no image argument)\n");
    }

    /* The write tests get their OWN COPY of the image, argv[4], and that is
     * not tidiness: every other suite here reads the fixture and asserts
     * exact sizes and contents against it. A test that writes to the shared
     * image would pass on its own and change what the suites around it are
     * measuring - and it would do so depending on the order they run in,
     * which is the failure this file already carries two comments about. */
    if (argc > 4) {
        failures += fat_write_run_tests(argv[4]);
    } else {
        printf("\nfat write: skipped (no writable image argument)\n");
    }

    /* The ACL fixture the guest mounts as /mnt/d - and the Windows
     * SECURITY_DESCRIPTOR view of its secret.txt, which used to be checked
     * against a ZFS pool before ZFS left the tree. */
    failures += gnfs_fixture_run_tests();

    printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
