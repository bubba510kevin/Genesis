/* The callout and taskqueue halves of kldload's unload sweeps, checked.
 *
 * --- why this is a file and not four lines in kldload.c -------------------
 *
 * kld_unload (kernel/driver/kldload.c) refuses to unmap a module image while
 * anything still points into it, and it finds out by asking four registries
 * "do you hold a pointer inside [base, base+size)?". Two of those registries
 * are FreeBSD compat - the callout wheel and the taskqueue - and their types
 * only exist inside this include environment (-Ikernel/bsd/compat -D_KERNEL,
 * see build.py's EXTRA_INCLUDES). kldload.c is ordinary kernel source and
 * pulling that environment in to reach two structs would make it a BSD
 * translation unit, with kernel/bsd/compat/sys/bus.h ahead of the
 * kernel/include/bus.h it actually wants.
 *
 * So the split is by include environment rather than by subject, which is a
 * cost worth naming: these two checks and the two in kldload.c are one idea
 * in two files. kld_unload_selftest calls this and adds its failures, so
 * there is still one number at boot.
 *
 * --- what makes these tests rather than exercise ---------------------------
 *
 * Each sweep is checked in BOTH directions against a range exactly one object
 * wide: zero before the registration, one after it, zero again after it is
 * taken back. A sweep that returned zero for everything would pass a
 * negative-only test - and zero-for-everything is precisely the failure that
 * matters here, because it turns kld_unload's refusal into a rubber stamp
 * that always says yes.
 *
 * The range is one object wide, and not "the kernel", so that a callout some
 * other subsystem happens to have armed cannot make the positive case pass
 * by accident.
 *
 * And it is the STRUCT that is inside the range, not the function - both
 * handler functions here are kernel text either way. That is deliberate: a
 * struct callout is normally a field of the driver's softc and a struct task
 * is normally a field of the module's own data, so the wheel's LIST link and
 * the queue's TAILQ link run THROUGH the module image. A sweep that only
 * looked at c_func would call that clean and the next timer tick would walk
 * the wheel into unmapped memory - before any of the module's own code ran,
 * inside the timer interrupt. That is the quiet failure these two checks
 * exist to rule out.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/callout.h>
#include <sys/time.h>

#include "kprintf.h"

static void kld_st_callout(void *arg) { (void)arg; }
static void kld_st_task(void *ctx, int pending) { (void)ctx; (void)pending; }

static struct callout kld_st_co;
static struct task    kld_st_tk;

int kld_bsd_sweep_selftest(void);

int kld_bsd_sweep_selftest(void) {
    int failures = 0;

#define KST(cond, what)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            kprintf_c(0x0C, "kld: unload selftest: %s\n", (what));          \
            failures++;                                                     \
        }                                                                   \
    } while (0)

    /* callout. Armed an hour out so it cannot fire during the test, and
     * stopped again whatever happens. */
    KST(callout_count_in_range((uint64_t)&kld_st_co, sizeof(kld_st_co)) == 0,
        "callout sweep found an unarmed callout");
    callout_init(&kld_st_co, 1);
    callout_reset(&kld_st_co, hz * 3600, kld_st_callout, NULL);
    KST(callout_count_in_range((uint64_t)&kld_st_co, sizeof(kld_st_co)) == 1,
        "callout sweep missed an armed callout in range");
    callout_stop(&kld_st_co);
    KST(callout_count_in_range((uint64_t)&kld_st_co, sizeof(kld_st_co)) == 0,
        "callout sweep still finds a stopped callout");

    /* taskqueue. The kernel is non-preemptive and nothing between the enqueue
     * and the count yields, so the service thread cannot have taken the task
     * off the list yet - this is a deterministic observation and not a race
     * that usually goes the right way. taskqueue_drain is what lets the task
     * run, and is also what makes the third assertion a wait rather than a
     * guess. */
    KST(taskqueue_count_in_range((uint64_t)&kld_st_tk, sizeof(kld_st_tk)) == 0,
        "taskqueue sweep found a task that was never enqueued");
    TASK_INIT(&kld_st_tk, 0, kld_st_task, NULL);
    taskqueue_enqueue(taskqueue_thread, &kld_st_tk);
    KST(taskqueue_count_in_range((uint64_t)&kld_st_tk, sizeof(kld_st_tk)) == 1,
        "taskqueue sweep missed a queued task in range");
    taskqueue_drain(taskqueue_thread, &kld_st_tk);
    KST(taskqueue_count_in_range((uint64_t)&kld_st_tk, sizeof(kld_st_tk)) == 0,
        "taskqueue sweep still finds a drained task");

    return failures;
#undef KST
}
