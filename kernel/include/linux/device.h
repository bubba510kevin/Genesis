#ifndef LINUX_DEVICE_H
#define LINUX_DEVICE_H

#include "linux/types.h"
#include "kprintf.h"

/* Reduced struct device - name and parent only. Real LinuxKPI's version
 * (vendsrc/sys/compat/linuxkpi/common/include/linux/device.h) carries a
 * bsddev handle, a kobject, devres lists, irqents - all FreeBSD-devres/
 * kobj plumbing Genesis has no equivalent of and driver probe/remove code
 * does not touch directly. Add fields here only when a real driver reads
 * one; a field nothing uses is a lie about what this shim supports. */
struct device {
    const char    *name;
    struct device *parent;
    /* Driver-private pointer. Added because it is not plumbing - it is the
     * one field driver code genuinely reads and writes itself, in probe and
     * again in remove/irq, and there is no way to write a Linux driver
     * without it. */
    void          *driver_data;
};

static inline void dev_set_drvdata(struct device *dev, void *data) {
    dev->driver_data = data;
}
static inline void *dev_get_drvdata(const struct device *dev) {
    return dev->driver_data;
}
static inline const char *dev_name(const struct device *dev) {
    return dev->name != 0 ? dev->name : "(unnamed)";
}

/* dev is unused - Genesis has no per-device log routing yet, every message
 * goes through the one kprintf sink (screen + serial, see kprintf.h). The
 * parameter stays in the macro so real driver source (dev_err(&pdev->dev,
 * ...)) needs no edits; when per-device routing matters this is the one
 * place it gets added. */
#define dev_err(dev, fmt, ...)    kprintf_c(0x0C, fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)   kprintf_c(0x0E, fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) kprintf_c(0x0F, fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)   kprintf_c(0x0F, fmt, ##__VA_ARGS__)
/* No-op, matching LinuxKPI's own choice - Genesis has no debug-log gate to
 * hook this to yet. */
#define dev_dbg(dev, fmt, ...)    do { } while (0)

#endif
