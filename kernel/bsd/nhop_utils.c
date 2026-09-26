/* net/route/nhop_utils.c, VENDORED WHOLE.
 *
 * The chained hash table and index bitmap the nexthop control code is
 * built on.
 *
 * One file, one upstream file - see kernel/bsd/route_prelude.h for why the
 * routing subsystem is split this way rather than merged into one
 * translation unit like kernel/bsd/ip.c.
 */

#include "route_prelude.h"

#include "vendor/nhop_utils.inc"
