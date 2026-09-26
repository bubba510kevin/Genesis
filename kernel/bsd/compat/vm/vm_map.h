#ifndef GENESIS_BSD_COMPAT_VM_vm_map_H
#define GENESIS_BSD_COMPAT_VM_vm_map_H
/* See kernel/bsd/README.md. Genesis has no vm_map.h equivalent; what uma_core.c
 * needs from this header is declared in vm/vm_extern.h or vm/vm_page.h,
 * which is where the real backing lives. */
#include <vm/vm.h>
#include <vm/vm_param.h>

/* vm_map, the kernel's address-space map.
 *
 * Genesis has no vm_map: kernel VA is handed out by kvm_alloc_range
 * (kernel/vmalloc.c) and there is no map object to insert into. UMA touches
 * it in exactly one place - uma_startup2 inserts its boot reserve so the map
 * knows about memory carved out before the map existed - and with no map
 * that bookkeeping has nothing to record. The insert is a no-op returning
 * success rather than a panic, because the reserve IS still allocated; what
 * is missing is only the record of it. */
struct vm_map_dummy { int m_dummy; };
extern struct vm_map_dummy genesis_kernel_map;
#define kernel_map                       (&genesis_kernel_map)
#define vm_map_lock(m)                   do { } while (0)
#define vm_map_unlock(m)                 do { } while (0)
#define vm_map_insert(m, o, off, s, e, p, mp, c)  (0)
#define MAP_NOFAULT                      0
#ifndef KERN_SUCCESS
#define KERN_SUCCESS                     0
#endif
/* vm_radix_reserve_kva is NOT declared here. uma_core.c declares it itself
 * (`extern void vm_radix_reserve_kva(void);`), so anything here - macro or
 * inline - collides with that declaration. The definition lives in
 * kernel/bsd/uma_vendor.c. */
#endif
