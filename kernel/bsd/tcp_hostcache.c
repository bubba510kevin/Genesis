/* netinet/tcp_hostcache.c, VENDORED WHOLE (kernel/bsd/vendor/tcp_hostcache.inc).
 *
 * Per-destination metrics (MTU, RTT, ssthresh) remembered across
 * connections.
 *
 * From FreeBSD main at 8b668bc7e7c8 (2026-08-10), the same snapshot every
 * other file in kernel/bsd/ was taken from - see kernel/bsd/README.md. Not
 * edited: anything this needs to build is supplied in compat/ or in the
 * Genesis glue, never by changing the upstream file.
 */

#include "route_prelude.h"

#include "vendor/tcp_hostcache.inc"
