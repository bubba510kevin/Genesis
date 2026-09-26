#ifndef GENESIS_BSD_COMPAT_SYS_SMR_H
#define GENESIS_BSD_COMPAT_SYS_SMR_H

/* Safe Memory Reclamation - REAL now, in kernel/bsd/kern_smr.c.
 *
 * This header used to panic on every entry point, with a comment saying the
 * plan called for exactly that: "provide the real header and PANIC on that
 * path rather than a silent no-op, so a future zone that needs SMR says so
 * instead of corrupting quietly."
 *
 * It said so. netinet/in_pcb.c creates its PCB zone with UMA_ZONE_SMR,
 * because it looks sockets up without a lock - so the panic fired on the
 * first boot with the PCB layer in, which is the mechanism working as
 * designed. See kern_smr.c for the implementation and for where it is
 * coarser than upstream's.
 */

#include <sys/types.h>
#include <sys/_smr.h>

struct smr;
typedef struct smr *smr_t;

#define SMR_SEQ_INVALID  0

smr_t     smr_create(const char *name, int limit, int flags);
void      smr_destroy(smr_t smr);
smr_seq_t smr_advance(smr_t smr);
int       smr_poll(smr_t smr, smr_seq_t goal, bool wait);
void      smr_wait(smr_t smr, smr_seq_t goal);
void      smr_synchronize(smr_t smr);
void      smr_init(void);

void      smr_enter_impl(smr_t smr);
void      smr_exit_impl(smr_t smr);

/* Macros over the functions, matching upstream's spelling. The read side is
 * two instructions and is on the packet receive path, so it is worth not
 * paying for a call - but the body is in the .c file rather than inlined
 * here, because the per-CPU depth array is what it touches and that belongs
 * with the rest of the implementation. */
#define smr_enter(smr)  smr_enter_impl(smr)
#define smr_exit(smr)   smr_exit_impl(smr)

#endif
