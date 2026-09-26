#ifndef _SYS_RMAN_H_
#define _SYS_RMAN_H_

#include <machine/bus.h>
#include <machine/resource.h>

/* <sys/rman.h> - the resource manager, as a driver reads it.
 *
 * Upstream this is a real allocator with a region tree. Genesis's allocator
 * lives in kernel/driver/bus.c (bus_alloc_resource, with real overlap
 * detection), so what is needed here is only the ACCESSORS a driver reads an
 * allocated resource back through.
 *
 * rman_get_start/size/end/rid are already declared in kernel/include/sys/bus.h
 * and are not repeated here - one declaration, and this header is always
 * included alongside that one by driver source.
 *
 * What IS here is the bustag/bushandle pair, which is the piece that only
 * makes sense once machine/bus.h exists: a resource knows whether it is
 * memory or ports and where it starts, and those are exactly the two things
 * bus_space needs.
 */

struct resource;

/* Derived from the resource rather than stored on it: the tag IS "which of
 * the two address spaces", which the resource's type already says, and the
 * handle IS the start address. Nothing else to keep in sync. */
bus_space_tag_t    rman_get_bustag(struct resource *r);
bus_space_handle_t rman_get_bushandle(struct resource *r);

/* The mapped kernel virtual address of a MEMORY resource, for a driver that
 * wants a pointer rather than bus_space calls. NULL for a port resource,
 * which has no mapping - see kernel/driver/newbus_compat.c. */
void *rman_get_virtual(struct resource *r);

#define rman_get_device(r) ((device_t)0)

#endif
