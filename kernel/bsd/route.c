/* net/route.c, VENDORED WHOLE.
 *
 * The routing subsystem's front door: rtalloc's callers, rtredirect(),
 * rt_ifmsg() and the interface-address hooks the rest of the kernel calls.
 *
 * One file, one upstream file - see kernel/bsd/route_prelude.h for why the
 * routing subsystem is split this way rather than merged into one
 * translation unit like kernel/bsd/ip.c.
 */

#include "route_prelude.h"

#include "vendor/route.inc"
