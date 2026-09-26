/* The radix tree and the interface cloner, VENDORED.
 *
 *   net/radix.c   - the PATRICIA trie every routing table is built on: a
 *                   longest-prefix match over variable-length keys with the
 *                   mask stored beside the value. rn_refines() - "is mask A
 *                   more specific than mask B" - is also what net/if.c's
 *                   ifa_ifwithnet() uses to break ties between two interface
 *                   addresses that both match a destination.
 *   kern/subr_unit.c - the unit-number allocator. A run-length-encoded
 *                   bitmap, so allocating unit 0 out of a 2^31 space costs
 *                   one small allocation rather than a real bitmap.
 *
 * --- what was tried and dropped: net/if_clone.c -------------------------
 * Interface cloning (`ifconfig lo0 create`) is the one part of net/if.c's
 * ioctl surface with no implementation here. if_clone.c itself vendors
 * cleanly; what it drags in is NETLINK - four handler slots in struct
 * if_clone are netlink dump/create callbacks, and reaching them means
 * netlink's message writer, its parser and its bitset attribute encoding.
 * That is a socket-family-sized subsystem for a facility with no caller: no
 * userland ifconfig exists to issue SIOCIFCREATE.
 *
 * So if_clone_create/destroy/list are answered in kernel/bsd/netglue.c with
 * "no cloners are registered", which is true. The consequence is named there
 * rather than hidden: there is no lo0.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <sys/jail.h>
#include <sys/proc.h>
#include <sys/sockio.h>
#include <sys/eventhandler.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_clone.h>
#include <net/if_types.h>
#include <net/radix.h>
#include <net/route.h>
#include <net/vnet.h>

#include "kprintf.h"

/* kern/subr_unit.c - the unit-number allocator, vendored whole. if_clone
 * hands out lo0, lo1, ... from one of these, and the file is worth having
 * rather than approximating: it is a run-length-encoded bitmap so that
 * allocating unit 0 out of a 2^31 space costs one small allocation, and a
 * naive version would either cap the space or allocate a real bitmap for it. */
#include "vendor/subr_unit.inc"

#include "vendor/radix.inc"
