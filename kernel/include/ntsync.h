#ifndef NTSYNC_H
#define NTSYNC_H

#include "object.h"
#include "process.h"
#include "typesk.h"

/* The NT 6+ synchronisation objects that are not dispatcher objects
 * (ROADMAP item 16(k)): keyed events and I/O completion ports. Both are
 * things a thread BLOCKS on but never waits on with WaitForSingleObject, so
 * they live beside dispatch.c rather than in it. The NT syscalls over them
 * are in kernel/exec/nt.c.
 *
 * --- Keyed events ---------------------------------------------------------
 * A rendezvous, not a flag. NtWaitForKeyedEvent(key) blocks until some
 * thread calls NtReleaseKeyedEvent with the same key on the same object,
 * and NtReleaseKeyedEvent blocks until there is a waiter to release - each
 * call is matched with exactly one call of the other kind, oldest first.
 * Nothing is remembered: a release with nobody waiting does not "bank" a
 * wake for later, it waits for a waiter. This is what NT's critical
 * sections and SRW locks fell back on before Windows 8 (and what they still
 * use when NtWaitForAlertByThreadId is not there): the key is the address of
 * the lock, and one global keyed event serves every lock in the process.
 *
 * --- I/O completion ports -------------------------------------------------
 * A queue of completion packets {key, apc context, status, information}.
 * NtSetIoCompletion (PostQueuedCompletionStatus) appends one;
 * NtRemoveIoCompletion (GetQueuedCompletionStatus) takes the oldest, or
 * blocks until there is one. The queue is unbounded, as on NT - a post
 * fails only when kernel memory does. The concurrency value NT uses to cap
 * how many threads run at once off one port is recorded and not enforced:
 * every waiter that can get a packet gets one. */

/* --- keyed events --- */
object_t *keyed_event_create(void);
/* The process-wide keyed event a NULL handle names (NT's
 * \KernelObjects\CritSecOutOfMemoryEvent). Created on first use. */
object_t *keyed_event_global(void);

/* A thread is going away without returning from a rendezvous (terminated,
 * or killed by a fault): drop its pending entry so no later call pairs with
 * a thread that will never run again. Called from the exit path. */
void keyed_event_forget(process_t *p);

#define KEYED_WAIT     0
#define KEYED_RELEASE  1
/* Rendezvous on (obj, key) as a waiter or a releaser. `deadline` is absolute
 * ticks, 0 for none. 0 when matched, -110 at the deadline, -4 when a signal
 * interrupted the wait (the thread is being terminated), -22 when obj is
 * not a keyed event. */
int keyed_event_rendezvous(object_t *obj, uint64 key, int kind,
                           uint64 deadline);

/* --- I/O completion ports --- */
typedef struct {
    uint64 key;            /* CompletionKey: what the port was told        */
    uint64 apc_context;    /* the OVERLAPPED pointer, for kernel32         */
    uint64 status;         /* IO_STATUS_BLOCK.Status                       */
    uint64 information;    /* IO_STATUS_BLOCK.Information                  */
} io_packet_t;

object_t *io_completion_create(uint32 concurrency);
/* 0, -ENOMEM, or -22 when obj is not a completion port. */
int io_completion_post(object_t *obj, const io_packet_t *pk);
/* Take up to `max` packets, blocking until at least one is there.
 * Returns how many were taken (>= 1), -110 at the deadline, -4 on a signal,
 * -22 when obj is not a completion port. With `alertable` set, a user APC
 * queued to the calling thread also ends the wait - before anything is
 * taken, as with the dispatcher's alertable waits - returning
 * IO_REMOVE_APC. */
#define IO_REMOVE_APC (-1000)
int io_completion_remove(object_t *obj, io_packet_t *out, int max,
                         uint64 deadline, int alertable);
/* Packets queued now, or -22. */
int io_completion_depth(object_t *obj);

#endif
