#include "kprintf.h"
#include "kthread.h"
#include "paging.h"
#include "process.h"
#include "sched.h"
#include "timer.h"
#include "typesk.h"

/* The check for kernel threads.
 *
 * --- why this is a boot-time selftest and not a systest section -----------
 * Nothing in ring 3 can reach a kernel thread. There is no syscall that
 * creates one and there is not going to be: a kernel thread with a userspace
 * creator is a kernel module entry point, which is ROADMAP item 4's problem
 * and not this one. So this is the only place the path is exercised at all,
 * which is the same argument net_selftest and lock_selftest are here under.
 *
 * --- what makes this a test rather than a smoke check ---------------------
 * "A kernel thread ran" is a claim that a completely broken implementation
 * can also produce: a kthread_create that just CALLED the function inline
 * would set every flag this test looks at. Three checks are what separate
 * the two, and each has a control:
 *
 *   THE STACK   The thread records its own RSP, and it must be inside its
 *               own kstack slot - not the boot stack the creator is on. An
 *               inline call would record the creator's.
 *
 *   THE BLOCK   The thread sleeps on a channel; the creator then observes it
 *               in PROC_BLOCKED and observes itself still running. That pair
 *               is what distinguishes DESCHEDULING from kern_synch.c's old
 *               `hlt` idle, which would leave the thread PROC_RUNNING and
 *               the creator not running at all. Then a real wakeup() must
 *               return 0 from the sleep rather than EWOULDBLOCK, because a
 *               sleep that merely timed out ten seconds later is not a
 *               wakeup and must not be allowed to look like one.
 *
 *   THE FLAGS   The thread records its own RFLAGS, and IF must be set. This
 *               is the check that has no symptom short of a dead machine: a
 *               kernel thread entered with interrupts off never receives a
 *               timer tick, so it is not slow, it is the end of the boot.
 *
 * The cap check follows the corollary about negative-only tests: it asserts
 * both that the KTHREAD_MAX+1'th creation is refused AND that the first
 * KTHREAD_MAX succeed, because a kthread_create that returned NULL for
 * everything would pass the refusal on its own.
 */

/* Bounded by TICKS, not by iterations.
 *
 * It was an iteration count, and that was wrong in the way a test-harness
 * bug usually is - it reported a failure in the code under test. A yield
 * loop spins through five hundred iterations in well under one 10ms tick, so
 * anything waiting on a real deadline gave up long before the deadline could
 * arrive, and the test said the timer had not fired when in truth it had not
 * been given a chance to. Anything that waits on a timer has to be bounded
 * by the same clock the timer runs on. */
#define WAIT_TICKS 300    /* three seconds at 100Hz */

/* Baseline kernel-thread count, sampled at entry rather than assumed to be
 * zero: the taskqueue's servicing thread (kernel/bsd/kern_taskqueue.c) is
 * created during boot and is still alive here. Asserting absolute counts was
 * a test that only passed while this file was the only thing that had ever
 * made a kernel thread. */
static int baseline_count;

/* Declared here rather than by including <sys/systm.h>: that header brings in
 * the vendored FreeBSD <sys/proc.h>, whose `struct thread` collides with
 * kernel/include/process.h's. Same collision kernel/include/ksleep.h exists
 * for, and the same one-line answer. */
void wakeup(const void *chan);
int  genesis_tsleep(const void *chan, int pri, const char *wmesg, int timo);

static int check(int cond, const char *what, int *failures) {
    if (!cond) {
        kprintf_c(0x0C, "kthread selftest: %s\n", what);
        (*failures)++;
    }
    return cond;
}

/* --- what the probe thread records -------------------------------------- */

static volatile int    probe_stage;        /* 0 -> 1 -> 2 -> 3               */
static volatile uint64 probe_rsp;
static volatile uint64 probe_flags;
static volatile int    probe_is_kthread;
static volatile int    probe_arg_ok;
static void * volatile probe_space;
static volatile int    probe_sleep_ret;
static volatile int    probe_pid;

static int probe_token = 0x5A5A;
static char sleep_chan;

static void probe_thread(void *arg) {
    uint64 rsp, fl;
    process_t *me;

    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(fl));

    me = proc_current();

    probe_rsp        = rsp;
    probe_flags      = fl;
    probe_is_kthread = kthread_running();
    probe_arg_ok     = (arg == (void *)&probe_token);
    probe_space      = (void *)vmm_current_space();
    probe_pid        = (me != NULL) ? me->pid : -1;
    probe_stage      = 1;

    /* Hand the CPU back so the creator can inspect everything above while
     * this thread is still alive. Without this the thread would run to
     * completion in one go and every check below would be made against a
     * slot that had already been reclaimed. */
    kthread_yield();

    probe_stage = 2;

    /* The sleep the whole item is about. timo 0 means "no deadline of my
     * own", which kern_synch.c turns into its ten-second ceiling - so a
     * broken wakeup path is a slow test rather than a hung machine, and the
     * return value is what tells the two apart. */
    probe_sleep_ret = genesis_tsleep(&sleep_chan, 0, "ktprobe", 0);

    probe_stage = 3;
    /* Returning is kthread_exit(). Deliberately not calling it explicitly:
     * "returning from fn is the same as calling kthread_exit" is a documented
     * promise in kthread.h and this is the only thing that checks it. */
}

/* --- the cap check ------------------------------------------------------- */

static volatile int cap_ran;

static void cap_thread(void *arg) {
    (void)arg;
    cap_ran++;
}

/* Yield until `stage` is reached or the deadline passes. Returns the number
 * of yields spent, or -1 on timeout. */
static int yield_until_stage(int stage) {
    uint64 deadline = timer_ticks_now() + WAIT_TICKS;
    int spun = 0;

    while (probe_stage < stage) {
        if (timer_ticks_now() >= deadline) {
            return -1;
        }
        kthread_yield();
        spun++;
    }
    return spun;
}

int kthread_selftest(void) {
    int failures = 0;
    process_t *p;
    int slot;
    int spun;

    probe_stage    = 0;
    cap_ran        = 0;
    baseline_count = kthread_count();

    /* --- 1. creation does not run the thread ----------------------------
     * The contract kthread.h states, and the control that makes every check
     * below mean something: if creation ran the body inline, probe_stage
     * would already be 1 here and the "it was scheduled" check further down
     * would pass without a context switch ever happening. */
    p = kthread_create(probe_thread, &probe_token, "kt-probe");
    if (!check(p != NULL, "kthread_create returned NULL", &failures)) {
        return failures;
    }
    slot = proc_index(p);
    check(probe_stage == 0, "the thread body ran during kthread_create - it "
                            "was called, not scheduled", &failures);
    check(p->state == PROC_READY, "a fresh kernel thread was not PROC_READY",
          &failures);
    check(kthread_running() == 0, "kthread_running() answered yes in the "
                                  "creating context, which is not a kernel "
                                  "thread", &failures);
    check(kthread_count() == baseline_count + 1,
          "kthread_count() did not see the new thread", &failures);

    /* --- 2. it is actually scheduled ------------------------------------ */
    spun = yield_until_stage(1);
    if (!check(spun >= 0, "the kernel thread was never scheduled - suspect "
                          "the fabricated stack or switch_context", &failures)) {
        return failures;
    }

    /* --- 3. it ran as a kernel thread, on its own stack ------------------ */
    check(probe_is_kthread == 1, "kthread_running() answered no INSIDE a "
                                 "kernel thread", &failures);
    check(probe_arg_ok, "the argument passed to kthread_create did not arrive",
          &failures);
    check(probe_pid == p->pid, "proc_current() inside the thread was not the "
                               "thread", &failures);

    /* The stack check, and the reason it is written as a range rather than as
     * "not the boot stack": a thread running on ANOTHER thread's kstack slot
     * is just as wrong and would pass the weaker test. */
    check(probe_rsp > p->thread.kstack_base &&
          probe_rsp <= p->thread.kstack_top,
          "the kernel thread did not run on its own kernel stack", &failures);

    /* IF, bit 9. See the header comment: this one has no symptom short of a
     * machine that stops. */
    check((probe_flags & 0x200ULL) != 0,
          "the kernel thread was entered with interrupts DISABLED - it would "
          "never receive a timer tick", &failures);

    /* The address space is the kernel's, not a borrowed user one. */
    check(probe_space == (void *)vmm_kernel_space(),
          "the kernel thread did not run in the kernel address space",
          &failures);

    /* --- 4. sleep(9) really deschedules --------------------------------- */
    spun = yield_until_stage(2);
    if (!check(spun >= 0, "the kernel thread never reached its sleep",
               &failures)) {
        return failures;
    }

    /* The pair that distinguishes a deschedule from an idle. Reaching this
     * line at all is half of it - the creator is running, which it could not
     * be if the thread were sitting in `hlt` - and the thread's state is the
     * other half. */
    check(p->state == PROC_BLOCKED,
          "the sleeping kernel thread was not PROC_BLOCKED - sleep(9) idled "
          "the CPU instead of descheduling", &failures);
    check(probe_stage == 2, "the thread passed its sleep without being woken",
          &failures);

    wakeup(&sleep_chan);

    spun = yield_until_stage(3);
    if (!check(spun >= 0, "wakeup() did not release the sleeping kernel "
                          "thread", &failures)) {
        return failures;
    }
    /* 0 is a real wakeup, EWOULDBLOCK(35) is the ten-second ceiling expiring.
     * Without this the check above would pass on a machine where wakeup() did
     * nothing at all and the sleep simply timed out. */
    check(probe_sleep_ret == 0,
          "the sleep returned EWOULDBLOCK - it timed out rather than being "
          "woken, so wakeup() reached nobody", &failures);

    /* --- 5. exit reclaims the slot -------------------------------------- */
    {
        uint64 deadline = timer_ticks_now() + WAIT_TICKS;

        while (proc_at(slot) != NULL && timer_ticks_now() < deadline) {
            kthread_yield();
        }
        check(proc_at(slot) == NULL,
              "an exited kernel thread's slot was never reclaimed", &failures);
        check(kthread_count() == baseline_count,
              "kthread_count() did not fall back to its starting value after "
              "the thread exited", &failures);
    }

    /* --- 6. the cap is enforced, and only at the cap --------------------- */
    {
        /* How many are left, not KTHREAD_MAX: the taskqueue already holds
         * one of the six. Creating KTHREAD_MAX unconditionally asserted that
         * nothing else in the kernel had ever wanted a thread, which stopped
         * being true the moment something did. */
        int room = KTHREAD_MAX - baseline_count;
        uint64 deadline;
        int i;
        int made_ok = 1;

        if (room < 1) {
            kprintf_c(0x0E, "kthread selftest: no room below KTHREAD_MAX - "
                            "the cap check was skipped\n");
        } else {
            for (i = 0; i < room; i++) {
                if (kthread_create(cap_thread, NULL, "kt-cap") == NULL) {
                    made_ok = 0;
                }
            }
            check(made_ok, "kthread_create refused a thread BELOW "
                           "KTHREAD_MAX - the cap is a blanket refusal, not "
                           "a limit", &failures);

            kprintf_c(0x07, "kthread selftest: one refusal below is "
                            "expected\n");
            check(kthread_create(cap_thread, NULL, "kt-over") == NULL,
                  "kthread_create accepted a thread ABOVE KTHREAD_MAX",
                  &failures);

            deadline = timer_ticks_now() + WAIT_TICKS;
            while (kthread_count() > baseline_count &&
                   timer_ticks_now() < deadline) {
                kthread_yield();
            }
            check(kthread_count() == baseline_count,
                  "the capped kernel threads did not all run and exit",
                  &failures);
            check(cap_ran == room,
                  "not every capped kernel thread reached its body",
                  &failures);
        }
    }

    if (failures == 0) {
        kprintf("kthread: selftest passed\n");
    } else {
        kprintf_c(0x0C, "kthread: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
