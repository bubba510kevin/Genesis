/* kern/uipc_domain.c and net/toeplitz.c, VENDORED WHOLE.
 *
 *   kern/uipc_domain.c - the protocol DOMAIN registry: domain_add() links a
 *                        family (inet, inet6, local) into the list, and
 *                        pffindproto()/pffindtype() are how the socket layer
 *                        turns (family, type, protocol) into a struct
 *                        protosw. netinet/in_proto.c's DOMAIN_SET(inet) is
 *                        the caller.
 *   net/toeplitz.c     - the Toeplitz hash RSS uses to spread flows across
 *                        receive queues. Here because netinet/in_fib.c calls
 *                        it when a lookup asks for a flow hash. One receive
 *                        queue, so nothing is being spread; the hash still
 *                        has to be the right one, because a driver that
 *                        computes it in hardware and a stack that computes it
 *                        in software have to agree.
 */

#include "route_prelude.h"

#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/eventhandler.h>
#include <sys/taskqueue.h>

#include <net/toeplitz.h>

#include "vendor/uipc_domain.inc"
#include "vendor/toeplitz.inc"
