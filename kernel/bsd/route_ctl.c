/* net/route/route_ctl.c, VENDORED WHOLE.
 *
 * Add, delete and change a route - the half of the subsystem that mutates,
 * and the one that has to get the nexthop refcounting right.
 *
 * One file, one upstream file - see kernel/bsd/route_prelude.h for why the
 * routing subsystem is split this way rather than merged into one
 * translation unit like kernel/bsd/ip.c.
 */

#include "route_prelude.h"

#include "vendor/route_ctl.inc"
