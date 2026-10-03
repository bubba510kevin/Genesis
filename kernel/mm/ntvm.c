#include "kheap.h"
#include "nt.h"
#include "ntvm.h"
#include "paging.h"
#include "object.h"
#include "pmm.h"
#include "section.h"
#include "typesk.h"

/* See ntvm.h. Everything runs under the big kernel lock. */

#define PAGE 0x1000ULL

typedef struct {
    uint64 start;            /* page aligned                                 */
    uint64 pages;
    uint64 alloc_base;       /* the reservation this run belongs to          */
    uint64 alloc_size;       /* that reservation's size, for MEM_RELEASE     */
    uint32 alloc_protect;
    uint32 protect;          /* PAGE_* without PAGE_GUARD; 0 when reserved   */
    uint8  committed;
    uint8  guard;
    uint32 type;             /* MEM_PRIVATE, or MEM_MAPPED for a view        */
    object_t *section;       /* a view's section (one reference per view)    */
    uint64 view_offset;      /* where the view's first page is in it          */
} run_t;

typedef struct {
    run_t *runs;
    int    n, cap;
} ntvm_t;

static ntvm_t *vm_of(address_space_t *as, int create);

static void destroy_hook(address_space_t *as) {
    ntvm_destroy(as);
}

static ntvm_t *vm_of(address_space_t *as, int create) {
    ntvm_t *vm;

    if (as->ntvm != NULL || !create) {
        return as->ntvm;
    }
    vm = kmalloc(sizeof(*vm));
    if (vm == NULL) {
        return NULL;
    }
    vm->runs = NULL;
    vm->n = vm->cap = 0;
    as->ntvm = vm;
    vmm_space_destroy_hook = destroy_hook;
    return vm;
}

void ntvm_destroy(address_space_t *as) {
    ntvm_t *vm = as != NULL ? as->ntvm : NULL;

    if (vm == NULL) {
        return;
    }
    /* Each view holds one reference on its section: dropped here, once per
     * view (at the run that starts it), before the space's frames go. */
    for (int i = 0; i < vm->n; i++) {
        if (vm->runs[i].type == MEM_MAPPED &&
            vm->runs[i].start == vm->runs[i].alloc_base) {
            ob_deref(vm->runs[i].section);
        }
    }
    if (vm->runs != NULL) {
        kfree(vm->runs);
    }
    kfree(vm);
    as->ntvm = NULL;
}

/* --- the run list ----------------------------------------------------------- */

static uint64 run_end(const run_t *r) {
    return r->start + r->pages * PAGE;
}

static int find(const ntvm_t *vm, uint64 addr) {
    int i;

    for (i = 0; vm != NULL && i < vm->n; i++) {
        if (addr >= vm->runs[i].start && addr < run_end(&vm->runs[i])) {
            return i;
        }
    }
    return -1;
}

static int grow(ntvm_t *vm, int extra) {
    run_t *n;
    int cap, i;

    if (vm->n + extra <= vm->cap) {
        return 1;
    }
    cap = vm->cap == 0 ? 16 : vm->cap * 2;
    while (cap < vm->n + extra) {
        cap *= 2;
    }
    n = kmalloc(sizeof(run_t) * (uint64)cap);
    if (n == NULL) {
        return 0;
    }
    for (i = 0; i < vm->n; i++) {
        n[i] = vm->runs[i];
    }
    if (vm->runs != NULL) {
        kfree(vm->runs);
    }
    vm->runs = n;
    vm->cap = cap;
    return 1;
}

static int insert_at(ntvm_t *vm, int at, const run_t *r) {
    int i;

    if (!grow(vm, 1)) {
        return 0;
    }
    for (i = vm->n; i > at; i--) {
        vm->runs[i] = vm->runs[i - 1];
    }
    vm->runs[at] = *r;
    vm->n++;
    return 1;
}

static void remove_at(ntvm_t *vm, int at) {
    int i;

    for (i = at; i + 1 < vm->n; i++) {
        vm->runs[i] = vm->runs[i + 1];
    }
    vm->n--;
}

/* Make `addr` a run boundary. 0 only when out of memory. */
static int split_at(ntvm_t *vm, uint64 addr) {
    int i = find(vm, addr);
    run_t tail;

    if (i < 0 || vm->runs[i].start == addr) {
        return 1;
    }
    tail = vm->runs[i];
    tail.start = addr;
    tail.pages = (run_end(&vm->runs[i]) - addr) / PAGE;
    if (!insert_at(vm, i + 1, &tail)) {
        return 0;
    }
    vm->runs[i].pages -= tail.pages;
    return 1;
}

static int same(const run_t *a, const run_t *b) {
    return run_end(a) == b->start && a->alloc_base == b->alloc_base &&
           a->committed == b->committed && a->protect == b->protect &&
           a->guard == b->guard && a->type == b->type &&
           a->section == b->section;
}

static void coalesce(ntvm_t *vm) {
    int i = 0;

    while (i + 1 < vm->n) {
        if (same(&vm->runs[i], &vm->runs[i + 1])) {
            vm->runs[i].pages += vm->runs[i + 1].pages;
            remove_at(vm, i + 1);
        } else {
            i++;
        }
    }
}

/* Is [start, end) wholly inside ONE allocation, every page accounted for?
 * Returns that allocation's base, or 0. */
static uint64 one_allocation(const ntvm_t *vm, uint64 start, uint64 end) {
    uint64 at = start, base;
    int i = find(vm, start);

    if (i < 0) {
        return 0;
    }
    base = vm->runs[i].alloc_base;
    while (at < end) {
        if (i >= vm->n || (vm->runs[i].start != at && at != start) ||
            vm->runs[i].alloc_base != base) {
            return 0;
        }
        at = run_end(&vm->runs[i]);
        i++;
    }
    return base;
}

/* --- protections -------------------------------------------------------------- */

static int valid_protect(uint32 p, int allow_noaccess_guard) {
    uint32 base = p & 0xFFu;

    if (p & ~(0xFFu | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) {
        return 0;
    }
    if (base == 0 || (base & (base - 1)) != 0) {
        return 0;                       /* exactly one base protection */
    }
    if ((p & PAGE_GUARD) && base == PAGE_NOACCESS && !allow_noaccess_guard) {
        return 0;
    }
    if ((p & PAGE_NOCACHE) && (p & PAGE_WRITECOMBINE)) {
        return 0;
    }
    return 1;
}

/* The PTE for a committed page. NOACCESS and guard pages stay present for
 * the kernel and invisible to ring 3 - see ntvm.h. */
static uint64 pte_flags(uint32 protect, int guard);

/* ...for a page of a VIEW: the same, except that WRITECOPY means what it
 * says. The section's frame is mapped read-only with PAGE_COW, and the
 * first write takes a private copy through the ordinary copy-on-write fault
 * (vmm_handle_write_fault) - the frame has the section's reference as well
 * as this one, so it is always shared and always copied. */
static uint64 view_pte_flags(uint32 protect, int guard) {
    uint64 f = pte_flags(protect, guard);

    if ((protect & 0xFFu) == PAGE_WRITECOPY ||
        (protect & 0xFFu) == PAGE_EXECUTE_WRITECOPY) {
        f = (f & ~(uint64)PAGE_RW) | PAGE_COW;
    }
    return f;
}

static uint64 pte_flags(uint32 protect, int guard) {
    uint64 f = PAGE_PRESENT;

    switch (protect & 0xFFu) {
    case PAGE_READONLY:
        f |= PAGE_USER | PAGE_NX;
        break;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
        f |= PAGE_USER | PAGE_RW | PAGE_NX;
        break;
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
        f |= PAGE_USER;
        break;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        f |= PAGE_USER | PAGE_RW;
        break;
    default:                            /* PAGE_NOACCESS */
        f |= PAGE_NX;
        break;
    }
    if (guard) {
        f &= ~(uint64)PAGE_USER;
    }
    return f;
}

/* What a mapped page that ntvm did not make reports as its protection. */
static uint32 protect_of_pte(uint64 flags) {
    int x = (flags & PAGE_NX) == 0;
    int w = (flags & PAGE_RW) != 0;

    if (!(flags & PAGE_USER)) {
        return PAGE_NOACCESS;
    }
    if (x) {
        return w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    }
    return w ? PAGE_READWRITE : PAGE_READONLY;
}

/* Can a view of `section` have `protect`? Writing needs a writable
 * section, executing an executable one; copy-on-write needs only read. */
static int view_protect_ok(object_t *section, uint32 protect) {
    uint64 size;
    uint32 sp, p = protect & 0xFFu;
    int s_write, s_exec;

    if (section_info(section, &size, &sp) != 0) {
        return 0;
    }
    s_write = sp == PAGE_READWRITE || sp == PAGE_EXECUTE_READWRITE;
    s_exec  = sp == PAGE_EXECUTE_READ || sp == PAGE_EXECUTE_READWRITE ||
              sp == PAGE_EXECUTE_WRITECOPY;
    if ((p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE) && !s_write) {
        return 0;
    }
    if ((p & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
              PAGE_EXECUTE_WRITECOPY)) && !s_exec) {
        return 0;
    }
    return 1;
}

/* --- commit and decommit ------------------------------------------------------ */

static void unmap_range(address_space_t *as, uint64 start, uint64 pages) {
    uint64 k;

    for (k = 0; k < pages; k++) {
        if (vmm_get_phys_in(as, start + k * PAGE) != 0) {
            vmm_unmap_page_in(as, start + k * PAGE, VMM_FREE_FRAME);
        }
    }
}

/* Back [start, start + pages) with fresh zeroed frames. All or nothing. */
static int back_range(address_space_t *as, uint64 start, uint64 pages,
                      uint64 flags) {
    uint64 k, b;

    for (k = 0; k < pages; k++) {
        phys_addr_t f = pmm_alloc_frame();
        uint8 *z;

        if (f == 0) {
            unmap_range(as, start, k);
            return 0;
        }
        z = (uint8 *)phys_to_virt(f);
        for (b = 0; b < PAGE; b++) {
            z[b] = 0;
        }
        if (!vmm_map_page_in(as, start + k * PAGE, f, flags)) {
            pmm_free_frame(f);
            unmap_range(as, start, k);
            return 0;
        }
    }
    return 1;
}

/* Commit every RESERVED page of [start, end) - which lies in one allocation
 * - with `protect`. Pages already committed keep their protection, as on
 * NT. */
static uint32 commit_range(address_space_t *as, ntvm_t *vm, uint64 start,
                           uint64 end, uint32 protect) {
    uint64 need = 0;
    int i;

    for (i = 0; i < vm->n; i++) {
        run_t *r = &vm->runs[i];
        uint64 s = r->start > start ? r->start : start;
        uint64 e = run_end(r) < end ? run_end(r) : end;

        if (s < e && !r->committed) {
            need += (e - s) / PAGE;
        }
    }
    /* Refused up front rather than half done: a commit that runs the
     * machine out of frames part way leaves nothing behind. 64 frames are
     * kept back for the kernel's own page tables and heap. */
    if (need + 64 > pmm_free_frames()) {
        return STATUS_NO_MEMORY;
    }
    if (!split_at(vm, start) || !split_at(vm, end)) {
        return STATUS_NO_MEMORY;
    }
    for (i = 0; i < vm->n; i++) {
        run_t *r = &vm->runs[i];

        if (r->start < start || run_end(r) > end || r->committed) {
            continue;
        }
        if (!back_range(as, r->start, r->pages,
                        pte_flags(protect, (protect & PAGE_GUARD) != 0))) {
            coalesce(vm);
            return STATUS_NO_MEMORY;
        }
        r->committed = 1;
        r->protect   = protect & ~PAGE_GUARD;
        r->guard     = (protect & PAGE_GUARD) != 0;
    }
    coalesce(vm);
    return STATUS_SUCCESS;
}

/* --- placing a new allocation --------------------------------------------------- */

static int range_free(address_space_t *as, const ntvm_t *vm, uint64 start,
                      uint64 end) {
    uint64 a;
    int i;

    for (i = 0; vm != NULL && i < vm->n; i++) {
        if (vm->runs[i].start < end && run_end(&vm->runs[i]) > start) {
            return 0;
        }
    }
    /* Something the kernel placed there itself (an image, a TEB, a stack)
     * is not in the run list; it is in the page tables. Checked page by
     * page, bounded so a huge reservation at a chosen base does not turn
     * into millions of walks: past 64MB only the first and last pages are
     * looked at - nothing the kernel places is that large and unaligned. */
    if (end - start <= 0x4000000ULL) {
        for (a = start; a < end; a += PAGE) {
            if (vmm_get_phys_in(as, a) != 0) {
                return 0;
            }
        }
        return 1;
    }
    return vmm_get_phys_in(as, start) == 0 &&
           vmm_get_phys_in(as, end - PAGE) == 0;
}

/* First fit, bottom up or top down, in [NTVM_BASE, NTVM_LIMIT). */
static uint64 place(address_space_t *as, const ntvm_t *vm, uint64 size,
                    int top_down) {
    uint64 at;

    if (!top_down) {
        for (at = NTVM_BASE; at + size <= NTVM_LIMIT; ) {
            int i, moved = 0;

            for (i = 0; vm != NULL && i < vm->n; i++) {
                if (vm->runs[i].start < at + size && run_end(&vm->runs[i]) > at) {
                    at = (run_end(&vm->runs[i]) + NTVM_GRANULARITY - 1) &
                         ~(NTVM_GRANULARITY - 1);
                    moved = 1;
                    break;
                }
            }
            if (!moved) {
                return range_free(as, vm, at, at + size) ? at : 0;
            }
        }
        return 0;
    }
    for (at = (NTVM_LIMIT - size) & ~(NTVM_GRANULARITY - 1); at >= NTVM_BASE; ) {
        int i, moved = 0;

        for (i = 0; vm != NULL && i < vm->n; i++) {
            if (vm->runs[i].start < at + size && run_end(&vm->runs[i]) > at) {
                if (vm->runs[i].start < NTVM_BASE + size) {
                    return 0;
                }
                at = (vm->runs[i].start - size) & ~(NTVM_GRANULARITY - 1);
                moved = 1;
                break;
            }
        }
        if (!moved) {
            return range_free(as, vm, at, at + size) ? at : 0;
        }
    }
    return 0;
}

/* --- the four calls -------------------------------------------------------------- */

uint32 ntvm_allocate(address_space_t *as, uint64 *base, uint64 *size,
                     uint32 type, uint32 protect) {
    ntvm_t *vm;
    uint64 start, end;
    uint32 st;
    int i;

    if (*size == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (type & ~(MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN)) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((type & (MEM_COMMIT | MEM_RESERVE | MEM_RESET)) == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((type & MEM_RESET) && (type & (MEM_COMMIT | MEM_RESERVE))) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!(type & MEM_RESET) && !valid_protect(protect, 0)) {
        return STATUS_INVALID_PAGE_PROTECTION;
    }
    vm = vm_of(as, 1);
    if (vm == NULL) {
        return STATUS_NO_MEMORY;
    }

    /* MEM_COMMIT alone with no base reserves too - VirtualAlloc(NULL, n,
     * MEM_COMMIT, ...) is the common spelling of "give me memory". */
    if ((type & MEM_RESERVE) || ((type & MEM_COMMIT) && *base == 0)) {
        run_t r;

        if (*base != 0) {
            start = *base & ~(NTVM_GRANULARITY - 1);
            end = (*base + *size + PAGE - 1) & ~(PAGE - 1);
            if (end <= start || end > NTVM_USER_TOP) {
                return STATUS_INVALID_PARAMETER;
            }
            if (!range_free(as, vm, start, end)) {
                return STATUS_CONFLICTING_ADDRESSES;
            }
        } else {
            uint64 sz = (*size + PAGE - 1) & ~(PAGE - 1);

            if (sz == 0 || sz > NTVM_LIMIT - NTVM_BASE) {
                return STATUS_NO_MEMORY;
            }
            start = place(as, vm, sz, (type & MEM_TOP_DOWN) != 0);
            if (start == 0) {
                return STATUS_NO_MEMORY;
            }
            end = start + sz;
        }
        r.start = start;
        r.pages = (end - start) / PAGE;
        r.alloc_base = start;
        r.alloc_size = end - start;
        r.alloc_protect = protect;
        r.protect = 0;
        r.committed = 0;
        r.guard = 0;
        r.type = MEM_PRIVATE;
        r.section = NULL;
        r.view_offset = 0;
        for (i = 0; i < vm->n && vm->runs[i].start < start; i++) {
        }
        if (!insert_at(vm, i, &r)) {
            return STATUS_NO_MEMORY;
        }
        if (type & MEM_COMMIT) {
            st = commit_range(as, vm, start, end, protect);
            if (st != STATUS_SUCCESS) {
                remove_at(vm, find(vm, start));
                return st;
            }
        }
        *base = start;
        *size = end - start;
        return STATUS_SUCCESS;
    }

    /* Commit (or reset) inside an existing reservation. */
    start = *base & ~(PAGE - 1);
    end = (*base + *size + PAGE - 1) & ~(PAGE - 1);
    if (one_allocation(vm, start, end) == 0) {
        return find(vm, start) < 0 ? STATUS_MEMORY_NOT_ALLOCATED
                                   : STATUS_CONFLICTING_ADDRESSES;
    }
    if (type & MEM_RESET) {
        /* "The contents are no longer of interest" - they may be kept, so
         * keeping them is a correct implementation. Committed pages only. */
        for (i = find(vm, start); i < vm->n && vm->runs[i].start < end; i++) {
            if (!vm->runs[i].committed) {
                return STATUS_NOT_COMMITTED;
            }
        }
    } else {
        st = commit_range(as, vm, start, end, protect);
        if (st != STATUS_SUCCESS) {
            return st;
        }
    }
    *base = start;
    *size = end - start;
    return STATUS_SUCCESS;
}

uint32 ntvm_free(address_space_t *as, uint64 *base, uint64 *size,
                 uint32 type) {
    ntvm_t *vm = vm_of(as, 0);
    uint64 start, end;
    int i;

    if (type != MEM_RELEASE && type != MEM_DECOMMIT) {
        return STATUS_INVALID_PARAMETER;
    }
    i = find(vm, *base);
    if (i < 0) {
        return STATUS_MEMORY_NOT_ALLOCATED;
    }
    if (vm->runs[i].type == MEM_MAPPED) {
        return 0xC000001Bu;          /* STATUS_UNABLE_TO_DELETE_SECTION */
    }

    if (type == MEM_RELEASE) {
        uint64 ab = vm->runs[i].alloc_base, asz = vm->runs[i].alloc_size;

        if (*base != ab) {
            return STATUS_FREE_VM_NOT_AT_BASE;
        }
        if (*size != 0 && ((*size + PAGE - 1) & ~(PAGE - 1)) != asz) {
            return STATUS_UNABLE_TO_FREE_VM;
        }
        for (i = 0; i < vm->n; ) {
            if (vm->runs[i].alloc_base == ab) {
                if (vm->runs[i].committed) {
                    unmap_range(as, vm->runs[i].start, vm->runs[i].pages);
                }
                remove_at(vm, i);
            } else {
                i++;
            }
        }
        *size = asz;
        return STATUS_SUCCESS;
    }

    /* MEM_DECOMMIT. Size 0 means "to the end of the allocation". */
    start = *base & ~(PAGE - 1);
    if (*size == 0) {
        end = vm->runs[i].alloc_base + vm->runs[i].alloc_size;
    } else {
        end = (*base + *size + PAGE - 1) & ~(PAGE - 1);
    }
    if (one_allocation(vm, start, end) == 0) {
        return STATUS_UNABLE_TO_FREE_VM;
    }
    if (!split_at(vm, start) || !split_at(vm, end)) {
        return STATUS_NO_MEMORY;
    }
    for (i = 0; i < vm->n; i++) {
        run_t *r = &vm->runs[i];

        if (r->start >= start && run_end(r) <= end && r->committed) {
            unmap_range(as, r->start, r->pages);
            r->committed = 0;
            r->protect = 0;
            r->guard = 0;
        }
    }
    coalesce(vm);
    *base = start;
    *size = end - start;
    return STATUS_SUCCESS;
}

uint32 ntvm_protect(address_space_t *as, uint64 *base, uint64 *size,
                    uint32 new_protect, uint32 *old_protect) {
    ntvm_t *vm = vm_of(as, 0);
    uint64 start, end, a;
    int i;

    if (!valid_protect(new_protect, 0)) {
        return STATUS_INVALID_PAGE_PROTECTION;
    }
    start = *base & ~(PAGE - 1);
    end = (*base + *size + PAGE - 1) & ~(PAGE - 1);
    if (*size == 0 || end <= start) {
        return STATUS_INVALID_PARAMETER;
    }
    i = find(vm, start);
    if (i < 0) {
        return STATUS_MEMORY_NOT_ALLOCATED;
    }
    if (one_allocation(vm, start, end) == 0) {
        return STATUS_CONFLICTING_ADDRESSES;
    }
    for (; i < vm->n && vm->runs[i].start < end; i++) {
        if (!vm->runs[i].committed) {
            return STATUS_NOT_COMMITTED;
        }
    }
    i = find(vm, start);
    *old_protect = vm->runs[i].protect | (vm->runs[i].guard ? PAGE_GUARD : 0);
    if (vm->runs[i].type == MEM_MAPPED &&
        !view_protect_ok(vm->runs[i].section, new_protect)) {
        return 0xC000004Eu;          /* STATUS_SECTION_PROTECTION */
    }

    if (!split_at(vm, start) || !split_at(vm, end)) {
        return STATUS_NO_MEMORY;
    }
    for (i = 0; i < vm->n; i++) {
        run_t *r = &vm->runs[i];

        if (r->start >= start && run_end(r) <= end) {
            r->protect = new_protect & ~PAGE_GUARD;
            r->guard = (new_protect & PAGE_GUARD) != 0;
        }
    }
    /* The same frames, new flags. */
    {
        int mapped = vm->runs[find(vm, start)].type == MEM_MAPPED;

        for (a = start; a < end; a += PAGE) {
            phys_addr_t f = vmm_get_phys_in(as, a);
            int guard = (new_protect & PAGE_GUARD) != 0;

            if (f != 0) {
                (void)vmm_map_page_in(as, a, f & ~0xFFFULL,
                                      mapped ? view_pte_flags(new_protect, guard)
                                             : pte_flags(new_protect, guard));
            }
        }
    }
    coalesce(vm);
    *base = start;
    *size = end - start;
    return STATUS_SUCCESS;
}

uint32 ntvm_query(address_space_t *as, uint64 addr, ntvm_mbi_t *out,
                  const uint64 *images, int nimages) {
    ntvm_t *vm = vm_of(as, 0);
    uint64 page = addr & ~(PAGE - 1), a, limit;
    int i, k;

    if (addr >= NTVM_USER_TOP) {
        return STATUS_INVALID_PARAMETER;
    }
    out->base_address = page;
    out->partition_id = 0;
    out->pad0 = 0;
    out->pad1 = 0;

    i = find(vm, page);
    if (i >= 0) {
        run_t *r = &vm->runs[i];

        out->allocation_base = r->alloc_base;
        out->allocation_protect = r->alloc_protect;
        out->region_size = run_end(r) - page;
        out->state = r->committed ? MEM_COMMIT : MEM_RESERVE;
        out->protect = r->committed ? (r->protect | (r->guard ? PAGE_GUARD : 0))
                                    : 0;
        out->type = r->type;
        return STATUS_SUCCESS;
    }

    /* Not made through ntvm. In an image? */
    for (k = 0; k < nimages; k++) {
        uint64 ib = images[2 * k], isz = images[2 * k + 1];

        if (page >= ib && page < ib + isz) {
            uint64 f = vmm_get_flags_in(as, page);
            uint32 prot = protect_of_pte(f);

            out->allocation_base = ib;
            out->allocation_protect = PAGE_EXECUTE_WRITECOPY;
            out->type = MEM_IMAGE;
            if (vmm_get_phys_in(as, page) == 0) {
                /* A hole inside the image's extent: reserved, as NT shows
                 * the gap between sections. */
                for (a = page; a < ib + isz && vmm_get_phys_in(as, a) == 0;
                     a += PAGE) {
                }
                out->state = MEM_RESERVE;
                out->protect = 0;
                out->region_size = a - page;
                return STATUS_SUCCESS;
            }
            for (a = page; a < ib + isz && vmm_get_phys_in(as, a) != 0 &&
                           protect_of_pte(vmm_get_flags_in(as, a)) == prot;
                 a += PAGE) {
            }
            out->state = MEM_COMMIT;
            out->protect = prot;
            out->region_size = a - page;
            return STATUS_SUCCESS;
        }
    }

    /* Mapped by the kernel (a TEB, a stack, the mmap window, the shared
     * page): private, committed, with the protection its PTE has. */
    limit = page + 0x4000000ULL;               /* scan at most 64MB */
    if (vmm_get_phys_in(as, page) != 0) {
        uint32 prot = protect_of_pte(vmm_get_flags_in(as, page));

        for (a = page; a < limit && vmm_get_phys_in(as, a) != 0 &&
                       find(vm, a) < 0 &&
                       protect_of_pte(vmm_get_flags_in(as, a)) == prot;
             a += PAGE) {
        }
        out->allocation_base = page;
        out->allocation_protect = prot;
        out->region_size = a - page;
        out->state = MEM_COMMIT;
        out->protect = prot;
        out->type = MEM_PRIVATE;
        return STATUS_SUCCESS;
    }

    /* Free: up to the next run, mapped page or image, within the scan. */
    for (k = 0; vm != NULL && k < vm->n; k++) {
        if (vm->runs[k].start > page && vm->runs[k].start < limit) {
            limit = vm->runs[k].start;
        }
    }
    for (k = 0; k < nimages; k++) {
        if (images[2 * k] > page && images[2 * k] < limit) {
            limit = images[2 * k];
        }
    }
    for (a = page; a < limit && vmm_get_phys_in(as, a) == 0; a += PAGE) {
    }
    out->allocation_base = 0;
    out->allocation_protect = 0;
    out->region_size = a - page;
    out->state = MEM_FREE;
    out->protect = PAGE_NOACCESS;
    out->type = 0;
    return STATUS_SUCCESS;
}

int ntvm_guard_fault(address_space_t *as, uint64 addr) {
    ntvm_t *vm = vm_of(as, 0);
    uint64 page = addr & ~(PAGE - 1);
    phys_addr_t f;
    int i = find(vm, page);

    if (i < 0 || !vm->runs[i].committed || !vm->runs[i].guard) {
        return 0;
    }
    /* Clear the guard on this one page and give ring 3 its access back;
     * the faulting instruction re-runs after the handler and succeeds. */
    if (!split_at(vm, page) || !split_at(vm, page + PAGE)) {
        return 0;
    }
    i = find(vm, page);
    vm->runs[i].guard = 0;
    f = vmm_get_phys_in(as, page);
    if (f != 0) {
        (void)vmm_map_page_in(as, page, f & ~0xFFFULL,
                              vm->runs[i].type == MEM_MAPPED
                                  ? view_pte_flags(vm->runs[i].protect, 0)
                                  : pte_flags(vm->runs[i].protect, 0));
    }
    coalesce(vm);
    return 1;
}

/* --- views of sections ------------------------------------------------------------ */

uint32 ntvm_map_view(address_space_t *as, object_t *section, uint64 *base,
                     uint64 offset, uint64 *view_size, uint32 protect,
                     uint32 type) {
    ntvm_t *vm = vm_of(as, 1);
    uint64 sec_size, size, start, k;
    uint32 sec_protect;
    run_t r;
    int i;

    if (vm == NULL) {
        return STATUS_NO_MEMORY;
    }
    if (section_info(section, &sec_size, &sec_protect) != 0) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if ((offset & (NTVM_GRANULARITY - 1)) != 0 || offset >= sec_size) {
        return 0xC0000041u;            /* STATUS_MAPPED_ALIGNMENT */
    }
    if (!valid_protect(protect, 0)) {
        return STATUS_INVALID_PAGE_PROTECTION;
    }
    if (!view_protect_ok(section, protect)) {
        return 0xC000004Eu;            /* STATUS_SECTION_PROTECTION */
    }
    size = *view_size != 0 ? *view_size : sec_size - offset;
    if (offset + size > sec_size) {
        return STATUS_INVALID_PARAMETER;
    }
    size = (size + PAGE - 1) & ~(PAGE - 1);

    if (*base != 0) {
        start = *base & ~(NTVM_GRANULARITY - 1);
        if (start + size > NTVM_USER_TOP || !range_free(as, vm, start,
                                                         start + size)) {
            return STATUS_CONFLICTING_ADDRESSES;
        }
    } else {
        start = place(as, vm, size, (type & MEM_TOP_DOWN) != 0);
        if (start == 0) {
            return STATUS_NO_MEMORY;
        }
    }

    for (k = 0; k < size / PAGE; k++) {
        phys_addr_t f = section_frame(section, offset / PAGE + k);

        pmm_ref_frame(f);
        if (f == 0 || !vmm_map_page_in(as, start + k * PAGE, f,
                                       view_pte_flags(protect,
                                                      (protect & PAGE_GUARD) != 0))) {
            if (f != 0) {
                pmm_free_frame(f);
            }
            while (k-- > 0) {
                vmm_unmap_page_in(as, start + k * PAGE, VMM_FREE_FRAME);
            }
            return STATUS_NO_MEMORY;
        }
    }
    r.start = start;
    r.pages = size / PAGE;
    r.alloc_base = start;
    r.alloc_size = size;
    r.alloc_protect = protect;
    r.protect = protect & ~PAGE_GUARD;
    r.committed = 1;
    r.guard = (protect & PAGE_GUARD) != 0;
    r.type = MEM_MAPPED;
    r.section = section;
    r.view_offset = offset;
    for (i = 0; i < vm->n && vm->runs[i].start < start; i++) {
    }
    if (!insert_at(vm, i, &r)) {
        unmap_range(as, start, size / PAGE);
        return STATUS_NO_MEMORY;
    }
    ob_ref(section);                    /* the view's reference */
    *base = start;
    *view_size = size;
    return STATUS_SUCCESS;
}

uint32 ntvm_unmap_view(address_space_t *as, uint64 addr) {
    ntvm_t *vm = vm_of(as, 0);
    object_t *section;
    uint64 ab;
    int i = find(vm, addr);

    if (i < 0 || vm->runs[i].type != MEM_MAPPED) {
        return 0xC0000019u;            /* STATUS_NOT_MAPPED_VIEW */
    }
    ab = vm->runs[i].alloc_base;
    section = vm->runs[i].section;
    for (i = 0; i < vm->n; ) {
        if (vm->runs[i].alloc_base == ab) {
            unmap_range(as, vm->runs[i].start, vm->runs[i].pages);
            remove_at(vm, i);
        } else {
            i++;
        }
    }
    ob_deref(section);
    return STATUS_SUCCESS;
}

uint32 ntvm_flush(address_space_t *as, uint64 *base, uint64 *size) {
    ntvm_t *vm = vm_of(as, 0);
    uint64 start, end, sec_off;
    int i = find(vm, *base);

    if (i < 0 || vm->runs[i].type != MEM_MAPPED) {
        return 0xC0000019u;            /* STATUS_NOT_MAPPED_VIEW */
    }
    start = *base & ~(PAGE - 1);
    end = *size == 0 ? vm->runs[i].alloc_base + vm->runs[i].alloc_size
                     : (*base + *size + PAGE - 1) & ~(PAGE - 1);
    if (end > vm->runs[i].alloc_base + vm->runs[i].alloc_size) {
        end = vm->runs[i].alloc_base + vm->runs[i].alloc_size;
    }
    sec_off = vm->runs[i].view_offset;
    if (section_flush(vm->runs[i].section,
                      sec_off + (start - vm->runs[i].alloc_base),
                      end - start) != 0) {
        return 0xC0000185u;            /* STATUS_IO_DEVICE_ERROR */
    }
    *base = start;
    *size = end - start;
    return STATUS_SUCCESS;
}
