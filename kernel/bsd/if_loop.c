/* net/if_loop.c, VENDORED WHOLE (kernel/bsd/vendor/if_loop.inc).
 *
 * The loopback interface, lo0 - 127.0.0.1, and the path a packet a
 * host sends to itself takes back into its own input.
 *
 * From FreeBSD main at 8b668bc7e7c8 (2026-08-10), the same snapshot every
 * other file in kernel/bsd/ was taken from - see kernel/bsd/README.md. Not
 * edited: anything this needs to build is supplied in compat/ or in the
 * Genesis glue, never by changing the upstream file.
 */

#include "route_prelude.h"

#include "vendor/if_loop.inc"
