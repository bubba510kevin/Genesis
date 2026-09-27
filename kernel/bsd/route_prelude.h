/* The include set every vendored routing file needs.
 *
 * One header rather than fifteen copies of the same twenty #includes. Each of
 * kernel/bsd/route*.c, nhop*.c, nhgrp*.c and in_fib.c is a three-line file:
 * this prelude, then the one vendored .inc it owns.
 *
 * --- why one file per upstream file -------------------------------------
 * The first attempt put the whole routing subsystem in one translation unit,
 * which is what kernel/bsd/ip.c does for the five IP files. It does not work
 * here, and the reason is worth recording: nhop_ctl.c and nhgrp.c each define
 * a STATIC djb_hash() and a static consider_resize(), and route_helpers.c
 * defines a union sockaddr_union that another file also defines. Those are
 * file-scope names upstream, which is exactly why upstream can have two of
 * them - and merging the files turns that into a redefinition error.
 *
 * Splitting them back out is therefore not bookkeeping, it is the thing that
 * lets the files stay unmodified. It has the side effect of making the
 * mapping to upstream one-to-one and checkable with md5sum.
 */

#ifndef GENESIS_BSD_ROUTE_PRELUDE_H
#define GENESIS_BSD_ROUTE_PRELUDE_H

#define _KERNEL 1

/* The config(8)-generated option headers, FIRST. INET in particular gates
 * declarations inside <net/route/route_ctl.h> (rt_get_inet_prefix_plen), and
 * a header included before INET is defined yields a file with the IPv4 half
 * missing and no error until the call site. */
#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_route.h"

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/rmlock.h>
#include <sys/sx.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/proc.h>
#include <sys/ucred.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/sockio.h>
#include <sys/epoch.h>
#include <sys/refcount.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/radix.h>
#include <net/route.h>
#include <net/route/route_ctl.h>
#include <net/route/route_var.h>
#include <net/route/nhop.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/in_fib.h>

#include "kprintf.h"

#endif /* GENESIS_BSD_ROUTE_PRELUDE_H */
