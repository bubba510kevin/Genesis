/* SYSINIT: the boot-time initialiser table, and the code that walks it.
 *
 * --- why this exists now, when it deliberately did not before ------------
 * <sys/systm.h> used to define SYSINIT to nothing, with a comment saying
 * Genesis has no link sets and that objcopy would flatten one away in any
 * case. The first half was true and the second half was wrong: `objcopy -O
 * binary` keeps every ALLOCATED section's contents and drops only the section
 * table, and ld's __start_/__stop_ symbols are resolved at link time. So a
 * link set survives perfectly well - kernel/lib/ksyms_data.c has relied on
 * exactly that property since the symbol table was added.
 *
 * The reason to build it is the failure mode the old arrangement had. An
 * initialiser declared by a vendored file was silently DROPPED, and had to be
 * re-discovered by hand and called from somewhere. That cost real time twice:
 * once for uma_startup1/uma_startup2, and once for ether_init and
 * vnet_ether_init - where the missing call looked exactly like working
 * hardware that received nothing, because netisr had no NETISR_ETHER handler
 * and every frame was freed one layer below where anyone was looking.
 *
 * With the whole IPv4 stack vendored there are now around forty of them.
 * Finding forty by hand is not a thing to do once, let alone every time a
 * file is added.
 *
 * --- what this is NOT ---------------------------------------------------
 * It is not a replacement for kernel/flk.c. flk.c stays the readable,
 * hand-written statement of what comes before what, and that is a deliberate
 * property of this kernel. What runs here is the initialisation of the
 * VENDORED subsystems only - the ones that declared themselves through
 * upstream's mechanism - and flk.c decides at which point in its own sequence
 * that happens.
 *
 * So: one call, from one place, at a point flk.c chooses. Not magic; a
 * function with a name, like everything else in that file.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/linker_set.h>
#include <sys/module.h>

#include "kprintf.h"
#include "ksyms.h"

/* The set every SYSINIT() lands in. Upstream's name, because
 * <sys/kernel.h> - which is vendored - is what puts entries there. */
SET_DECLARE(sysinit_set, struct sysinit);
SET_DECLARE(sysuninit_set, struct sysinit);

static int sysinit_ran;
static int sysinit_count;

/* --- ordering -----------------------------------------------------------
 *
 * Entries are sorted by (subsystem, order), both of which are enum values
 * from <sys/kernel.h> chosen to be sparse so a new subsystem can be inserted
 * between two existing ones. SI_SUB_MBUF is 0x2700000, SI_SUB_PROTO_DOMAIN is
 * 0x8800000; the gaps are the point.
 *
 * The sort is an insertion sort over an array of pointers, done in place at
 * boot. n is around forty and this runs once, so the algorithm does not
 * matter; what matters is that it is STABLE, because two entries with the
 * same (subsystem, order) must run in link order, and upstream relies on that
 * for the several protocols that register at SI_SUB_PROTO_DOMAIN /
 * SI_ORDER_ANY.
 *
 * Upstream sorts the same way, in kern/init_main.c's mi_startup(), and for
 * the same reason: the linker emits set entries in whatever order the objects
 * appeared on the command line, which is alphabetical here (build.py's
 * sources() sorts) and is therefore stable but meaningless.
 */
static void sort_entries(struct sysinit **first, struct sysinit **last) {
    struct sysinit **p, **q;

    for (p = first + 1; p < last; p++) {
        struct sysinit *key = *p;

        for (q = p; q > first; q--) {
            struct sysinit *prev = *(q - 1);

            if (prev->subsystem < key->subsystem ||
                (prev->subsystem == key->subsystem &&
                 prev->order <= key->order)) {
                break;
            }
            *q = prev;
        }
        *q = key;
    }
}

/* Run every registered initialiser, in order.
 *
 * Idempotent: calling it twice does nothing the second time. That is not
 * defensive padding - flk.c's ordering has moved before and an initialiser
 * run twice is a duplicate zone, a duplicate netisr registration, or a
 * double-linked domain, none of which announce themselves.
 */
void genesis_sysinit_run(void) {
    struct sysinit **sipp;

    if (sysinit_ran) {
        return;
    }
    sysinit_ran = 1;

    sort_entries(SET_BEGIN(sysinit_set), SET_LIMIT(sysinit_set));

    SET_FOREACH(sipp, sysinit_set) {
        struct sysinit *si = *sipp;

        if (si->func == NULL) {
            continue;
        }
        /* SI_SUB_DUMMY is upstream's "never executed; for the linker" entry -
         * it exists so the set is non-empty in a kernel with no other
         * SYSINIT, which is a real configuration upstream and would otherwise
         * leave __start_ and __stop_ undefined. */
        if (si->subsystem == SI_SUB_DUMMY) {
            continue;
        }
        si->func(si->udata);
        sysinit_count++;
    }
}

int genesis_sysinit_count(void) {
    return (sysinit_count);
}

/* The set entry SI_SUB_DUMMY exists for.
 *
 * Without at least one member, `set_sysinit_set` is an empty section, ld does
 * not emit it, and __start_set_sysinit_set / __stop_set_sysinit_set are
 * undefined at link time. Upstream has the same entry in kern/init_main.c for
 * the same reason. */
static void sysinit_placeholder(const void *dummy __unused) {
}
SYSINIT(placeholder, SI_SUB_DUMMY, SI_ORDER_ANY, sysinit_placeholder, NULL);

/* --- report -------------------------------------------------------------
 *
 * Prints the count and the subsystem range covered. Not the whole list: forty
 * lines at boot is noise, and what is worth knowing is that the set was found
 * at all - an empty set means the linker script dropped the section, which is
 * the one failure this mechanism has.
 */
void genesis_sysinit_report(uint8 color) {
    struct sysinit **first = SET_BEGIN(sysinit_set);
    struct sysinit **last = SET_LIMIT(sysinit_set);
    long total = last - first;

    if (total <= 1) {
        kprintf_c(0x0C, "sysinit: THE LINK SET IS EMPTY (%d entries) - "
                        "linker.ld is not keeping set_sysinit_set\n",
                  (int)total);
        return;
    }
    kprintf_c(color, "sysinit: %d of %d initialisers ran, subsystems %x..%x\n",
              sysinit_count, (int)total,
              (unsigned)(*first)->subsystem, (unsigned)(*(last - 1))->subsystem);
}

/* The SYSINIT that <sys/module.h>'s DECLARE_MODULE expands to: deliver
 * MOD_LOAD to a module linked into the kernel. A handler's failure is
 * printed, not fatal - upstream's module_register_init does the same. */
void genesis_module_load(const void *moddata) {
    const moduledata_t *md = moddata;
    int error;

    if (md->evhand == NULL) {
        return;
    }
    error = md->evhand(NULL, MOD_LOAD, md->priv);
    if (error != 0) {
        kprintf_c(0x0C, "module %s: MOD_LOAD failed, error %d\n",
                  md->name, error);
    }
}
