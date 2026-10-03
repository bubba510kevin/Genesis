#ifndef IOMGR_H
#define IOMGR_H

#include "typesk.h"

struct object;
struct open_file;
struct address_space;

/* Asynchronous (overlapped) I/O - ROADMAP 16(o). kernel/fs/iomgr.c.
 *
 * A handle opened without FILE_SYNCHRONOUS_IO_(NON)ALERT is ASYNCHRONOUS
 * (OF_NT_ASYNC on its open instance). On one, an operation that can proceed
 * at once does, and is still reported the asynchronous way; one that would
 * block becomes a PENDING request (an IRP) and the call returns
 * STATUS_PENDING. A kernel thread completes pending requests as their
 * objects become ready, writing into the issuing process's memory through
 * the direct map (it is not that process).
 *
 * Completing a request - now or later - means: the IO_STATUS_BLOCK filled
 * in, the event set (if one was given), the APC routine queued to the
 * issuing thread (if one was given), and a packet posted to the completion
 * port the file is associated with (if it is, and unless
 * FILE_SKIP_COMPLETION_PORT_ON_SUCCESS skips it for an immediate success).
 *
 * Pending kinds: reads (a pipe, named-pipe end, console) and a named-pipe
 * server's listen. Writes complete when issued: a pipe write that has to
 * wait for room waits in the call, as a synchronous one would. */

#define OF_NT_ASYNC              0x1u   /* no FILE_SYNCHRONOUS_IO_* */
#define OF_NT_SKIP_PORT_SUCCESS  0x2u   /* FILE_SKIP_COMPLETION_PORT_ON_SUCCESS */
#define OF_NT_SKIP_SET_EVENT     0x4u   /* FILE_SKIP_SET_EVENT_ON_HANDLE */

#define IRP_READ    1
#define IRP_LISTEN  2

/* How an operation is to be reported. */
typedef struct {
    uint64            iosb;       /* user address of the IO_STATUS_BLOCK */
    struct object    *event;      /* not referenced; iomgr takes its own */
    uint64            apc;
    uint64            apc_ctx;
} io_notify_t;

void iomgr_init(void);

/* Report a finished operation the asynchronous way (from the issuing
 * thread, for one that completed at once). `immediate` applies the
 * skip-on-success rule. */
void iomgr_complete_now(struct open_file *of, const io_notify_t *n,
                        uint32 status, uint64 information, int immediate);

/* Queue a pending request. Returns STATUS_PENDING, or an error status if it
 * could not be queued. */
uint32 iomgr_queue(struct open_file *of, int kind, uint64 buf, uint32 len,
                   uint64 offset, const io_notify_t *n);

/* NtCancelIoFile(Ex): cancel this process's pending requests on `of` - all
 * of them, or the one whose IO_STATUS_BLOCK is at `iosb` - completing each
 * with STATUS_CANCELLED. Returns how many. */
int iomgr_cancel(struct open_file *of, uint64 iosb, int match_iosb);

/* The open instance's last reference is going: its requests are cancelled
 * (and completed, as NT's cleanup does). */
void iomgr_file_closed(struct open_file *of);

/* An address space is being destroyed: its requests are dropped without
 * being completed - there is nowhere left to complete them to. */
void iomgr_space_gone(struct address_space *as);

/* Write `n` bytes at user address `uva` of address space `as`, which need
 * not be the current one. 0, or -14 if any page is not mapped writable. */
int iomgr_copy_out(struct address_space *as, uint64 uva, const void *src,
                   uint64 n);

#endif
