#ifndef _SYS_SELINFO_H_
#define _SYS_SELINFO_H_

#include <sys/event.h>

/* struct selinfo - what a process blocked in select/poll on this object is
 * recorded in, plus the knote list for kqueue.
 *
 * Genesis has its own readiness queue (kernel/proc/waitq.c, ROADMAP item 9's
 * poll work) and nothing that registers through this structure. It appears
 * BY VALUE inside struct socket and struct sockbuf, so it has to be a
 * complete type of the right shape; nothing reads it.
 *
 * selrecord is still a no-op: nothing registers here, because Genesis's poll
 * does not keep a per-object waiter list.
 *
 * --- BUT THE WAKEUPS ARE REAL NOW, and that changed when sockets became
 * --- descriptors -----------------------------------------------------------
 *
 * The sentence above used to end "and they are no-ops here - a socket layer
 * built on this would block through waitq instead". That was true while the
 * only caller was the kernel's own net_selftest, which calls soreceive
 * directly. The moment socket(2) existed (ROADMAP item 6), poll(2) could be
 * handed a socket - and poll parks on the SHARED readiness queue that every
 * waitq_wake_all in the kernel pokes (see kernel/proc/waitq.c, which argues
 * for one shared queue precisely so that no wake site can be forgotten).
 *
 * The socket layer never calls waitq_wake_all. It wakes through here. So a
 * poll on a socket slept until its timeout while the data sat in the receive
 * buffer - found exactly that way: a blocking recvfrom returned the datagram
 * immediately and a five-second poll on the same descriptor reported nothing.
 *
 * This is the right hook rather than a convenient one. sowakeup funnels every
 * socket-buffer readiness change through selwakeuppri, it is the compat
 * layer's own function so vendored code stays untouched, and "wake whatever
 * is selecting on this object" is exactly what Genesis's readiness queue is.
 */

/* Defined in kernel/bsd/netglue.c - a one-line bridge, kept out of this
 * header because reaching kernel/include/waitq.h from a BSD compat header
 * pulls in process.h and its colliding `struct thread`. */
void genesis_sel_wakeup(void);

/* si_note is the REAL struct knlist from the vendored <sys/event.h>. It used
 * to be a one-int placeholder defined inline here, which worked while nothing
 * touched it; kern/uipc_socket.c passes &so->so_rdsel.si_note to
 * knlist_init(), so the type has to be the one those functions take. */
struct selinfo {
    void *si_thread;
    int   si_flags;
    struct knlist si_note;
};

#define selrecord(td, sip)       do { } while (0)
#define selwakeup(sip)           genesis_sel_wakeup()
#define seldrain(sip)            do { } while (0)
/* Is anything blocked in select/poll on this object? Never - see above, no
 * caller registers here. kern/uipc_sockbuf.c tests it before doing the work
 * of a wakeup, so answering "no" skips work that would have no recipient. */
#define SEL_WAITING(sip)         (0)
#define selwakeuppri(sip, pri)   genesis_sel_wakeup()

#endif
