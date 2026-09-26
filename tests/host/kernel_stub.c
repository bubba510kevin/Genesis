/* Host stubs for kernel facilities the tested files call and the harness has
 * no equivalent of.
 *
 * Every one of these appeared after the harness was last run, because the
 * files under test grew a dependency and nothing linked them off-target in
 * between. They are here rather than #ifdef'd into the kernel for run.sh's
 * stated reason: the kernel builds with no test scaffolding in it at all.
 *
 * The rule each stub follows is the same one dev_stub.c follows - answer the
 * way a machine with none of that facility would answer, so a test that
 * DEPENDS on the facility fails rather than passing on a fake. None of these
 * is a simulation.
 */

#include "typesk.h"

/* --- ksmp.h -------------------------------------------------------------
 * paging.c's invlpg calls this to invalidate the same page on every other
 * CPU. There is one host thread and no other CPU, so there is nothing to
 * shoot down. The bookkeeping the vmm tests check happens before the call. */
void smp_tlb_shootdown(uint64 addr) {
    (void)addr;
}

/* --- vmalloc.h ----------------------------------------------------------
 * The kernel VA allocator. Answering 0 is not a failure to implement it: 0 is
 * "the region is exhausted", which is exactly the answer both callers here
 * are written to survive.
 *
 * pe_driver_window_init and kstack_init both treat it as OPTIONAL - they fall
 * back to their compiled-in base constant and say so in their own comments -
 * and run.sh already rewrites KSTACK_BASE to a host-mappable address for that
 * fallback path. A stub that handed out plausible addresses instead would
 * make the tests exercise a VA allocator that is not the kernel's. */
uint64 kvm_alloc_range(uint64 size, uint64 align) {
    (void)size; (void)align;
    return 0;
}

void kvm_free_range(uint64 base) {
    (void)base;
}

/* --- ntoskrnl_exports.h -------------------------------------------------
 * The synthetic kernel-export table a .sys import can resolve against. The
 * table lives in kernel/exec/ntoskrnl_exports.c, which pulls in wdm.c and the
 * whole driver model behind it; the PE tests are about parsing and relocating
 * an image, not about what its imports resolve to.
 *
 * "No" and "unresolved" is the honest answer for a build with no driver model
 * linked in, and it is the answer pe.c already handles - find_export refuses
 * rather than guesses on a missing export, which is the path this takes. A
 * pe_test that ever needs a real resolution will fail here loudly instead of
 * being handed an address that means nothing. */
int nt_is_synthetic_dll(const char *dll_name) {
    (void)dll_name;
    return 0;
}

int nt_resolve_import(const char *dll_name, const char *symbol_name,
                      uint64 *addr) {
    (void)dll_name; (void)symbol_name; (void)addr;
    return 0;
}

/* --- kprintf, for the host -----------------------------------------------
 *
 * kernel/fs/pcache.c reports and self-tests through kprintf, and the host
 * build has no console driver behind it. Routed to stdout rather than
 * swallowed: the page cache's own selftest runs in the guest, but its report
 * lines are how a failing HOST test says which page it was looking at, and a
 * stub that discarded them would make that debugging session silent.
 *
 * Declared here rather than in a header because these two are the whole
 * surface the host build needs, and kernel/include/kprintf.h drags in the
 * screen and serial drivers behind it. */
#include <stdio.h>
#include <stdarg.h>

void kprintf(const char *fmt, ...);
void kprintf_c(unsigned char color, const char *fmt, ...);

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void kprintf_c(unsigned char color, const char *fmt, ...) {
    va_list ap;
    (void)color;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
