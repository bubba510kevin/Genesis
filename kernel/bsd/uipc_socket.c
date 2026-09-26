/* kern/uipc_socket.c, VENDORED WHOLE.
 *
 * The socket layer: socreate/sobind/soconnect/solisten/soaccept, the send and
 * receive paths (sosend_generic, soreceive_generic and the dgram/stream
 * variants), the socket option plumbing (sooptcopyin/sooptcopyout), and the
 * upcall and shutdown machinery.
 *
 * This is the layer between a protocol and whatever asks it to do something.
 * It is vendored rather than reduced because everything above IP is written
 * against it by name: udp_usrreq.c calls soreserve() and sbappendaddr(),
 * tcp_usrreq.c drives the whole state machine through sonewconn() and
 * soisconnected(), and each of those has a contract about which lock is held
 * and what the buffer accounting looks like that a smaller version would not
 * keep.
 *
 * --- what it is NOT connected to ----------------------------------------
 * There is no socket(2). Genesis's syscall table (kernel/proc/syscall.c) has
 * no socket calls in it, and kern/uipc_syscalls.c - the file that would
 * bridge them, through file descriptors and the VFS - is not vendored.
 *
 * So sockets here are reachable from KERNEL code only: socreate() and the
 * rest are callable, and kernel/bsd/net_selftest.c calls them. That is a real
 * limit and it is the honest place to draw the line - bridging to userland
 * means a struct file, a descriptor table entry and a fileops vector, which
 * is VFS work rather than network work.
 */

#include "route_prelude.h"

#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/sockbuf.h>
#include <sys/sockopt.h>
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/eventhandler.h>
#include <sys/taskqueue.h>
#include <sys/sysproto.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/ktls.h>
#include <sys/un.h>
#include <sys/unpcb.h>
#include <sys/jail.h>
#include <sys/priv.h>
#include <sys/event.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "vendor/uipc_socket.inc"
