/* Per-device sysctl contexts - dev.<name>.<unit>.<knob>.
 *
 * --- what was missing, and how it presented ------------------------------
 *
 * Two functions: device_get_sysctl_ctx and device_get_sysctl_tree. Real
 * FreeBSD driver source calls them in attach to hang its tunables somewhere,
 * and neither existed here. The failure was not "a knob is missing" - it was
 * that vendsrc/sys/dev/rl/if_rl.c DID NOT COMPILE:
 *
 *   if_rl.c:655: implicit declaration of function 'device_get_sysctl_ctx'
 *   sysctl.h:259: invalid type argument of '->' (have 'int')
 *
 * which stopped modules/build.sh (and therefore the userland build (Genesis-userland's build.sh)) dead at
 * that file, with `set -e`, before the two modules after it. The ROADMAP has
 * carried "the userland build (Genesis-userland's build.sh) exits 1 on the rtl8139 module" as an open item;
 * this is the whole of it. if_re.c calls the same pair in re_add_sysctls.
 *
 * --- where the state lives, and why it is a side table --------------------
 *
 * Not on struct bus_dev. This is FreeBSD-compat state and bus.c's own device
 * model has no notion of it - the same argument, and the same shape, as
 * newbus_compat.c's description table. bus.h stays a file about devices,
 * drivers and resources.
 *
 * The table is here rather than in newbus_compat.c only because of the
 * include environment: struct sysctl_ctx_list is a compat type and
 * kernel/driver/ is not built with -Ikernel/bsd/compat. <sys/bus.h> resolves
 * from here to kernel/include/sys/bus.h, the Newbus shim, which is what
 * miibus.c already relies on - so this one file can see both worlds.
 *
 * --- the naming trap ------------------------------------------------------
 *
 * sysctl_add_oid DOES NOT COPY THE NAME (see the comment there - upstream
 * strdups, this does not, and every caller so far passes a string literal in
 * kernel text). That rules out the obvious implementation of the unit level,
 * which is to snprintf the unit into a buffer: the node would outlive the
 * buffer and the name would dangle. So unit names come out of a static table
 * of decimal strings, which is both correct and cheaper than the alternative.
 *
 * The devclass level is safe for a different reason: device_get_name returns
 * either the device's own name array in bus.c's static pool, or the driver's
 * .name - and a MODULE's driver name lives in the module image, which is why
 * device_sysctl_fini below exists and why bus.c calls it on detach.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/sysctl.h>
#include <sys/bus.h>

#include "kprintf.h"

/* Declared, not included. kernel/include/bus.h has this, and including it
 * here would pull in Genesis's own driver_t alongside <sys/bus.h>'s
 * method-table one - the collision that header's comment exists to prevent.
 * device_t and bus_dev_t are the same incomplete type, so the declaration is
 * identical either way. */
device_t bus_first_device(void);

/* One per device that ever asked. 32 rather than BUS_DEV_MAX (64) because
 * only devices whose DRIVER wants a knob take a slot, and today that is two
 * of them. Exhaustion is not fatal - see device_slot. */
#define DEVSYSCTL_MAX 32

struct devsysctl {
    device_t                dev;      /* NULL for a free slot */
    struct sysctl_ctx_list  ctx;
    struct sysctl_oid      *tree;     /* the dev.<name>.<unit> node */
};

static struct devsysctl slots[DEVSYSCTL_MAX];

/* The root of the device subtree. Created on first use rather than declared
 * with SYSCTL_ROOT_NODE, because a static root would need registering from
 * the boot path at a moment nothing else cares about, and this has exactly
 * one creator. */
static struct sysctl_oid *dev_root;

/* Decimal names for the unit level. See the file comment: the name is not
 * copied, so it has to outlive the node, and a stack buffer does not. */
static const char *const unit_names[64] = {
    "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",
    "8",  "9",  "10", "11", "12", "13", "14", "15",
    "16", "17", "18", "19", "20", "21", "22", "23",
    "24", "25", "26", "27", "28", "29", "30", "31",
    "32", "33", "34", "35", "36", "37", "38", "39",
    "40", "41", "42", "43", "44", "45", "46", "47",
    "48", "49", "50", "51", "52", "53", "54", "55",
    "56", "57", "58", "59", "60", "61", "62", "63"
};

static int name_same(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return a == b;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Find a child node by name, or make one.
 *
 * sysctl_add_oid does NOT do this - it creates unconditionally - so two
 * devices of the same driver would otherwise produce two "dev.rl" nodes.
 * Upstream's sysctl_add_oid does look first; this is that half, kept here
 * rather than changed there because every other caller in the tree relies on
 * being given a fresh node.
 *
 * The node is created with a NULL context on purpose: it is SHARED between
 * every unit of the driver, so it must not be freed when the first one
 * detaches. Nothing frees these, which is a bounded leak - one node per
 * driver name that ever attached - and the alternative is a refcount on a
 * structure that has no other reason to have one. */
static struct sysctl_oid *child_named(struct sysctl_oid_list *parent,
                                      const char *name,
                                      struct sysctl_ctx_list *ctx) {
    struct sysctl_oid *o;

    RB_FOREACH(o, sysctl_oid_list, parent) {
        if (name_same(o->oid_name, name)) {
            return o;
        }
    }
    return sysctl_add_oid(ctx, parent, OID_AUTO, name,
                          CTLTYPE_NODE | CTLFLAG_RW, NULL, 0, NULL, "N",
                          NULL, NULL);
}

static struct devsysctl *device_slot(device_t dev) {
    int i;
    int free_slot = -1;
    const char *name;
    int unit;
    struct devsysctl *ds;
    struct sysctl_oid *dcnode;

    if (dev == NULL) {
        return NULL;
    }
    for (i = 0; i < DEVSYSCTL_MAX; i++) {
        if (slots[i].dev == dev) {
            return &slots[i];
        }
        if (slots[i].dev == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        /* Reported, not silent, and not fatal: the driver's knobs are
         * missing but its attach carries on. A halt here would take the
         * machine down over a tunable nobody reads. */
        kprintf_c(0x0C, "devsysctl: DEVSYSCTL_MAX (%d) reached - no sysctl "
                        "node for %s\n", DEVSYSCTL_MAX, device_get_name(dev));
        return NULL;
    }

    if (dev_root == NULL) {
        dev_root = sysctl_add_oid(NULL, NULL, OID_AUTO, "dev",
                                  CTLTYPE_NODE | CTLFLAG_RW, NULL, 0, NULL,
                                  "N", "Devices", NULL);
        if (dev_root == NULL) {
            return NULL;
        }
    }

    ds = &slots[free_slot];
    ds->dev = dev;
    sysctl_ctx_init(&ds->ctx);

    name = device_get_name(dev);
    if (name == NULL || name[0] == '\0') {
        name = "unknown";
    }
    unit = device_get_unit(dev);
    if (unit < 0 || unit >= (int)(sizeof(unit_names) / sizeof(unit_names[0]))) {
        /* An unnamed device has unit -1. Upstream would not have a tree for
         * it at all; here it gets unit 0's name, because the alternative is
         * returning NULL from device_get_sysctl_tree and every caller
         * immediately dereferences it through SYSCTL_CHILDREN. */
        unit = 0;
    }

    dcnode = child_named(SYSCTL_CHILDREN(dev_root), name, NULL);
    if (dcnode == NULL) {
        ds->dev = NULL;
        return NULL;
    }
    /* The unit node DOES belong to this device's context, so detaching the
     * device takes it away again. */
    ds->tree = child_named(SYSCTL_CHILDREN(dcnode), unit_names[unit],
                           &ds->ctx);
    if (ds->tree == NULL) {
        ds->dev = NULL;
        return NULL;
    }
    return ds;
}

struct sysctl_ctx_list *device_get_sysctl_ctx(device_t dev) {
    struct devsysctl *ds = device_slot(dev);

    return ds == NULL ? NULL : &ds->ctx;
}

struct sysctl_oid *device_get_sysctl_tree(device_t dev) {
    struct devsysctl *ds = device_slot(dev);

    return ds == NULL ? NULL : ds->tree;
}

/* Free everything a device hung off its context, and give the slot back.
 *
 * Called by bus.c when a driver is detached, and that is not housekeeping -
 * it is the reason this function exists. sysctl_add_oid does not copy the
 * name, and a knob added by a MODULE names itself with a string literal in
 * that module's .rodata. Leave the node registered after the module is
 * unloaded and the sysctl tree holds a char* into unmapped memory, which the
 * next walk of the tree reads. oid_arg1 is worse: it points at the driver's
 * softc, and a reader would follow it.
 *
 * Safe on a device that never asked for a context. */
void device_sysctl_fini(device_t dev) {
    int i;

    for (i = 0; i < DEVSYSCTL_MAX; i++) {
        if (slots[i].dev == dev && dev != NULL) {
            (void)sysctl_ctx_free(&slots[i].ctx);
            slots[i].tree = NULL;
            slots[i].dev  = NULL;
            return;
        }
    }
}

/* How many sysctl nodes have any pointer into [base, base+size)?
 *
 * The fifth registry kld_unload sweeps, and the one its comment used to name
 * as the example of what it could NOT check. Four pointers per node can point
 * into a module and each is read by a different thing: oid_name (read by any
 * walk of the tree), oid_handler (called), oid_arg1 (followed by the handler,
 * and usually the driver's softc), and the struct sysctl_oid itself, whose
 * RB links are walked by everything.
 *
 * Recursive over the tree rather than over a flat list, because that is the
 * shape the tree has; depth is bounded by the tree's own depth, which is
 * three for a device knob (dev / name / unit / leaf). */
static int oid_list_count(struct sysctl_oid_list *list, uint64_t base,
                          uint64_t size) {
    struct sysctl_oid *o;
    int n = 0;

    RB_FOREACH(o, sysctl_oid_list, list) {
        uint64_t self = (uint64_t)o;
        uint64_t nm   = (uint64_t)o->oid_name;
        uint64_t hd   = (uint64_t)o->oid_handler;
        uint64_t a1   = (uint64_t)o->oid_arg1;

        if ((self >= base && self < base + size) ||
            (nm != 0 && nm >= base && nm < base + size) ||
            (hd != 0 && hd >= base && hd < base + size) ||
            (a1 != 0 && a1 >= base && a1 < base + size)) {
            n++;
        }
        n += oid_list_count(&o->oid_children, base, size);
    }
    return n;
}

int sysctl_count_in_range(uint64_t base, uint64_t size) {
    extern struct sysctl_oid_list sysctl__children;

    if (size == 0) {
        return 0;
    }
    return oid_list_count(&sysctl__children, base, size);
}

/* --- are those two real? -------------------------------------------------
 *
 * Both directions against a range one object wide, for the reason
 * kld_sweep_selftest.c spells out: a sweep that answered zero for everything
 * would pass a negative-only test, and zero-for-everything is what turns
 * kld_unload's refusal into a rubber stamp.
 *
 * The object put in range is the ARG, not the handler - a knob's arg1 is the
 * driver's softc, which is the pointer that actually lives in the module, and
 * a sweep that only looked at oid_handler would miss it. Same choice, and the
 * same reason, as the irq sweep's ctx.
 *
 * And device_sysctl_fini is checked by the third assertion rather than
 * separately: the node is gone from the tree afterwards, which is the only
 * thing "it was freed" can mean from outside. */
static int devsysctl_marker;

int devsysctl_selftest(void);

int devsysctl_selftest(void) {
    int failures = 0;
    device_t dev = NULL;
    struct sysctl_ctx_list *ctx;
    struct sysctl_oid *tree;
    uint64_t base = (uint64_t)&devsysctl_marker;
    uint64_t size = sizeof(devsysctl_marker);

#define DST(cond, what)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            kprintf_c(0x0C, "devsysctl: %s\n", (what));                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

    DST(sysctl_count_in_range(base, size) == 0,
        "the sweep found a node before one was added");

    /* A device is needed, and a real one would mean attaching a driver. The
     * table is keyed on the pointer and never dereferences it, so a distinct
     * non-NULL value that is not a real device is enough - EXCEPT that
     * device_get_name and device_get_unit do dereference it. So use a real
     * one: the first child bus.c has. */
    dev = bus_first_device();
    if (dev == NULL) {
        kprintf_c(0x0E, "devsysctl: no devices - sweep NOT exercised\n");
        goto done;
    }

    ctx  = device_get_sysctl_ctx(dev);
    tree = device_get_sysctl_tree(dev);
    DST(ctx != NULL && tree != NULL,
        "a device got no sysctl context or no tree");
    if (ctx == NULL || tree == NULL) {
        goto done;
    }
    /* The same device asked twice must get the SAME context, or every attach
     * that calls both accessors would build two trees. */
    DST(device_get_sysctl_ctx(dev) == ctx && device_get_sysctl_tree(dev) == tree,
        "a second call built a second context for one device");

    SYSCTL_ADD_INT(ctx, SYSCTL_CHILDREN(tree), OID_AUTO, "genesis_selftest",
                   CTLFLAG_RD, &devsysctl_marker, 0, "");
    DST(sysctl_count_in_range(base, size) == 1,
        "the sweep missed a node whose arg is in range");

    device_sysctl_fini(dev);
    DST(sysctl_count_in_range(base, size) == 0,
        "the node survived device_sysctl_fini");

done:
    if (failures == 0) {
        kprintf("devsysctl: selftest passed\n");
    } else {
        kprintf_c(0x0C, "devsysctl: selftest FAILED (%d)\n", failures);
    }
    return failures;
#undef DST
}
