#ifndef SYS_BUS_H
#define SYS_BUS_H

#include "modinit.h"
#include "newbus_compat.h"

/* Real FreeBSD driver source gets NULL from <sys/param.h>, which it includes
 * before anything else. Genesis has no sys/param.h on the general include
 * path - kernel/bsd/compat/ has one, but that directory is prepended only for
 * the vendored subtree (see build.py's EXTRA_INCLUDES, and the comment there
 * about two vendored trees disagreeing about what <sys/param.h> means).
 *
 * Defining it here rather than adding a second sys/param.h: a driver always
 * includes <sys/bus.h>, and one more header that two trees would have to
 * agree about is exactly the problem EXTRA_INCLUDES exists to contain. */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* What real FreeBSD Newbus driver source includes (`#include <sys/bus.h>`,
 * confirmed against vendsrc/sys/dev/virtio/pci/virtio_pci_legacy.c and
 * vendsrc/sys/dev/e1000/if_em.h). Source-compat only, the same posture
 * kernel/include/linux/ already established: real driver .c recompiles
 * against this unmodified and links into Genesis's own bus.c - no .ko is ever
 * loaded (see the plan's "why FreeBSD .ko can't be loaded" research).
 *
 * device_t/bus_dev_t are declared here as the SAME incomplete type
 * (`struct bus_dev`) bus.h itself forward-declares - legal to redeclare
 * identically, and deliberately NOT done by including bus.h directly: that
 * would also pull in Genesis's own, differently-shaped `driver_t`
 * (probe/attach/detach fields) and collide with this file's driver_t
 * (method-table shape) below. This header and kernel/include/bus.h are
 * never included together in one translation unit - real driver source
 * uses this one, kernel/newbus_compat.c (the adapter) uses bus.h directly -
 * two disjoint audiences by design. */
typedef struct bus_dev  bus_dev_t;
typedef bus_dev_t       *device_t;

/* Same priority scale as kernel/include/bus.h's BUS_PROBE_* - a probe()
 * written against this header returns straight into bus_probe_and_attach's
 * comparison logic unchanged, no adapter translates the value. */
#define BUS_PROBE_SPECIFIC       0
#define BUS_PROBE_VENDOR       (-10)
#define BUS_PROBE_DEFAULT      (-20)
#define BUS_PROBE_LOW_PRIORITY (-40)
#define BUS_PROBE_GENERIC     (-100)
#define BUS_PROBE_HOOVER      (-1000000)
#define BUS_PROBE_NOWILDCARD  (-2000000000)

/* Boot passes - same values and same meaning as kernel/include/bus.h's,
 * which is where the model is documented. Duplicated rather than shared for
 * the same reason the BUS_PROBE_* scale above is: this header and bus.h are
 * never included together in one translation unit, by design. */
#define BUS_PASS_ROOT        0
#define BUS_PASS_BUS         10
#define BUS_PASS_CPU         20
#define BUS_PASS_RESOURCE    30
#define BUS_PASS_INTERRUPT   40
#define BUS_PASS_TIMER       50
#define BUS_PASS_SCHEDULER   60
#define BUS_PASS_SUPPORTDEV  100
#define BUS_PASS_DEFAULT     1000000

/* Resource types and flags, spelled the way real driver source spells them.
 * Same values as bus.h's. */
#define SYS_RES_IRQ      1
#define SYS_RES_DRQ      2
#define SYS_RES_MEMORY   3
#define SYS_RES_IOPORT   4

#define RF_ALLOCATED   0x0001
#define RF_ACTIVE      0x0002
#define RF_SHAREABLE   0x0004
#define RF_OPTIONAL    0x0008

/* PCIR_BAR(n) - the config-space offset of BAR n. Real driver source passes
 * this as the rid to bus_alloc_resource_any, so it has to resolve here;
 * bus.c's rid_to_bar accepts both this form and a bare 0-5 index.
 *
 * It genuinely belongs to <dev/pci/pcireg.h>, which is vendored verbatim at
 * kernel/bsd/compat/dev/pci/pcireg.h and must not be edited. Guarded so that
 * a translation unit seeing both headers takes the vendored definition and
 * does not warn - rather than guarding the vendored file, which is the one
 * thing that would cost the ability to re-fetch it. */
/* Spelled with PCIR_BARS, character for character as the vendored
 * <dev/pci/pcireg.h> spells it, and NOT as (0x10 + (x) * 4).
 *
 * Both expand to the same number. The reason the text has to match is that
 * driver source includes <sys/bus.h> before <dev/pci/pcireg.h>, so the
 * vendored header redefines this one - and C only permits a redefinition
 * silently when the replacement token sequence is IDENTICAL. An #ifndef
 * guard here cannot help, because this file is the one that comes first. */
#ifndef PCIR_BARS
#define PCIR_BARS       0x10
#endif
#ifndef PCIR_BAR
#define PCIR_BAR(x)             (PCIR_BARS + (x) * 4)
#endif

typedef struct freebsd_driver driver_t;

/* device_method_t and the six built-in *_desc tokens live in
 * newbus_compat.h (included above) - shared, untypedef'd, between this
 * file and kernel/newbus_compat.c. */

#define DEVMETHOD(NAME, FUNC) { &NAME##_desc, (void *)(FUNC) }
#define DEVMETHOD_END         { 0, 0 }

/* evh/arg (real FreeBSD: an event handler + its argument, for module
 * load/unload notification) are accepted, not honoured. There IS an unload
 * path now (kld_unload), and this is still not wired to it: the event handler
 * upstream expects is a DECLARED per-module callback, and kld_unload works
 * from the image's address range precisely so that it does not depend on a
 * module declaring anything - see kernel/driver/kldload.c. A driver that
 * needs teardown gets it through module_exit or through its detach method,
 * both of which are called. Same "accepted, not wired up" posture
 * linux/interrupt.h's IRQF_SHARED already takes. Expands to a generated
 * init function, not a call - module_pci_driver's own trick - so real
 * driver source needs zero edits; flk.c calls the generated function by
 * name, same as every other model. */
/* Two things now, not one. The generated function keeps its old name so
 * flk.c's explicit calls still work for drivers compiled INTO the kernel,
 * and GENESIS_MODULE_INIT registers the same function in the .genesis_modinit
 * section so kldload finds it when the driver arrives as a .ko instead. A
 * driver source file does not know which of the two it is going to be, which
 * is exactly why it must not have to say. */
/* The generated symbol is named for the driver AND THE BUS, which is what
 * upstream does (DECLARE_MODULE(name##_##busname, ...)) and is not
 * cosmetic. A driver may register on more than one bus - if_rl.c has both
 * DRIVER_MODULE(rl, pci, ...) and DRIVER_MODULE(rl, cardbus, ...) - and
 * naming the generated function for the driver alone makes those two a
 * duplicate definition. Found exactly that way, on the first real driver
 * that did it. */
#define DRIVER_MODULE(name, busname, driver, evh, arg) \
    int name##_##busname##_newbus_module_init(void) { \
        return newbus_register_driver_on(#busname, \
                                         (struct freebsd_driver *)&(driver)); \
    } \
    GENESIS_MODULE_INIT(name##_##busname##_newbus_module_init)

/* Implemented in kernel/newbus_compat.c: thin wrappers over bus_get_softc/
 * bus_get_ivars (bus.h) - device_t and bus_dev_t are literally the same
 * pointer type, so these need no translation, only a name real driver
 * source already expects. */
void *device_get_softc(device_t dev);
void *device_get_ivars(device_t dev);
device_t device_get_parent(device_t dev);
const char *device_get_name(device_t dev);

/* --- resources -----------------------------------------------------------
 * struct resource is opaque here exactly as it is upstream: a driver gets a
 * pointer from bus_alloc_resource_any and reads it back through rman_get_*.
 * The underlying type is kernel/include/bus.h's bus_resource_t; this header
 * cannot say so, since including bus.h would reintroduce the driver_t
 * collision this file's header comment explains. Declared as its own
 * incomplete type instead - a pointer is a pointer, and the driver never
 * dereferences it. */
struct resource;

/* The widths here are `unsigned long long` and `unsigned int` rather than
 * upstream's u_long/u_int typedefs because they must match kernel/include/
 * bus.h's uint64/uint32 EXACTLY - these are declarations of the same two
 * symbols, and the definitions in kernel/bus.c are compiled against that
 * header. Same width on LP64 either way; spelling them identically is what
 * makes that a fact rather than a coincidence. */
struct resource *bus_alloc_resource_any(device_t dev, int type, int *rid,
                                        unsigned int flags);
struct resource *bus_alloc_resource(device_t dev, int type, int *rid,
                                    unsigned long long start,
                                    unsigned long long end,
                                    unsigned long long count,
                                    unsigned int flags);
int bus_release_resource(device_t dev, int type, int rid,
                         struct resource *r);

/* rman_get_* are the names real driver source reads a resource back
 * through. Implemented in kernel/newbus_compat.c over bus.h's
 * bus_get_resource_* - separate names, so unlike the two functions above
 * there is no cross-header symbol to keep in agreement. */
unsigned long long rman_get_start(struct resource *r);
unsigned long long rman_get_size(struct resource *r);
unsigned long long rman_get_end(struct resource *r);
int                rman_get_rid(struct resource *r);

/* --- register access -----------------------------------------------------
 *
 * bus_read_N / bus_write_N, the modern FreeBSD spelling. Real driver source
 * also uses the older bus_space_read_N(tag, handle, off) three-argument form,
 * which needs a bus_space_tag_t and bus_space_handle_t pulled off the
 * resource first. Both exist upstream; this provides the newer one, because
 * the tag/handle pair is a machine-dependent abstraction over "which address
 * space" and there is exactly one of those here - so a tag would be a
 * parameter every caller passes and nothing reads.
 *
 * A driver written against the old form needs the three-line change to the
 * new one. That is a real, if small, source-compat gap, and it is here
 * rather than papered over with a fake tag.
 *
 * These work on BOTH memory and I/O-port resources, dispatching on what the
 * resource actually is - which is the whole point of the abstraction, and
 * why a driver can be written once for a device that appears at either. */
unsigned char  bus_read_1(struct resource *r, unsigned long off);
unsigned short bus_read_2(struct resource *r, unsigned long off);
unsigned int   bus_read_4(struct resource *r, unsigned long off);
void bus_write_1(struct resource *r, unsigned long off, unsigned char v);
void bus_write_2(struct resource *r, unsigned long off, unsigned short v);
void bus_write_4(struct resource *r, unsigned long off, unsigned int v);

/* --- interrupts ----------------------------------------------------------
 *
 * Upstream's signature, with the filter/handler split: `filter` runs at
 * interrupt level and decides whether the interrupt was ours, `handler` runs
 * later in a thread. Genesis has no interrupt threads, so a handler given
 * here runs at interrupt level too - which is stricter than the driver
 * asked for, not looser, and is said out loud because a driver that sleeps
 * in its handler would be legal upstream and is not here.
 *
 * INTR_TYPE_* and the cookie are accepted and the cookie is real - it is
 * what bus_teardown_intr uses to find the registration again. */
#define INTR_TYPE_TTY   0x01
#define INTR_TYPE_BIO   0x02
#define INTR_TYPE_NET   0x04
#define INTR_TYPE_CAM   0x08
#define INTR_TYPE_MISC  0x10
#define INTR_MPSAFE     0x200

/* What a filter returns. FILTER_STRAY means "not my device" - the interrupt
 * was somebody else's on a shared line - and FILTER_HANDLED means it was
 * ours and is dealt with. Getting these the wrong way round on a shared
 * line either steals another driver's interrupt or lets the line stay
 * asserted forever. */
#define FILTER_STRAY      1
#define FILTER_HANDLED    2
#define FILTER_SCHEDULE_THREAD 4

typedef int  driver_filter_t(void *);
typedef void driver_intr_t(void *);

int bus_setup_intr(device_t dev, struct resource *irq, int flags,
                   driver_filter_t *filter, driver_intr_t *handler,
                   void *arg, void **cookiep);
int bus_teardown_intr(device_t dev, struct resource *irq, void *cookie);

/* Deliver this interrupt to CPU `cpu` from now on (upstream's per-queue
 * spreading: a multi-queue NIC binds queue n's vector to CPU n). Real: the
 * IOAPIC entry's destination is rewritten. EINVAL for a CPU that is not
 * online; EOPNOTSUPP for a line the IOAPIC does not own (or the clock). */
int bus_bind_intr(device_t dev, struct resource *irq, int cpu);

/* Name the handler for reports ("re0:rx"). Recorded and printed by
 * newbus_intr_report. */
int bus_describe_intr(device_t dev, struct resource *irq, void *cookie,
                      const char *fmt, ...);

/* --- the rest of what driver source calls -------------------------------- */

/* device_printf prefixes the device's name and unit, which is the whole
 * reason drivers use it instead of printf - "em0: link up" rather than an
 * unattributed line. */
/* Has this device's driver actually attached? A driver's detach path asks,
 * because detach can be reached from a failed attach - where half its state
 * exists - as well as from a real teardown. */
int  device_is_attached(device_t dev);

/* Detach every child of `dev`. A driver with child devices (a MAC driver
 * with a miibus child, say) calls this from its own detach before tearing
 * down its own state, so the children release their resources first. */
int  bus_generic_detach(device_t dev);
int  bus_generic_attach(device_t dev);
device_t device_add_child(device_t dev, const char *name, int unit);
int  device_delete_child(device_t dev, device_t child);

/* The kernel-environment / device-hint lookup a driver uses for tunables:
 * resource_int_value("re", 0, "prefer_iomap", &v). Genesis has a real hint
 * source (kernel/driver/hints.c, a compiled-in device.hints in upstream's
 * format), so this is a real lookup and not a stub - it returns ENOENT when
 * no hint names that key, which is what a driver treats as "use the
 * default". */
int resource_int_value(const char *name, int unit, const char *resname,
                       int *result);
int resource_string_value(const char *name, int unit, const char *resname,
                          const char **result);

void device_printf(device_t dev, const char *fmt, ...);
void device_set_desc(device_t dev, const char *desc);
const char *device_get_desc(device_t dev);
int         device_get_unit(device_t dev);
const char *device_get_nameunit(device_t dev);

/* --- per-device sysctl (kernel/bsd/kern_devsysctl.c) ---------------------
 *
 * dev.<name>.<unit>. Real driver source calls these in attach to hang its
 * tunables somewhere - if_rl.c's twister_enable, if_re.c's re_add_sysctls -
 * and until they existed those two files DID NOT COMPILE, which is what made
 * modules/build.sh (and the userland build (Genesis-userland's build.sh) with it) exit 1.
 *
 * The context and the tree are created on first use and the same device gets
 * the same pair every time, so a driver may call either accessor in any
 * order. The tree is never NULL for a real device, because every caller
 * immediately dereferences it through SYSCTL_CHILDREN.
 *
 * Forward declarations rather than an include: <sys/sysctl.h> is a large
 * header and a driver that does not use these should not pay for it. Anyone
 * calling them includes it anyway - the macros are all there. */
struct sysctl_ctx_list;
struct sysctl_oid;
struct sysctl_ctx_list *device_get_sysctl_ctx(device_t dev);
struct sysctl_oid      *device_get_sysctl_tree(device_t dev);

/* Free everything the device hung off its context. Called by bus.c when a
 * driver is detached; a driver does not normally call it. */
void device_sysctl_fini(device_t dev);

/* --- and NOT malloc itself ------------------------------------------------
 *
 * <sys/malloc.h> owns malloc(9), on FreeBSD and here: kernel/bsd/compat/sys/
 * malloc.h declares it, defines struct malloc_type as a real type, and
 * kernel/bsd/mbuf.c defines the function.
 *
 * This header declared it too for a while, which was wrong in the way that
 * only shows up once two include paths meet: a driver compiled with BOTH
 * -Ikernel/bsd/compat and -Ikernel/include sees two declarations of one
 * symbol and the compiler rejects the second. A driver wanting malloc(9)
 * includes <sys/malloc.h>, which is what FreeBSD driver source already does.
 *
 * Only the pieces <sys/bus.h> genuinely owns stay here - the flags, because
 * a driver passes M_NOWAIT to bus_dma and other bus routines without
 * necessarily including malloc.h at all. Guarded so whichever header is seen
 * first wins. */
#ifndef M_WAITOK
#define M_WAITOK  0x0002
#define M_NOWAIT  0x0001
#define M_ZERO    0x0100
#endif

#endif
