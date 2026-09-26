#ifndef _MACHINE_PARAM_H_
#define _MACHINE_PARAM_H_

/* <machine/param.h> - the amd64 machine constants <sys/param.h> builds on.
 *
 * Adapted rather than vendored: the real one carries the whole memory-layout
 * description (KVA base, direct-map base, the page-table level constants),
 * all of which Genesis owns in kernel/include/paging.h and must not have a
 * second, disagreeing copy of.
 */

#define MACHINE         "amd64"
#define MACHINE_ARCH    "amd64"

#define PAGE_SHIFT      12
#define PAGE_SIZE       (1 << PAGE_SHIFT)
#define PAGE_MASK       (PAGE_SIZE - 1)

/* MAXCPU sizes the per-CPU arrays vendored code declares. It must be at
 * least Genesis's own ACPI_MAX_CPUS (kernel/arch/acpi.c) or a per-CPU array
 * is short by however many CPUs the machine actually has. */
#define MAXCPU          32

#define CACHE_LINE_SHIFT 6
#define CACHE_LINE_SIZE (1 << CACHE_LINE_SHIFT)

#define ALIGNBYTES      (sizeof(long) - 1)
#define ALIGN(p)        (((unsigned long)(p) + ALIGNBYTES) & ~ALIGNBYTES)
/* Whether a type can be accessed at an arbitrary address. x86 permits
 * unaligned access for ordinary integer loads, which is why FreeBSD's amd64
 * defines this as always true - and network code uses it to decide whether
 * it may read a header field in place or must copy it out first. Saying
 * "false" here would be safe but would make every protocol header get
 * copied; saying "true" is both faster and what the hardware actually does. */
#define ALIGNED_POINTER(p, t)   1

/* PHYS_TO_DMAP - the amd64 direct map, which lets any physical address be
 * reached at a fixed virtual offset without a mapping call.
 *
 * Genesis has exactly this (kernel/include/paging.h's PHYSMAP_BASE, and
 * phys_to_virt over it), so this is a rename rather than an adaptation. It
 * is spelled out rather than including paging.h because that header is on
 * Genesis's include path and this one is on the vendored tree's - the two
 * deliberately do not meet.
 *
 * MUST agree with paging.h's PHYSMAP_BASE. They are two spellings of one
 * constant and there is no compiler check that they match. */
#ifndef PHYS_TO_DMAP
#define PHYSMAP_BASE_COMPAT 0xFFFFFE0000000000ULL
#define PHYS_TO_DMAP(x)  ((void *)(PHYSMAP_BASE_COMPAT + (unsigned long long)(x)))
#define DMAP_TO_PHYS(x)  ((unsigned long long)(x) - PHYSMAP_BASE_COMPAT)
#endif

#endif
