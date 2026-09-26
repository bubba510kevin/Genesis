#include "bus.h"
#include "hints.h"
#include "kprintf.h"
#include "newbus_compat.h"
#include "typesk.h"

/* Exercises what Part 4 added to bus.c. Kept out of bus.c itself so the
 * engine has no test-only code in it, and so the synthetic devclasses and
 * drivers below cannot be mistaken for real ones.
 *
 * All of this is boot-time only: bus.c is not reachable from ring 3, and
 * there is no syscall that would make it so. Same posture as the mbuf and
 * callout selftests - see src/verif.c.
 *
 * These devices are created with a NULL ivars pointer and are never given
 * to a real bus. That is deliberate: the point is to test the MATCHING
 * ENGINE, and a test that needed real PCI hardware present would be testing
 * QEMU's device model too.
 */

/* --- what the pass-ordering test records -------------------------------- */

#define ATTACH_LOG_MAX 8

/* Identities, not names. The first version of this logged a `const char *`
 * and compared a byte of it, which is both fragile and was wrong on the
 * first try - a pointer comparison against the driver_t itself cannot be
 * off by one and does not need a string compare pulled in to check. */
static const void *attach_log[ATTACH_LOG_MAX];
static int         attach_log_count;

static void log_attach(const void *who) {
    if (attach_log_count < ATTACH_LOG_MAX) {
        attach_log[attach_log_count++] = who;
    }
}

/* --- drivers for the pass-ordering test ---------------------------------
 *
 * Registered LATE-pass first, deliberately. Under the old single-pass loop
 * the registration order decided everything, so this ordering is what makes
 * the test able to fail.
 *
 * Forward-declared because each attach() logs its own driver_t's address,
 * which is defined below it. */
static driver_t late_driver;
static driver_t early_driver;
static driver_t parent_driver;

static int late_probe(bus_dev_t *dev) {
    (void)dev;
    return BUS_PROBE_DEFAULT;
}

static int late_attach(bus_dev_t *dev) {
    (void)dev;
    log_attach(&late_driver);
    return 0;
}

static int early_probe(bus_dev_t *dev) {
    (void)dev;
    /* WORSE priority than late_probe. So if passes are working, "early"
     * still attaches first - proving the pass gate is what decided it and
     * not the probe ranking, which would have picked the other one. */
    return BUS_PROBE_GENERIC;
}

static int early_attach(bus_dev_t *dev) {
    (void)dev;
    log_attach(&early_driver);
    return 0;
}

static driver_t late_driver = {
    .name = "st_late",  .probe = late_probe,  .attach = late_attach,
    .detach = NULL, .softc_size = 0, .pass = BUS_PASS_DEFAULT, .methods = NULL,
};

static driver_t early_driver = {
    .name = "st_early", .probe = early_probe, .attach = early_attach,
    .detach = NULL, .softc_size = 0, .pass = BUS_PASS_RESOURCE, .methods = NULL,
};

/* --- drivers for the devclass-inheritance test -------------------------- */

static int parent_probe(bus_dev_t *dev) {
    (void)dev;
    return BUS_PROBE_DEFAULT;
}

static int parent_attach(bus_dev_t *dev) {
    (void)dev;
    log_attach(&parent_driver);
    return 0;
}

static driver_t parent_driver = {
    .name = "st_parent", .probe = parent_probe, .attach = parent_attach,
    .detach = NULL, .softc_size = 0, .pass = 0, .methods = NULL,
};

/* --- the custom-interface test -------------------------------------------
 *
 * This is the KOBJ generalization, demonstrated the way a real port would
 * use it: a desc token this file defines itself, a DEVMETHOD-shaped entry
 * for it in a method table, and a lookup that finds it. Nothing in bus.c or
 * newbus_compat.c knows this interface exists - which is the claim being
 * tested, since before Part 4 only the three built-in methods survived
 * registration at all. */

const int st_custom_desc;

typedef int (*st_custom_fn)(bus_dev_t *dev, int arg);

static int st_custom_impl(bus_dev_t *dev, int arg) {
    (void)dev;
    return arg + 1;
}

/* Built by hand rather than through DEVMETHOD() because that macro lives in
 * sys/bus.h, which cannot be included here: this file needs bus.h's
 * driver_t, and the two deliberately collide (see sys/bus.h's header
 * comment). The layout is device_method_t's, which is what matters. */
static device_method_t st_methods[] = {
    { &st_custom_desc, (void *)st_custom_impl },
    { 0, 0 },
};

static driver_t custom_driver = {
    .name = "st_custom", .probe = NULL, .attach = NULL,
    .detach = NULL, .softc_size = 0, .pass = 0, .methods = st_methods,
};

/* ------------------------------------------------------------------------ */

/* A driver that refuses to be matched by wildcard probing.
 *
 * Upstream's users of BUS_PROBE_NOWILDCARD are drivers that would probe
 * DESTRUCTIVELY if auto-attached - the reason the mechanism exists at all -
 * so a driver of this shape must only ever run against a device somebody
 * named on purpose. */
static int st_nowild_attaches;

static int st_nowild_probe(bus_dev_t *dev) {
    (void)dev;
    /* Enthusiastic, and it still must not win on an unnamed device. Probing
     * successfully is the point: if this returned "not mine" the test would
     * pass for the wrong reason. */
    return BUS_PROBE_NOWILDCARD;
}

static int st_nowild_attach(bus_dev_t *dev) {
    (void)dev;
    st_nowild_attaches++;
    return 0;
}

static driver_t nowild_driver = {
    "st_nowild", st_nowild_probe, st_nowild_attach, NULL, 0, 0, NULL
};

/* --- section 6's drivers: unloading ---------------------------------------
 *
 * Three of them, because "can this driver be taken off the bus" has three
 * different answers and a test that only saw one could not tell them apart.
 *
 * They are attached and removed ONE AT A TIME on a single devclass, rather
 * than living on three devclasses at once, for two reasons. Only one driver
 * wins a device, so three simultaneous drivers would need three devclasses
 * and three more entries in a pool that halts the machine when it fills. And
 * re-using the devclass tests something a static arrangement cannot: that
 * bus_unregister_range really emptied the candidate list, because the next
 * round's driver is the only one in it. */

#define ST_UNL_RES_BASE 0xE0100000ULL

static int st_unl_attaches, st_unl_detaches;
static int st_leak_attaches;
static int st_stuck_attaches;
static bus_resource_t *st_unl_res;
static bus_resource_t *st_leak_res;

/* WELL BEHAVED: allocates a resource in attach and releases it in detach. */
static int st_unl_probe(bus_dev_t *dev) {
    (void)dev;
    return BUS_PROBE_SPECIFIC;
}

static int st_unl_attach(bus_dev_t *dev) {
    int rid = 0;

    st_unl_attaches++;
    st_unl_res = bus_alloc_resource(dev, SYS_RES_MEMORY, &rid,
                                    ST_UNL_RES_BASE, 0, 0x1000, 0);
    return 0;
}

static void st_unl_detach(bus_dev_t *dev) {
    st_unl_detaches++;
    if (st_unl_res != NULL) {
        bus_release_resource(dev, SYS_RES_MEMORY, 0, st_unl_res);
        st_unl_res = NULL;
    }
}

static driver_t st_unl_driver = {
    "st_unl", st_unl_probe, st_unl_attach, st_unl_detach,
    sizeof(int), 0, NULL
};

/* LEAKY: has a detach, and its detach forgets the resource. Not a strawman -
 * it is the ordinary bug, and the only place it becomes visible is the count
 * bus_unregister_range hands back. */
static int st_leak_probe(bus_dev_t *dev) {
    (void)dev;
    return BUS_PROBE_SPECIFIC;
}

static int st_leak_attach(bus_dev_t *dev) {
    int rid = 0;

    st_leak_attaches++;
    st_leak_res = bus_alloc_resource(dev, SYS_RES_MEMORY, &rid,
                                     ST_UNL_RES_BASE, 0, 0x1000, 0);
    return st_leak_res == NULL ? -1 : 0;
}

static void st_leak_detach(bus_dev_t *dev) {
    (void)dev;   /* and nothing else - that is the point */
}

static driver_t st_leak_driver = {
    "st_leak", st_leak_probe, st_leak_attach, st_leak_detach,
    0, 0, NULL
};

/* STUCK: no detach method at all, which is a driver saying it cannot be taken
 * off the device. Must be REFUSED, and refused without changing anything. */
static int st_stuck_probe(bus_dev_t *dev) {
    (void)dev;
    return BUS_PROBE_SPECIFIC;
}

static int st_stuck_attach(bus_dev_t *dev) {
    (void)dev;
    st_stuck_attaches++;
    return 0;
}

static driver_t st_stuck_driver = {
    "st_stuck", st_stuck_probe, st_stuck_attach, NULL, 0, 0, NULL
};

int bus_selftest(void) {
    devclass_t  dc_pass;
    devclass_t  dc_child;
    devclass_t  dc_parent;
    bus_dev_t  *anchor;
    bus_dev_t  *d1;
    bus_dev_t  *d2;
    bus_dev_t  *d3;
    bus_resource_t *r1;
    bus_resource_t *r2;
    bus_resource_t *r3;
    int rid;
    int failures = 0;
    devclass_t  dc_unload;
    bus_dev_t  *du;
    int         leaked;
    int         removed;

    attach_log_count = 0;

    /* An anchor with no devclass of its own - never probed, exists only as
     * a parent. Same role pci.c's root node plays. */
    anchor = bus_add_child(NULL, NULL, NULL);
    if (anchor == NULL) {
        kprintf_c(0x0C, "bus selftest: BUS_DEV_MAX exhausted\n");
        return 1;
    }

    /* --- 1. pass ordering ------------------------------------------------
     * Two children, two drivers. The later-pass driver has the BETTER probe
     * priority and is registered FIRST, so under the old single-pass loop
     * it would have won both devices before the early-pass driver ran at
     * all. Passes working means "early" attaches to both first. */
    dc_pass = devclass_find("st_pass");
    devclass_add_driver(dc_pass, &late_driver);
    devclass_add_driver(dc_pass, &early_driver);

    d1 = bus_add_child(anchor, dc_pass, NULL);
    d2 = bus_add_child(anchor, dc_pass, NULL);
    if (d1 == NULL || d2 == NULL) {
        kprintf_c(0x0C, "bus selftest: BUS_DEV_MAX exhausted\n");
        return failures + 1;
    }

    bus_attach_children(anchor);

    if (attach_log_count != 2) {
        kprintf_c(0x0C, "bus selftest: %d attaches, expected 2\n",
                  attach_log_count);
        failures++;
    } else if (attach_log[0] != &early_driver ||
               attach_log[1] != &early_driver) {
        kprintf_c(0x0C, "bus selftest: the BUS_PASS_DEFAULT driver "
                        "attached before the BUS_PASS_RESOURCE one\n");
        failures++;
    }
    if (bus_get_driver(d1) != &early_driver ||
        bus_get_driver(d2) != &early_driver) {
        kprintf_c(0x0C, "bus selftest: late-pass driver won despite a "
                        "BUS_PASS_RESOURCE candidate\n");
        failures++;
    }

    /* --- 2. devclass inheritance ----------------------------------------
     * A child devclass with NO drivers of its own, parented to one that has
     * a match. Nothing registered directly can match, so the only way this
     * attaches is by walking up. */
    dc_child  = devclass_find("st_child");
    dc_parent = devclass_find("st_inherit");
    devclass_add_driver(dc_parent, &parent_driver);

    if (devclass_set_parent(dc_child, dc_parent) != 0) {
        kprintf_c(0x0C, "bus selftest: devclass_set_parent refused a "
                        "valid link\n");
        failures++;
    }
    /* A cycle must be refused, not discovered as a hang later. */
    if (devclass_set_parent(dc_parent, dc_child) == 0) {
        kprintf_c(0x0C, "bus selftest: devclass_set_parent accepted a "
                        "cycle\n");
        failures++;
    }
    if (devclass_set_parent(dc_child, dc_child) == 0) {
        kprintf_c(0x0C, "bus selftest: devclass_set_parent accepted a "
                        "self-link\n");
        failures++;
    }

    d3 = bus_add_child(anchor, dc_child, NULL);
    if (d3 == NULL) {
        kprintf_c(0x0C, "bus selftest: BUS_DEV_MAX exhausted\n");
        return failures + 1;
    }
    bus_probe_and_attach(d3);
    if (bus_get_driver(d3) != &parent_driver) {
        kprintf_c(0x0C, "bus selftest: devclass inheritance did not reach "
                        "the parent's driver\n");
        failures++;
    }

    /* --- 3. bus_get_parent ----------------------------------------------- */
    if (bus_get_parent(d3) != anchor || bus_get_parent(anchor) != NULL) {
        kprintf_c(0x0C, "bus selftest: bus_get_parent wrong\n");
        failures++;
    }

    /* --- 4. resource conflict detection -----------------------------------
     * The gap item 11 names in as many words. Explicit ranges rather than
     * BARs, so this tests the allocator and not pci_bar_size. */
    rid = 0;
    r1 = bus_alloc_resource(d1, SYS_RES_MEMORY, &rid,
                            0xE0000000ULL, 0, 0x1000, 0);
    if (r1 == NULL) {
        kprintf_c(0x0C, "bus selftest: first allocation refused\n");
        failures++;
    }

    /* Same bytes, different device - must be refused. This is the whole
     * point of the mechanism, and before Part 4 it could not be detected
     * because there was no mechanism. */
    kprintf("bus selftest: the next line is expected - a deliberate "
            "overlapping allocation\n");
    rid = 1;
    r2 = bus_alloc_resource(d2, SYS_RES_MEMORY, &rid,
                            0xE0000800ULL, 0, 0x1000, 0);
    if (r2 != NULL) {
        kprintf_c(0x0C, "bus selftest: overlapping allocation ACCEPTED\n");
        failures++;
        bus_release_resource(d2, SYS_RES_MEMORY, 1, r2);
    }

    /* Adjacent but not overlapping - must be accepted. A conflict checker
     * that refuses this is as broken as one that accepts the case above,
     * and off-by-one at the boundary is exactly how that happens. */
    rid = 2;
    r2 = bus_alloc_resource(d2, SYS_RES_MEMORY, &rid,
                            0xE0001000ULL, 0, 0x1000, 0);
    if (r2 == NULL) {
        kprintf_c(0x0C, "bus selftest: adjacent allocation refused\n");
        failures++;
    }

    /* Same range, different TYPE - a memory region and an I/O port range at
     * the same number are unrelated. */
    rid = 3;
    r3 = bus_alloc_resource(d2, SYS_RES_IOPORT, &rid,
                            0xE0000000ULL, 0, 0x1000, 0);
    if (r3 == NULL) {
        kprintf_c(0x0C, "bus selftest: same range in another type "
                        "refused\n");
        failures++;
    }

    if (r1 != NULL) {
        if (bus_get_resource_start(r1) != 0xE0000000ULL ||
            bus_get_resource_size(r1) != 0x1000 ||
            bus_get_resource_end(r1) != 0xE0000FFFULL) {
            kprintf_c(0x0C, "bus selftest: resource accessors wrong\n");
            failures++;
        }
    }

    /* Releasing someone else's resource must be refused - see the comment
     * on bus_release_resource for the failure mode that prevents. */
    if (r1 != NULL && bus_release_resource(d2, SYS_RES_MEMORY, 0, r1) == 0) {
        kprintf_c(0x0C, "bus selftest: released another device's "
                        "resource\n");
        failures++;
    }

    /* Release for real, then the previously-conflicting range must become
     * available - proving release actually frees rather than just marking. */
    if (r1 != NULL && bus_release_resource(d1, SYS_RES_MEMORY, 0, r1) != 0) {
        kprintf_c(0x0C, "bus selftest: owner could not release\n");
        failures++;
    }
    rid = 4;
    r1 = bus_alloc_resource(d2, SYS_RES_MEMORY, &rid,
                            0xE0000800ULL, 0, 0x400, 0);
    if (r1 == NULL) {
        kprintf_c(0x0C, "bus selftest: range still held after release\n");
        failures++;
    }

    /* Clean up, so the boot-time resource report shows only what real
     * drivers hold. */
    bus_release_resource(d2, SYS_RES_MEMORY, 4, r1);
    bus_release_resource(d2, SYS_RES_MEMORY, 2, r2);
    bus_release_resource(d2, SYS_RES_IOPORT, 3, r3);

    /* --- 5. general method dispatch --------------------------------------
     * A custom interface method, reached through newbus_method_get. Before
     * Part 4 this method would not have survived driver registration. */
    {
        st_custom_fn fn = (st_custom_fn)newbus_driver_method_get(
            &custom_driver, &st_custom_desc);

        if (fn == NULL) {
            kprintf_c(0x0C, "bus selftest: custom method not found\n");
            failures++;
        } else if (fn(NULL, 41) != 42) {
            kprintf_c(0x0C, "bus selftest: custom method returned wrong\n");
            failures++;
        }
    }
    /* A method the driver does not implement is NULL, not a wild pointer. */
    if (newbus_driver_method_get(&custom_driver, &st_custom_desc + 1) != NULL) {
        kprintf_c(0x0C, "bus selftest: unimplemented method not NULL\n");
        failures++;
    }
    /* A Genesis-native driver has no table at all and must answer NULL
     * rather than walk one. */
    if (newbus_driver_method_get(&early_driver, &st_custom_desc) != NULL) {
        kprintf_c(0x0C, "bus selftest: native driver returned a method\n");
        failures++;
    }

    /* --- BUS_PROBE_NOWILDCARD, both halves (Part 16) --------------------
     *
     * A test that only checked the refusal could not tell correct gating
     * from a driver that never attaches at all - so this runs the SAME
     * driver against the SAME kind of device twice, differing only in
     * whether the device was named. */
    {
        devclass_t  dc = devclass_find("st_nowild");
        bus_dev_t  *anon;
        bus_dev_t  *named;

        st_nowild_attaches = 0;
        devclass_add_driver(dc, &nowild_driver);

        /* 1. A device the bus merely enumerated. The driver probes and
         *    returns BUS_PROBE_NOWILDCARD; nothing else is registered in
         *    this devclass, so it is the ONLY candidate - and it must
         *    still lose. */
        anon = bus_add_child(NULL, dc, NULL);
        bus_attach_children(NULL);
        bus_probe_and_attach(anon);
        if (st_nowild_attaches != 0) {
            kprintf_c(0x0C, "bus selftest: a NOWILDCARD driver attached to "
                            "a device nobody named\n");
            failures++;
        }
        if (device_has_class(anon)) {
            kprintf_c(0x0C, "bus selftest: an enumerated device claims to "
                            "have been named\n");
            failures++;
        }

        /* 2. The same driver, a device that WAS named. Now it must win. */
        named = bus_add_child(NULL, dc, NULL);
        if (device_set_devclass(named, "nowild") != 0) {
            kprintf_c(0x0C, "bus selftest: first nowild unit was not 0\n");
            failures++;
        }
        if (!device_has_class(named)) {
            kprintf_c(0x0C, "bus selftest: naming a device did not set "
                            "hasclass\n");
            failures++;
        }
        bus_probe_and_attach(named);
        if (st_nowild_attaches != 1) {
            kprintf_c(0x0C, "bus selftest: a NOWILDCARD driver did not "
                            "attach to a device that WAS named\n");
            failures++;
        }

        /* 3. Unit numbers count up, which is what makes a name an
         *    identity rather than a label. */
        {
            bus_dev_t *second = bus_add_child(NULL, dc, NULL);

            if (device_set_devclass(second, "nowild") != 1) {
                kprintf_c(0x0C, "bus selftest: second nowild unit was not "
                                "1\n");
                failures++;
            }
        }

        /* 4. The hint that names it is really readable, and the disabled
         *    one really reads as disabled. Both, because a hint lookup that
         *    returned NULL for everything would pass a test that only
         *    checked the disabled case. */
        if (hint_get("nowild", 0, "at") == NULL) {
            kprintf_c(0x0C, "bus selftest: hint.nowild.0.at not found\n");
            failures++;
        }
        if (hint_disabled("nowild", 0)) {
            kprintf_c(0x0C, "bus selftest: nowild0 reads as disabled\n");
            failures++;
        }
        if (!hint_disabled("nowild", 1)) {
            kprintf_c(0x0C, "bus selftest: nowild1 does not read as "
                            "disabled\n");
            failures++;
        }
        /* A unit that has no hints at all must answer NULL rather than
         * matching unit 1's by prefix - "1" must not match "10". */
        if (hint_get("nowild", 10, "at") != NULL) {
            kprintf_c(0x0C, "bus selftest: unit 10 matched unit 1's hint\n");
            failures++;
        }
    }

    /* --- 6. taking a driver back out (module unload) ---------------------
     *
     * bus_unregister_range is what kldload calls before it unmaps a module
     * image; see bus.c for why the question is an address range. Everything
     * here is asked of a range exactly one driver_t wide, so a sweep that
     * matched too broadly would show up as the wrong driver disappearing.
     *
     * ROUND A - a well-behaved driver comes off cleanly. */
    dc_unload = devclass_find("st_unload");
    devclass_add_driver(dc_unload, &st_unl_driver);
    du = bus_add_child(anchor, dc_unload, NULL);
    if (du == NULL) {
        kprintf_c(0x0C, "bus selftest: BUS_DEV_MAX exhausted\n");
        return failures + 1;
    }
    bus_probe_and_attach(du);
    if (bus_get_driver(du) != &st_unl_driver || st_unl_attaches != 1) {
        kprintf_c(0x0C, "bus selftest: st_unl did not attach\n");
        failures++;
    }
    if (bus_can_unregister_range((uint64)&st_unl_driver,
                                 sizeof(st_unl_driver)) != 0) {
        kprintf_c(0x0C, "bus selftest: a driver WITH a detach method was "
                        "reported unremovable\n");
        failures++;
    }
    leaked = -1;
    removed = bus_unregister_range((uint64)&st_unl_driver,
                                   sizeof(st_unl_driver), &leaked);
    if (removed != 1) {
        kprintf_c(0x0C, "bus selftest: unregister removed %d registrations, "
                        "expected 1\n", removed);
        failures++;
    }
    if (st_unl_detaches != 1) {
        kprintf_c(0x0C, "bus selftest: detach was not called on unregister\n");
        failures++;
    }
    if (leaked != 0) {
        kprintf_c(0x0C, "bus selftest: %d resources reported leaked by a "
                        "driver that released its own\n", leaked);
        failures++;
    }
    if (bus_get_driver(du) != NULL || bus_get_softc(du) != NULL) {
        kprintf_c(0x0C, "bus selftest: device still bound after unregister\n");
        failures++;
    }
    /* The control for "removed 1": ask again. A function returning a constant
     * passes the check above and fails this one. */
    if (bus_unregister_range((uint64)&st_unl_driver,
                             sizeof(st_unl_driver), NULL) != 0) {
        kprintf_c(0x0C, "bus selftest: the driver was still registered after "
                        "being unregistered\n");
        failures++;
    }

    /* ROUND B - a detach that forgets its resource. The count is the only
     * place that is visible, and the reclaim has to be real: the same bytes
     * must be allocatable afterwards, which they would not be if the slot had
     * merely been counted. */
    devclass_add_driver(dc_unload, &st_leak_driver);
    du = bus_add_child(anchor, dc_unload, NULL);
    if (du == NULL) {
        kprintf_c(0x0C, "bus selftest: BUS_DEV_MAX exhausted\n");
        return failures + 1;
    }
    bus_probe_and_attach(du);
    if (bus_get_driver(du) != &st_leak_driver || st_leak_attaches != 1) {
        kprintf_c(0x0C, "bus selftest: st_leak did not attach\n");
        failures++;
    }
    leaked = -1;
    removed = bus_unregister_range((uint64)&st_leak_driver,
                                   sizeof(st_leak_driver), &leaked);
    if (removed != 1 || leaked != 1) {
        kprintf_c(0x0C, "bus selftest: leaky detach reported %d removed / "
                        "%d leaked, expected 1 / 1\n", removed, leaked);
        failures++;
    }
    rid = 0;
    st_leak_res = bus_alloc_resource(du, SYS_RES_MEMORY, &rid,
                                     ST_UNL_RES_BASE, 0, 0x1000, 0);
    if (st_leak_res == NULL) {
        kprintf_c(0x0C, "bus selftest: the leaked resource was counted but "
                        "not reclaimed\n");
        failures++;
    } else {
        bus_release_resource(du, SYS_RES_MEMORY, 0, st_leak_res);
        st_leak_res = NULL;
    }

    /* ROUND C - a driver with no detach method, attached. Refused, and
     * refused WITHOUT CHANGING ANYTHING, which is the half that matters: a
     * refusal that had already detached half the devices would be worse than
     * no refusal at all.
     *
     * Round A is the other half of this pair. Without it, a
     * bus_can_unregister_range that returned -1 for everything would pass. */
    devclass_add_driver(dc_unload, &st_stuck_driver);
    du = bus_add_child(anchor, dc_unload, NULL);
    if (du == NULL) {
        kprintf_c(0x0C, "bus selftest: BUS_DEV_MAX exhausted\n");
        return failures + 1;
    }
    bus_probe_and_attach(du);
    if (bus_get_driver(du) != &st_stuck_driver || st_stuck_attaches != 1) {
        kprintf_c(0x0C, "bus selftest: st_stuck did not attach\n");
        failures++;
    }
    if (bus_can_unregister_range((uint64)&st_stuck_driver,
                                 sizeof(st_stuck_driver)) != -1) {
        kprintf_c(0x0C, "bus selftest: an attached driver with NO detach "
                        "method was reported removable\n");
        failures++;
    }
    if (bus_unregister_range((uint64)&st_stuck_driver,
                             sizeof(st_stuck_driver), NULL) != -1) {
        kprintf_c(0x0C, "bus selftest: unregister of an undetachable driver "
                        "was accepted\n");
        failures++;
    }
    if (bus_get_driver(du) != &st_stuck_driver) {
        kprintf_c(0x0C, "bus selftest: a REFUSED unregister still unbound "
                        "the device\n");
        failures++;
    }

    if (failures == 0) {
        kprintf("bus: selftest passed\n");
    } else {
        kprintf_c(0x0C, "bus: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
