#ifndef GENESIS_BSD_COMPAT_VM_VM_PAGE_H
#define GENESIS_BSD_COMPAT_VM_VM_PAGE_H

/* The VM page layer, over Genesis's PMM.
 *
 * This is the seam the plan's Part 12 said the port would MOVE rather than
 * remove: what remains shimmed is a vm_page layer over allocators Genesis
 * genuinely owns (pmm.c, paging.c, vmalloc.c), instead of a fake allocator
 * standing in for a real one. FreeBSD itself draws the line here -
 * uk_allocf/uk_freef are function pointers precisely so a keg's page source
 * can be swapped.
 *
 * A vm_page here is a token carrying one physical address. Genesis's PMM has
 * no per-page structure array, so the "page" a caller gets back is
 * synthesised from the frame it allocated and freed by reading the address
 * back out. That is enough for UMA, which only ever asks a page for its
 * physical address. */

#include <vm/vm.h>
#include <vm/vm_param.h>

/* plinks is upstream's per-page link union - UMA chains free slabs through
 * it. Only the TAILQ arm is used here, and it has to be a real member of the
 * real struct rather than a side table: uma_core.c takes the address of
 * m->plinks.s.pv and hands it back later. */
#include <sys/queue.h>

struct uma_slab;
struct uma_zone;

/* A list of pages. Upstream's is in vm_page.h and is the head type
 * pcpu_page_alloc builds a slab's pages onto before mapping them. */
TAILQ_HEAD(pglist, vm_page);

struct vm_page {
    vm_paddr_t phys_addr;
    int        in_use;
    union {
        TAILQ_ENTRY(vm_page) q;
        struct {
            struct vm_page *pv;
            void           *pi;
        } s;
        unsigned long memguard;
        /* UMA stores the owning slab and zone here - vtoslab/vsetzoneslab in
         * vm/uma_int.h read and write these two fields by name, which is why
         * this arm is a struct and not a void *. It is the mechanism by which
         * a free() finds which zone an address belongs to, so getting it
         * wrong does not fail to compile, it frees to the wrong zone. */
        struct {
            struct uma_slab *slab;
            struct uma_zone *zone;
        } uma;
    } plinks;
    unsigned char  order;
    unsigned char  pool;
    unsigned short flags;
};

#define VM_ALLOC_NORMAL     0x0001
#define VM_ALLOC_WIRED      0x0002
#define VM_ALLOC_ZERO       0x0004
#define VM_ALLOC_NOOBJ      0x0008
#define VM_ALLOC_NOWAIT     0x0010
#define VM_ALLOC_WAITOK     0x0020
#define VM_ALLOC_WAITFAIL   0x0040
#define VM_ALLOC_NODUMP     0x0080
#define VM_ALLOC_INTERRUPT  0x0100
#define VM_ALLOC_SYSTEM     0x0200
#define VM_ALLOC_NOFREE     0x0400
#define VM_ALLOC_COUNT(x)   0

vm_page_t genesis_vm_page_alloc_noobj(int req);
vm_page_t genesis_vm_page_alloc_noobj_contig(int req, unsigned long npages);
void      genesis_vm_page_free(vm_page_t m);

#define vm_page_alloc_noobj(req)                    genesis_vm_page_alloc_noobj(req)
#define vm_page_alloc_noobj_domain(d, req)          genesis_vm_page_alloc_noobj(req)
#define vm_page_alloc_noobj_contig(req, n, l, h, a, b, mt) \
        genesis_vm_page_alloc_noobj_contig((req), (n))
#define vm_page_alloc_noobj_contig_domain(d, req, n, l, h, a, b, mt) \
        genesis_vm_page_alloc_noobj_contig((req), (n))
#define vm_page_free(m)                             genesis_vm_page_free(m)
/* Unwiring is bookkeeping on a page's wire count, and Genesis's PMM has no
 * wire count - a frame is allocated or it is not. Returning 1 means "the
 * last wire is gone", which is what the caller then acts on by freeing.
 * (void)(m) so the argument is still evaluated-and-discarded rather than
 * dropped, which is what -Wunused-value was complaining about. */
#define vm_page_unwire_noq(m)                       ((void)(m), 1)

#define VM_PAGE_TO_PHYS(m)  ((m)->phys_addr)
#define PHYS_TO_VM_PAGE(p)  genesis_phys_to_vm_page(p)
vm_page_t genesis_phys_to_vm_page(vm_paddr_t pa);

#endif
