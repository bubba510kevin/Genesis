#ifndef _MACHINE_RESOURCE_H_
#define _MACHINE_RESOURCE_H_

/* <machine/resource.h> - the SYS_RES_* resource types.
 *
 * Same values as kernel/include/bus.h's and kernel/include/sys/bus.h's, and
 * guarded so that whichever of the three a translation unit sees first wins
 * without the others redefining it. They are the same four numbers spelled in
 * three headers because three different audiences include three different
 * subsets - Genesis's own bus code, FreeBSD driver source, and this
 * machine-dependent header that real driver source also includes.
 */

#ifndef SYS_RES_IRQ
#define SYS_RES_IRQ     1
#define SYS_RES_DRQ     2
#define SYS_RES_MEMORY  3
#define SYS_RES_IOPORT  4
#endif

#endif
