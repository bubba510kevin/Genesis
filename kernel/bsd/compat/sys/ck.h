#ifndef _SYS_CK_H_
#define _SYS_CK_H_
/* FreeBSD's shim over the vendored Concurrency Kit. See ck_queue.h for why
 * the queues are adapted onto <sys/queue.h> rather than vendored, and
 * sys/epoch.h for the same argument about the epoch they exist to serve. */
#include <sys/queue.h>
#include <ck_queue.h>
/* ck_pr_load_ptr / ck_pr_store_ptr - a single pointer read or written
 * without tearing, and without a fence.
 *
 * Concurrency Kit spells these out per architecture because it supports
 * architectures where a naked pointer store is not atomic. On x86-64 an
 * aligned 8-byte load or store IS atomic, so `volatile` is the whole
 * implementation - it only has to stop the COMPILER from splitting or
 * reordering it, which is exactly what these two names promise and no more.
 * Neither carries a barrier upstream either.
 *
 * net/if.c uses them for the interface index table, which is read by any CPU
 * while another may be attaching an interface. */
#define ck_pr_load_ptr(p)       (*(void * volatile *)(p))
#define ck_pr_store_ptr(p, v)   (*(void * volatile *)(p) = (v))
#define ck_pr_load_uint(p)      (*(volatile unsigned int *)(p))
#define ck_pr_store_uint(p, v)  (*(volatile unsigned int *)(p) = (v))
#define ck_pr_load_16(p)        (*(volatile uint16_t *)(p))
#define ck_pr_store_16(p, v)    (*(volatile uint16_t *)(p) = (v))

#endif
