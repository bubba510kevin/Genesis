#ifndef GENESIS_BSD_COMPAT_SYS__SMR_H
#define GENESIS_BSD_COMPAT_SYS__SMR_H
#include <sys/types.h>
typedef uint32_t smr_seq_t;
struct smr;
typedef struct smr *smr_t;
/* SMR's assertion helpers. Upstream's spelling, and they compile away with
 * INVARIANTS off the same way every other KASSERT here does - which is what
 * makes them safe to define rather than leave missing: the arguments are
 * still parsed, so a typo inside one is caught. */
#ifndef SMR_ASSERT
#define	SMR_ASSERT(ex, fn)						\
    KASSERT((ex), (fn ": Assertion " #ex " failed"))
#define	SMR_ASSERT_ENTERED(smr)		do { } while (0)
#define	SMR_ASSERT_NOT_ENTERED(smr)	do { } while (0)
#endif

#endif
