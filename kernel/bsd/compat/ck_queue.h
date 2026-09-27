#ifndef _CK_QUEUE_H_COMPAT_
#define _CK_QUEUE_H_COMPAT_

#include <sys/queue.h>

/* CK's concurrent queues, ADAPTED onto the ordinary ones.
 *
 * --- what CK is and why this is a legitimate mapping ----------------------
 * Concurrency Kit's lists have exactly the same SHAPE as <sys/queue.h>'s -
 * the same next/prev pointers, the same insertion arithmetic. What differs
 * is that CK's versions publish those pointers with explicit store fences,
 * so a reader walking the list WITHOUT holding a lock cannot observe a
 * half-linked entry. FreeBSD uses them for structures read inside the net
 * epoch: the interface list, an interface's address list.
 *
 * Genesis has no epoch (see sys/epoch.h) and therefore no lock-free readers.
 * Every walk of these lists happens with the relevant lock held or on a
 * single CPU during attach. With no unlocked reader, the fences protect
 * nothing, and CK_STAILQ is STAILQ.
 *
 * --- why ADAPTED rather than VENDORED -------------------------------------
 * The real ck_queue.h was tried first, which is this tree's default. It
 * includes ck_pr.h for the fences, which includes ck_md.h, ck_limits.h and
 * ck_stdint.h - and ck_stdint.h pulls in the COMPILER's stdint, whose
 * int64_t then conflicts with kernel/bsd/compat/sys/types.h's. Vendoring it
 * means adopting CK's type system alongside Genesis's, for macros that
 * expand to the same pointer writes either way.
 *
 * --- when this stops being correct ----------------------------------------
 * The moment anything walks one of these lists without a lock. That is a
 * real possibility if a receive path ever runs concurrently with an
 * interface being detached - which is precisely what the epoch exists to
 * make safe. If sys/epoch.h ever becomes real, this file has to become real
 * with it, and they are the same change.
 */

#define CK_STAILQ_HEAD(name, type)          STAILQ_HEAD(name, type)
#define CK_STAILQ_HEAD_INITIALIZER(head)    STAILQ_HEAD_INITIALIZER(head)
#define CK_STAILQ_ENTRY(type)               STAILQ_ENTRY(type)
#define CK_STAILQ_INIT(head)                STAILQ_INIT(head)
#define CK_STAILQ_EMPTY(head)               STAILQ_EMPTY(head)
#define CK_STAILQ_FIRST(head)               STAILQ_FIRST(head)
#define CK_STAILQ_NEXT(elm, field)          STAILQ_NEXT(elm, field)
#define CK_STAILQ_FOREACH(var, head, field) STAILQ_FOREACH(var, head, field)
#define CK_STAILQ_FOREACH_SAFE(var, head, field, tvar) \
    STAILQ_FOREACH_SAFE(var, head, field, tvar)
#define CK_STAILQ_INSERT_HEAD(head, elm, field) \
    STAILQ_INSERT_HEAD(head, elm, field)
#define CK_STAILQ_INSERT_TAIL(head, elm, field) \
    STAILQ_INSERT_TAIL(head, elm, field)
#define CK_STAILQ_INSERT_AFTER(head, tqelm, elm, field) \
    STAILQ_INSERT_AFTER(head, tqelm, elm, field)
#define CK_STAILQ_REMOVE(head, elm, type, field) \
    STAILQ_REMOVE(head, elm, type, field)
#define CK_STAILQ_REMOVE_HEAD(head, field)  STAILQ_REMOVE_HEAD(head, field)
#define CK_STAILQ_REMOVE_AFTER(head, elm, field) \
    STAILQ_REMOVE_AFTER(head, elm, field)
#define CK_STAILQ_CONCAT(head1, head2)      STAILQ_CONCAT(head1, head2)
#define CK_STAILQ_SWAP(head1, head2, type)  STAILQ_SWAP(head1, head2, type)

#define CK_LIST_HEAD(name, type)            LIST_HEAD(name, type)
#define CK_LIST_HEAD_INITIALIZER(head)      LIST_HEAD_INITIALIZER(head)
#define CK_LIST_ENTRY(type)                 LIST_ENTRY(type)
#define CK_LIST_INIT(head)                  LIST_INIT(head)
#define CK_LIST_EMPTY(head)                 LIST_EMPTY(head)
#define CK_LIST_FIRST(head)                 LIST_FIRST(head)
#define CK_LIST_NEXT(elm, field)            LIST_NEXT(elm, field)
#define CK_LIST_FOREACH(var, head, field)   LIST_FOREACH(var, head, field)
#define CK_LIST_FOREACH_SAFE(var, head, field, tvar) \
    LIST_FOREACH_SAFE(var, head, field, tvar)
#define CK_LIST_INSERT_HEAD(head, elm, field) \
    LIST_INSERT_HEAD(head, elm, field)
#define CK_LIST_INSERT_AFTER(listelm, elm, field) \
    LIST_INSERT_AFTER(listelm, elm, field)
#define CK_LIST_INSERT_BEFORE(listelm, elm, field) \
    LIST_INSERT_BEFORE(listelm, elm, field)
#define CK_LIST_REMOVE(elm, field)          LIST_REMOVE(elm, field)
#define CK_LIST_SWAP(head1, head2, type)    LIST_SWAP(head1, head2, type)

#define CK_SLIST_HEAD(name, type)           SLIST_HEAD(name, type)
#define CK_SLIST_ENTRY(type)                SLIST_ENTRY(type)
#define CK_SLIST_INIT(head)                 SLIST_INIT(head)
#define CK_SLIST_EMPTY(head)                SLIST_EMPTY(head)
#define CK_SLIST_FIRST(head)                SLIST_FIRST(head)
#define CK_SLIST_NEXT(elm, field)           SLIST_NEXT(elm, field)
#define CK_SLIST_FOREACH(var, head, field)  SLIST_FOREACH(var, head, field)
#define CK_SLIST_INSERT_HEAD(head, elm, field) \
    SLIST_INSERT_HEAD(head, elm, field)
#define CK_SLIST_REMOVE(head, elm, type, field) \
    SLIST_REMOVE(head, elm, type, field)
#define CK_SLIST_REMOVE_HEAD(head, field)   SLIST_REMOVE_HEAD(head, field)

/* Resume a list walk from a given element rather than the head.
 * netinet/in_pcb.c uses it to continue a hash-bucket scan after a lookup
 * that was interrupted. Same adaptation as every other CK_ macro in this
 * file - onto sys/queue.h - for the reason at the top. */
/* Two more the TCP host cache walks its buckets with. */
#ifndef CK_SLIST_REMOVE_AFTER
#define CK_SLIST_REMOVE_AFTER(elm, field)   SLIST_REMOVE_AFTER(elm, field)
#endif
#ifndef CK_SLIST_FOREACH_SAFE
#define CK_SLIST_FOREACH_SAFE(var, head, field, tvar) \
	SLIST_FOREACH_SAFE(var, head, field, tvar)
#endif

#ifndef CK_LIST_FOREACH_FROM
#define	CK_LIST_FOREACH_FROM(var, head, field)				\
	for ((var) = ((var) ? (var) : CK_LIST_FIRST((head)));		\
	    (var);							\
	    (var) = CK_LIST_NEXT((var), field))
#endif

#endif
