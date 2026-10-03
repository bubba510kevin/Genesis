#include "cpu.h"
#include "e820.h"
#include "io.h"
#include "paging.h"
#include "iomgr.h"
#include "pmm.h"
#include "ksmp.h"
#include "kprintf.h"
#include "timer.h"
#include "typesk.h"

#if !defined(__x86_64__)
#error "paging.c is the long-mode implementation; build it with -m64"
#endif

/* ===========================================================================
 * Long-mode (IA-32e) paging.
 * ---------------------------------------------------------------------------
 * Four levels, 64-bit entries, and a 48-bit virtual address split five ways:
 *
 *   [63:48] sign extension   [47:39] PML4   [38:30] PDPT
 *   [29:21] PD               [20:12] PT     [11:0] offset
 *
 * --- Reaching page tables ---
 * A page table is a physical frame, and the CPU can only be told about
 * physical frames - so to EDIT one, the kernel needs a virtual address that
 * resolves to it. There are three ways to arrange that, and this file used to
 * use the second:
 *
 *   1. Identity map everything. Simple, and gives away the low half of the
 *      address space that user code wants.
 *   2. Recursive mapping: point one PML4 slot at the PML4 itself and let the
 *      MMU walk the hierarchy as though it were one level shallower. Costs
 *      one slot and no memory at all.
 *   3. Direct map: map all of physical RAM at a fixed offset. Costs one PD
 *      frame per gigabyte.
 *
 * Recursive mapping has a ceiling that is easy to miss until you hit it: the
 * trick works by making the CPU re-walk CR3, so it can only expose the tables
 * of the address space currently loaded. Editing any OTHER address space -
 * which is the entire job of fork, exec, and process teardown - means
 * switching CR3 to it first. The direct map has no such restriction: any
 * table of any space is phys_to_virt(entry & ADDR_MASK), reachable whether or
 * not that space is the one running. That is why this file moved to (3), and
 * why every function below takes the address space as a parameter.
 *
 * --- Canonical addresses ---
 * Only bits 47:0 are translated. Bits 63:48 must all equal bit 47, so
 * 0x0000800000000000 through 0xFFFF7FFFFFFFFFFF do not exist and touching one
 * is a #GP rather than the page fault you would expect. Every entry point
 * checks.
 *
 * --- A note on NX ---
 * Bit 63 is no-execute, but only once EFER.NXE is set. Until then it is a
 * reserved bit, and a reserved bit in a live entry faults with err bit 3 set,
 * which looks nothing like a permissions problem.
 *
 * --- Upper-half sharing ---
 * Every address space gets the same PML4 entries for the kernel window and
 * the direct map, copied at creation. Kernel mappings are therefore visible
 * from every space without synchronisation, which is what makes a syscall
 * able to touch kernel data without a CR3 switch.
 * ======================================================================== */

#define PT_ENTRIES        512

/* Physical address field is bits 51:12. Masking with the 32-bit habit of
 * 0xFFFFF000 silently truncates anything above 4GB. */
#define ADDR_MASK         0x000FFFFFFFFFF000ULL
#define PAGE_HUGE         0x80ULL        /* PS: 1GB at PDPT, 2MB at PD */

#define KERNEL_PML4_SLOT  511ULL
#define KERNEL_PT_COUNT   (KERNEL_MAP_SIZE / 0x200000ULL)

#define ONE_GB            0x40000000ULL
#define TWO_MB            0x00200000ULL

/* The kernel window: KERNEL_VMA + P -> physical P, for P < KERNEL_MAP_SIZE.
 * Static, because it has to exist before there is a PMM to allocate from.
 * 4KB granular so no walk below it ever meets a large page. */
__attribute__((aligned(4096))) static uint64 pml4[PT_ENTRIES];
__attribute__((aligned(4096))) static uint64 kernel_pdpt[PT_ENTRIES];
__attribute__((aligned(4096))) static uint64 kernel_pd[PT_ENTRIES];
__attribute__((aligned(4096))) static uint64 kernel_pt[KERNEL_PT_COUNT][PT_ENTRIES];

static address_space_t kernel_space;
/* The space loaded in CR3 is a property of a CPU: each runs its own thread,
 * in its own process's space. Kept under the old name - every use below
 * means "the one loaded HERE" - and resolved through the per-CPU block. */
#define current_space (smp_this_cpu()->cur_space)

static int physmap_ready;

static inline uint64 pml4_index(virt_addr_t v) { return (v >> 39) & 0x1FFULL; }
static inline uint64 pdpt_index(virt_addr_t v) { return (v >> 30) & 0x1FFULL; }
static inline uint64 pd_index(virt_addr_t v)   { return (v >> 21) & 0x1FFULL; }
static inline uint64 pt_index(virt_addr_t v)   { return (v >> 12) & 0x1FFULL; }

/* Bits 63:48 must replicate bit 47. */
static inline int is_canonical(virt_addr_t v) {
    uint64 top = v >> 47;
    return top == 0ULL || top == 0x1FFFFULL;
}

static void invlpg_local(virt_addr_t virt_addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

/* Drop the translation for `virt_addr` in `as` from every TLB that can hold
 * it, and return only when they all have.
 *
 * invlpg is a per-CPU instruction: it drops the translation from THIS core's
 * TLB and says nothing to any other. With several CPUs running threads of one
 * process, a caller that unmaps a page and frees the frame has otherwise
 * handed that frame back while another core still translates to it - silent
 * memory corruption, not a fault.
 *
 * WHICH CPUs is the whole question. The kernel half is shared by every
 * space, so a change there can be cached on every CPU. A user address in
 * `as` can be cached only where `as` is LOADED - without PCIDs, a CR3 load
 * flushes every non-global entry, so a CPU that switched away holds nothing
 * for it - and those are the CPUs asked. */
static void tlb_invalidate(address_space_t *as, virt_addr_t virt_addr) {
    if (virt_addr >= 0xFFFF800000000000ULL) {
        invlpg_local(virt_addr);
        smp_tlb_shootdown(virt_addr);
        return;
    }
    if (as == current_space) {
        invlpg_local(virt_addr);
    }
    smp_tlb_shootdown_space(as, virt_addr);
}

static void flush_tlb(void) {
    uint64 cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* A table, as a pointer we can write through.
 *
 * The whole file reduces to this one line. Before the direct map existed this
 * was four different macros' worth of recursive-slot address arithmetic, each
 * of which had to be re-derived if the recursive slot ever moved. */
static inline uint64 *table_at(phys_addr_t phys) {
    return (uint64 *)phys_to_virt(phys);
}

static int table_is_empty(const uint64 *table) {
    uint64 i;
    for (i = 0; i < PT_ENTRIES; i++) {
        if (table[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* --- the direct map ----------------------------------------------------- */

/* Built after the kernel window and before CR3 is loaded, so its own tables
 * are written through KERNEL_VMA rather than through the map it is building.
 * That is why the frames come from pmm_alloc_frame_below(KERNEL_MAP_SIZE):
 * they have to live somewhere the kernel window already covers. */
static int physmap_init(const e820_map_t *map) {
    phys_addr_t top = e820_highest_usable(map);
    phys_addr_t pdpt_phys;
    uint64 *pdpt;
    uint64 gb, gb_count, i;

    if (top == 0) {
        return 0;
    }
    gb_count = (top + ONE_GB - 1) / ONE_GB;
    if (gb_count > PT_ENTRIES) {
        gb_count = PT_ENTRIES;   /* 512GB is one PML4 slot's worth */
    }

    pdpt_phys = pmm_alloc_frame_below(KERNEL_MAP_SIZE);
    if (pdpt_phys == 0) {
        return 0;
    }
    pdpt = (uint64 *)(KERNEL_VMA + pdpt_phys);
    for (i = 0; i < PT_ENTRIES; i++) {
        pdpt[i] = 0;
    }

    for (gb = 0; gb < gb_count; gb++) {
        phys_addr_t pd_phys = pmm_alloc_frame_below(KERNEL_MAP_SIZE);
        uint64 *pd;

        if (pd_phys == 0) {
            return 0;
        }
        pd = (uint64 *)(KERNEL_VMA + pd_phys);

        /* 2MB pages: 512 of them per PD, so one frame per gigabyte. Mapping
         * the whole gigabyte even when RAM ends partway through it is
         * deliberate - the entries past the end are never dereferenced, and
         * the alternative is a partial PD plus a special case here. */
        for (i = 0; i < PT_ENTRIES; i++) {
            pd[i] = (gb * ONE_GB + i * TWO_MB) | PAGE_PRESENT | PAGE_RW | PAGE_HUGE;
        }
        pdpt[gb] = (pd_phys & ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
    }

    pml4[PHYSMAP_PML4_SLOT] = (pdpt_phys & ADDR_MASK) | PAGE_PRESENT | PAGE_RW;
    return 1;
}

void paging_init(const e820_map_t *map) {
    uint64 i, page_count;
    phys_addr_t pml4_phys;

    for (i = 0; i < PT_ENTRIES; i++) {
        pml4[i]        = 0;
        kernel_pdpt[i] = 0;
        kernel_pd[i]   = 0;
    }
    for (i = 0; i < KERNEL_PT_COUNT; i++) {
        uint64 j;
        for (j = 0; j < PT_ENTRIES; j++) {
            kernel_pt[i][j] = 0;
        }
    }

    page_count = KERNEL_MAP_SIZE / PMM_PAGE_SIZE;
    for (i = 0; i < page_count; i++) {
        kernel_pt[i / PT_ENTRIES][i % PT_ENTRIES] =
            (i * PMM_PAGE_SIZE) | PAGE_PRESENT | PAGE_RW;
    }
    for (i = 0; i < KERNEL_PT_COUNT; i++) {
        kernel_pd[pd_index(KERNEL_VMA) + i] =
            (virt_to_phys((virt_addr_t)kernel_pt[i]) & ADDR_MASK)
            | PAGE_PRESENT | PAGE_RW;
    }

    kernel_pdpt[pdpt_index(KERNEL_VMA)] =
        (virt_to_phys((virt_addr_t)kernel_pd) & ADDR_MASK) | PAGE_PRESENT | PAGE_RW;

    pml4[KERNEL_PML4_SLOT] =
        (virt_to_phys((virt_addr_t)kernel_pdpt) & ADDR_MASK) | PAGE_PRESENT | PAGE_RW;

    /* Before the CR3 load, so the hierarchy is complete when it takes effect
     * and there is no window in which phys_to_virt resolves to nothing. */
    physmap_ready = physmap_init(map);

    /* Deliberately no identity map. The trampoline's tables had one so it
     * could survive turning paging on; execution is already in the higher
     * half by the time this runs. */
    pml4_phys = virt_to_phys((virt_addr_t)pml4) & ADDR_MASK;
    kernel_space.root = pml4_phys;
    current_space = &kernel_space;

    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64)pml4_phys) : "memory");

    /* CR0.WP: make the read-only bit apply to ring 0 as well.
     *
     * It is clear out of reset, and while it is clear the kernel can write
     * through a read-only mapping without faulting. That is invisible until
     * copy-on-write exists and then it is silent corruption: a write(2) or a
     * read(2) landing in a forked process's shared buffer would modify the
     * page BOTH processes are still sharing, with no fault to trigger the
     * copy. Every write to a COW page has to trap, including the kernel's
     * own - which is what this bit buys. */
    {
        uint64 cr0 = 0;   /* initialised so the host test harness, which
                           * strips inline asm, does not read it unset */
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= (1ULL << 16);
        __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    }
}

int paging_physmap_ready(void) {
    return physmap_ready;
}

/* --- no-execute ---------------------------------------------------------
 *
 * EFER.NXE is a single bit, and turning it on is the easy half. The half
 * worth writing down is what it does to bit 63 BEFORE it is on: the bit is
 * reserved, not ignored, so an entry carrying it faults on every access -
 * read, write and fetch alike - with error bit 3 set. That fault names the
 * faulting address and says "reserved bit", which reads like a corrupt page
 * table rather than like a loader that asked for one flag too many.
 *
 * The kernel therefore never lets a caller create that entry. PAGE_NX is
 * always safe to pass; it is masked out here when the CPU cannot honour it,
 * which turns a machine without NX into one that merely does not enforce it.
 * That is the correct failure: enforcement is a hardening property, and no
 * program's correctness depends on being denied a fetch it was not going to
 * make.
 *
 * Enabling this is per-CPU state, so a second CPU will need the same write in
 * its own bring-up path. There is one CPU today; the comment is the reminder.
 */
static int nx_enabled;

int paging_enable_nx(void) {
    if (nx_enabled) {
        return 1;
    }
    if (!cpu_has_nx()) {
        return 0;
    }
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    nx_enabled = 1;
    return 1;
}

int paging_nx_enabled(void) {
    return nx_enabled;
}

/* Which bits of a caller's flags word belong in a LEAF entry: the low twelve,
 * plus bit 63 once it means something. Everything between is either the
 * physical address or reserved, and a caller that sets one of those has made
 * a mistake this should not propagate into a page table. */
static uint64 leaf_flag_mask(void) {
    return nx_enabled ? (0xFFFULL | PAGE_NX) : 0xFFFULL;
}

address_space_t *vmm_kernel_space(void)  { return &kernel_space; }
address_space_t *vmm_current_space(void) { return current_space; }

/* --- address space lifecycle --------------------------------------------
 * A space is a PML4 frame. Creating one means allocating that frame, zeroing
 * the user half, and copying the kernel's upper-half entries so that a
 * syscall taken in any space can still reach kernel data without a CR3
 * switch. Those entries are shared, not copied deeply - one kernel_pdpt, one
 * direct map, referenced from every PML4 - which is what makes a kernel
 * mapping made after a space is created visible in it anyway.
 *
 * This is the thing recursive mapping could not have done. Tearing down a
 * space means walking tables that belong to an address space that is NOT
 * loaded in CR3; through the direct map that is ordinary pointer chasing. */

static address_space_t space_pool[MAX_ADDRESS_SPACES];
static int space_used[MAX_ADDRESS_SPACES];

address_space_t *vmm_space_create(void) {
    phys_addr_t root;
    uint64 *table;
    uint64 i;
    int slot;

    for (slot = 0; slot < MAX_ADDRESS_SPACES; slot++) {
        if (!space_used[slot]) {
            break;
        }
    }
    if (slot == MAX_ADDRESS_SPACES) {
        return NULL;
    }

    root = pmm_alloc_frame();
    if (root == 0) {
        return NULL;
    }
    if ((root & ~ADDR_MASK) != 0 || (root & 0xFFFULL) != 0) {
        pmm_free_frame(root);
        return NULL;
    }

    table = table_at(root);
    for (i = 0; i < PT_ENTRIES; i++) {
        table[i] = 0;
    }
    /* Share every kernel slot. Copying the ENTRY, not the table beneath it -
     * so kernel_pdpt and the direct map's PDPT have one instance each and a
     * later kernel mapping needs no fixups across spaces. */
    table[KERNEL_PML4_SLOT]  = pml4[KERNEL_PML4_SLOT];
    table[PHYSMAP_PML4_SLOT] = pml4[PHYSMAP_PML4_SLOT];

    space_used[slot] = 1;
    space_pool[slot].root = root;
    space_pool[slot].ntvm = NULL;
    return &space_pool[slot];
}

/* Free every user page and every user page table in a space.
 *
 * Only the lower half is walked: the kernel slots hold entries copied from
 * the kernel's own PML4, and following them would free the kernel's page
 * tables out from under every other space. That is the single most dangerous
 * mistake available in this function, and the reason the loop bounds are
 * written as a slot check rather than as "all 512 entries". */
void (*vmm_space_destroy_hook)(address_space_t *as);

void vmm_space_destroy(address_space_t *as) {
    uint64 *pml4v;
    uint64 i4, i3, i2, i1;
    int slot;

    if (as == NULL || as == &kernel_space || as->root == 0) {
        return;
    }
    if (as == current_space) {
        return;   /* would pull the tables out from under the running code */
    }
    /* ...on ANY CPU. Every thread of the space has left its CPU before its
     * slot can be freed (process.h's oncpu), and switching away loads the
     * next thread's space, so this should never trip - which is exactly why
     * it is checked rather than assumed: the alternative is a CPU walking
     * freed page tables. */
    {
        int c;

        for (c = 0; c < smp_cpu_count(); c++) {
            if (smp_cpu(c)->cur_space == as) {
                kprintf_c(0x0C, "vmm: refusing to destroy a space cpu%d has "
                                "loaded\n", c);
                return;
            }
        }
    }

    /* Asynchronous I/O still aimed at this space has nowhere to land. */
    iomgr_space_gone(as);
    if (vmm_space_destroy_hook != NULL) {
        vmm_space_destroy_hook(as);
    }
    pml4v = table_at(as->root);

    for (i4 = 0; i4 < PT_ENTRIES; i4++) {
        uint64 *pdptv;

        if (i4 == KERNEL_PML4_SLOT || i4 == PHYSMAP_PML4_SLOT) {
            continue;                       /* shared with the kernel */
        }
        if (!(pml4v[i4] & PAGE_PRESENT)) {
            continue;
        }
        pdptv = table_at(pml4v[i4] & ADDR_MASK);

        for (i3 = 0; i3 < PT_ENTRIES; i3++) {
            uint64 *pdv;

            if (!(pdptv[i3] & PAGE_PRESENT) || (pdptv[i3] & PAGE_HUGE)) {
                continue;
            }
            pdv = table_at(pdptv[i3] & ADDR_MASK);

            for (i2 = 0; i2 < PT_ENTRIES; i2++) {
                uint64 *ptv;

                if (!(pdv[i2] & PAGE_PRESENT) || (pdv[i2] & PAGE_HUGE)) {
                    continue;
                }
                ptv = table_at(pdv[i2] & ADDR_MASK);

                for (i1 = 0; i1 < PT_ENTRIES; i1++) {
                    /* Device memory (PAGE_DEVICE) is not a frame this
                     * space owns - see paging.h. */
                    if ((ptv[i1] & PAGE_PRESENT) &&
                        !(ptv[i1] & PAGE_DEVICE)) {
                        /* No refcount, so this assumes the space held the
                         * last mapping of every frame in it. True while
                         * nothing is shared; the assumption that fork with
                         * copy-on-write breaks. */
                        pmm_free_frame(ptv[i1] & ADDR_MASK);
                    }
                }
                pmm_free_frame(pdv[i2] & ADDR_MASK);
            }
            pmm_free_frame(pdptv[i3] & ADDR_MASK);
        }
        pmm_free_frame(pml4v[i4] & ADDR_MASK);
    }

    pmm_free_frame(as->root);
    as->root = 0;

    for (slot = 0; slot < MAX_ADDRESS_SPACES; slot++) {
        if (&space_pool[slot] == as) {
            space_used[slot] = 0;
        }
    }
}

/* --- copy-on-write fork --------------------------------------------------
 *
 * Cloning a space maps the same frames into a second set of tables and takes
 * a reference to each, rather than copying any page. Both sides lose write
 * permission on anything that had it, and gain PAGE_COW to say why. The first
 * write from either side faults; vmm_handle_write_fault below hands that side
 * a private copy and leaves the other with the original.
 *
 * The tables themselves ARE copied - a page table is per-space, because its
 * entries are what diverge. Only the leaf frames are shared. That costs one
 * frame per 2MB of mapped address space per fork, which for a shell forking a
 * few pages of stack and a mapped binary is a handful of frames.
 *
 * The write bit is cleared in the SOURCE as well as the destination, and
 * missing that is the classic way to write this function wrong: the parent
 * would keep writing into pages the child can see, and the child would
 * observe the parent's variables changing underneath it. */

static int clone_page(address_space_t *dst, virt_addr_t virt,
                      uint64 *src_entry) {
    uint64      entry = *src_entry;
    phys_addr_t frame = entry & ADDR_MASK;
    uint64      flags;

    /* Device memory is shared as it stands, writable on both sides and with
     * no reference taken: there is no frame to count and nothing to copy -
     * see PAGE_DEVICE in paging.h. */
    if (entry & PAGE_DEVICE) {
        flags = (entry & (0xFFFULL | PAGE_NX)) & ~(uint64)PAGE_PRESENT;
        return vmm_map_page_in(dst, virt, frame, flags);
    }

    /* Write permission goes away on both sides, and PAGE_COW records that the
     * page is only read-only because it is shared. A page that was already
     * read-only is shared as-is: it needs a reference, but marking it COW
     * would turn a genuine protection fault into a silent copy. */
    if (entry & PAGE_RW) {
        *src_entry = (entry & ~(uint64)PAGE_RW) | PAGE_COW;
        entry      = *src_entry;
    }

    /* PAGE_NX lives at bit 63, outside the low twelve this used to take, so
     * masking with 0xFFF alone handed the child an EXECUTABLE copy of every
     * page the parent had marked non-executable - a protection that silently
     * evaporated on fork, in the child only, which is the half of the pair
     * nobody looks at. */
    flags = (entry & (0xFFFULL | PAGE_NX)) & ~(uint64)PAGE_PRESENT;

    if (!vmm_map_page_in(dst, virt, frame, flags)) {
        return 0;
    }
    pmm_ref_frame(frame);
    return 1;
}

address_space_t *vmm_space_clone(address_space_t *src) {
    address_space_t *dst;
    uint64 *src_pml4;
    uint64  i4, i3, i2, i1;

    if (src == NULL || src->root == 0) {
        return NULL;
    }
    dst = vmm_space_create();
    if (dst == NULL) {
        return NULL;
    }

    src_pml4 = table_at(src->root);

    /* Lower half only. The kernel slots were copied by vmm_space_create as
     * shared entries, and walking into them would clone the kernel's own page
     * tables - and then free them when this space is destroyed. */
    for (i4 = 0; i4 < PT_ENTRIES; i4++) {
        uint64 *pdptv;

        if (i4 == KERNEL_PML4_SLOT || i4 == PHYSMAP_PML4_SLOT) {
            continue;
        }
        if (!(src_pml4[i4] & PAGE_PRESENT)) {
            continue;
        }
        pdptv = table_at(src_pml4[i4] & ADDR_MASK);

        for (i3 = 0; i3 < PT_ENTRIES; i3++) {
            uint64 *pdv;

            if (!(pdptv[i3] & PAGE_PRESENT) || (pdptv[i3] & PAGE_HUGE)) {
                continue;
            }
            pdv = table_at(pdptv[i3] & ADDR_MASK);

            for (i2 = 0; i2 < PT_ENTRIES; i2++) {
                uint64 *ptv;

                if (!(pdv[i2] & PAGE_PRESENT) || (pdv[i2] & PAGE_HUGE)) {
                    continue;
                }
                ptv = table_at(pdv[i2] & ADDR_MASK);

                for (i1 = 0; i1 < PT_ENTRIES; i1++) {
                    virt_addr_t virt;

                    if (!(ptv[i1] & PAGE_PRESENT)) {
                        continue;
                    }
                    virt = (virt_addr_t)((i4 << 39) | (i3 << 30) |
                                         (i2 << 21) | (i1 << 12));
                    /* Rebuild the sign extension: an index of 256 or above in
                     * the PML4 names an address whose top sixteen bits are
                     * ones, and handing a non-canonical address to
                     * vmm_map_page_in gets it rejected. */
                    if (i4 >= 256) {
                        virt |= 0xFFFF000000000000ULL;
                    }

                    if (!clone_page(dst, virt, &ptv[i1])) {
                        vmm_space_destroy(dst);
                        return NULL;
                    }
                }
            }
        }
    }

    /* The source's entries changed, so anything cached from before is stale.
     * Only if the source is the space actually loaded - flushing for a space
     * that is not in CR3 would evict unrelated translations. */
    if (src == current_space) {
        flush_tlb();
    }
    /* And everywhere else it is loaded: a sibling THREAD of the forking one,
     * running on another CPU, holds writable translations for pages that
     * are copy-on-write as of now - a write through one would land in the
     * frame the child shares. */
    smp_tlb_shootdown_space(src, SMP_TLB_ALL);
    return dst;
}

/* "This CPU just handled a write fault on this page" - within the same
 * tick. A retry after a stale-TLB flush comes back in microseconds; a
 * genuine protection fault on a page resolved long ago must not be mistaken
 * for one, and the tick bound is what separates them. */
static void cow_fault_note(virt_addr_t addr) {
    struct cpu_local *c = smp_this_cpu();

    c->cow_last_addr = addr & ~0xFFFULL;
    c->cow_last_tick = timer_ticks_now();
}

static int cow_fault_repeats(virt_addr_t addr) {
    struct cpu_local *c = smp_this_cpu();

    return c->cow_last_addr == (addr & ~0xFFFULL) &&
           timer_ticks_now() - c->cow_last_tick <= 1;
}

int vmm_handle_write_fault(virt_addr_t addr, uint64 error_code) {
    uint64 *pml4v, *pdptv, *pdv, *ptv;
    uint64  entry;
    uint64  i1;
    phys_addr_t old_frame, new_frame;
    address_space_t *as = current_space;

    /* A copy-on-write fault is specifically a WRITE to a PRESENT page. A
     * not-present fault is a missing mapping and a read fault on a read-only
     * page is nonsense; neither is ours. */
    if ((error_code & 0x1) == 0 || (error_code & 0x2) == 0) {
        return 0;
    }
    if (as == NULL || !is_canonical(addr)) {
        return 0;
    }

    pml4v = table_at(as->root);
    if (!(pml4v[pml4_index(addr)] & PAGE_PRESENT)) return 0;

    pdptv = table_at(pml4v[pml4_index(addr)] & ADDR_MASK);
    if (!(pdptv[pdpt_index(addr)] & PAGE_PRESENT) ||
         (pdptv[pdpt_index(addr)] & PAGE_HUGE)) return 0;

    pdv = table_at(pdptv[pdpt_index(addr)] & ADDR_MASK);
    if (!(pdv[pd_index(addr)] & PAGE_PRESENT) ||
         (pdv[pd_index(addr)] & PAGE_HUGE)) return 0;

    ptv   = table_at(pdv[pd_index(addr)] & ADDR_MASK);
    i1    = pt_index(addr);
    entry = ptv[i1];

    if ((entry & PAGE_PRESENT) && (entry & PAGE_RW) && !(entry & PAGE_COW) &&
        !(error_code & 0x8) &&
        (!(error_code & 0x4) || (entry & PAGE_USER)) &&
        !cow_fault_repeats(addr)) {
        /* Already writable: another thread of this process, on another CPU,
         * took the same copy-on-write fault first and resolved it, and this
         * CPU's TLB still held the read-only entry (the resolver's shootdown
         * crossed with this fault). Not a protection fault - drop the stale
         * entry and let the write retry. */
        /* Once. If this CPU faults on the same page again, the flush did not
         * help and the fault is real - the guard is what keeps a genuine
         * protection fault from becoming an endless fault-and-retry. */
        cow_fault_note(addr);
        invlpg_local(addr);
        return 1;
    }
    cow_fault_note(addr);
    if (!(entry & PAGE_PRESENT) || !(entry & PAGE_COW) || (entry & PAGE_RW)) {
        smp_this_cpu()->cow_last_addr = 0;
        return 0;   /* not a shared page - a real protection fault */
    }

    old_frame = entry & ADDR_MASK;

    /* The last sharer inherits the page instead of copying it. Without this
     * every forked page is eventually duplicated even when the other side
     * exited long ago, and a shell that forks in a loop grows without bound.
     */
    if (pmm_frame_refs(old_frame) <= 1) {
        ptv[i1] = (entry | PAGE_RW) & ~(uint64)PAGE_COW;
        tlb_invalidate(as, addr);
        return 1;
    }

    new_frame = pmm_alloc_frame();
    if (new_frame == 0) {
        return 0;   /* out of memory: the caller kills the process */
    }

    {
        const uint64 *from = (const uint64 *)phys_to_virt(old_frame);
        uint64       *to   = (uint64 *)phys_to_virt(new_frame);
        uint64        i;

        for (i = 0; i < PMM_PAGE_SIZE / sizeof(uint64); i++) {
            to[i] = from[i];
        }
    }

    /* Same bit-63 point as clone_page: the private copy inherits the
     * original's flags, and NX is one of them. Dropping it here would mean a
     * page became executable at the moment it was first written to. */
    ptv[i1] = (new_frame & ADDR_MASK)
            | ((entry & (0xFFFULL | PAGE_NX)) & ~(uint64)PAGE_COW)
            | PAGE_RW | PAGE_PRESENT;
    tlb_invalidate(as, addr);

    /* One fewer sharer of the original. This never frees it - the refcount
     * was above one to get here - but it is what lets the LAST sharer take
     * the cheap path above. */
    pmm_free_frame(old_frame);
    return 1;
}

void vmm_switch_to(address_space_t *as) {
    if (as == NULL || as->root == 0 || as == current_space) {
        return;
    }
    current_space = as;
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64)as->root) : "memory");
}

/* --- walking ------------------------------------------------------------ */

/* Make table[index] present, allocating and zeroing a frame if it is not, and
 * hand back a pointer to the level below.
 *
 * The frame is zeroed BEFORE it is installed now. Under recursive mapping it
 * could not be: a fresh frame was unreachable until its parent entry made it
 * so, which forced an install-then-zero order and a window in which a live
 * entry pointed at uninitialised memory. Through the direct map every frame
 * is already addressable, so the ordering hazard is gone. */
static int ensure_table(uint64 *table, uint64 index, uint64 flags, uint64 **out_child) {
    uint64 entry = table[index];
    phys_addr_t phys;
    uint64 *child;
    uint64 i;

    if (entry & PAGE_PRESENT) {
        if (entry & PAGE_HUGE) {
            return 0;   /* a large page occupies this range; refuse rather
                         * than silently corrupt it by descending */
        }
        /* Permissions are ANDed across all four levels, so a kernel-only
         * parent blocks a user page beneath it. Raising USER on a parent can
         * invalidate cached permissions for anything already mapped below it,
         * and there is no way to name that range to invlpg - so flush. Rare
         * enough not to matter. */
        if ((flags & PAGE_USER) && !(entry & PAGE_USER)) {
            table[index] = entry | PAGE_USER;
            flush_tlb();
        }
        *out_child = table_at(entry & ADDR_MASK);
        return 1;
    }

    phys = pmm_alloc_frame();
    if (phys == 0) {
        return 0;
    }
    /* A frame with bits above 51, or an unaligned one, makes an entry with a
     * reserved bit set - which faults as err bit 3 at whatever address the
     * entry claimed, with nothing pointing back here. */
    if ((phys & ~ADDR_MASK) != 0 || (phys & 0xFFFULL) != 0) {
        pmm_free_frame(phys);
        return 0;
    }

    child = table_at(phys);
    for (i = 0; i < PT_ENTRIES; i++) {
        child[i] = 0;
    }

    table[index] = (phys & ADDR_MASK) | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
    *out_child = child;
    return 1;
}

/* Does a change to `virt_addr` in `as` need this CPU's TLB flushed?
 *
 * The obvious answer - "only if `as` is the space that is loaded" - is what
 * this used to do, and it is wrong for the KERNEL HALF.
 *
 * Every address space shares the kernel's PML4 slots: they hold the same
 * physical tables, not copies. So a mapping edited through vmm_kernel_space()
 * is immediately visible in the loaded space too, whatever that is - and the
 * TLB entry for it is live regardless of which address-space OBJECT the
 * caller named.
 *
 * kstack_alloc and kstack_free are exactly this case: they map and unmap
 * through vmm_kernel_space() while a USER process is current. With the old
 * test they invalidated nothing, so:
 *
 *   1. a kernel stack slot is freed and its frames returned;
 *   2. the slot is reallocated and mapped to DIFFERENT frames - no invlpg;
 *   3. kstack_alloc zeroes it, and thread_bootstrap_stack lays out the new
 *      thread's frame - both writing through the STALE entry, into the OLD
 *      frames;
 *   4. the next CR3 load flushes the TLB, and the thread is switched to -
 *      reading the new frames, which nothing ever wrote.
 *
 * switch_context then pops six zeroes and returns to address 0. That is the
 * fault this fixes: RIP 0, a kernel stack full of zeroes, and no backtrace
 * because the frame chain is zeroed too. It needed a vfork child to exit,
 * be reaped, and the parent to vfork again reusing the same slot - which is
 * ordinary shell behaviour and is why it eventually showed up.
 */
/* (needs_local_flush answered "does THIS CPU need to flush" and is
 * subsumed by tlb_invalidate, which answers "which CPUs do", above.) */
int vmm_map_page_in(address_space_t *as, virt_addr_t virt_addr,
                    phys_addr_t phys_addr, uint64 flags) {
    uint64 *pml4v, *pdptv, *pdv, *ptv;

    if (as == NULL || !is_canonical(virt_addr)) {
        return 0;
    }
    if ((phys_addr & ~ADDR_MASK) != 0 || (phys_addr & 0xFFFULL) != 0) {
        return 0;
    }

    pml4v = table_at(as->root);

    /* The intermediate levels are built from PAGE_USER alone; nothing else in
     * `flags` describes them. PAGE_NX especially must not reach them - on a
     * parent it means "nothing under this is executable", which would take
     * out the image's text along with the data page being mapped. */
    if (!ensure_table(pml4v, pml4_index(virt_addr), flags, &pdptv)) return 0;
    if (!ensure_table(pdptv, pdpt_index(virt_addr), flags, &pdv))   return 0;
    if (!ensure_table(pdv,   pd_index(virt_addr),   flags, &ptv))   return 0;

    ptv[pt_index(virt_addr)] =
        (phys_addr & ADDR_MASK) | (flags & leaf_flag_mask()) | PAGE_PRESENT;

    /* invlpg acts on whatever is loaded, so flushing for a user space that
     * is NOT loaded would evict an unrelated translation - but the kernel
     * half is shared and must always be flushed. See needs_local_flush. */
    tlb_invalidate(as, virt_addr);
    return 1;
}

/* Free a table frame and clear the entry that pointed at it. */
static void reclaim(uint64 *parent, uint64 index) {
    phys_addr_t phys = parent[index] & ADDR_MASK;
    parent[index] = 0;
    pmm_free_frame(phys);
}

void vmm_unmap_page_in(address_space_t *as, virt_addr_t virt_addr, int free_frame) {
    uint64 *pml4v, *pdptv, *pdv, *ptv;
    uint64 i4, i3, i2, i1;
    phys_addr_t frame;

    if (as == NULL || !is_canonical(virt_addr)) {
        return;
    }
    i4 = pml4_index(virt_addr);
    i3 = pdpt_index(virt_addr);
    i2 = pd_index(virt_addr);
    i1 = pt_index(virt_addr);

    pml4v = table_at(as->root);
    if (!(pml4v[i4] & PAGE_PRESENT)) return;

    pdptv = table_at(pml4v[i4] & ADDR_MASK);
    if (!(pdptv[i3] & PAGE_PRESENT) || (pdptv[i3] & PAGE_HUGE)) return;

    pdv = table_at(pdptv[i3] & ADDR_MASK);
    if (!(pdv[i2] & PAGE_PRESENT) || (pdv[i2] & PAGE_HUGE)) return;

    ptv = table_at(pdv[i2] & ADDR_MASK);
    if (!(ptv[i1] & PAGE_PRESENT)) return;

    frame = ptv[i1] & ADDR_MASK;
    if (ptv[i1] & PAGE_DEVICE) {
        free_frame = 0;                 /* not ours - see paging.h */
    }
    ptv[i1] = 0;
    tlb_invalidate(as, virt_addr);
    if (free_frame) {
        pmm_free_frame(frame);
    }

    /* Reclaim tables the removal emptied. Under recursive mapping this was
     * left undone - three levels can empty at once and the bookkeeping was
     * awkward - which meant a process that mapped and unmapped across a wide
     * address range leaked a frame per 2MB touched, permanently.
     *
     * Never for the kernel's own slots: those point at static arrays in .bss
     * and at the direct map's bootstrap frames, neither of which came from
     * the PMM in a form it can take back, and both of which every other
     * address space shares. */
    if (i4 == KERNEL_PML4_SLOT || i4 == PHYSMAP_PML4_SLOT) {
        return;
    }
    if (!table_is_empty(ptv)) return;
    reclaim(pdv, i2);

    if (!table_is_empty(pdv)) return;
    reclaim(pdptv, i3);

    if (!table_is_empty(pdptv)) return;
    reclaim(pml4v, i4);
}

/* The leaf entry's flags. See paging.h for why only the leaf's.
 *
 * Deliberately a separate walk from vmm_get_phys_in rather than a shared one
 * returning both: that function has three exits for huge pages that
 * synthesise an address out of two fields, and threading a flags output
 * through them would make the common case carry the rare one. This one has no
 * huge-page case because it answers 0 for anything it cannot describe as a
 * 4KB leaf - which is the honest answer to "what are this page's flags" when
 * the mapping is not a page. */
uint64 vmm_get_flags_in(address_space_t *as, virt_addr_t virt_addr) {
    uint64 *pml4v, *pdptv, *pdv, *ptv;
    uint64 entry;

    if (as == NULL || !is_canonical(virt_addr)) {
        return 0;
    }
    pml4v = table_at(as->root);
    entry = pml4v[pml4_index(virt_addr)];
    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }
    pdptv = table_at(entry & ADDR_MASK);
    entry = pdptv[pdpt_index(virt_addr)];
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_HUGE)) {
        return 0;
    }
    pdv = table_at(entry & ADDR_MASK);
    entry = pdv[pd_index(virt_addr)];
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_HUGE)) {
        return 0;
    }
    ptv = table_at(entry & ADDR_MASK);
    entry = ptv[pt_index(virt_addr)];
    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }
    return entry & leaf_flag_mask();
}

phys_addr_t vmm_get_phys_in(address_space_t *as, virt_addr_t virt_addr) {
    uint64 *pml4v, *pdptv, *pdv, *ptv;
    uint64 entry;

    if (as == NULL || !is_canonical(virt_addr)) {
        return 0;
    }

    pml4v = table_at(as->root);
    entry = pml4v[pml4_index(virt_addr)];
    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }

    pdptv = table_at(entry & ADDR_MASK);
    entry = pdptv[pdpt_index(virt_addr)];
    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }
    if (entry & PAGE_HUGE) {            /* 1GB page */
        return (phys_addr_t)((entry & 0x000FFFFFC0000000ULL)
                             | (virt_addr & 0x3FFFFFFFULL));
    }

    pdv = table_at(entry & ADDR_MASK);
    entry = pdv[pd_index(virt_addr)];
    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }
    if (entry & PAGE_HUGE) {            /* 2MB page */
        return (phys_addr_t)((entry & 0x000FFFFFFFE00000ULL)
                             | (virt_addr & 0x1FFFFFULL));
    }

    ptv = table_at(entry & ADDR_MASK);
    entry = ptv[pt_index(virt_addr)];
    if (!(entry & PAGE_PRESENT)) {
        return 0;
    }
    return (phys_addr_t)((entry & ADDR_MASK) | (virt_addr & 0xFFFULL));
}

phys_addr_t vmm_alloc_page_in(address_space_t *as, virt_addr_t virt_addr,
                              uint64 flags) {
    phys_addr_t phys = pmm_alloc_frame();

    if (phys == 0) {
        return 0;
    }
    if ((phys & ~ADDR_MASK) != 0 || (phys & 0xFFFULL) != 0) {
        pmm_free_frame(phys);
        return 0;
    }
    /* The mapping can still fail - a table below it may be missing and
     * unallocatable. Returning phys anyway tells the caller it owns a page it
     * cannot touch, which is how an out-of-memory condition used to surface
     * as a page fault inside the caller's own memset. */
    if (!vmm_map_page_in(as, virt_addr, phys, flags)) {
        pmm_free_frame(phys);
        return 0;
    }
    return phys;
}

/* --- current-space wrappers --------------------------------------------- */

int vmm_map_page(virt_addr_t v, phys_addr_t p, uint64 f) {
    return vmm_map_page_in(current_space, v, p, f);
}

void vmm_unmap_page(virt_addr_t v) {
    vmm_unmap_page_in(current_space, v, VMM_KEEP_FRAME);
}

void vmm_unmap_page_free(virt_addr_t v) {
    vmm_unmap_page_in(current_space, v, VMM_FREE_FRAME);
}

phys_addr_t vmm_get_phys(virt_addr_t v) {
    return vmm_get_phys_in(current_space, v);
}

phys_addr_t vmm_alloc_page(virt_addr_t v, uint64 f) {
    return vmm_alloc_page_in(current_space, v, f);
}
