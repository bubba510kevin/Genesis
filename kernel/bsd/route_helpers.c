/* net/route/route_helpers.c, VENDORED WHOLE.
 *
 * The read side: rib_lookup and the walk callbacks everything else uses
 * to ask the table a question without holding its lock.
 *
 * One file, one upstream file - see kernel/bsd/route_prelude.h for why the
 * routing subsystem is split this way rather than merged into one
 * translation unit like kernel/bsd/ip.c.
 */

#include "route_prelude.h"

#include "vendor/route_helpers.inc"
