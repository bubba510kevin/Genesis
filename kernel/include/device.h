#ifndef DEVICE_H
#define DEVICE_H

#include "object.h"
#include "typesk.h"

/* The device vtable, and one attached device behind it.
 *
 * --- Why this exists, and why it is not an IRP stack ----------------------
 * Three separate items wanted "the I/O manager" and meant three different
 * things by it: a volume object that parses \Device\HarddiskVolume2\bin\sh
 * into a file (item 1), a dispatch surface for WDM drivers to be called
 * through (item 4), and something for a NIC to attach itself to (item 6).
 * Deciding it three times produces three boundaries; deciding it once and
 * too big produces an IRP stack before there is a second driver to justify
 * one.
 *
 * So this is the fs_ops_t cut, applied a layer down. That split is the
 * working precedent in this tree: a vtable, a per-instance state pointer, and
 * negative errno out - and the proof it was real was syscall.c and fileobj.c
 * no longer including fat.h. The same proof applies here: syscall.c must not
 * include disk.h to open a file on a volume.
 *
 * What an IRP stack buys over this is asynchrony, cancellation, and a driver
 * stacking on top of another driver's requests. Genesis is uniprocessor,
 * every ata_read is synchronous, and there is exactly one driver per device.
 * All three of those are load-bearing for a real IRP and none of them is true
 * yet - and building the queue before the second CPU means testing an
 * asynchronous path with nothing that can be asynchronous.
 *
 * It is a first cut rather than a wrong turn because every slot below is one
 * IRP major function, deliberately:
 *
 *     parse    IRP_MJ_CREATE with a remaining name
 *     read     IRP_MJ_READ
 *     write    IRP_MJ_WRITE
 *     control  IRP_MJ_DEVICE_CONTROL
 *     detach   IRP_MJ_PNP / SURPRISE_REMOVAL
 *
 * The day a WDM .sys needs IoCallDriver, the dispatch table it registers is
 * translated into one of these five, and the callers above do not change.
 * Adding the IRP later is widening a call; adding it now is inventing five
 * queues with one entry each.
 *
 * --- What a device is, as against an object -------------------------------
 * object.c already answers "what can I do with this handle": read, write,
 * poll, close. A device is what is on the other end of that, and it needs two
 * things an object_t has no room for.
 *
 * The first is PARSING. \Device\HarddiskVolume2 is an object and
 * \Device\HarddiskVolume2\bin\sh is a FILE on it, and the namespace hands the
 * remainder over without interpreting it (see ns.h). Something has to turn
 * "\bin\sh" into a second object, and it is not the namespace and not the
 * filesystem - it is the device, which knows which volume it is.
 *
 * The second is PRESENCE. A file object is valid until it is closed. A device
 * object can outlive the hardware: a USB stick is pulled while a process
 * holds a descriptor on a file on it, and the correct answer is -ENODEV on
 * every subsequent operation, not a page fault in a driver reading a device
 * struct that was freed. So presence is a flag checked on every operation
 * rather than a lifetime managed by freeing.
 */

typedef struct device device_t;

/* What kind of thing this is. Not a driver model and not a class in the WDM
 * sense - the minimum needed to answer stat() honestly and to let a bus
 * enumerator find its children. */
typedef enum {
    DEVICE_KIND_NONE = 0,
    DEVICE_KIND_DISK,        /* a whole raw disk                            */
    DEVICE_KIND_VOLUME,      /* one partition, parses a path remainder      */
    DEVICE_KIND_CHAR,        /* console, serial, keyboard                   */
    DEVICE_KIND_NET          /* reserved for item 6; nothing sets it yet    */
} device_kind_t;

/* Removable: the medium can go away while the device object is referenced.
 * Not the same as "the object can be deleted" - see dev_detach. */
#define DEVICE_REMOVABLE  0x0001

/* Set by dev_detach, cleared by nothing. A device that has been removed stays
 * removed; the object is reused only after the last reference drops and the
 * slot is recycled, and by then it is a different device. */
#define DEVICE_GONE       0x0002

/* Route this device's I/O through the block cache (bcache.h).
 *
 * Set by the driver, not derived from the kind, and set only by the BOTTOM
 * device in a stack. A volume forwards to its parent disk, so a volume marked
 * cached would keep a second copy of blocks its parent already holds - two
 * views of one thing, which is the bug shape this tree keeps finding. The
 * flag being explicit is what makes that decision visible in disk.c and
 * absent from volume.c, instead of implied by a rule in device.c that both
 * would have to keep agreeing with. */
#define DEVICE_CACHED     0x0004

typedef struct device_ops {
    const char *name;

    /* Turn an unparsed namespace remainder into an object.
     *
     * `remainder` is what followed this device's name, in NAMESPACE spelling:
     * backslash-separated, possibly empty. An empty remainder means the
     * device itself was named, and a device that has nothing to hand back for
     * that returns -EINVAL rather than NULL-with-zero.
     *
     * On success *out holds a REFERENCED object and the caller owns that
     * reference, matching ns_lookup - so a caller that resolves through
     * either has one ownership rule, not two.
     *
     * NULL for a device with no namespace below it, which is every character
     * device: \Device\Console\anything is -ENOTDIR, and that answer comes
     * from this slot being NULL rather than from each driver remembering to
     * write it. */
    int (*parse)(device_t *dev, const char *remainder, uint32 access,
                 object_t **out);

    /* Bytes at an offset. Both may be NULL; a NULL write is what makes a
     * read-only medium answer -EROFS in one place, exactly as a NULL
     * fs_ops_t::write does one layer up. */
    int64 (*read)(device_t *dev, uint64 offset, void *buf, uint64 n);
    int64 (*write)(device_t *dev, uint64 offset, const void *buf, uint64 n);

    /* Everything that is not a byte transfer: geometry, eject, media-change
     * polling, and the ioctl surface. Codes live in each driver's header, not
     * here - a central list of every control code every driver will ever have
     * is the file nobody can add to without touching.
     *
     * REACHED FROM ring 3 now. sys_ioctl dispatches here for any descriptor
     * that names a device and is not the console; ROADMAP item 14 listed this
     * slot as documented, wired and having no caller at all, which meant the
     * next person to add a control code would have found it did nothing.
     *
     * `arg_size` is decoded from the Linux ioctl encoding - dir(2) size(14)
     * type(8) nr(8) - so a driver is told how big its argument is without a
     * table mapping every code to a length. It is 0 for the legacy terminal
     * codes, which never reach here. */
    int (*control)(device_t *dev, uint32 code, void *arg, uint64 arg_size);

    /* Which of OB_POLLIN / OB_POLLOUT hold right now, for poll(2). Optional:
     * a device without one is "always ready", which is the truth for a disk
     * and was the answer every device gave before a device that can BLOCK
     * on read (the mouse) existed. A driver with a poll slot must also wake
     * waitq_readiness() - through waitq_wake_all on any queue - when the
     * answer changes, or a poller sleeps through the event. */
    int (*poll)(device_t *dev, int events);

    /* The physical page behind byte `offset` of the device, for mmap(2) -
     * the framebuffer is the first device with one. `offset` is page
     * aligned. On success *phys is the page's physical address and
     * *cache_flags the caching bits the mapping must carry (PAGE_PCD for
     * video memory); -EINVAL for an offset past the end.
     *
     * The page is DEVICE memory: it is mapped with PAGE_DEVICE, which tells
     * the VMM it owns no frame there - munmap and exit must not free it, and
     * fork must share it rather than copy-on-write it. */
    int (*mmap)(device_t *dev, uint64 offset, uint64 *phys,
                uint64 *cache_flags);

    /* The medium went away. Called by dev_detach AFTER the namespace names
     * are gone and DEVICE_GONE is set, so a driver freeing its own state here
     * cannot race a lookup that is still in flight.
     *
     * Must not free the device_t: references to it are still out there, held
     * by open descriptors that have not been closed yet, and every one of
     * them is about to get -ENODEV from dev_read/dev_write rather than a use
     * after free. */
    void (*detach)(device_t *dev);
} device_ops_t;

struct device {
    const device_ops_t *ops;
    device_kind_t       kind;
    uint32              flags;
    void               *body;        /* the driver's own per-device state  */
    uint64              size;        /* bytes, or 0 for a non-sized device */
    uint32              block_size;  /* 0 if not block-addressable         */

    /* The device this one was created on, or NULL. A volume's parent is the
     * disk it is a partition of, and that is what makes pulling the disk
     * detach every volume on it without disk.c keeping its own list. */
    device_t           *parent;

    /* Its own name in the namespace, so dev_detach can undo exactly the names
     * dev_attach made. Storing it beats recomputing it: the naming rule lives
     * in one function, and a rule that is applied twice is a rule that
     * eventually disagrees with itself. */
    char                ns_name[64];
    char                dos_name[16];   /* the \??\ alias, or empty        */

    int                 in_use;
};

/* Take a free device slot. Returns NULL when the pool is full. The device is
 * NOT yet in the namespace and nothing can find it - fill it in and call
 * dev_attach. */
device_t *dev_alloc(void);

/* Name the device at `ns_name`, and optionally alias it at \??\<dos_name>.
 * Pass NULL or "" for dos_name to skip the alias.
 *
 * Returns 0, or a negative errno with the device left unattached and the
 * slot released - a half-attached device that is in one directory and not the
 * other is precisely the /dev inconsistency devices.c exists to prevent. */
int dev_attach(device_t *dev, const char *ns_name, const char *dos_name);

/* The medium is gone. Removes both namespace names, sets DEVICE_GONE, and
 * calls ops->detach. Every child device (a volume on this disk) is detached
 * first, depth first.
 *
 * Safe to call on a device with open handles on it, which is the entire
 * point: after this the NAME is unresolvable and the OBJECT is still alive
 * for as long as something holds it, answering -ENODEV. That is the split NT
 * draws between deleting a device object and the object's last reference
 * going away, and it is why removal cannot be implemented as "free it". */
void dev_detach(device_t *dev);

/* Release a detached slot for reuse. Only valid once nothing references the
 * device; today nothing calls it and slots are never recycled, which is the
 * honest state of affairs rather than a free that might be early. */
void dev_free(device_t *dev);

/* The operations, wrapped - the same argument as fs.h makes for its wrappers,
 * plus one more that only applies here: DEVICE_GONE is checked in ONE place.
 * A presence check that each driver performs is a presence check one driver
 * forgets, and the failure is a read from hardware that is not there. */
int64 dev_read(device_t *dev, uint64 offset, void *buf, uint64 n);
int64 dev_write(device_t *dev, uint64 offset, const void *buf, uint64 n);

/* The same two, with the block cache skipped.
 *
 * Two callers, and both are structural rather than a matter of taste: the
 * cache itself, which would otherwise recurse into its own front door, and a
 * device with no DEVICE_CACHED flag, which reaches these through dev_read
 * anyway. Everything else must use dev_read - a driver or a filesystem
 * reaching for the raw form is asking for a copy of a block that the cache
 * may hold a newer version of, and there is no correct reason to want that.
 *
 * They are named rather than static in device.c because bcache.c is a
 * separate file on purpose; the alternative was a callback pointer, which is
 * indirection bought with nothing. */
int64 dev_read_raw(device_t *dev, uint64 offset, void *buf, uint64 n);
int64 dev_write_raw(device_t *dev, uint64 offset, const void *buf, uint64 n);
int   dev_control(device_t *dev, uint32 code, void *arg, uint64 arg_size);
/* poll: the driver's answer, or "always ready" for a driver with no poll
 * slot or a device that has gone (its read then says -ENODEV). mmap: the physical
 * page behind a page-aligned offset (see device_ops_t::mmap), -ENODEV for a
 * device that cannot be mapped. */
int   dev_poll(device_t *dev, int events);
int   dev_mmap(device_t *dev, uint64 offset, uint64 *phys,
               uint64 *cache_flags);
int   dev_parse(device_t *dev, const char *remainder, uint32 access,
                object_t **out);

/* Non-zero if the device is still present. */
int dev_present(const device_t *dev);

/* --- reaching a device through a handle ---------------------------------
 *
 * ONE object_type_t for every device, with the device_t as its body. That is
 * what the vtable buys: a serial port and a raw disk are the same object as
 * far as read(2) is concerned, and they differ in the device_ops_t behind
 * them rather than in an object_type_t each. Adding a driver adds no object
 * type, which is the difference from where disk.c stands today. */
object_t *dev_object_create(device_t *dev);

/* Non-zero if this object is a device. What stat() asks before reading a body
 * as a device_t - the same question disk_is_block asks, generalised, and for
 * the same reason: reading one kind of body as another produces a plausible
 * number rather than an error. */
int dev_object_is(const object_t *obj);

/* The device behind an object, or NULL if it is not one. */
device_t *dev_from_object(const object_t *obj);

/* Resolve a namespace result - an object plus whatever followed its name -
 * into the object the caller actually asked for.
 *
 * This is the ONE place a remainder gets consumed, and it exists because
 * there were about to be three. ns_lookup hands back (object, remainder) to
 * NtCreateFile, to sys_openat's /dev path, and to stat; each of them had to
 * decide what a non-empty remainder means, and each of them was going to
 * decide it slightly differently.
 *
 * NtCreateFile's version was already wrong in a way worth spelling out: it
 * converted "\etc\motd" to "/etc/motd" and called fileobj_open, which routes
 * through the MOUNT TABLE. Correct while there was one volume and it was the
 * root. With two, \??\D:\notes.txt resolves to the volume D:, discards it,
 * and reads /notes.txt off whatever is mounted at "/" - a path that names one
 * disk and reads another, with no error anywhere.
 *
 * On success *out holds a referenced object and `obj`'s reference has been
 * consumed either way, so the caller has exactly one thing to release on
 * every path. */
int dev_open_object(object_t *obj, const char *remainder, uint32 access,
                    object_t **out);

/* Walk every attached device. Stops early on a non-zero return, which is
 * returned. For the boot report and for volume.c's "which disks are there"
 * pass. */
int dev_iterate(int (*cb)(device_t *dev, void *ctx), void *ctx);

/* Print one line per device. Behind the same verbosity flag as ns_report. */
void dev_report(void);

#endif
