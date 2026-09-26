#ifndef GENESIS_BSD_COMPAT_VM_VM_H
#define GENESIS_BSD_COMPAT_VM_VM_H

/* Genesis shim for FreeBSD's <vm/vm.h>: the base VM types uma_core.c uses.
 *
 * Not vendored, because the real one is the header of an entire VM subsystem
 * - vm_map, vm_object, vm_pager - that Genesis does not have and this pass
 * is not building. What uma_core.c actually needs from it is a handful of
 * integer typedefs and the protection flags, and those are what is here. */

#include <sys/types.h>
#include <sys/param.h>

/* vm_offset_t and vm_paddr_t come from <sys/types.h>, which the mbuf port
 * already established - defining them again here would be two typedefs of
 * the same name and, worse, could disagree about width. Only the ones that
 * header does NOT carry are declared here. */
typedef unsigned long   vm_size_t;
typedef unsigned long   vm_pindex_t;
typedef unsigned char   vm_prot_t;
typedef int             vm_memattr_t;

#define VM_PROT_NONE      ((vm_prot_t)0x00)
#define VM_PROT_READ      ((vm_prot_t)0x01)
#define VM_PROT_WRITE     ((vm_prot_t)0x02)
#define VM_PROT_EXECUTE   ((vm_prot_t)0x04)
#define VM_PROT_ALL       (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)
#define VM_PROT_RW        (VM_PROT_READ | VM_PROT_WRITE)

#define VM_MEMATTR_DEFAULT 0

struct vm_page;
typedef struct vm_page *vm_page_t;

struct vm_object;
typedef struct vm_object *vm_object_t;

struct vm_map;
typedef struct vm_map *vm_map_t;

#endif
