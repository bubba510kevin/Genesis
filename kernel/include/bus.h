#ifndef BUS_H
#define BUS_H

#include "typesk.h"

/* A from-scratch, hand-written reduction of FreeBSD's Newbus: device_t /
 * driver_t / devclass_t, and the probe-then-attach loop that binds the
 * best-fit driver to each discovered bus child. Real Newbus builds this on
 * KOBJ - a generated method-table/vtable-inheritance system with its own
 * code-generation step (.m files). Genesis has no build-time code
 * generation and is not trying to support loadable driver modules yet
 * (ROADMAP item 4's /lib/modules/, /boot/kernel/ paths are still
 * aspirational), so this keeps the SHAPE - probe ranks candidate drivers,
 * attach binds the winner, a bus's own attach is "probe-and-attach every
 * child" - without the machinery that shape doesn't need yet. Growing
 * towards real KOBJ later is widening this, not replacing it, the same
 * argument device.h makes about device_ops_t and an IRP stack.
 *
 * That widening has now happened once, for ROADMAP item 11. A driver_t
 * carries its whole method table (see .methods below), so a FreeBSD driver's
 * device_suspend/_resume/_shutdown and any custom interface it declares are
 * dispatchable through newbus_compat.h's newbus_method_get instead of being
 * discarded at registration; resources are allocated through the bus with
 * real conflict detection instead of read off raw ivars; boot ordering is
 * multi-pass instead of registration-order; devclasses inherit. What is
 * still missing is the .m-file CODEGEN - the type-checked, interface-
 * versioned dispatch real KOBJ generates - not the dispatch itself.
 *
 * --- bus_dev_t vs device_t -------------------------------------------
 * device.h's device_t is the I/O-facing object behind a handle: read/
 * write/parse/control, reached through object.c and the namespace.
 * bus_dev_t is a different, earlier thing - a node in the bus TOPOLOGY,
 * used only while matching a driver to a piece of hardware. Most bus_dev_t
 * instances never become a device_t at all (a PCI host bridge has nothing
 * to read or write). The ones that do - a NIC, eventually - do it
 * explicitly, the same two-step ata.c-then-disk.c already takes: a
 * driver's attach() talks to the raw bus_dev_t/ivars to find its hardware,
 * and SEPARATELY calls dev_alloc/dev_attach from device.h if and when it
 * has real I/O to expose. The two types are not layered on one another and
 * are not meant to be - keeping them apart is what lets a bus (which knows
 * nothing about read/write) stay ignorant of device_ops_t, the same way
 * device.c stays ignorant of ATA.
 */

/* Opaque - always reached through bus_add_child's return value and the
 * accessors below, never allocated directly. Definition lives in bus.c,
 * same reasoning FreeBSD keeps struct _device private to subr_bus.c: bus
 * code and driver code must not depend on each other's view of the field
 * layout. */
typedef struct bus_dev bus_dev_t;

/* What probe() returns. Exactly one of these per candidate, or a positive
 * value (conventionally the driver's own "not mine" errno) to mean no
 * match. Zero wins immediately without trying the remaining candidates;
 * otherwise the LEAST NEGATIVE value wins once every candidate has been
 * tried.
 *
 * This is now upstream's FULL scale, with upstream's exact values. The
 * previous version stopped at GENERIC and said to add HOOVER and NOWILDCARD
 * "when a real conflict shows up, not before" - four competing drivers on
 * one devclass is close enough to that, and re-deriving the priority
 * arithmetic later against drivers already written to the narrow scale is
 * worse than widening it now.
 *
 * HOOVER is for a catch-all that should lose to every real driver but still
 * beat nothing at all - what pci_generic.c actually is. NOWILDCARD is a
 * driver that should never win an ordinary probe.
 *
 * A NOTE ON NOWILDCARD, because half of upstream's meaning is missing here.
 * On FreeBSD it means "attach only when a hint or a devclass explicitly
 * names me", and subr_bus.c enforces that with a `hasclass` test against
 * per-device configuration Genesis has no equivalent of. What works here is
 * the numeric half: a NOWILDCARD driver loses to literally everything else,
 * including HOOVER. The explicit-naming half needs a device-hint mechanism,
 * and inventing one to fill this in would be adding capability, not
 * unsimplifying. Recorded so the gap is visible rather than assumed
 * closed. */
#define BUS_PROBE_SPECIFIC       0
#define BUS_PROBE_VENDOR       (-10)
#define BUS_PROBE_DEFAULT      (-20)
#define BUS_PROBE_LOW_PRIORITY (-40)
#define BUS_PROBE_GENERIC     (-100)
#define BUS_PROBE_HOOVER      (-1000000)
#define BUS_PROBE_NOWILDCARD  (-2000000000)

/* --- boot passes ---------------------------------------------------------
 * Upstream's device pass model, same constants. A driver declares the
 * earliest pass it is willing to attach in; bus_attach_children runs the
 * passes in ascending order and only considers drivers whose pass has been
 * reached.
 *
 * The gap this closes: before, "driver A's probe needs driver B already
 * attached" worked or did not work depending on devclass_add_driver call
 * ORDER in flk.c - an ordering constraint expressed nowhere near the
 * drivers it constrains, and silently broken by adding a driver in the
 * wrong place. Now B declares BUS_PASS_RESOURCE, A declares
 * BUS_PASS_DEFAULT, and registration order stops mattering.
 *
 * A driver_t leaving .pass zero gets BUS_PASS_DEFAULT, not BUS_PASS_ROOT -
 * see driver_pass() in bus.c. Zero is the C default for an omitted
 * initialiser, and silently making every existing driver attach in the
 * earliest possible pass would be a real behaviour change disguised as a
 * new field. */
#define BUS_PASS_ROOT        0
#define BUS_PASS_BUS         10
#define BUS_PASS_CPU         20
#define BUS_PASS_RESOURCE    30
#define BUS_PASS_INTERRUPT   40
#define BUS_PASS_TIMER       50
#define BUS_PASS_SCHEDULER   60
#define BUS_PASS_SUPPORTDEV  100
#define BUS_PASS_DEFAULT     1000000

/* --- resources -----------------------------------------------------------
 * Upstream's SYS_RES_* types. The gap item 11 names: "today a driver reads
 * pci_bar_size straight off the raw pci_ivars_t with no bus-mediated
 * conflict detection at all" - two drivers could map the same BAR and
 * neither would find out. */
#define SYS_RES_IRQ      1
#define SYS_RES_DRQ      2
#define SYS_RES_MEMORY   3
#define SYS_RES_IOPORT   4

/* Flags for bus_alloc_resource. RF_ACTIVE is accepted and recorded but
 * activation is a no-op: on FreeBSD it maps the region into KVA, and here
 * a driver still reaches memory through paging.h's phys_to_virt directly.
 * Wiring activation to real mapping is a change to make when a driver
 * needs it, not before - but the flag is honoured in the sense that it is
 * stored and readable, so that change does not alter any call site. */
#define RF_ALLOCATED   0x0001
#define RF_ACTIVE      0x0002
#define RF_SHAREABLE   0x0004
#define RF_OPTIONAL    0x0008

/* Opaque, reached through the bus_get_resource_* accessors below - a driver
 * must not depend on the layout, which is what lets this grow a real rman
 * later.
 *
 * The struct TAG is upstream's `resource`, not `bus_resource`, and that is
 * load-bearing rather than cosmetic: kernel/include/sys/bus.h declares the
 * same bus_alloc_resource/bus_release_resource functions for real FreeBSD
 * driver source, spelled with `struct resource *`. Two different tags would
 * make those two declarations of one symbol disagree about its type. Same
 * arrangement bus_dev_t and device_t already use - one incomplete type,
 * two names, two audiences that never share a translation unit. */
typedef struct resource bus_resource_t;

typedef enum {
    DS_NOTPRESENT = 0,  /* no driver bound, or attach failed and unwound */
    DS_ATTACHING,       /* inside attach() - re-entrant attach is a bug   */
    DS_ATTACHED          /* attach() returned 0                            */
} bus_dev_state_t;

/* Longest device name, "foo" in foo0. Short on purpose: these are driver
 * names, not paths. */
#define BUS_DEVNAME_MAX 16

typedef struct genesis_driver {
    const char *name;

    /* Identify only. May be called on more than one candidate driver for
     * the same bus_dev_t, and the loser's binding is undone - do not touch
     * hardware state here beyond what is safe to read twice. Return a
     * BUS_PROBE_* priority, or a positive value for "not mine". */
    int (*probe)(bus_dev_t *dev);

    /* Real initialisation: map resources, program the hardware, allocate
     * whatever softc-owned state the driver needs. Called once, only on
     * the single winning driver. 0 on success; any other return unwinds
     * (softc freed, driver unbound, state set back to DS_NOTPRESENT). */
    int (*attach)(bus_dev_t *dev);

    /* Inverse of attach. Not called by anything yet - PCI devices do not
     * hot-remove in this pass - but declared now so a driver written
     * today does not need a signature change the day removal exists. */
    void (*detach)(bus_dev_t *dev);

    /* bus.c kcalloc()s this many bytes on a successful probe (before
     * attach runs) and kfree()s it if that driver loses or attach fails.
     * Zero means the driver keeps no per-instance state. */
    uint32 softc_size;

    /* Earliest boot pass this driver will attach in - see BUS_PASS_* above.
     * Zero means BUS_PASS_DEFAULT, which is what every driver written
     * before this field existed gets, and is why zero cannot mean
     * BUS_PASS_ROOT. */
    int pass;

    /* A FreeBSD device_method_t[] for a driver that came in through
     * kernel/newbus_compat.c, NULL for a Genesis-native driver.
     *
     * This is the KOBJ generalization item 11 asks for. Before, the
     * adapter walked the method table once, pulled out probe/attach/detach
     * by pointer identity, and THREW THE REST AWAY - so device_suspend,
     * device_resume, device_shutdown and any custom interface a real
     * FreeBSD driver declares were silently unreachable. Keeping the array
     * means newbus_method_get (newbus_compat.h) can dispatch any of them.
     *
     * Deliberately `const void *` and not `device_method_t *`: bus.h must
     * not include newbus_compat.h. Real KOBJ's dispatch is identity-based
     * underneath its codegen anyway, so an opaque pointer here loses
     * nothing that this kernel has. */
    const void *methods;
} driver_t;

typedef struct devclass *devclass_t;

/* Find the named devclass, creating it if this is the first time anything
 * has asked for it. Never fails in a way callers need to check -
 * DEVCLASS_MAX running out is a build-time-sized-wrong bug, not a runtime
 * condition, so bus.c halts loudly rather than returning NULL here. */
devclass_t devclass_find(const char *name);

/* Register a driver as a candidate for every future child added under this
 * devclass. Order matters only as a tie-break of last resort (see
 * bus_probe_and_attach in bus.c); do not rely on it. Returns 0, or -1 if
 * DRIVERS_PER_CLASS_MAX is full. */
int devclass_add_driver(devclass_t dc, driver_t *drv);

/* Add a bus_dev_t as `parent`'s child, in state DS_NOTPRESENT, carrying
 * `ivars` (bus-owned, e.g. a pci_ivars_t* - the child's driver reads it
 * through bus_get_ivars but does not own or free it). `dc` is the devclass
 * whose registered drivers will be tried against this child; pass NULL for
 * a node that exists only as a parent anchor and is never itself probed
 * (see pci.c's root). Returns NULL if BUS_DEV_MAX is full. Does not probe
 * or attach; that is a separate step so a bus can finish enumerating all
 * its children before any of them starts running driver code. */
bus_dev_t *bus_add_child(bus_dev_t *parent, devclass_t dc, void *ivars);

/* Try every driver registered on dev's devclass against dev, bind the
 * best-priority match (see BUS_PROBE_* above), and call its attach(). No
 * match is not an error - dev is left DS_NOTPRESENT and this returns 0,
 * the same "logged, not fatal" posture dev_attach in device.h takes for a
 * failed alias. Returns the winning driver's attach() result, or 0 if
 * nothing matched. */
int bus_probe_and_attach(bus_dev_t *dev);

/* Convenience: bus_probe_and_attach every child of `parent`, in the order
 * they were added. What a generic bus's own driver_t.attach typically is,
 * in its entirety. */
void bus_attach_children(bus_dev_t *parent);

/* Re-probe every UNATTACHED device in `dc`, for a driver that registered
 * after the boot-time attach pass already ran - which is every driver that
 * arrives in a loadable module, since modules are read off a filesystem and
 * that is up long after bus_attach_children. Upstream calls this
 * BUS_DRIVER_ADDED. Devices already attached are left alone; see the
 * implementation for why, and for what that costs. */
void bus_driver_added(devclass_t dc);

/* The driver-owned per-instance block bus.c allocated on a successful
 * probe, or NULL before a driver is bound. */
void *bus_get_softc(bus_dev_t *dev);

/* The bus-owned per-child data bus_add_child was given (e.g. a
 * pci_ivars_t*). Never NULL for a child a bus actually created. */
void *bus_get_ivars(bus_dev_t *dev);

/* The node this one was added under, or NULL for a root. bus.c has tracked
 * this since it existed; exposing it is what lets a driver walk up to its
 * bus the way real Newbus code does (device_get_parent). */
/* --- device identity (Part 15) -------------------------------------------
 *
 * Name a device explicitly, and give it the next free unit number for that
 * name - so the first is foo0, the second foo1. Returns the unit, or -1.
 *
 * This is what distinguishes a device somebody NAMED from one the bus merely
 * enumerated, and that distinction is the whole of BUS_PROBE_NOWILDCARD's
 * semantics (Part 16). Calling it a second time on the same device renames
 * it and takes a new unit. */
int device_set_devclass(bus_dev_t *dev, const char *name);

/* The explicitly-set name and unit, or NULL/-1 if the device was never
 * named.
 *
 * NOT called device_get_name: kernel/newbus_compat.c already owns that
 * symbol and gives it upstream's meaning, which is the DEVCLASS name and
 * falls back to the driver's. These are the raw accessors underneath it. */
const char *bus_get_devname(bus_dev_t *dev);
int         bus_get_devunit(bus_dev_t *dev);

/* Non-zero if something explicitly named this device - via
 * device_set_devclass or a hint - rather than it being matched by wildcard
 * probing. Upstream calls this `hasclass`. */
int device_has_class(bus_dev_t *dev);

bus_dev_t *bus_get_parent(bus_dev_t *dev);

/* The bound driver, or NULL before one is. Needed by newbus_method_get to
 * find the method table of whatever driver actually won. */
driver_t *bus_get_driver(bus_dev_t *dev);

/* dev's devclass, or NULL for a bare parent anchor. */
devclass_t bus_get_devclass(bus_dev_t *dev);

/* Give a devclass a parent to fall back on. If nothing registered directly
 * on `dc` matches a child, bus_probe_and_attach walks up the parent chain
 * and tries those drivers too - upstream's devclass inheritance, and the
 * reason a "pci_bridge" devclass can carry only what is special about
 * bridges and inherit the rest from "pci".
 *
 * A cycle would hang the probe loop, so bus.c refuses one and returns -1.
 * Returns 0 on success. */
int devclass_set_parent(devclass_t dc, devclass_t parent);

/* --- resources -----------------------------------------------------------
 * A real allocation with real conflict detection, replacing "every driver
 * reads pci_bar_size off the raw ivars and hopes".
 *
 * bus_alloc_resource reserves [start, start+count) of `type` for `dev` and
 * fails - returns NULL - if any of it is already held by another device.
 * That failure IS the feature: it is the thing that could not happen
 * before.
 *
 * `rid` is the caller's own identifier for the resource, passed by pointer
 * because upstream's API lets the bus rewrite it. For a PCI device it is
 * the BAR: either a small index 0-5 or the config-space offset PCIR_BAR(n)
 * (0x10 + 4n), both accepted, because real driver source uses the latter
 * and Genesis's own drivers naturally reach for the former.
 *
 * Passing start=0, end=~0, count=0 means "whatever the device says it
 * wants" - for PCI that is resolved through pci_bar_size(), which is the
 * one place this file knows about a specific bus. */
bus_resource_t *bus_alloc_resource(bus_dev_t *dev, int type, int *rid,
                                   uint64 start, uint64 end, uint64 count,
                                   uint32 flags);

/* The common form: "give me whatever this rid names, wherever it is". */
bus_resource_t *bus_alloc_resource_any(bus_dev_t *dev, int type, int *rid,
                                       uint32 flags);

/* Give it back. `r` must be one this device allocated; releasing another
 * device's resource is refused. Returns 0 on success, -1 otherwise. */
int bus_release_resource(bus_dev_t *dev, int type, int rid,
                         bus_resource_t *r);

/* Accessors, named after upstream's rman_get_* so a driver reads the same.
 * A NULL resource reads back as zero rather than faulting - a driver that
 * forgot to check bus_alloc_resource's return should get a wrong number,
 * not a page fault in the middle of its attach. */
uint64 bus_get_resource_start(bus_resource_t *r);
uint64 bus_get_resource_size(bus_resource_t *r);
uint64 bus_get_resource_end(bus_resource_t *r);
int    bus_get_resource_rid(bus_resource_t *r);
int    bus_get_resource_type(bus_resource_t *r);

/* Where a MEMORY resource has been mapped into kernel virtual space, and a
 * setter for whoever mapped it. Cached on the resource so a driver reading a
 * register in a loop does not remap per access. NULL until something maps
 * it; meaningless for an I/O-port resource. */
void  *bus_get_resource_mapping(bus_resource_t *r);
void   bus_set_resource_mapping(bus_resource_t *r, void *va);

/* One line per allocated resource - the same "report what was found"
 * posture as pci_report/e820_report, and what makes a conflict visible in
 * the boot log rather than only in a return value. */
void bus_resource_report(uint8 color);

/* --- module unload -------------------------------------------------------
 *
 * "Does anything here point into [base, base+size)?", asked by kldload.c
 * before it unmaps a module image. See bus.c for why the question is an
 * address range rather than a list of drivers the module declares.
 *
 * bus_can_unregister_range answers without changing anything: 0 if every
 * driver in the range can be taken off whatever it is attached to, -1 if one
 * is attached to a device and has no detach method - which is that driver
 * saying it cannot be removed, and is a refusal rather than a thing to
 * override.
 *
 * bus_unregister_range does it: detach every attached in-range driver, free
 * its softc, put the device back to DS_NOTPRESENT, and remove the driver from
 * every devclass candidate list. Returns how many driver registrations went,
 * or -1 if it refused for the reason above (in which case nothing changed).
 * `leaked_resources`, if non-NULL, receives the number of bus resources still
 * held after detach ran - always zero for a well-behaved driver, and the only
 * place a detach method that forgets to release one is visible. */
int bus_can_unregister_range(uint64 base, uint64 size);
bus_dev_t *bus_first_device(void);
int bus_unregister_range(uint64 base, uint64 size, int *leaked_resources);

/* How many devclasses exist, against the cap, and their names.
 *
 * Worth having because devclass_find HALTS when the pool is full rather than
 * failing - so without this the only way to learn the pool was nearly full
 * was to fill it, which stops the machine. Note that the bus selftest's four
 * devclasses are in this list and are never released; they are as permanent
 * as a real driver's. */
void bus_devclass_report(uint8 color);

/* Exercise the machinery this file added: resource conflict detection,
 * devclass inheritance, pass ordering, and method dispatch. Returns the
 * number of failures. Prints its own diagnosis. Boot-time only - none of
 * this is reachable from ring 3. */
int bus_selftest(void);

#endif
