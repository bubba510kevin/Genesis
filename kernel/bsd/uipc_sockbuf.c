/* kern/uipc_sockbuf.c, VENDORED WHOLE.
 *
 * The socket BUFFER: sbappend and its variants (the several ways a protocol
 * hands received data to a socket), sbdrop, sbcut, the high/low water mark
 * accounting that decides when a sender blocks and when a reader wakes, and
 * sbcreatecontrol for ancillary data.
 *
 * Separate translation unit from kern/uipc_socket.c because upstream keeps
 * them separate and both define file-scope statics.
 */

#include "route_prelude.h"

/* <sys/ucred.h> for the complete struct ucred: sbreserve_locked() charges a
 * socket buffer against so->so_cred->cr_uidinfo, so the credential cannot be
 * an incomplete type here. Upstream reaches it through <sys/proc.h>'s chain. */
#include <sys/ucred.h>
#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/sockbuf.h>
#include <sys/aio.h>
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/ktls.h>
#include <sys/event.h>
#include <sys/eventhandler.h>

#include "vendor/uipc_sockbuf.inc"
