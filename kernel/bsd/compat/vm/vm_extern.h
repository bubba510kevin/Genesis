#ifndef GENESIS_BSD_COMPAT_VM_VM_EXTERN_H
#define GENESIS_BSD_COMPAT_VM_VM_EXTERN_H

/* kva_alloc/kva_free and the pmap entry points, over Part 1's kernel VA
 * range allocator and paging.c.
 *
 * kva_alloc IS kvm_alloc_range under FreeBSD's name - the plan said so, and
 * it is the reason Part 1 came first: without a real VA allocator this would
 * have been a fourth hand-picked base constant. */

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <sys/malloc.h>

/* POINTER-typed, matching this FreeBSD version's own prototypes:
 *   void *kva_alloc(vm_size_t);              vm/vm_extern.h
 *   void  pmap_qenter(void *, vm_page_t *, int);   vm/pmap.h
 * An earlier draft here made them vm_offset_t, which is what older FreeBSD
 * used, and every call site failed to compile with an int-conversion
 * warning rather than an error - the kind of mismatch that would have
 * silently truncated on a 32-bit build. Checked against vendsrc rather than
 * remembered. */
void       *genesis_kva_alloc(vm_size_t size);
void        genesis_kva_free(void *addr, vm_size_t size);
void        genesis_pmap_qenter(void *va, vm_page_t *ma, int count);
void        genesis_pmap_qremove(void *va, int count);
vm_paddr_t  genesis_pmap_kextract(vm_offset_t va);

#define kva_alloc(size)             genesis_kva_alloc(size)
#define kva_free(addr, size)        genesis_kva_free((void *)(addr), (size))
#define pmap_qenter(va, ma, count)  genesis_pmap_qenter((void *)(va), (ma), (count))
#define pmap_qremove(va, count)     genesis_pmap_qremove((void *)(va), (count))
#define pmap_kextract(va)           genesis_pmap_kextract((vm_offset_t)(va))
#define pmap_extract(pm, va)        genesis_pmap_kextract((vm_offset_t)(va))
vm_offset_t genesis_pmap_map(vm_offset_t *virt, vm_paddr_t start,
                             vm_paddr_t end, int prot);
#define pmap_map(virt, s, e, prot)  ((void *)genesis_pmap_map((virt), (s), (e), (prot)))
/* pmap_remove takes a pmap and a VA range. Genesis has one kernel address
 * space reachable from here, so the pmap argument is ignored and the range
 * is unmapped page by page. */
void genesis_pmap_remove(vm_offset_t start, vm_offset_t end);
extern int genesis_kernel_pmap;
#define kernel_pmap                 (&genesis_kernel_pmap)
#define pmap_remove(pm, s, e)       genesis_pmap_remove((s), (e))

/* malloc(9) flags to vm_page allocation flags. Upstream's mapping, kept
 * because the two flag spaces are genuinely different numbers. */
static __inline int malloc2vm_flags(int malloc_flags) {
    int pflags = 0;

    if ((malloc_flags & M_ZERO) != 0) {
        pflags |= VM_ALLOC_ZERO;
    }
    if ((malloc_flags & M_NOWAIT) != 0) {
        pflags |= VM_ALLOC_NOWAIT;
    } else {
        pflags |= VM_ALLOC_WAITOK;
    }
    if ((malloc_flags & M_USE_RESERVE) != 0) {
        pflags |= VM_ALLOC_SYSTEM;
    }
    return pflags;
}

/* kmem_* compose from the two rows above - a VA range plus pages mapped into
 * it. Genesis's kmalloc already is exactly that composition, so these go
 * through it rather than re-deriving it. */
void *genesis_kmem_malloc(vm_size_t size, int flags);
void  genesis_kmem_free(void *addr, vm_size_t size);

#define kmem_malloc(size, flags)                    genesis_kmem_malloc((size), (flags))
#define kmem_malloc_domainset(ds, size, flags)      genesis_kmem_malloc((size), (flags))
#define kmem_malloc_domain(d, size, flags)          genesis_kmem_malloc((size), (flags))
#define kmem_alloc_contig_domainset(ds, s, f, l, h, a, b, mt) \
        genesis_kmem_malloc((s), (f))
#define kmem_free(addr, size)                       genesis_kmem_free((void *)(addr), (size))

#endif
