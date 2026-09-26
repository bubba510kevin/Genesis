#ifndef _SYS_MODULE_H_
#define _SYS_MODULE_H_

/* <sys/module.h> - hand-written, not vendored.
 *
 * The real one was tried verbatim and wants MAXPATHLEN plus FreeBSD's
 * module-event and linker-set machinery, none of which Genesis has: a module
 * here is loaded by kernel/driver/kldload.c through a .genesis_modinit
 * section (see kernel/include/modinit.h), not by a linker set walked at
 * boot. There IS an unload path now (kld_unload), and it still does not
 * deliver MOD_UNLOAD: that event goes to a handler named in a moduledata
 * linker set, and Genesis has no linker set to walk. A FreeBSD driver's
 * teardown reaches the same place through its detach method, which
 * bus_unregister_range does call.
 *
 * What a DRIVER needs from this header is small: the module event constants,
 * because driver source switches on them in an event handler, and the
 * moduledata/module_t types so that handler's signature parses. DRIVER_MODULE
 * itself lives in <sys/bus.h> here.
 */

#define MOD_LOAD     0
#define MOD_UNLOAD   1
#define MOD_SHUTDOWN 2
#define MOD_QUIESCE  3

typedef struct module *module_t;
typedef int (*modeventhand_t)(module_t, int, void *);

typedef struct moduledata {
    const char     *name;
    modeventhand_t  evhand;
    void           *priv;
} moduledata_t;

/* Accepted and NOT wired up. Upstream puts the moduledata into a linker set
 * that the module loader walks; Genesis's loader looks in .genesis_modinit
 * for a function pointer instead. A driver declaring one of these compiles
 * and its event handler is never called - which matters for MOD_UNLOAD, and
 * does not for MOD_LOAD, since DRIVER_MODULE's generated init covers that. */
#define DECLARE_MODULE(name, data, sub, order)  \
    static const moduledata_t *const __unused_##name##_moddata = &(data)
#define MODULE_VERSION(module, version)
#define MODULE_DEPEND(module, mdepend, vmin, vpref, vmax)

#endif

/* MODULE_PNP_INFO - a machine-readable description of what a driver matches,
 * emitted into a section that devd(8) reads to autoload modules on hotplug.
 * Genesis has no hotplug notification and no userland autoloader, so there is
 * nothing to read it. A no-op rather than a section nobody parses: an empty
 * section would be indistinguishable from a working one. */
#define MODULE_PNP_INFO(d, b, unique, t, n)
