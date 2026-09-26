#ifndef _MIIBUS_IF_H_
#define _MIIBUS_IF_H_

#include <sys/bus.h>
#include <newbus_compat.h>

/* "miibus_if.h" - HAND-WRITTEN, and upstream GENERATES it.
 *
 * On FreeBSD this file does not exist in the source tree at all. It is
 * produced at build time by tools/makeobjops.awk from dev/mii/miibus_if.m,
 * an interface-definition file, as part of the KOBJ code-generation step.
 * Genesis has no build-time code generation (kernel/include/bus.h says so
 * outright), so the generated artefact is written by hand instead.
 *
 * That is a real and permanent difference, not a shortcut. What codegen buys
 * upstream is that adding a method to the .m file updates every caller's
 * dispatch automatically and type-checks the signatures. Here both are
 * manual: this file and dev/mii/miibus_if.m have to be kept in agreement by
 * a person, and getting a signature wrong is a call through a mismatched
 * function pointer rather than a compile error.
 *
 * What makes it WORK at all is the KOBJ generalization ROADMAP item 11
 * added to kernel/driver/newbus_compat.c. driver_t carries its whole method
 * table and newbus_method_get dispatches any entry by descriptor identity -
 * which is exactly what real KOBJ does underneath its codegen, and it is why
 * a custom interface like this one needs no changes to bus.c.
 *
 * Before that, this could not have been written: the registration path
 * extracted probe/attach/detach by identity and DISCARDED everything else,
 * so a DEVMETHOD(miibus_readreg, ...) entry was silently unreachable. That
 * was the gap item 11 named, and this file is the first consumer of its
 * closing.
 */

/* The descriptor tokens. Only their ADDRESS matters - never their contents -
 * which is the same contract kernel/include/newbus_compat.h documents for
 * the six built-in device_* tokens. Defined in kernel/bsd/miibus.c. */
extern const int miibus_readreg_desc;
extern const int miibus_writereg_desc;
extern const int miibus_statchg_desc;
extern const int miibus_linkchg_desc;
extern const int miibus_mediainit_desc;

/* The dispatch macros a PHY driver calls to reach back into its MAC parent.
 * Each looks the method up on the device's driver and calls it, or returns a
 * benign default if the driver did not implement it - which is what KOBJ's
 * generated default methods do too. */
static __inline int MIIBUS_READREG(device_t dev, int phy, int reg) {
    int (*fn)(device_t, int, int) =
        (int (*)(device_t, int, int))newbus_method_get(dev, &miibus_readreg_desc);

    return fn != 0 ? fn(dev, phy, reg) : 0;
}

static __inline int MIIBUS_WRITEREG(device_t dev, int phy, int reg, int val) {
    int (*fn)(device_t, int, int, int) =
        (int (*)(device_t, int, int, int))newbus_method_get(dev,
                                                &miibus_writereg_desc);

    return fn != 0 ? fn(dev, phy, reg, val) : 0;
}

static __inline void MIIBUS_STATCHG(device_t dev) {
    void (*fn)(device_t) =
        (void (*)(device_t))newbus_method_get(dev, &miibus_statchg_desc);

    if (fn != 0) {
        fn(dev);
    }
}

static __inline void MIIBUS_LINKCHG(device_t dev) {
    void (*fn)(device_t) =
        (void (*)(device_t))newbus_method_get(dev, &miibus_linkchg_desc);

    if (fn != 0) {
        fn(dev);
    }
}

static __inline void MIIBUS_MEDIAINIT(device_t dev) {
    void (*fn)(device_t) =
        (void (*)(device_t))newbus_method_get(dev, &miibus_mediainit_desc);

    if (fn != 0) {
        fn(dev);
    }
}

#endif
