#include "dispatch.h"
#include "kprintf.h"
#include "ns.h"
#include "object.h"
#include "process.h"
#include "sched.h"
#include "timer.h"
#include "typesk.h"
#include "waitq.h"

/* See kernel/include/dispatch.h for what these are and why there are three.
 * What follows is the state and the four operations over it.
 *
 * --- one body type for three object types ---------------------------------
 * The three share a wait queue, a signalled test and a consume step, and
 * differ only in what those mean. Writing them as one struct with a kind tag
 * rather than three structs keeps the WAIT LOOP in one place - and the wait
 * loop is where the subtleties are, not in the state.
 *
 * They stay three object TYPES, because the type is what a caller sees: the
 * class in a stat, the name under \ObjectTypes, and the vtable that decides
 * whether a signal means "set" or "release".
 */

#define DISPATCH_MAX 64   /* events, semaphores, mutants AND one per NT thread */

typedef enum {
    D_EVENT = 0,
    D_SEMAPHORE,
    D_MUTANT,
    D_THREAD
} disp_kind_t;

typedef struct dispatcher {
    int          in_use;
    disp_kind_t  kind;

    /* Event. `manual` is NotificationEvent vs SynchronizationEvent. */
    int          manual;
    int          signalled;

    /* Semaphore. */
    int64        count;
    int64        limit;

    /* Mutant. `owner` is a pid, 0 for unowned; `depth` is the recursion
     * count, which is what makes this not a semaphore of one. `abandoned`
     * is set when the owner died holding it (dispatch_owner_exited) and is
     * cleared by the next take, which is told so - WAIT_ABANDONED. */
    int          owner;
    int          depth;
    int          abandoned;

    /* Thread. `signalled` (shared with Event) is "has exited"; these two
     * are what NtQueryInformationThread reports - the tid, and the full
     * 32-bit exit code, which is NOT the process_t's exit_status: that is
     * a POSIX wait status and keeps only eight bits. */
    int          tid;
    uint32       exit_code;
    /* The CPU time it had used when it exited - GetThreadTimes on a handle
     * to a finished thread, whose process slot is long gone. */
    uint64       cpu_ticks;

    wait_queue_t q;
} dispatcher_t;

static dispatcher_t disp_pool[DISPATCH_MAX];

static dispatcher_t *disp_alloc(disp_kind_t kind) {
    int i;

    for (i = 0; i < DISPATCH_MAX; i++) {
        if (!disp_pool[i].in_use) {
            dispatcher_t *d = &disp_pool[i];

            d->in_use    = 1;
            d->kind      = kind;
            d->manual    = 0;
            d->signalled = 0;
            d->count     = 0;
            d->limit     = 0;
            d->owner     = 0;
            d->depth     = 0;
            d->abandoned = 0;
            d->tid       = 0;
            d->exit_code = 0;
            waitq_init(&d->q);
            return d;
        }
    }
    return NULL;
}

/* Who is asking, for a mutant's ownership. A pid, because that is the
 * identity every context here has - a user process inside a syscall and a
 * kernel thread both have one, and they are the two things that can wait. */
static int caller_id(void) {
    process_t *p = proc_current();

    return (p != NULL) ? p->pid : 0;
}

/* --- would a wait block? -------------------------------------------------
 *
 * The predicate, WITHOUT consuming. Used by poll, and by the wait loop's own
 * re-test - which is why it is a separate function from the consume below:
 * they run at different moments and only one of them may have side effects. */
static int disp_ready(const dispatcher_t *d, int who) {
    switch (d->kind) {
    case D_EVENT:
        return d->signalled;
    case D_SEMAPHORE:
        return d->count > 0;
    case D_MUTANT:
        /* Ready if nobody owns it, OR if the asker already does. The second
         * half is the recursion, and leaving it out is the bug where a thread
         * deadlocks against a mutant it is already holding. */
        return d->owner == 0 || d->owner == who;
    case D_THREAD:
        return d->signalled;
    }
    return 0;
}

/* --- take it -------------------------------------------------------------
 *
 * Called only when disp_ready said yes AND with interrupts disabled, so that
 * nothing can slip between the test and the take. That pairing is the whole
 * reason wait cannot be built out of poll: the gap between asking and taking
 * is exactly where another waiter gets there first.
 *
 * Returns 1 when the take was of an ABANDONED mutant, 0 otherwise. */
static int disp_consume(dispatcher_t *d, int who) {
    int abandoned = 0;

    switch (d->kind) {
    case D_EVENT:
        /* A notification event stays set - that is what "notification" means,
         * and it is why every waiter is released by one signal. A
         * synchronisation event auto-resets here, so exactly one waiter gets
         * through per signal however many were woken. */
        if (!d->manual) {
            d->signalled = 0;
        }
        break;
    case D_SEMAPHORE:
        d->count--;
        break;
    case D_MUTANT:
        /* Told once. The thread that inherits an abandoned mutant is the one
         * that has to decide whether what it protects is still consistent;
         * the one after that inherits it from a live owner and is told
         * nothing, because nothing happened to it. */
        abandoned = d->abandoned;
        d->abandoned = 0;
        d->owner = who;
        d->depth++;
        break;
    case D_THREAD:
        /* Nothing to take. A thread stays exited, so EVERY waiter gets
         * through - the notification-event shape, not the synchronisation
         * one. */
        break;
    }
    return abandoned;
}

/* --- the wait ------------------------------------------------------------ */

/* Save RFLAGS and disable interrupts; restore. Duplicated from
 * kernel/proc/ksleep.c rather than shared, matching this tree's preference
 * for a trivial helper per file. The REASON it is needed here is the same and
 * is written out there: a kernel thread runs with interrupts enabled, so
 * without this a signal can land between the readiness test and the block and
 * be delivered to an empty queue. A syscall path is already protected by
 * SFMASK having cleared IF; a kernel thread is not. */
static uint64 intr_disable(void) {
    uint64 flags;

    __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

static void intr_restore(uint64 flags) {
    __asm__ volatile ("pushq %0\n\tpopfq" : : "r"(flags) : "memory", "cc");
}

struct wait_ctx {
    dispatcher_t *d;
    int           who;
};

static int wait_ready(void *ctx) {
    struct wait_ctx *w = (struct wait_ctx *)ctx;

    return disp_ready(w->d, w->who);
}

static int disp_wait(object_t *obj, uint64 deadline) {
    dispatcher_t *d = (dispatcher_t *)obj->body;
    struct wait_ctx ctx;
    uint64 flags;
    int rc;

    if (d == NULL || !d->in_use) {
        return -22;
    }
    ctx.d   = d;
    ctx.who = caller_id();

    flags = intr_disable();
    for (;;) {
        /* Test and take with interrupts off, so the pair is indivisible.
         * waitq_wait_until would re-test for us, but it returns BETWEEN the
         * test and our take - and that gap is where a second waiter wins the
         * same permit. Testing here instead means the object is consumed
         * before anything else runs. */
        if (disp_ready(d, ctx.who)) {
            rc = disp_consume(d, ctx.who);
            intr_restore(flags);
            return rc;                      /* 0, or 1 for WAIT_ABANDONED */
        }

        rc = waitq_wait_until(&d->q, wait_ready, &ctx, deadline);
        if (rc == WAITQ_TIMEOUT) {
            intr_restore(flags);
            return -110;                    /* -ETIMEDOUT */
        }
        if (rc == WAITQ_SIGNAL) {
            /* A POSIX signal arrived instead. Reported rather than looped on:
             * every blocking path in this kernel needs this check, and
             * forgetting it is what makes a process unkillable while it
             * waits. */
            intr_restore(flags);
            return -4;                      /* -EINTR */
        }
        /* Ready, but not taken yet - loop round and take it under the same
         * interrupts-off window as the test. */
    }
}

/* --- waiting on several at once ------------------------------------------
 *
 * WaitForMultipleObjects. A thread is on one wait queue at a time, so a
 * multi-object waiter parks on the readiness queue that EVERY
 * waitq_wake_all also wakes - poll's arrangement, and for poll's reasons
 * (waitq.h). It re-tests its own objects on each wake and sleeps again if
 * none was one of them.
 *
 * WAIT-ALL IS ALL OR NOTHING. The objects are taken together, in one
 * interrupts-off window, and only once every one of them is ready. Taking
 * each as it becomes available instead would hold the first mutex while
 * waiting for the second - which is exactly the lock-ordering deadlock
 * wait-all exists to let a program avoid. */

static int is_dispatcher(const object_t *obj);

struct multi_ctx {
    dispatcher_t **d;
    int            n;
    int            wait_all;
    int            who;
    process_t     *alertable;   /* the waiter, if an APC may end the wait */
};

static int multi_ready(void *ctx) {
    struct multi_ctx *m = (struct multi_ctx *)ctx;
    int i;

    if (m->alertable != NULL && m->alertable->nt_apc_head != NULL) {
        return 1;
    }
    for (i = 0; i < m->n; i++) {
        int r = !m->d[i]->in_use || disp_ready(m->d[i], m->who);

        if (m->wait_all && !r) {
            return 0;
        }
        if (!m->wait_all && r) {
            return 1;
        }
    }
    return m->wait_all;
}

int dispatch_wait_multiple(object_t **objs, int n, int wait_all,
                           int alertable, uint64 deadline) {
    dispatcher_t *d[DISPATCH_WAIT_MAX];
    struct multi_ctx ctx;
    uint64 flags;
    int i, j, rc;

    if (n < 1 || n > DISPATCH_WAIT_MAX) {
        return -22;
    }
    for (i = 0; i < n; i++) {
        if (objs[i] == NULL || !is_dispatcher(objs[i])) {
            return -22;                     /* not waitable */
        }
        d[i] = (dispatcher_t *)objs[i]->body;
        if (d[i] == NULL || !d[i]->in_use) {
            return -22;
        }
        /* The same object twice in a wait-all could never be satisfied for
         * a semaphore of one and would be taken twice for anything else.
         * NT refuses it; so does this. */
        for (j = 0; wait_all && j < i; j++) {
            if (d[j] == d[i]) {
                return DISPATCH_WAIT_DUPLICATE;
            }
        }
    }
    ctx.d        = d;
    ctx.n        = n;
    ctx.wait_all = wait_all;
    ctx.who      = caller_id();
    ctx.alertable = alertable ? proc_current() : NULL;

    flags = intr_disable();
    for (;;) {
        /* A queued APC ends an alertable wait before anything is taken -
         * checked first, as NT does, so an APC queued before the wait
         * began is not held up by an object that happens to be ready. */
        if (ctx.alertable != NULL && ctx.alertable->nt_apc_head != NULL) {
            intr_restore(flags);
            return DISPATCH_WAIT_APC;
        }
        for (i = 0; i < n; i++) {
            if (!d[i]->in_use) {
                intr_restore(flags);
                return -22;                 /* destroyed while we waited */
            }
        }
        if (wait_all) {
            if (multi_ready(&ctx)) {
                int abandoned = -1;

                for (i = 0; i < n; i++) {
                    if (disp_consume(d[i], ctx.who) && abandoned < 0) {
                        abandoned = i;
                    }
                }
                intr_restore(flags);
                return abandoned >= 0 ? DISPATCH_WAIT_ABANDONED + abandoned
                                      : 0;
            }
        } else {
            /* The LOWEST ready index wins, as on NT - a program that lists
             * its shutdown event first is relying on exactly that. */
            for (i = 0; i < n; i++) {
                if (disp_ready(d[i], ctx.who)) {
                    rc = disp_consume(d[i], ctx.who);
                    intr_restore(flags);
                    return rc ? DISPATCH_WAIT_ABANDONED + i : i;
                }
            }
        }

        rc = waitq_wait_until(waitq_readiness(), multi_ready, &ctx, deadline);
        if (rc == WAITQ_TIMEOUT) {
            intr_restore(flags);
            return -110;
        }
        if (rc == WAITQ_SIGNAL) {
            intr_restore(flags);
            return -4;
        }
    }
}

/* --- the signal ---------------------------------------------------------- */

static int disp_signal(object_t *obj, int op, int64 count, int64 *prev) {
    dispatcher_t *d = (dispatcher_t *)obj->body;
    uint64 flags;
    int rc = 0;

    if (d == NULL || !d->in_use) {
        return -22;
    }

    flags = intr_disable();
    switch (d->kind) {
    case D_EVENT:
        if (prev != NULL) {
            *prev = d->signalled;
        }
        if (op == OB_SIG_RESET) {
            d->signalled = 0;
        } else {
            d->signalled = 1;
        }
        break;

    case D_SEMAPHORE:
        if (op == OB_SIG_RESET) {
            rc = -22;                       /* meaningless for a semaphore */
            break;
        }
        if (count <= 0) {
            rc = -22;
            break;
        }
        /* REFUSED, not clamped, and the count is left exactly where it was.
         *
         * A caller that releases more permits than it took has a counting
         * bug. Clamping to the limit makes that bug invisible forever - the
         * semaphore keeps working, with a count that no longer corresponds to
         * anything the program believes. NT returns an error here and so does
         * this. */
        if (d->count + count > d->limit) {
            rc = -22;
            break;
        }
        if (prev != NULL) {
            *prev = d->count;
        }
        d->count += count;
        break;

    case D_MUTANT:
        if (op == OB_SIG_RESET) {
            rc = -22;
            break;
        }
        /* Release by somebody who does not own it is an ERROR. A mutant is
         * the one dispatcher object with an owner, so "release" is not a
         * thing anyone may do - and a no-op here would let a thread that
         * never held the lock unlock it for the thread that does. */
        if (d->owner != caller_id() || d->depth == 0) {
            rc = -1;                        /* -EPERM */
            break;
        }
        if (prev != NULL) {
            *prev = d->depth;
        }
        /* Recursive: released as many times as taken, and only the last one
         * hands it over. */
        if (--d->depth == 0) {
            d->owner = 0;
        }
        break;

    case D_THREAD:
        /* Only the thread's own exit signals it (thread_object_exited),
         * never a caller: NtSetEvent on a thread handle is a type error,
         * and letting it through would release a WaitForSingleObject on a
         * thread that is still running. */
        rc = -22;
        break;
    }

    if (rc == 0) {
        /* Woken with the state already changed and interrupts still off, so a
         * waiter that runs the instant this returns finds the object in the
         * state this call put it in. waitq_wake_all only marks processes
         * runnable, so it is safe here and from an interrupt handler. */
        waitq_wake_all(&d->q);
    }
    intr_restore(flags);
    return rc;
}

/* --- poll: would it block, WITHOUT taking anything ----------------------- */

static int disp_poll(object_t *obj, int events) {
    dispatcher_t *d = (dispatcher_t *)obj->body;

    (void)events;
    if (d == NULL || !d->in_use) {
        return OB_POLLNVAL;
    }
    /* POLLIN for "a wait would succeed". Not POLLOUT: there is nothing to
     * write to a dispatcher object, and reporting it writable would make
     * poll(POLLOUT) spin on an object no write can ever consume. */
    return disp_ready(d, caller_id()) ? OB_POLLIN : 0;
}

static void disp_destroy(object_t *obj) {
    dispatcher_t *d = (dispatcher_t *)obj->body;

    if (d != NULL) {
        /* Anybody still waiting is woken before the body goes. They re-test,
         * find the object no longer in use, and get -EINVAL - which is a
         * return rather than a wait on something that no longer exists. */
        waitq_wake_all(&d->q);
        d->in_use = 0;
    }
}

static const object_type_t event_type = {
    .name    = "Event",
    .klass   = OBJ_EVENT,
    .poll    = disp_poll,
    .wait    = disp_wait,
    .signal  = disp_signal,
    .destroy = disp_destroy
};

static const object_type_t semaphore_type = {
    .name    = "Semaphore",
    .klass   = OBJ_SEMAPHORE,
    .poll    = disp_poll,
    .wait    = disp_wait,
    .signal  = disp_signal,
    .destroy = disp_destroy
};

static const object_type_t mutant_type = {
    .name    = "Mutant",
    .klass   = OBJ_MUTANT,
    .poll    = disp_poll,
    .wait    = disp_wait,
    .signal  = disp_signal,
    .destroy = disp_destroy
};

static const object_type_t thread_type = {
    .name    = "Thread",
    .klass   = OBJ_THREAD,
    .poll    = disp_poll,
    .wait    = disp_wait,
    .signal  = disp_signal,
    .destroy = disp_destroy
};

static int is_dispatcher(const object_t *obj) {
    return obj->type == &event_type || obj->type == &semaphore_type ||
           obj->type == &mutant_type || obj->type == &thread_type;
}

/* --- creation ------------------------------------------------------------ */

object_t *event_create(int manual, int initial) {
    dispatcher_t *d = disp_alloc(D_EVENT);
    object_t *obj;

    if (d == NULL) {
        return NULL;
    }
    d->manual    = manual ? 1 : 0;
    d->signalled = initial ? 1 : 0;

    obj = ob_create(&event_type, d);
    if (obj == NULL) {
        d->in_use = 0;
    }
    return obj;
}

object_t *semaphore_create(int64 initial, int64 limit) {
    dispatcher_t *d;
    object_t *obj;

    /* Refused rather than adjusted. A semaphore created with more permits
     * than its ceiling is a caller that has its two arguments the wrong way
     * round, and quietly swapping them produces a working object with the
     * wrong capacity. */
    if (limit < 1 || initial < 0 || initial > limit) {
        return NULL;
    }
    d = disp_alloc(D_SEMAPHORE);
    if (d == NULL) {
        return NULL;
    }
    d->count = initial;
    d->limit = limit;

    obj = ob_create(&semaphore_type, d);
    if (obj == NULL) {
        d->in_use = 0;
    }
    return obj;
}

object_t *mutant_create(int owned) {
    dispatcher_t *d = disp_alloc(D_MUTANT);
    object_t *obj;

    if (d == NULL) {
        return NULL;
    }
    if (owned) {
        /* Owned from birth, which is CreateMutex(bInitialOwner=TRUE) and is
         * NOT the same as creating it and then waiting on it: the second has
         * a window in which somebody else can take it. */
        d->owner = caller_id();
        d->depth = 1;
    }
    obj = ob_create(&mutant_type, d);
    if (obj == NULL) {
        d->in_use = 0;
    }
    return obj;
}

/* --- an owner that dies ------------------------------------------------- */

void dispatch_owner_exited(int pid) {
    uint64 flags;
    int i;

    if (pid == 0) {
        return;                             /* 0 is "unowned", not a pid */
    }
    flags = intr_disable();
    for (i = 0; i < DISPATCH_MAX; i++) {
        dispatcher_t *d = &disp_pool[i];

        if (!d->in_use || d->kind != D_MUTANT || d->owner != pid) {
            continue;
        }
        /* Released whatever the depth: nobody is left to make the other
         * releases. Not released SILENTLY - a lock whose holder died
         * mid-update guards data in whatever state it was left, and handing
         * it on as though it had been released properly is how that data
         * gets trusted. So it is marked, and the next owner is told. */
        d->owner     = 0;
        d->depth     = 0;
        d->abandoned = 1;
        waitq_wake_all(&d->q);
    }
    intr_restore(flags);
}

/* --- threads ------------------------------------------------------------- */

object_t *thread_object_create(int tid) {
    dispatcher_t *d = disp_alloc(D_THREAD);
    object_t *obj;

    if (d == NULL) {
        return NULL;
    }
    d->tid = tid;
    obj = ob_create(&thread_type, d);
    if (obj == NULL) {
        d->in_use = 0;
    }
    return obj;
}

void thread_object_exited(object_t *obj, uint32 exit_code) {
    dispatcher_t *d;
    uint64 flags;

    if (obj == NULL || obj->type != &thread_type) {
        return;
    }
    d = (dispatcher_t *)obj->body;
    if (d == NULL || !d->in_use) {
        return;
    }
    flags = intr_disable();
    /* First exit wins. A thread is retired at most once, but the retire
     * path and the thread's own exit can both reach here for one thread
     * (exit_group racing NtTerminateThread), and the code the thread asked
     * for is the one to keep. */
    if (!d->signalled) {
        d->exit_code = exit_code;
        d->signalled = 1;
        waitq_wake_all(&d->q);
    }
    intr_restore(flags);
}

void thread_object_record_cpu(object_t *obj, uint64 cpu_ticks) {
    dispatcher_t *d;

    if (obj == NULL || obj->type != &thread_type) {
        return;
    }
    d = (dispatcher_t *)obj->body;
    if (d != NULL && d->in_use) {
        d->cpu_ticks = cpu_ticks;             /* the latest word wins */
    }
}

int thread_object_cpu(object_t *obj, uint64 *cpu_ticks) {
    dispatcher_t *d;

    if (obj == NULL || obj->type != &thread_type) {
        return -22;
    }
    d = (dispatcher_t *)obj->body;
    if (d == NULL || !d->in_use) {
        return -22;
    }
    *cpu_ticks = d->cpu_ticks;
    return 0;
}

int thread_object_query(object_t *obj, int *tid, uint32 *exit_code) {
    dispatcher_t *d;

    if (obj == NULL || obj->type != &thread_type) {
        return -22;
    }
    d = (dispatcher_t *)obj->body;
    if (d == NULL || !d->in_use) {
        return -22;
    }
    if (tid != NULL) {
        *tid = d->tid;
    }
    if (exit_code != NULL) {
        *exit_code = d->exit_code;
    }
    return d->signalled;
}

/* --- naming -------------------------------------------------------------- */

#define BNO_PREFIX "\\BaseNamedObjects\\"

static int bno_path(const char *name, char *out, uint32 cap) {
    const char *p = BNO_PREFIX;
    uint32 i = 0;

    if (name == NULL || name[0] == '\0') {
        return -22;
    }
    while (p[i] != '\0') {
        if (i + 1 >= cap) {
            return -36;
        }
        out[i] = p[i];
        i++;
    }
    while (*name != '\0') {
        if (i + 1 >= cap) {
            return -36;                     /* -ENAMETOOLONG */
        }
        out[i++] = *name++;
    }
    out[i] = '\0';
    return 0;
}

int dispatch_create_named(const char *name, object_t *obj) {
    char path[NS_PATH_MAX];
    int rc;

    if (obj == NULL) {
        return -22;
    }
    rc = bno_path(name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    /* ns_insert takes its own reference and reports -EEXIST itself, so this
     * does not pre-check: a look-then-insert would have a window, and the
     * whole point of a named mutex is that two processes racing to create it
     * get one object between them. */
    return ns_insert(path, obj);
}

object_t *dispatch_open_named(const char *name) {
    char path[NS_PATH_MAX];
    ns_entry_t *e;

    if (bno_path(name, path, sizeof(path)) != 0) {
        return NULL;
    }
    e = ns_lookup_entry(path);
    if (e == NULL || e->kind != NS_OBJECT || e->object == NULL) {
        return NULL;
    }
    /* A reference for the caller. The namespace keeps its own, so the object
     * outlives every opener - which is what "named" has to mean. */
    ob_ref(e->object);
    return e->object;
}

/* --- init ---------------------------------------------------------------- */

void dispatch_init(void) {
    /* The four directories ROADMAP item 14 lists as missing. Created even
     * though only the first has anything in it yet: a namespace where
     * \KernelObjects does not exist answers -ENOENT for a path that is simply
     * empty, and those are different questions. */
    (void)ns_mkdir("\\BaseNamedObjects");
    (void)ns_mkdir("\\KernelObjects");
    (void)ns_mkdir("\\ObjectTypes");
    (void)ns_mkdir("\\Sessions");

    (void)ob_register_type(&event_type);
    (void)ob_register_type(&semaphore_type);
    (void)ob_register_type(&mutant_type);
    (void)ob_register_type(&thread_type);

    /* After \ObjectTypes exists, and after the three above are registered -
     * this is the sweep that publishes everything anybody registered before
     * the directory was there. */
    ob_publish_types();
}
