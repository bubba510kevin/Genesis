/* eventhandler(9): named publish/subscribe lists.
 *
 * --- why this had to become real ----------------------------------------
 * It was a shim: EVENTHANDLER_REGISTER expanded to NULL, EVENTHANDLER_DEFINE
 * to nothing, EVENTHANDLER_INVOKE to nothing. The comment on it said the
 * honest accounting was short - the vendored tree registered two handlers,
 * one for flushing fragments when an interface departs and one for vm_lowmem,
 * and neither could fire on this machine.
 *
 * That accounting was wrong, and the way it was wrong is the interesting
 * part. netinet/in.c does NOT call in_ifattach() from anywhere; it registers
 * it:
 *
 *     EVENTHANDLER_DEFINE(ifnet_arrival_event, in_ifattach, NULL, ...);
 *
 * in_ifattach() is what gives an interface its IPv4 data - the link-layer
 * address table ARP resolves into, and the IGMP state. With the event
 * compiled out it never ran, `ifp->if_inet` stayed NULL, and the first thing
 * to touch it was arp_add_ifa_lle() during `ifconfig re0 inet ...`, which
 * page-faulted reading through NULL. The backtrace pointed at ARP. The cause
 * was a subscription that had been silently dropped at compile time.
 *
 * That is the same failure mode SYSINIT had, one layer down, and it has the
 * same fix: build the mechanism rather than keep discovering its absence.
 *
 * --- what this is -------------------------------------------------------
 * Upstream's design, reduced to what has callers. A named list per event, a
 * priority-ordered list of handlers on it, and a lock. Upstream additionally
 * has per-list reference counting so a handler can deregister itself from
 * inside an invocation, and a "list is being destroyed" state; nothing here
 * deregisters at all, so neither is built - and both are named here rather
 * than left as a surprise for the first caller that tries.
 *
 * The registration side of EVENTHANDLER_DEFINE is upstream's own macro,
 * unchanged: it emits a SYSINIT that calls eventhandler_register(). That
 * works because SYSINIT is real (kernel/bsd/sysinit.c).
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/eventhandler.h>

#include "kheap.h"
#include "kprintf.h"

/* The generic entry, plus the untyped function pointer every typed entry
 * carries as its second member. <sys/_eventhandler.h>'s EVENTHANDLER_DECLARE
 * emits `struct eventhandler_entry_<name> { struct eventhandler_entry ee;
 * <type> eh_func; }`, and EVENTHANDLER_INVOKE casts to that. So the layout
 * here has to match, and this is upstream's own definition of the untyped
 * form - it lives in kern/subr_eventhandler.c there. */
struct eventhandler_entry_generic {
    struct eventhandler_entry	ee;
    void			(*func)(void);
};

static TAILQ_HEAD(, eventhandler_list) eventhandler_lists =
    TAILQ_HEAD_INITIALIZER(eventhandler_lists);

/* One lock for every list.
 *
 * Upstream has a global lock for the list-of-lists and a per-list lock, so
 * that invoking one event does not exclude registering on another. Here
 * registration happens at boot and invocation happens afterwards, so the two
 * barely overlap and one lock is the whole requirement.
 *
 * It is a real mutex rather than nothing because invocation genuinely can run
 * on either CPU - ifnet_link_event fires from the NIC's interrupt handler. */
static struct mtx eventhandler_mtx;
static int eventhandler_ready;

static int name_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b);
}

static void eventhandler_setup(void) {
    if (!eventhandler_ready) {
        mtx_init(&eventhandler_mtx, "eventhandler", NULL, MTX_DEF);
        eventhandler_ready = 1;
    }
}

/* Find a list by name, creating it if it does not exist.
 *
 * Created on demand rather than declared, because that is upstream's model:
 * a subscriber and a publisher find each other by string, and either may come
 * first. The name is NOT copied - every caller passes a string literal, which
 * is what the #name in the macros produces. */
static struct eventhandler_list *list_find(const char *name, int create) {
    struct eventhandler_list *list;

    TAILQ_FOREACH(list, &eventhandler_lists, el_link) {
        if (name_eq(list->el_name, name)) {
            return (list);
        }
    }
    if (!create) {
        return (NULL);
    }
    list = (struct eventhandler_list *)kmalloc(sizeof(*list));
    if (list == NULL) {
        return (NULL);
    }
    memset(list, 0, sizeof(*list));
    list->el_name = (char *)(uintptr_t)name;
    /* The list's OWN lock, which EVENTHANDLER_INVOKE takes and drops around
     * each handler call - see the macro in <sys/eventhandler.h>. Separate
     * from the global lock above, which only covers the list-of-lists. */
    mtx_init(&list->el_lock, "eventhandler list", NULL, MTX_DEF);
    TAILQ_INIT(&list->el_entries);
    TAILQ_INSERT_TAIL(&eventhandler_lists, list, el_link);
    return (list);
}

/* Create a list up front. EVENTHANDLER_LIST_DEFINE emits a SYSINIT that calls
 * this, so that EVENTHANDLER_DIRECT_INVOKE can skip the name lookup. */
struct eventhandler_list *eventhandler_create_list(const char *name) {
    struct eventhandler_list *list;

    eventhandler_setup();
    mtx_lock(&eventhandler_mtx);
    list = list_find(name, 1);
    mtx_unlock(&eventhandler_mtx);
    return (list);
}

struct eventhandler_list *eventhandler_find_list(const char *name) {
    struct eventhandler_list *list;

    eventhandler_setup();
    mtx_lock(&eventhandler_mtx);
    list = list_find(name, 0);
    mtx_unlock(&eventhandler_mtx);
    return (list);
}

/* Register a handler.
 *
 * `epn` is the whole entry, already allocated and filled in by the caller -
 * that is upstream's shape, and it is what carries the typed function pointer
 * (see <sys/_eventhandler.h>'s EVENTHANDLER_DECLARE, which declares a
 * per-event struct whose first member is this generic one). Casting to the
 * typed struct on the way out is what makes EVENTHANDLER_INVOKE type-correct
 * rather than a variadic cast.
 *
 * Ordered by priority ascending, so a handler that must run first asks for a
 * lower number. Upstream's rule.
 */
eventhandler_tag eventhandler_register(struct eventhandler_list *list,
                                       const char *name, void *func,
                                       void *arg, int priority) {
    struct eventhandler_entry_generic *eg;
    struct eventhandler_entry *ep, *existing;

    eventhandler_setup();

    eg = (struct eventhandler_entry_generic *)kmalloc(sizeof(*eg));
    if (eg == NULL) {
        kprintf_c(0x0E, "eventhandler: out of memory registering '%s'\n",
                  name != NULL ? name : "?");
        return (NULL);
    }
    memset(eg, 0, sizeof(*eg));
    eg->func = (void (*)(void))func;
    ep = &eg->ee;
    ep->ee_arg = arg;
    ep->ee_priority = priority;

    mtx_lock(&eventhandler_mtx);
    if (list == NULL) {
        list = list_find(name, 1);
    }
    if (list == NULL) {
        mtx_unlock(&eventhandler_mtx);
        kfree(eg);
        return (NULL);
    }
    TAILQ_FOREACH(existing, &list->el_entries, ee_link) {
        if (existing->ee_priority > priority) {
            TAILQ_INSERT_BEFORE(existing, ep, ee_link);
            break;
        }
    }
    if (existing == NULL) {
        TAILQ_INSERT_TAIL(&list->el_entries, ep, ee_link);
    }
    mtx_unlock(&eventhandler_mtx);
    return (ep);
}

/* Deregistration.
 *
 * Removes the entry and frees it. What is NOT here is upstream's protection
 * against deregistering DURING an invocation: upstream marks the entry dead
 * and defers the free until the list's reference count drops, so a walk in
 * progress does not follow a freed link.
 *
 * Nothing in this tree deregisters, so that protection has no caller. It is
 * named because the day something does deregister from inside a handler, this
 * is where the use-after-free will be.
 */
void eventhandler_deregister(struct eventhandler_list *list,
                             eventhandler_tag tag) {
    if (list == NULL || tag == NULL) {
        return;
    }
    mtx_lock(&eventhandler_mtx);
    TAILQ_REMOVE(&list->el_entries, tag, ee_link);
    mtx_unlock(&eventhandler_mtx);
    kfree(tag);
}

void eventhandler_deregister_nowait(struct eventhandler_list *list,
                                    eventhandler_tag tag) {
    eventhandler_deregister(list, tag);
}

/* Upstream's EVENTHANDLER_DIRECT_INVOKE resolves the list once at
 * registration and stores the pointer, so a hot event does not do a string
 * search. EVENTHANDLER_LIST_DEFINE emits that pointer; this prepares it. */
void eventhandler_prune_list(struct eventhandler_list *list) {
    (void)list;     /* nothing is ever marked dead - see deregister above */
}

/* --- report --------------------------------------------------------------
 *
 * Which events exist and how many subscribers each has. Worth printing at
 * boot precisely because the failure this mechanism was built to fix was a
 * subscription that silently did not exist.
 */
void genesis_eventhandler_report(uint8 color) {
    struct eventhandler_list *list;
    struct eventhandler_entry *ep;
    int lists = 0;
    int handlers = 0;

    TAILQ_FOREACH(list, &eventhandler_lists, el_link) {
        int n = 0;

        TAILQ_FOREACH(ep, &list->el_entries, ee_link) {
            n++;
        }
        lists++;
        handlers += n;
        if (n > 0) {
            kprintf_c(color, "  %s: %d handler%s\n", list->el_name, n,
                      n == 1 ? "" : "s");
        }
    }
    kprintf_c(color, "eventhandler: %d lists, %d handlers\n", lists, handlers);
}
