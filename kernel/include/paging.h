#ifndef PAGING_H
#define PAGING_H

#include "e820.h"
#include "typesk.h"

#define PAGE_PRESENT  0x1
#define PAGE_RW       0x2
#define PAGE_USER     0x4

/* Bits 9, 10 and 11 of a page table entry are ignored by the CPU and
 * available to software. Bit 9 marks a page that is shared copy-on-write: it
 * is mapped read-only in two or more spaces, and a write fault on it means
 * "give the writer a private copy", not "kill the process".
 *
 * The distinction has to live in the entry rather than in a side table
 * because the fault handler has nothing else to go on: a genuinely read-only
 * mapping and a copy-on-write one produce byte-identical faults. */
#define PAGE_COW      0x200

/* No-execute. Bit 63 of a leaf entry: an instruction fetch from the page
 * faults with error bit 4 set, whatever the read and write permissions say.
 *
 * Two things make this different from every other flag here. It is the only
 * one outside the low twelve bits, so it does not survive the `entry & 0xFFF`
 * that the copy-on-write paths use to carry flags from an entry to a new
 * mapping - both of those mask it in explicitly. And it is only a permission
 * bit once EFER.NXE is set; before that it is RESERVED, and a reserved bit in
 * a present entry faults on every access rather than on a fetch. So
 * vmm_map_page_in strips it when NX is not enabled, and no caller has to know
 * whether it is.
 *
 * Only ever set on a LEAF entry. Setting it on a PML4, PDPT or PD entry makes
 * the whole subtree non-executable, which is a different and much broader
 * statement than the one the caller is making about one page. */
#define PAGE_NX       0x8000000000000000ULL

/* Base of the kernel half. Must match KERNEL_VMA in linker.ld - the two are
 * a matched pair, and nothing checks that for you. */
#define KERNEL_VMA    0xFFFFFFFF80000000ULL   /* top 2GB: -mcmodel=kernel */

/* How much physical memory the kernel window at KERNEL_VMA covers.
 *
 * This used to have to be at least as large as all of RAM, because it was the
 * only way the kernel could reach an arbitrary frame. It is not any more - the
 * direct map below does that job - so this only has to cover the kernel image,
 * the VGA text buffer at 0xB8000, and the handful of frames used to bootstrap
 * the direct map itself.
 *
 * It is ALSO the kernel image's size limit, and that is new. The image is
 * loaded at KERNEL_LMA = 0x100000 (see linker.ld) and linked at KERNEL_VMA +
 * KERNEL_LMA, so every byte of it has to fall inside this window or the code
 * past the end of it is simply not mapped. Before the load address moved
 * above 1MB the binding constraint was elsewhere - the image grew toward
 * boot.asm's 32-bit stack at 0x90000 - and build.py carried a hand-raised
 * MAX_IMG_SIZE to catch it. build.py now reads THIS constant instead, so the
 * limit lives in one place and raising it is this one edit.
 *
 * 16MB, against a kernel currently under 600KB. The cost is .bss only - eight
 * page tables, 32KB, none of it in the boot image - which is why this is set
 * far past what is needed rather than trimmed to it. */
#define KERNEL_MAP_SIZE 0x01000000ULL

/* --- the direct map -------------------------------------------------------
 * Every byte of physical RAM, mapped once at a fixed offset, using 2MB pages,
 * built at boot from the E820 map. PML4 slot 508, so 512GB of window.
 *
 * This is what replaced the recursive mapping. Recursive mapping is elegant
 * and costs nothing, but it can only ever expose the tables of the address
 * space you are currently executing in - the whole trick is making the CPU
 * walk CR3's own hierarchy. That is a hard ceiling the moment a second
 * address space exists, because editing it then means switching CR3 to it or
 * temporarily mapping it somewhere. With a direct map, any table at any level
 * of any address space is just phys_to_virt(entry & ADDR_MASK).
 *
 * Cost is one PD frame per gigabyte of RAM, plus one PDPT. Under 5 frames for
 * a 4GB machine, versus 512GB of address space for the recursive slot. */
#define PHYSMAP_PML4_SLOT  508ULL
#define PHYSMAP_BASE       0xFFFFFE0000000000ULL

/* virt_to_phys is for KERNEL IMAGE SYMBOLS ONLY - things linked at KERNEL_VMA
 * whose VMA/LMA delta linker.ld guarantees. It is not the inverse of
 * phys_to_virt and never was: a heap pointer, an MMIO address or a page
 * mapped by vmm_map_page has no fixed offset relationship to its frame. Use
 * vmm_get_phys() for those.
 *
 * phys_to_virt goes through the direct map, so it resolves for ANY physical
 * address in RAM - but only once paging_init() has built it. Nothing may call
 * it before then; the kernel window is the only thing available that early,
 * and code needing it says KERNEL_VMA + phys explicitly. */
static inline phys_addr_t virt_to_phys(virt_addr_t virt) {
    return (phys_addr_t)(virt - KERNEL_VMA);
}
static inline virt_addr_t phys_to_virt(phys_addr_t phys) {
    return (virt_addr_t)(PHYSMAP_BASE + phys);
}

/* --- address spaces -------------------------------------------------------
 * A page table hierarchy and nothing else. There is no process concept here
 * on purpose: this is the object a process will eventually own, not the
 * process itself.
 *
 * Everything used to operate implicitly on whatever was in CR3, which is
 * indistinguishable from correct while exactly one address space exists and
 * completely wrong the moment a second one does. Making it a parameter now
 * means fork/exec is a matter of filling in vmm_space_create() rather than
 * revisiting every mapping call site. */
typedef struct address_space {
    phys_addr_t root;   /* physical address of this space's PML4 */
} address_space_t;

/* Statically pooled rather than heap allocated: the count is small, the
 * lifetime is the process's, and a fixed pool means a leak shows up as
 * "cannot create process" rather than as heap fragmentation. */
#define MAX_ADDRESS_SPACES 32

/* A new space with the kernel half shared and the user half empty, or NULL if
 * the pool or memory is exhausted. */
address_space_t *vmm_space_create(void);

/* A new space mapping everything `src` maps, copy-on-write.
 *
 * No user page is copied. Every writable user page becomes read-only and
 * PAGE_COW in BOTH spaces, and its frame gains a reference; the first write
 * from either side faults and vmm_handle_write_fault hands that side a
 * private copy. Read-only pages are shared as they are, with a reference
 * taken but no COW bit - a write to one of those is a real fault and stays
 * one.
 *
 * Returns NULL if the pool, the frame allocator, or an intervening page table
 * ran out, having freed whatever it had built so far. The source is left
 * usable in that case: its pages may be marked copy-on-write with a single
 * reference, which the fault handler resolves by simply restoring write
 * permission. */
address_space_t *vmm_space_clone(address_space_t *src);

/* Try to resolve a write fault at `addr` in the current space.
 *
 * Returns 1 if the fault was a copy-on-write fault and has been resolved -
 * the faulting instruction can be retried and will now succeed. Returns 0 if
 * this was not a copy-on-write fault, which leaves it to the caller to decide
 * between a kernel panic and killing the process. */
int vmm_handle_write_fault(virt_addr_t addr, uint64 error_code);

/* Free every user page and user page table. Refuses to touch the kernel space
 * or the currently loaded one - destroying the tables you are executing on
 * faults on the next instruction, with no output. */
void vmm_space_destroy(address_space_t *as);

/* Load a space into CR3. */
void vmm_switch_to(address_space_t *as);

/* Builds the kernel window and the direct map, then loads CR3. Call after
 * pmm_init() - the direct map's own tables are allocated from the PMM. */
void paging_init(const e820_map_t *map);

/* Non-zero if the direct map was built successfully. If this is 0 the kernel
 * cannot reach page tables at all and nothing below will work - it is a hard
 * failure, reported rather than silently limped past. */
int paging_physmap_ready(void);

/* Turn on no-execute enforcement, if the CPU has it. Returns 1 if PAGE_NX
 * means anything from here on, 0 if it will be stripped from every mapping.
 *
 * Call once, at boot, after paging_init and before the first user image is
 * loaded. Ordering matters in one direction only: EFER.NXE may be set while
 * bit 63 is clear everywhere (which it is, until a loader asks for it), but a
 * mapping made with PAGE_NX before NXE is on is a reserved-bit fault waiting
 * for its first access. */
int paging_enable_nx(void);

/* Whether the above succeeded. Callers do not need this to decide what flags
 * to pass - PAGE_NX is safe to request unconditionally - it exists so the
 * boot banner can say which of the two the machine got. */
int paging_nx_enabled(void);

/* The kernel's address space. Kernel mappings made here are shared by every
 * space created later, because every PML4 gets the same upper-half entries. */
address_space_t *vmm_kernel_space(void);

/* Whatever CR3 currently holds. Only ever the kernel space today. */
address_space_t *vmm_current_space(void);

/* Map one page. Returns 1 on success, 0 if it could not be done - no memory
 * for an intervening table, a non-canonical address, or a large page in the
 * way. A 0 return means NOTHING was changed. Do not ignore it: the failure
 * otherwise resurfaces as a page fault at the caller's first access, in
 * whatever context that happens to be. */
int vmm_map_page_in(address_space_t *as, virt_addr_t virt_addr,
                    phys_addr_t phys_addr, uint64 flags);

/* Remove a mapping. If free_frame is non-zero the underlying frame goes back
 * to the PMM - only correct if this was its last mapping, which is what the
 * absence of refcounting in pmm.c currently forces. Page tables left empty by
 * the removal are reclaimed, except in the kernel's own PML4 slots. */
#define VMM_KEEP_FRAME 0
#define VMM_FREE_FRAME 1
void vmm_unmap_page_in(address_space_t *as, virt_addr_t virt_addr, int free_frame);

/* Physical address behind a virtual one, or 0 if unmapped. */
phys_addr_t vmm_get_phys_in(address_space_t *as, virt_addr_t virt_addr);

/* The LEAF entry's flag bits for `virt_addr`, or 0 if nothing is mapped.
 *
 * Only the leaf's own bits, which is what a caller checking a protection
 * wants: permissions are ANDed down the four levels, so an intermediate entry
 * can be more permissive than the page and never less. Anything asserting
 * "this page is not writable" is asking about the leaf.
 *
 * Exists so a protection can be CHECKED rather than assumed. kldload.c seals
 * module text read-execute and its data no-execute, and the only way to test
 * that without faulting the machine on purpose is to read the mapping back. */
uint64 vmm_get_flags_in(address_space_t *as, virt_addr_t virt_addr);

/* Allocate a frame and map it in one call. Returns the frame, or 0 on
 * failure - and on failure the frame is returned to the PMM rather than
 * leaked, and no partial mapping is left behind. */
phys_addr_t vmm_alloc_page_in(address_space_t *as, virt_addr_t virt_addr,
                              uint64 flags);

/* --- current-space convenience wrappers -----------------------------------
 * Identical to the above against vmm_current_space(). Almost every caller
 * wants this; the explicit form exists for the code that will not. */
int         vmm_map_page(virt_addr_t virt_addr, phys_addr_t phys_addr, uint64 flags);
void        vmm_unmap_page(virt_addr_t virt_addr);
void        vmm_unmap_page_free(virt_addr_t virt_addr);
phys_addr_t vmm_get_phys(virt_addr_t virt_addr);
phys_addr_t vmm_alloc_page(virt_addr_t virt_addr, uint64 flags);

#endif
