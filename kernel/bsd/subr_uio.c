/* uiomove(9), VENDORED function for function from kern/subr_uio.c.
 *
 * A struct uio is a scatter/gather description of where a transfer's data
 * lives - an array of iovecs, a direction, a residual count, and whether the
 * addresses are in the kernel or in a user process. uiomove() is the one
 * operation on it: copy n bytes between a kernel buffer and wherever the uio
 * points, advancing the uio by what it moved.
 *
 * It is the ENTIRE data path of a socket. sosend() takes a uio from the
 * caller and turns it into mbufs; soreceive() takes mbufs and fills a uio.
 * Whether that copy checks the destination is the difference between a
 * socket layer and a way for a user process to write to arbitrary kernel
 * memory - which is why this is vendored rather than reduced to a memcpy.
 *
 * The UIO_USERSPACE case goes through copyin/copyout (kernel/bsd/kern_env.c),
 * which validate against syscall.c's user_ptr_ok(). See that file for the one
 * gap those still have: the address is checked, the RANGE is not.
 *
 * Five functions taken and the rest of subr_uio.c left: the remainder is the
 * physical-copy and vm_map machinery for sendfile and physio, which have no
 * caller here.
 */

#include "route_prelude.h"

#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>

/* --- the environment the five vendored functions need -------------------
 *
 * All of it is thread-flag machinery for FAULT HANDLING, and none of it has a
 * counterpart here: upstream's uiomove can be called with a lock held and
 * arranges, through TDP_NOFAULTING and a pcb fault handler, for a copy that
 * touches an unmapped user page to return EFAULT instead of sleeping. Genesis
 * has no pcb fault handler (see kernel/bsd/kern_env.c's copyin/copyout for
 * the same gap stated from the other side), so the flags have nothing to
 * gate.
 *
 * They are defined rather than the file edited, which keeps subr_uio.inc a
 * verbatim copy - and each one is named so the absence is visible. */
#define	TDP_DEADLKTREAT		0x00000040
#define	TDP_NOFAULTING		0x00080000
#define	TDP_RESETSPUR		0x00100000

/* Set/restore thread private flags. There is one flag word per thread
 * upstream; here there is nothing to set them on and nothing that reads
 * them, so these record nothing and return "no change". */
static __inline int curthread_pflags_set(int flags) { (void)flags; return (0); }
static __inline void curthread_pflags_restore(int save) { (void)save; }

/* Yield if this thread has been running too long. Upstream's uiomove calls it
 * between iovecs so a large copy cannot monopolise a CPU. There is no
 * preemptible kernel context here to yield FROM - see kernel/bsd/kern_synch.c
 * - so this is a no-op, and the consequence is that one very large socket
 * write holds its CPU for the duration. */
static __inline void maybe_yield(void) { }

/* The largest single transfer. Upstream's non-DEVFS value. */
#define	IOSIZE_MAX	0x7fffffffffffffffLL

/* uiomove() and uiomove_nofault() are defined before uiomove_faultflag() in
 * the vendored fragment, exactly as they are upstream - where subr_uio.c has
 * a prototype for it near the top of the file. The fragment starts below that
 * prototype, so it is restated here. */
static int uiomove_faultflag(void *cp, int n, struct uio *uio, int nofault);

#include "vendor/subr_uio.inc"
