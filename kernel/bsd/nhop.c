/* net/route/nhop.c, VENDORED WHOLE.
 *
 * The NEXTHOP object's public face. A route no longer stores its own
 * gateway: it points at a refcounted, deduplicated nexthop carrying the
 * gateway, the interface, the MTU and a precomputed link-layer header.
 *
 * One file, one upstream file - see kernel/bsd/route_prelude.h for why the
 * routing subsystem is split this way rather than merged into one
 * translation unit like kernel/bsd/ip.c.
 */

#include "route_prelude.h"

#include "vendor/nhop.inc"
