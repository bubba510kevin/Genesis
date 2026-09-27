#ifndef LINUX_MODULE_H
#define LINUX_MODULE_H

#include "modinit.h"

/* All no-ops, matching LinuxKPI's own choice (vendsrc/sys/compat/linuxkpi/
 * common/include/linux/module.h): these are Linux module-loader metadata,
 * strings for `modinfo` to print rather than anything the loader acts on.
 * Genesis's kldload does not read them - it has no vermagic to check, since
 * a module here is rebuilt against these headers rather than being a binary
 * from another kernel. Real driver source keeps calling these; they just
 * vanish at preprocessing. */
#define MODULE_LICENSE(name)
#define MODULE_AUTHOR(name)
#define MODULE_DESCRIPTION(name)
#define MODULE_INFO(tag, info)
#define MODULE_FIRMWARE(firmware)

/* Real LinuxKPI expands this into a generated FreeBSD KOBJ method table
 * for build-time device matching. Genesis's driver_t has no such
 * generated-table step (see bus.h) - linux_pci_register_driver walks
 * id_table directly at runtime, so there is nothing for this macro to
 * generate. */
#define MODULE_DEVICE_TABLE(bus, table)

/* --- the entry point -----------------------------------------------------
 *
 * This is what a Linux driver actually declares, and until now this header
 * did not define it at all - so `module_init(foo)` was an undeclared
 * identifier and no real driver source could compile, never mind load.
 *
 * Linux's own definition for a modular build is an alias: `int init_module(void)
 * __attribute__((alias("foo")))`. That works because Linux's loader looks for
 * the symbol `init_module`. Genesis's looks for a pointer in a section
 * instead - see modinit.h for why the symbol-name approach could not be made
 * to cover DRIVER_MODULE and module_pci_driver as well - so this emits one
 * of those.
 *
 * The effect is the same from the driver's side, which is the only side that
 * matters here: declare your init function, it gets called on load, and a
 * non-zero return fails the load. */
#define module_init(fn) GENESIS_MODULE_INIT(fn)

/* module_exit is WIRED UP now. It used to be accepted and discarded, with a
 * comment saying so - "kldload has no unload path at all; nothing frees a
 * module's memory, unregisters its driver, or calls this".
 *
 * There is one now (kld_unload, kernel/driver/kldload.c), and this emits a
 * pointer into .genesis_modexit exactly as module_init emits one into
 * .genesis_modinit. Linux's own definition is the alias `void cleanup_module(void)
 * __attribute__((alias("foo")))`, for a loader that looks the symbol up by
 * name; see modinit.h for why the section is the mechanism here instead.
 *
 * What changed for a driver written against this header: nothing in the
 * source, and everything in the behaviour. The function is called. Anything
 * it forgets to unregister is now a detected, reported refusal to unload
 * rather than a leak nobody notices. */
#define module_exit(fn) GENESIS_MODULE_EXIT(fn)

/* late_initcall: run `fn` once the kernel is fully up - every CPU scheduling,
 * per-CPU areas replicated, workqueues usable. A module loaded at boot (before
 * that point) has its late initcalls queued and run then; one loaded later
 * has them run at load. Linux's ordering guarantee, in the only form a
 * module here can observe. */
int linux_register_late_initcall(int (*fn)(void));
#define late_initcall(fn)                                                      static int __lkpi_late_##fn(void) {                                           return linux_register_late_initcall(fn);                              }                                                                          GENESIS_MODULE_INIT(__lkpi_late_##fn)
#define late_initcall_sync(fn) late_initcall(fn)

/* THIS_MODULE is a pointer to the module's own struct module. Driver source
 * passes it to registration functions that record an owner for refcounting.
 * Genesis has no module refcounting, so there is nothing to point at and
 * nothing that would read it - NULL, rather than a fabricated object that
 * would imply the refcount works. */
struct module;
#define THIS_MODULE ((struct module *)0)

#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define MODULE_VERSION(ver)
#define MODULE_ALIAS(alias)

#endif
