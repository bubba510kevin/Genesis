#ifndef NEWBUS_COMPAT_H
#define NEWBUS_COMPAT_H

/* The one real definition of FreeBSD's driver_t layout ({.name, .methods,
 * .size} - confirmed against vendsrc/sys/dev/virtio/pci/virtio_pci_legacy.c's
 * own vtpci_legacy_driver initializer). This struct has NO typedef here on
 * purpose: kernel/include/sys/bus.h (what real driver source includes)
 * typedefs it to the bare name `driver_t` for that source's sake, but
 * kernel/newbus_compat.c needs BOTH that shape and Genesis's own, completely
 * different, driver_t (kernel/include/bus.h) visible in one translation
 * unit - two types cannot both be spelled `driver_t` unqualified in the same
 * file. Untypedef'd here, newbus_compat.c reaches this one as `struct
 * freebsd_driver` explicitly and bus.h's `driver_t` as itself, with no
 * collision - the same kind of same-name problem linux/pci.h's <pci.h>
 * angle-include fix solved differently last pass. */
struct freebsd_driver {
    const char    *name;
    void          *methods;   /* device_method_t[], see below */
    unsigned long  size;      /* softc size - same field bus.h's own
                                * driver_t.softc_size means */
    int            pass;      /* BUS_PASS_* (bus.h); 0 means DEFAULT.
                                * Upstream keeps the pass in the DRIVER_MODULE
                                * ordering macro rather than in driver_t, but
                                * a field costs nothing and lets a driver say
                                * so where it is defined. */
};

/* { desc, func } pairs. Real KOBJ's desc is a generated interface-
 * description struct (from .m-file codegen Genesis doesn't have - see
 * kernel/include/bus.h's own "no build-time code generation" stance);
 * here it is just some distinct address per built-in method name, matched
 * by pointer identity in newbus_register_driver the same way real KOBJ
 * dispatch is identity-based underneath its codegen. Declared here
 * (rather than in sys/bus.h, where DEVMETHOD/DRIVER_MODULE live) so both
 * sys/bus.h (driver source's view) and newbus_compat.c (the adapter) see
 * the identical layout without re-including each other. */
typedef struct {
    const void *desc;
    void       *func;
} device_method_t;

/* Only the six built-in device_t methods have a real desc token - a
 * custom interface's own DEVMETHOD entries (e.g. virtio_bus_*) don't
 * match any of them and are silently skipped when the table is walked;
 * nothing in this pass's bus.c calls a custom interface anyway. Defined
 * in kernel/newbus_compat.c. */
extern const int device_probe_desc;
extern const int device_attach_desc;
extern const int device_detach_desc;
extern const int device_suspend_desc;
extern const int device_resume_desc;
extern const int device_shutdown_desc;

/* Implemented in kernel/newbus_compat.c. Walks drv->methods, builds one
 * Genesis-native driver_t (bus.h) directly, registers it on the "pci"
 * devclass. Not called automatically - see bus.h's "no magic" rule; the
 * DRIVER_MODULE macro in sys/bus.h generates a named init function that
 * calls this, and flk.c calls that function by hand, the same pattern
 * module_pci_driver/IoCreateDriver already established. */
int newbus_register_driver(struct freebsd_driver *drv);

/* The same, onto a NAMED devclass. DRIVER_MODULE's busname argument reaches
 * this - a driver may register on more than one bus, and if_rl.c registers
 * on pci, on cardbus, and registers miibus on itself. */
int newbus_register_driver_on(const char *busname, struct freebsd_driver *drv);

/* --- general KOBJ-shaped method dispatch --------------------------------
 * The gap item 11 names: newbus_register_driver used to extract
 * probe/attach/detach and throw the rest of the method table away, so
 * device_suspend/_resume/_shutdown and every custom interface method a real
 * FreeBSD driver declares were unreachable. The array is kept now, and
 * these find any method in it.
 *
 * `desc` is a desc token's ADDRESS - one of the six built-ins below, or a
 * driver-defined one. A custom interface needs nothing from this file: it
 * declares its own `const int foo_method_desc;`, uses DEVMETHOD(foo_method,
 * fn) like any other, and looks it up here. That is the whole point - real
 * KOBJ is identity-based underneath its .m-file codegen too, so a token
 * address is a faithful stand-in for a generated descriptor struct.
 *
 * Returns NULL if the driver does not implement the method, which is a
 * normal answer and not an error - the same thing a KOBJ lookup gives for
 * an unimplemented method. The caller casts to the interface's own
 * signature; there is no type checking here and there is none in real KOBJ
 * either.
 *
 * Spelled with explicit struct tags rather than bus.h's bus_dev_t and
 * driver_t typedefs, because this header is also included by
 * kernel/include/sys/bus.h - real driver source's view - where `driver_t`
 * means something else entirely (struct freebsd_driver, above). A tag
 * cannot be shadowed by a typedef, so these two declarations mean the same
 * thing in both audiences without either one including bus.h.
 *
 * Declared at file scope first: a struct tag first seen inside a prototype's
 * parameter list gets PROTOTYPE scope, so each prototype would declare its
 * own incompatible type - and this header is specifically trying to name the
 * SAME types bus.h does. */
struct bus_dev;
struct genesis_driver;

void *newbus_method_get(struct bus_dev *dev, const void *desc);

/* Same, against a driver directly, for a caller that has not bound a device
 * yet - bus.c's own driver_t, which is `struct genesis_driver`. */
void *newbus_driver_method_get(struct genesis_driver *drv, const void *desc);

#endif
