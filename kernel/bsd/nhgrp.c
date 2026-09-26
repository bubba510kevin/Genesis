/* net/route/nhgrp.c, VENDORED WHOLE.
 *
 * Nexthop GROUPS - multipath. Nothing in this tree creates one; the files
 * are here because nhop_ctl.c's free path dispatches on whether a nexthop
 * is a group.
 *
 * One file, one upstream file - see kernel/bsd/route_prelude.h for why the
 * routing subsystem is split this way rather than merged into one
 * translation unit like kernel/bsd/ip.c.
 */

#include "route_prelude.h"

#include "vendor/nhgrp.inc"
