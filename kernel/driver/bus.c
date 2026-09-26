#include "bus.h"
#include "klock.h"
#include "kheap.h"
#include "kprintf.h"
#include "pci.h"
#include "screen.h"
#include "typesk.h"

/* The matching engine described in bus.h.
 *
 * Item 11 named five gaps in this file's "reduction of Newbus". Four are
 * closed here - a general method table rather than three extracted function
 * pointers (the KOBJ generalization, in newbus_compat.c and reached through
 * driver_t.methods), real bus_alloc_resource/bus_release_resource with
 * conflict detection, multi-pass boot ordering, and devclass inheritance -
 * and the fifth, the widened BUS_PROBE_* scale, is in bus.h with a note on
 * the half of NOWILDCARD that still needs a device-hint mechanism.
 *
 * Still deliberately absent: hot-remove (nothing frees a bus_dev_t slot; PCI
 * devices do not disappear at runtime here) and real KOBJ codegen.
 */

/* Raised from 8, which was not enough to finish booting with the data disk
 * attached - and the symptom was the worst kind, because devclass_find HALTS
 * on exhaustion rather than returning NULL (see bus.h: it is a build-time
 * sizing bug, not a runtime condition). The machine printed "bus:
 * DEVCLASS_MAX exceeded" during kld_load_directories and stopped, before sti,
 * with every selftest after it unreached.
 *
 * What was actually consuming them is worth writing down, because raising the
 * number does not address it: FOUR OF THE EIGHT WERE THE BUS SELFTEST'S.
 * bus_selftest.c calls devclass_find for "st_pass", "st_child", "st_inherit"
 * and "st_nowild", nothing ever frees a devclass slot (there is no hot-remove
 * here, by design), and a test's devclass is indistinguishable from a real
 * one afterwards. So the budget available to actual drivers was
 * DEVCLASS_MAX - 4, and "pci" plus "miibus" plus what a loadable NIC module
 * brings reached it exactly.
 *
 * Thirty-two rather than twelve, for the reason the pools here are all
 * generous: a devclass is a name and a small driver array, the cost is bytes
 * of .bss, and the failure mode of guessing low is a halt with no output
 * rather than a degraded boot. bus_devclass_report prints the live count at
 * boot so the headroom is visible instead of being rediscovered this way. */
#define DEVCLASS_MAX          32

/* Raised from 8 when the first real vendored driver landed. The PCI devclass
 * holds pci_generic, three model demos, test.sys and then whatever loadable
 * modules bring - which reached the old cap exactly, and the symptom was
 * "kld: LKPIAHCI.KO: init returned -1" from a module whose driver simply
 * could not be added. The honest error path worked; the limit was wrong. */
#define DRIVERS_PER_CLASS_MAX 16
#define BUS_DEV_MAX           64
#define BUS_RESOURCE_MAX      64

/* How deep devclass_set_parent chains are allowed to get before bus.c
 * calls it a cycle. Small on purpose: a legitimate chain here is two or
 * three ("pci_bridge" -> "pci"), and the check exists to stop a mistake
 * from hanging the probe loop, not to support deep hierarchies. */
#define DEVCLASS_DEPTH_MAX    8

struct devclass {
    char             name[16];
    driver_t        *drivers[DRIVERS_PER_CLASS_MAX];
    int              driver_count;
    struct devclass *parent;

    /* Has the boot-time attach pass run over devices in THIS devclass?
     *
     * Per-devclass, not one flag for the whole file, and that distinction is
     * not hypothetical - it was a bug. kernel/driver/bus_selftest.c calls
     * bus_attach_children on its own synthetic devclass, and it runs early in
     * flk.c, long before the PCI drivers register. A single global flag was
     * therefore already set by the time pci_generic registered, so
     * bus_driver_added did real work during boot registration and attached
     * the first-registered driver - pci_generic, which matches everything -
     * to every function on the bus before the three real demo drivers
     * existed. */
    int              attached;
};

struct bus_dev {
    bus_dev_t       *parent;
    devclass_t       devclass;
    driver_t         *driver;
    void             *ivars;
    void             *softc;
    bus_dev_state_t  state;

    /* --- IDENTITY (Part 15) ---------------------------------------------
     *
     * `devclass` above says WHOSE DRIVERS TO TRY. It does not say what this
     * device IS. Upstream's device_add_child(parent, "foo", unit) gives a
     * device a name and a unit number, and its `hasclass` flag records
     * whether something NAMED it or whether it was merely enumerated by its
     * parent bus - which is the test BUS_PROBE_NOWILDCARD turns on.
     *
     * unit is -1 until a name is set. name[0] == '\0' means unnamed, which
     * is the state every device is in when the bus enumerates it. */
    char             name[BUS_DEVNAME_MAX];
    int              unit;
    int              hasclass;   /* named explicitly, not just enumerated  */

    /* The BUS_PROBE_* score the currently attached driver won with. Read by
     * try_replace to decide whether a driver arriving later is better -
     * "better" has to mean something, and without this it could only mean
     * "arrived later". */
    int              attach_pri;
};

struct resource {
    bus_dev_t *owner;       /* NULL for a free slot */
    int        type;        /* SYS_RES_*                */
    int        rid;
    uint64     start;
    uint64     count;
    uint32     flags;

    /* The kernel virtual address a MEMORY resource has been mapped at, or
     * NULL if it has not been mapped yet. Cached on the resource because
     * bus_read_4 is called in a loop by driver code and ioremapping per
     * access would be absurd. Meaningless for an I/O-port resource, where
     * the port number is the address. */
    void      *mapping;
};

static struct devclass devclass_pool[DEVCLASS_MAX];
static int             devclass_count;

/* Children in the order bus_add_child created them - a flat pool rather
 * than a per-parent linked list, since the only walk this pass needs is
 * bus_attach_children's "every child of this one bus", and that gets it
 * from a linear scan with no pointers to maintain. Nothing frees a slot:
 * PCI devices do not disappear at runtime in this pass, the same honesty
 * device.h's dev_free comment gives for device_t. */
static bus_dev_t bus_dev_pool[BUS_DEV_MAX];
static int       bus_dev_count;

static struct resource resource_pool[BUS_RESOURCE_MAX];

static int str_eq(const char *a, const char *b) {
    uint64 i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return a[i] == b[i];
}

/* The registration and resource pools.
 *
 * bus.c's pools - devclasses, drivers per devclass, device nodes, and the
 * resource list - are all fixed arrays with a bump counter, and every one of
 * them is a read-modify-write with no atomicity. Two CPUs registering a
 * driver at once would both read the same count and both write index N.
 *
 * Nothing does that today: registration happens once, from flk.c, on the
 * BSP. This is here because item 11's whole point is to stop leaving a
 * simplification in place after its precondition is gone - "only one CPU
 * ever calls this" stopped being a fact about the machine and became an
 * assumption about the call sites the moment Part 10 landed.
 *
 * One lock for all four pools rather than four. They are touched together
 * (bus_add_child takes a node and reads a devclass) and never in a hot path,
 * so splitting them would add lock-ordering rules to protect nothing. */
static mtx_t bus_mtx = { 0, 0, "bus", 0xFFFFFFFFu, 0, 0 };

/* Each public entry point below is a thin lock/call/unlock wrapper around a
 * _locked twin holding the original body. Split this way rather than by
 * taking the lock inside the body, so that every return path is covered by
 * construction - the kh_calloc_locked bug this pass found was exactly an
 * early return that skipped an unlock, and bus_alloc_resource has five. */
static int             devclass_add_driver_locked(devclass_t dc, driver_t *drv);
static bus_dev_t      *bus_add_child_locked(bus_dev_t *parent, devclass_t dc,
                                            void *ivars);
static bus_resource_t *bus_alloc_resource_locked(bus_dev_t *dev, int type,
                                                 int *rid, uint64 start,
                                                 uint64 end, uint64 count,
                                                 uint32 flags);
static int             bus_release_resource_locked(bus_dev_t *dev, int type,
                                                   int rid, bus_resource_t *r);

devclass_t devclass_find(const char *name) {
    int i;
    struct devclass *dc;
    int k;

    for (i = 0; i < devclass_count; i++) {
        if (str_eq(devclass_pool[i].name, name)) {
            return &devclass_pool[i];
        }
    }

    if (devclass_count >= DEVCLASS_MAX) {
        /* Sized wrong at build time, not a runtime condition - see bus.h. */
        print_string("bus: DEVCLASS_MAX exceeded\n", 0x0C);
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    dc = &devclass_pool[devclass_count++];
    for (k = 0; k + 1 < (int)sizeof(dc->name) && name[k] != '\0'; k++) {
        dc->name[k] = name[k];
    }
    dc->name[k] = '\0';
    dc->driver_count = 0;
    return dc;
}

int devclass_add_driver(devclass_t dc, driver_t *drv) {
    int rc;
    kmtx_lock(&bus_mtx);
    rc = devclass_add_driver_locked(dc, drv);
    kmtx_unlock(&bus_mtx);
    return rc;
}

static int devclass_add_driver_locked(devclass_t dc, driver_t *drv) {
    if (dc->driver_count >= DRIVERS_PER_CLASS_MAX) {
        return -1;
    }
    dc->drivers[dc->driver_count++] = drv;
    return 0;
}

int devclass_set_parent(devclass_t dc, devclass_t parent) {
    devclass_t walk;
    int depth;

    if (dc == NULL) {
        return -1;
    }

    /* Refuse a cycle rather than discover it as a hang in the probe loop.
     * Walking the proposed parent's own chain back to `dc` is the whole
     * check: if dc is anywhere above parent, linking them closes a loop. */
    for (walk = parent, depth = 0; walk != NULL; walk = walk->parent) {
        if (walk == dc) {
            return -1;
        }
        if (++depth > DEVCLASS_DEPTH_MAX) {
            return -1;
        }
    }

    dc->parent = parent;
    return 0;
}

bus_dev_t *bus_add_child(bus_dev_t *parent, devclass_t dc, void *ivars) {
    bus_dev_t *d;
    kmtx_lock(&bus_mtx);
    d = bus_add_child_locked(parent, dc, ivars);
    kmtx_unlock(&bus_mtx);
    return d;
}

static bus_dev_t *bus_add_child_locked(bus_dev_t *parent, devclass_t dc, void *ivars) {
    bus_dev_t *d;

    if (bus_dev_count >= BUS_DEV_MAX) {
        return NULL;
    }
    d = &bus_dev_pool[bus_dev_count++];
    /* Unnamed and unclaimed. A bus enumerating a device says whose drivers
     * to try, not what the device is - see the identity fields in
     * struct bus_dev. */
    d->name[0]  = '\0';
    d->unit     = -1;
    d->hasclass = 0;
    d->parent   = parent;
    d->devclass = dc;
    d->driver   = NULL;
    d->ivars    = ivars;
    d->softc    = NULL;
    d->state    = DS_NOTPRESENT;
    return d;
}

/* Bind `drv` to `dev`, allocating its softc. Mirrors FreeBSD's
 * device_set_driver: rebinding frees whatever softc the previous driver
 * (if any) held first, so a losing candidate never leaks its tentative
 * allocation into the winner's slot. drv may be NULL to unbind and free
 * without binding anything new. */
static void bind_driver(bus_dev_t *dev, driver_t *drv) {
    if (dev->softc != NULL) {
        kfree(dev->softc);
        dev->softc = NULL;
    }
    dev->driver = drv;
    if (drv != NULL && drv->softc_size > 0) {
        dev->softc = kcalloc(1, drv->softc_size);
    }
}

/* A driver's effective pass. Zero means "field never set" and gets
 * BUS_PASS_DEFAULT - see the BUS_PASS_* comment in bus.h for why zero
 * cannot be taken at face value as BUS_PASS_ROOT. */
static int driver_pass(const driver_t *drv) {
    return drv->pass == 0 ? BUS_PASS_DEFAULT : drv->pass;
}

/* Try every driver on `dc`, then on its parent devclass, and so on.
 *
 * Devclass inheritance is a fallback and not a merge: the parent's drivers
 * are only tried if nothing registered directly on the child matched at
 * all. That is upstream's behaviour and it is the useful one - a
 * "pci_bridge" devclass carrying one specific driver should not have to
 * out-rank every generic "pci" driver to win its own children.
 *
 * `pass` gates which drivers are eligible; a driver that wants a later pass
 * is skipped entirely this time round and gets its chance when
 * bus_attach_children comes back.
 */
static driver_t *probe_best(bus_dev_t *dev, devclass_t dc, int pass,
                            int *out_pri) {
    int depth;

    for (depth = 0; dc != NULL && depth <= DEVCLASS_DEPTH_MAX;
         dc = dc->parent, depth++) {
        driver_t *best = NULL;
        int       best_pri = 0;
        int       i;

        for (i = 0; i < dc->driver_count; i++) {
            driver_t *drv = dc->drivers[i];
            int result;

            if (driver_pass(drv) > pass) {
                continue;
            }
            if (drv->probe == NULL) {
                continue;
            }

            bind_driver(dev, drv);
            result = drv->probe(dev);

            if (result == BUS_PROBE_SPECIFIC) {
                *out_pri = result;
                return drv;             /* certain match, stop looking */
            }
            if (result > 0) {
                continue;                /* "not mine"; the next iteration's
                                             bind_driver (or the no-match
                                             path in the caller) frees this
                                             tentative softc */
            }
            /* THE NOWILDCARD GATE (Part 16).
             *
             * Part 4 added the value with upstream's number and got the
             * NUMERIC half right - such a driver ranks below everything,
             * including BUS_PROBE_HOOVER. Upstream means more than that.
             * subr_bus.c's device_probe_child skips any driver returning
             * <= BUS_PROBE_NOWILDCARD UNLESS the device `hasclass` - that
             * is, unless something explicitly NAMED which driver this
             * device is, rather than the device being matched by wildcard
             * probing.
             *
             * That test needs device identity, which bus_add_child could
             * not express until Part 15: a devclass says whose drivers to
             * TRY, not what a device IS. It is why this was scheduled here
             * and recorded in ROADMAP's Owed section rather than faked.
             *
             * Note the direction. This does NOT mean "only try NOWILDCARD
             * drivers on named devices" - it means a NOWILDCARD driver
             * declines to be chosen for a device nobody named, however
             * enthusiastically it probed. Its own probe still ran; what is
             * refused is letting it win. */
            if (result <= BUS_PROBE_NOWILDCARD && !device_has_class(dev)) {
                continue;
            }
            if (best == NULL || result > best_pri) {
                best = drv;
                best_pri = result;
            }
        }

        if (best != NULL) {
            *out_pri = best_pri;
            return best;
        }
        /* Nothing here matched - fall through to the parent devclass. */
    }

    *out_pri = 0;
    return NULL;
}

/* Probe and attach `dev`, considering only drivers eligible in `pass`. */
static int probe_and_attach_pass(bus_dev_t *dev, int pass) {
    driver_t *best;
    int       best_pri;
    int       rc;

    if (dev->devclass == NULL || dev->state == DS_ATTACHED) {
        return 0;
    }

    best = probe_best(dev, dev->devclass, pass, &best_pri);

    if (best == NULL) {
        bind_driver(dev, NULL);
        dev->state = DS_NOTPRESENT;
        return 0;
    }
    if (best->attach == NULL) {
        bind_driver(dev, NULL);
        dev->state = DS_NOTPRESENT;
        return 0;
    }

    /* The trial loop leaves dev bound to whichever candidate ran LAST,
     * not necessarily the best one (a worse-priority driver tried after
     * the winner overwrote the binding). Re-bind only if that happened -
     * the common case of one driver per devclass, or the winner being
     * tried last, needs no extra alloc/free. */
    if (dev->driver != best) {
        bind_driver(dev, best);
    }
    dev->state = DS_ATTACHING;
    rc = best->attach(dev);
    if (rc != 0) {
        bind_driver(dev, NULL);
        dev->state = DS_NOTPRESENT;
        return rc;
    }
    /* Remembered so try_replace can tell whether a driver arriving later is
     * genuinely better. Re-running probe_best would give the same answer
     * only as long as no probe depends on state attach has since changed. */
    dev->attach_pri = best_pri;
    dev->state = DS_ATTACHED;
    return 0;
}

/* The public single-device entry point. Runs every pass against one device,
 * so a caller that only has one child to bring up gets the same ordering
 * guarantees bus_attach_children gives. */
int bus_probe_and_attach(bus_dev_t *dev) {
    static const int passes[] = {
        BUS_PASS_ROOT, BUS_PASS_BUS, BUS_PASS_CPU, BUS_PASS_RESOURCE,
        BUS_PASS_INTERRUPT, BUS_PASS_TIMER, BUS_PASS_SCHEDULER,
        BUS_PASS_SUPPORTDEV, BUS_PASS_DEFAULT
    };
    int p;

    for (p = 0; p < (int)(sizeof(passes) / sizeof(passes[0])); p++) {
        int rc = probe_and_attach_pass(dev, passes[p]);

        if (dev->state == DS_ATTACHED) {
            return 0;
        }
        if (rc != 0) {
            /* attach() ran and failed. Not "no driver yet" - a later pass
             * re-probing would call the same failing attach again. */
            return rc;
        }
    }
    return 0;
}

void bus_attach_children(bus_dev_t *parent) {
    static const int passes[] = {
        BUS_PASS_ROOT, BUS_PASS_BUS, BUS_PASS_CPU, BUS_PASS_RESOURCE,
        BUS_PASS_INTERRUPT, BUS_PASS_TIMER, BUS_PASS_SCHEDULER,
        BUS_PASS_SUPPORTDEV, BUS_PASS_DEFAULT
    };
    int p;
    int i;

    /* Passes outermost, children innermost - which is the whole point. The
     * old single loop attached each child completely before looking at the
     * next one, so "driver A's probe needs driver B attached" depended on
     * B's device coming earlier in the pool AND B's driver being registered
     * first. Now every BUS_PASS_RESOURCE driver on every child is attached
     * before any BUS_PASS_DEFAULT one is probed, whatever order either was
     * registered or enumerated in. */
    for (p = 0; p < (int)(sizeof(passes) / sizeof(passes[0])); p++) {
        for (i = 0; i < bus_dev_count; i++) {
            if (bus_dev_pool[i].parent == parent &&
                bus_dev_pool[i].state != DS_ATTACHED) {
                probe_and_attach_pass(&bus_dev_pool[i], passes[p]);
            }
        }
    }

    /* From here on, a driver registering into any devclass these children
     * belong to is a LATE arrival, and bus_driver_added does real work. */
    for (i = 0; i < bus_dev_count; i++) {
        if (bus_dev_pool[i].parent == parent &&
            bus_dev_pool[i].devclass != NULL) {
            bus_dev_pool[i].devclass->attached = 1;
        }
    }
}

/* A better driver than the one currently attached has arrived. Swap it in,
 * if that can be done safely.
 *
 * "Safely" is the whole question, and the answer here is: only if the
 * incumbent provides a detach method. A driver with no detach is not being
 * lazy - it is saying it cannot be taken off the device, because nothing
 * knows what it registered. Tearing one out anyway would leave its interrupt
 * handlers installed and its resources allocated, pointing at a softc that
 * bind_driver is about to free.
 *
 * This is what lets a real driver loaded from a module displace the generic
 * reporting driver that claimed the device at boot, which is the normal case
 * on both upstreams - and it is why kernel/dev/pci_generic.c now has a
 * detach method where it used to have NULL. */
static void try_replace(bus_dev_t *dev, int pass) {
    driver_t   *best;
    int         best_pri = 0;
    driver_t   *incumbent = dev->driver;
    void       *saved_softc;
    const char *old_name;
    int         old_pri;

    /* A driver with no detach method is not being lazy - it is saying it
     * cannot be taken off the device, because nothing knows what it
     * registered. Tearing one out anyway would leave its interrupt handlers
     * installed and its resources allocated, pointing at a softc that
     * bind_driver is about to free. */
    if (incumbent == NULL || incumbent->detach == NULL) {
        return;
    }
    old_name = incumbent->name;
    old_pri  = dev->attach_pri;

    /* --- the trial, with the incumbent's state put aside ------------------
     *
     * probe_best is DESTRUCTIVE: it calls bind_driver before each candidate's
     * probe, which frees dev->softc and repoints dev->driver, and it leaves
     * both pointing at whichever candidate ran last. That is fine on a device
     * being brought up for the first time - the only thing that ever called
     * it before - and catastrophic on a live one, because the attached
     * driver's softc is freed underneath it while it is still registered.
     *
     * Found the hard way: the first version called probe_best directly and
     * the boot log came out claiming a driver had replaced one that was
     * never on that device. The name being read back was simply the last
     * candidate the trial loop had bound.
     *
     * dev->softc is cleared first so bind_driver's free() has nothing to
     * find, and restored afterwards. */
    saved_softc = dev->softc;
    dev->softc  = NULL;
    best = probe_best(dev, dev->devclass, pass, &best_pri);
    if (dev->softc != NULL) {
        kfree(dev->softc);      /* the trial loop's last tentative softc */
    }
    dev->driver = incumbent;
    dev->softc  = saved_softc;

    /* STRICTLY better. An equal score must not swap, or two drivers of the
     * same priority would take the device from each other on every call. */
    if (best == NULL || best == incumbent || best->attach == NULL ||
        best_pri <= old_pri) {
        return;
    }

    /* And it must be a SPECIFIC claim, not a wildcard.
     *
     * Taking a device away from a driver that is working requires the
     * challenger to have actually recognised the hardware. A driver that
     * matches everything has recognised nothing - it is only ranked above
     * pci_generic because pci_generic ranks last, and letting it win means
     * a working driver is displaced by one that cannot possibly know better.
     *
     * BUS_PROBE_GENERIC is exactly the line upstream draws for "I will take
     * this if nobody else wants it". Anything at or below it may still CLAIM
     * an unattached device - probe_and_attach_pass is untouched - but may not
     * TAKE an attached one.
     *
     * Concrete: the WDM test.sys driver sets no ID table, so wdm.c matches
     * every function on the bus at BUS_PROBE_GENERIC. It loads after the boot
     * attach pass, and without this test it took three devices away from
     * pci_generic and failed to attach to all three. */
    if (best_pri <= BUS_PROBE_GENERIC) {
        return;
    }

    incumbent->detach(dev);
    dev->state = DS_NOTPRESENT;
    bind_driver(dev, best);
    dev->state = DS_ATTACHING;
    if (best->attach(dev) == 0) {
        dev->attach_pri = best_pri;
        dev->state = DS_ATTACHED;
        kprintf_c(0x0A, "bus: %s replaced %s (priority %d beats %d)\n",
                  best->name, old_name, best_pri, old_pri);
        return;
    }

    /* The replacement probed well and then declined to attach. PUT THE
     * INCUMBENT BACK.
     *
     * The first version left the device with no driver, reasoning that an
     * attach which already ran once might not be re-runnable. That was wrong
     * twice over: detach-then-attach is the ordinary driver lifecycle and is
     * exactly what detach exists to make safe, and the cost of being wrong
     * the other way is far higher - a working driver thrown away and the
     * device dark because some other driver probed optimistically.
     *
     * It showed up immediately. The WDM test.sys driver loads after the boot
     * attach pass and probes broadly; it took three devices away from
     * pci_generic, failed to attach to all three, and the boot log filled
     * with "the device now has no driver".
     *
     * Recovery is best-effort by nature. If the incumbent also refuses there
     * is nothing left to try, and that is reported rather than hidden. */
    bind_driver(dev, incumbent);
    dev->state = DS_ATTACHING;
    if (incumbent->attach != NULL && incumbent->attach(dev) == 0) {
        dev->attach_pri = old_pri;
        dev->state = DS_ATTACHED;
        return;
    }
    kprintf_c(0x0C, "bus: %s failed to attach and %s could not be restored - "
                    "the device now has no driver\n", best->name, old_name);
    bind_driver(dev, NULL);
    dev->state = DS_NOTPRESENT;
}

/* A driver arrived AFTER the boot-time attach pass already ran. Re-probe
 * every device in its devclass that nothing has claimed.
 *
 * --- why this has to exist --------------------------------------------
 * Until loadable modules worked, every driver in this kernel was registered
 * from flk.c before the single bus_attach_children call, so "register" and
 * "get probed" were the same moment and nothing had to reconcile them. A
 * module changes that: kld_load_directories runs long after that call,
 * because it needs a filesystem to read the module out of. A driver
 * registered from a module's init would therefore go into a devclass whose
 * devices had all been probed already, match nothing, and attach to nothing -
 * while looking, from the driver's side, exactly like a successful load.
 *
 * Both upstreams have this and call it the same thing: FreeBSD's
 * BUS_DRIVER_ADDED and Linux's driver-model bus re-probe on driver
 * registration. This is that.
 *
 * --- what it deliberately does not do ---------------------------------
 * It does not touch a device that is already DS_ATTACHED. A driver that
 * would have outbid the attached one had it been present at boot does NOT
 * get to take the device away UNLESS the incumbent provides a detach method
 * and the challenger made a specific claim - see try_replace, which is where
 * that judgement lives. So late registration can claim the unclaimed, and can
 * displace an incumbent that says it is safe to displace.
 *
 * That is a real limitation with a visible consequence: load order now
 * matters in a way it does not upstream, because the first adequate driver
 * to arrive keeps the device even if a better one loads a moment later. */
void bus_driver_added(devclass_t dc) {
    static const int passes[] = {
        BUS_PASS_ROOT, BUS_PASS_BUS, BUS_PASS_CPU, BUS_PASS_RESOURCE,
        BUS_PASS_INTERRUPT, BUS_PASS_TIMER, BUS_PASS_SCHEDULER,
        BUS_PASS_SUPPORTDEV, BUS_PASS_DEFAULT
    };
    int p, i;

    /* Nothing to do until the boot-time attach pass has run, and doing
     * something anyway is actively wrong.
     *
     * Every driver compiled into the kernel registers from flk.c BEFORE that
     * one bus_attach_children call, on purpose: they are all considered
     * together and priority decides who gets what. Probing on each
     * registration instead would make the FIRST driver registered attach to
     * everything it matches before the others exist - and pci_generic, a
     * BUS_PROBE_HOOVER driver that matches every function on the bus, is
     * registered first. Found exactly that way: the demo drivers stopped
     * matching their devices and pci_generic claimed all six. */
    if (dc == NULL || !dc->attached) {
        return;
    }
    for (p = 0; p < (int)(sizeof(passes) / sizeof(passes[0])); p++) {
        for (i = 0; i < bus_dev_count; i++) {
            bus_dev_t *dev = &bus_dev_pool[i];

            if (dev->devclass != dc) {
                continue;
            }
            if (dev->state != DS_ATTACHED) {
                probe_and_attach_pass(dev, passes[p]);
                continue;
            }
            try_replace(dev, passes[p]);
        }
    }
}

void *bus_get_softc(bus_dev_t *dev) {
    return dev->softc;
}

void *bus_get_ivars(bus_dev_t *dev) {
    return dev->ivars;
}

/* --- device identity ---------------------------------------------------- */

/* Local, per this tree's preference for a duplicated trivial helper over a
 * shared one. */
static int devname_eq(const char *a, const char *b) {
    uint32 i = 0;

    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

int device_set_devclass(bus_dev_t *dev, const char *name) {
    int i, unit = 0;
    uint32 k;

    if (dev == NULL || name == NULL || name[0] == '\0') {
        return -1;
    }

    /* The next free unit for this NAME, across every device. Scanning rather
     * than keeping a per-name counter: a counter would hand out a unit
     * already in use after a device is detached and another named, and there
     * is no detach path yet to keep such a counter honest. */
    for (i = 0; i < bus_dev_count; i++) {
        if (&bus_dev_pool[i] == dev) {
            continue;
        }
        if (bus_dev_pool[i].name[0] != '\0' &&
            devname_eq(bus_dev_pool[i].name, name) &&
            bus_dev_pool[i].unit >= unit) {
            unit = bus_dev_pool[i].unit + 1;
        }
    }

    for (k = 0; k < BUS_DEVNAME_MAX - 1 && name[k] != '\0'; k++) {
        dev->name[k] = name[k];
    }
    dev->name[k]  = '\0';
    dev->unit     = unit;
    dev->hasclass = 1;
    return unit;
}

const char *bus_get_devname(bus_dev_t *dev) {
    if (dev == NULL || dev->name[0] == '\0') {
        return NULL;
    }
    return dev->name;
}

int bus_get_devunit(bus_dev_t *dev) {
    return dev == NULL ? -1 : dev->unit;
}

int device_has_class(bus_dev_t *dev) {
    return dev != NULL && dev->hasclass;
}

bus_dev_t *bus_get_parent(bus_dev_t *dev) {
    return dev == NULL ? NULL : dev->parent;
}

driver_t *bus_get_driver(bus_dev_t *dev) {
    return dev == NULL ? NULL : dev->driver;
}

devclass_t bus_get_devclass(bus_dev_t *dev) {
    return dev == NULL ? NULL : dev->devclass;
}

/* --- resources -----------------------------------------------------------
 *
 * A flat pool with a linear overlap scan. Sixty-four allocations and a scan
 * per allocation is nothing at boot, and it keeps the whole thing readable;
 * upstream's rman is a sorted region list because it manages address space
 * for hundreds of devices with dynamic assignment, which is not what is
 * happening here. The property that matters - two devices cannot both hold
 * the same bytes - is the same either way.
 */

/* PCI spells a BAR two ways depending on who is asking: real FreeBSD driver
 * source passes PCIR_BAR(n), the config-space offset 0x10 + 4n, and
 * Genesis's own code naturally reaches for the index n. Accept both rather
 * than force one, because both callers are legitimate and the ranges do not
 * overlap - a valid index is 0-5, a valid BAR offset is 0x10-0x24. */
static int rid_to_bar(int rid) {
    if (rid >= 0 && rid <= 5) {
        return rid;
    }
    if (rid >= 0x10 && rid <= 0x24 && (rid & 3) == 0) {
        return (rid - 0x10) / 4;
    }
    return -1;
}

/* Do [a_start, a_start+a_count) and [b_start, b_start+b_count) share a
 * byte? Written with the two "entirely before / entirely after" cases
 * rather than as a positive test, because that is the form that stays
 * correct when a count is zero. */
static int ranges_overlap(uint64 a_start, uint64 a_count,
                          uint64 b_start, uint64 b_count) {
    if (a_count == 0 || b_count == 0) {
        return 0;
    }
    if (a_start + a_count <= b_start) {
        return 0;
    }
    if (b_start + b_count <= a_start) {
        return 0;
    }
    return 1;
}

bus_resource_t *bus_alloc_resource(bus_dev_t *dev, int type, int *rid,
                                   uint64 start, uint64 end, uint64 count,
                                   uint32 flags) {
    bus_resource_t *r;

    kmtx_lock(&bus_mtx);
    r = bus_alloc_resource_locked(dev, type, rid, start, end, count, flags);
    kmtx_unlock(&bus_mtx);
    return r;
}

static bus_resource_t *bus_alloc_resource_locked(bus_dev_t *dev, int type, int *rid,
                                   uint64 start, uint64 end, uint64 count,
                                   uint32 flags) {
    struct resource *slot = NULL;
    int i;

    if (dev == NULL || rid == NULL) {
        return NULL;
    }

    /* An IRQ resource. "Whatever the device says it wants" here means the
     * interrupt line PCI enumeration already recorded on the ivars - there
     * is no BAR to ask.
     *
     * This case did not exist until the first real vendored driver needed
     * it, and the symptom was exactly what you would want: re(4) printed its
     * own "couldn't allocate IRQ resources" and declined to attach. A driver
     * allocates SYS_RES_IRQ before bus_setup_intr, and without it there is
     * no resource to hand over.
     *
     * SHAREABLE by nature: a PCI INTx line is routed to several functions,
     * and refusing the second allocation would make a driver fail on a line
     * it is entitled to share. irq.c has done real chained dispatch since
     * the IRQ-sharing work, so the sharing is genuine and not a waiver. */
    if (count == 0 && type == SYS_RES_IRQ) {
        const pci_ivars_t *f = (const pci_ivars_t *)dev->ivars;

        if (f == NULL || f->int_line == 0 || f->int_line == 0xFF) {
            /* 0xFF is PCI's "no connection" encoding and 0 is "not routed".
             * Both mean there is no line, and handing back one anyway gives
             * a driver an interrupt that can never fire. */
            return NULL;
        }
        start = f->int_line;
        count = 1;
        flags |= RF_SHAREABLE;
    }

    /* "Whatever the device says it wants." For a PCI memory/IO resource
     * that means asking the BAR itself, which is the one bus-specific
     * thing this file does. A non-PCI child (nothing creates one yet) with
     * count == 0 gets a refusal rather than a guess. */
    if (count == 0 && (type == SYS_RES_MEMORY || type == SYS_RES_IOPORT)) {
        const pci_ivars_t *f = (const pci_ivars_t *)dev->ivars;
        int bar = rid_to_bar(*rid);
        uint64 bar_base;
        uint64 bar_size;
        int is_mem;

        if (f == NULL || bar < 0) {
            return NULL;
        }
        if (pci_bar_size(f, bar, &bar_base, &bar_size, &is_mem) != 0) {
            return NULL;
        }
        if (bar_size == 0) {
            return NULL;    /* BAR not implemented by the device */
        }
        /* A driver asking for SYS_RES_MEMORY must not be handed an I/O
         * BAR just because the index was right - that mismatch is exactly
         * the kind of thing a bus-mediated allocator exists to catch. */
        if ((type == SYS_RES_MEMORY) != (is_mem != 0)) {
            return NULL;
        }
        start = bar_base;
        count = bar_size;
    }

    if (count == 0) {
        return NULL;
    }
    if (end != 0 && start + count - 1 > end) {
        return NULL;
    }

    /* The conflict check - the thing that could not happen before. */
    for (i = 0; i < BUS_RESOURCE_MAX; i++) {
        struct resource *r = &resource_pool[i];

        if (r->owner == NULL) {
            if (slot == NULL) {
                slot = r;
            }
            continue;
        }
        if (r->type != type) {
            continue;
        }
        if (!ranges_overlap(start, count, r->start, r->count)) {
            continue;
        }
        /* Upstream lets two devices share a region when BOTH asked to -
         * a shared PCI interrupt line is the case that matters. Anything
         * else is a real conflict and is refused, loudly: a driver
         * silently mapping over another driver's BAR is the bug this
         * whole mechanism exists to make impossible. */
        if ((flags & RF_SHAREABLE) != 0 && (r->flags & RF_SHAREABLE) != 0) {
            continue;
        }
        kprintf_c(0x0C, "bus: resource conflict - type %d [%lx+%lx] "
                        "wanted by %s, held by %s\n",
                  type, start, count,
                  dev->driver != NULL ? dev->driver->name : "?",
                  r->owner->driver != NULL ? r->owner->driver->name : "?");
        return NULL;
    }

    if (slot == NULL) {
        kprintf_c(0x0C, "bus: BUS_RESOURCE_MAX exceeded\n");
        return NULL;
    }

    slot->owner = dev;
    slot->type  = type;
    slot->rid   = *rid;
    slot->start = start;
    slot->count = count;
    slot->flags = flags | RF_ALLOCATED;
    return slot;
}

bus_resource_t *bus_alloc_resource_any(bus_dev_t *dev, int type, int *rid,
                                       uint32 flags) {
    return bus_alloc_resource(dev, type, rid, 0, 0, 0, flags);
}

int bus_release_resource(bus_dev_t *dev, int type, int rid,
                         bus_resource_t *r) {
    int rc;

    kmtx_lock(&bus_mtx);
    rc = bus_release_resource_locked(dev, type, rid, r);
    kmtx_unlock(&bus_mtx);
    return rc;
}

static int bus_release_resource_locked(bus_dev_t *dev, int type, int rid,
                                       bus_resource_t *r) {
    (void)rid;

    if (r == NULL || dev == NULL) {
        return -1;
    }
    /* Releasing a resource you do not own is refused rather than tolerated.
     * The failure mode it prevents is one driver freeing another's region
     * and a third then allocating it - a conflict the checker above would
     * never see, because by then there is no conflict. */
    if (r->owner != dev || r->type != type) {
        return -1;
    }
    r->owner = NULL;
    r->flags = 0;
    return 0;
}

uint64 bus_get_resource_start(bus_resource_t *r) {
    return r == NULL ? 0 : r->start;
}

uint64 bus_get_resource_size(bus_resource_t *r) {
    return r == NULL ? 0 : r->count;
}

uint64 bus_get_resource_end(bus_resource_t *r) {
    return r == NULL || r->count == 0 ? 0 : r->start + r->count - 1;
}

int bus_get_resource_rid(bus_resource_t *r) {
    return r == NULL ? -1 : r->rid;
}

void *bus_get_resource_mapping(bus_resource_t *r) {
    return r != NULL ? r->mapping : NULL;
}

void bus_set_resource_mapping(bus_resource_t *r, void *va) {
    if (r != NULL) {
        r->mapping = va;
    }
}

int bus_get_resource_type(bus_resource_t *r) {
    return r == NULL ? 0 : r->type;
}

/* --- taking a driver back out --------------------------------------------
 *
 * ROADMAP item 4's other half. A loadable module's driver_t is a static
 * object INSIDE the module image, and so are its probe, attach and detach
 * functions - so the moment kldload unmaps that image, every pointer this
 * file holds to it is a pointer into unmapped memory. A devclass would keep
 * offering the driver to new children and fault on the first probe; an
 * attached device would fault on the first detach that never comes.
 *
 * --- why an ADDRESS RANGE and not a module handle -------------------------
 *
 * The alternative was for a module to declare which drivers it registered,
 * and for kldload to pass that list here. That is a list somebody forgets to
 * add to, and a missing entry is not a compile error - it is a stale pointer
 * that faults later, in a context that names nothing.
 *
 * The image's extent is the one fact that cannot be forgotten, because it is
 * measured rather than declared: anything inside it belongs to the module by
 * construction. bus.c never learns what a module is; it answers "do you hold
 * pointers into these bytes", which is a question it can answer completely.
 * Same reasoning as kldload's other sweeps - see kld_unload.
 *
 * --- what "can" means -----------------------------------------------------
 *
 * An ATTACHED device whose driver has no detach method cannot be taken off
 * it. try_replace already draws exactly this line and for exactly this
 * reason: a driver with no detach is not being lazy, it is saying nothing
 * knows what it registered, so tearing it out would leave its interrupt
 * handlers installed and its resources allocated. The difference is what
 * happens next - try_replace declines to swap and the machine carries on,
 * whereas here the module unload has to be REFUSED, because the alternative
 * is unmapping the code those handlers point at. */
static int ptr_in_range(const void *p, uint64 base, uint64 size) {
    uint64 a = (uint64)p;

    return p != NULL && a >= base && a < base + size;
}

int bus_can_unregister_range(uint64 base, uint64 size) {
    int i;

    if (size == 0) {
        return 0;
    }
    for (i = 0; i < bus_dev_count; i++) {
        bus_dev_t *dev = &bus_dev_pool[i];

        if (!ptr_in_range(dev->driver, base, size)) {
            continue;
        }
        if (dev->state == DS_ATTACHED && dev->driver->detach == NULL) {
            return -1;
        }
    }
    return 0;
}

/* Resources still owned by a device whose driver has just been detached.
 *
 * By construction these are LEAKED: a resource is allocated by driver code
 * against a device, and the driver is gone. Reclaiming them is safe for the
 * same reason - nothing is left that could be holding one - and the count is
 * worth returning rather than swallowing, because a non-zero answer is a bug
 * in that driver's detach method and this is the only place it is visible. */
static int reclaim_resources(bus_dev_t *dev) {
    int i, n = 0;

    kmtx_lock(&bus_mtx);
    for (i = 0; i < BUS_RESOURCE_MAX; i++) {
        if (resource_pool[i].owner == dev) {
            resource_pool[i].owner   = NULL;
            resource_pool[i].flags   = 0;
            resource_pool[i].mapping = NULL;
            n++;
        }
    }
    kmtx_unlock(&bus_mtx);
    return n;
}

/* The first device bus.c ever created, or NULL. Exists for
 * kernel/bsd/kern_devsysctl.c's selftest, which needs A device - any real one
 * - because device_get_name and device_get_unit dereference it and a
 * fabricated pointer would fault. Deliberately not a general iterator: the
 * pool is private and giving it a public walk invites callers that should be
 * using bus_attach_children. */
bus_dev_t *bus_first_device(void) {
    return bus_dev_count > 0 ? &bus_dev_pool[0] : NULL;
}

/* Declared rather than included: kernel/bsd/kern_devsysctl.c is a FreeBSD
 * compat translation unit and bus.c is not. See the file comment there for
 * why detach has to reach it - a knob added by a module names itself with a
 * string literal in that module's .rodata, and points oid_arg1 at a softc
 * that is about to be freed. */
void device_sysctl_fini(bus_dev_t *dev);

int bus_unregister_range(uint64 base, uint64 size, int *leaked_resources) {
    int i, j;
    int removed = 0;
    int leaked  = 0;

    if (leaked_resources != NULL) {
        *leaked_resources = 0;
    }
    if (size == 0) {
        return 0;
    }
    if (bus_can_unregister_range(base, size) != 0) {
        return -1;
    }

    /* --- detach, unbind ---------------------------------------------------
     *
     * NOT under bus_mtx. This calls into driver code, and a detach method
     * doing the ordinary thing - releasing the resources it allocated - would
     * call bus_release_resource and take the same lock. Every other place
     * that calls into a driver (probe_and_attach_pass, try_replace) is
     * unlocked for the same reason. */
    for (i = 0; i < bus_dev_count; i++) {
        bus_dev_t *dev = &bus_dev_pool[i];
        driver_t  *drv = dev->driver;

        if (!ptr_in_range(drv, base, size)) {
            continue;
        }
        if (dev->state == DS_ATTACHED && drv->detach != NULL) {
            drv->detach(dev);
        }
        dev->state      = DS_NOTPRESENT;
        dev->attach_pri = 0;
        leaked += reclaim_resources(dev);

        /* The driver's sysctl knobs, if it had any. Same hazard as the
         * resources and a quieter one: an oid holds the module's string
         * literal as its name and the freed softc as its arg. */
        device_sysctl_fini(dev);

        /* bind_driver(dev, NULL) is what frees the softc - and it has to
         * happen AFTER detach, not before, or the detach method reads a
         * freed pointer through bus_get_softc. */
        bind_driver(dev, NULL);
    }

    /* --- and out of every devclass's candidate list ----------------------
     *
     * Compaction rather than a tombstone, because probe_best walks
     * drivers[0..driver_count) and a hole in it would have to be checked for
     * in that loop - a test in the hot path to support a case that happens
     * at most a handful of times in a boot. */
    kmtx_lock(&bus_mtx);
    for (i = 0; i < devclass_count; i++) {
        struct devclass *dc = &devclass_pool[i];
        int keep = 0;

        for (j = 0; j < dc->driver_count; j++) {
            if (ptr_in_range(dc->drivers[j], base, size)) {
                removed++;
                continue;
            }
            dc->drivers[keep++] = dc->drivers[j];
        }
        dc->driver_count = keep;
    }
    kmtx_unlock(&bus_mtx);

    if (leaked_resources != NULL) {
        *leaked_resources = leaked;
    }
    return removed;
}

/* The devclass pool's occupancy, printed at boot.
 *
 * Exists because the only way this pool's size was ever observed was by
 * exhausting it, and exhausting it halts the machine. A count next to the cap
 * turns "how close are we" into a line in the boot log. */
void bus_devclass_report(uint8 color) {
    int i;

    kprintf_c(color, "bus devclasses: %d of %d\n", devclass_count,
              DEVCLASS_MAX);
    for (i = 0; i < devclass_count; i++) {
        kprintf_c(color, "  %s  %d driver(s)\n", devclass_pool[i].name,
                  devclass_pool[i].driver_count);
    }
}

void bus_resource_report(uint8 color) {
    static const char *type_name[] = { "?", "irq", "drq", "mem", "io" };
    int i;
    int any = 0;

    for (i = 0; i < BUS_RESOURCE_MAX; i++) {
        struct resource *r = &resource_pool[i];

        if (r->owner == NULL) {
            continue;
        }
        if (!any) {
            kprintf_c(color, "bus resources:\n");
            any = 1;
        }
        kprintf_c(color, "  %s rid %d  %lx + %lx  %s\n",
                  r->type >= 1 && r->type <= 4 ? type_name[r->type] : "?",
                  r->rid, r->start, r->count,
                  r->owner->driver != NULL ? r->owner->driver->name : "?");
    }
}
