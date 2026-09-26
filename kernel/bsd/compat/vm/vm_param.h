#ifndef GENESIS_BSD_COMPAT_VM_VM_PARAM_H
#define GENESIS_BSD_COMPAT_VM_VM_PARAM_H

/* Page geometry and the kernel address window, as uma_core.c sees them.
 *
 * The numbers are Genesis's, not FreeBSD's defaults: PAGE_SIZE is 4096 both
 * ways, but the kernel VA range is whatever kernel/include/vmalloc.h hands
 * out, and stating a different one here would give UMA an address space it
 * does not own. */

#include <vm/vm.h>

#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE  (1 << PAGE_SHIFT)
#endif
#ifndef PAGE_MASK
#define PAGE_MASK  (PAGE_SIZE - 1)
#endif

#ifndef trunc_page
#define trunc_page(x)  ((x) & ~(unsigned long)PAGE_MASK)
#endif
#ifndef round_page
#define round_page(x)  (((x) + PAGE_MASK) & ~(unsigned long)PAGE_MASK)
#endif
#ifndef atop
#define atop(x)        ((x) >> PAGE_SHIFT)
#endif
#ifndef ptoa
#define ptoa(x)        ((x) << PAGE_SHIFT)
#endif

/* ONE domain. Genesis has no NUMA topology, no SRAT parsing, and one
 * physical allocator - so every "which domain" question upstream asks has
 * the same answer, and the domain iterators below collapse to it. This is
 * the single biggest simplification in this shim and it is a true statement
 * about the machine rather than a deferral. */
#define MAXMEMDOM      1
#define VM_NDOMAIN     1

#define VM_MIN_KERNEL_ADDRESS  ((vm_offset_t)0xFFFFFE8000000000UL)
#define VM_MAX_KERNEL_ADDRESS  ((vm_offset_t)0xFFFFFEFFFFFFFFFFUL)

#define VM_LEVEL_0_ORDER 9

/* Cache line. 64 on every x86-64 part this kernel runs on. UMA uses it to
 * pad per-CPU and per-domain structures apart, which is a real performance
 * property even though nothing here measures it - and getting it wrong
 * silently costs rather than breaks. */
#ifndef CACHE_LINE_SHIFT
#define CACHE_LINE_SHIFT 6
#endif
#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE (1 << CACHE_LINE_SHIFT)
#endif

#endif
