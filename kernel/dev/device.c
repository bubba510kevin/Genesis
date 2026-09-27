#include "bcache.h"
#include "device.h"
#include "ns.h"
#include "object.h"
#include "screen.h"
#include "typesk.h"

/* The generic half of the device layer. See device.h for why it is a vtable
 * rather than an IRP stack.
 *
 * There is deliberately no driver knowledge in this file - no ATA, no
 * partition table, no filesystem. If something here ever needs to know what a
 * disk is, the boundary has failed and this is where it will be visible. That
 * is the same standard vfs.c is held to, and for the same reason. */

/* 24, up from 16 when the framebuffer and the mouse joined the disks,
 * volumes and serial port: the boot machine already used most of sixteen,
 * and a full pool fails dev_alloc quietly at attach time. */
#define DEVICE_MAX 24

static device_t dev_pool[DEVICE_MAX];

static int str_copy(char *dst, uint64 cap, const char *src) {
    uint64 i;

    for (i = 0; i + 1 < cap && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return src[i] == '\0' ? 0 : -36;         /* -ENAMETOOLONG */
}

device_t *dev_alloc(void) {
    int i;

    for (i = 0; i < DEVICE_MAX; i++) {
        if (!dev_pool[i].in_use) {
            device_t *d = &dev_pool[i];
            int k;

            /* Cleared by hand rather than by a memset the kernel does not
             * have. Every field, including the two names: a recycled slot
             * carrying the previous device's ns_name would make dev_detach
             * remove a name this device never inserted. */
            d->ops        = NULL;
            d->kind       = DEVICE_KIND_NONE;
            d->flags      = 0;
            d->body       = NULL;
            d->size       = 0;
            d->block_size = 0;
            d->parent     = NULL;
            for (k = 0; k < (int)sizeof(d->ns_name); k++) {
                d->ns_name[k] = '\0';
            }
            for (k = 0; k < (int)sizeof(d->dos_name); k++) {
                d->dos_name[k] = '\0';
            }
            d->in_use = 1;
            return d;
        }
    }
    return NULL;
}

void dev_free(device_t *dev) {
    if (dev != NULL) {
        dev->in_use = 0;
        dev->ops    = NULL;
    }
}

/* --- the object that a device is reached through -------------------------
 *
 * One object type for every device, with the device_t as its body. That is
 * the point of the vtable: disk.c's disk_type, a future serial_type and a
 * volume's raw form are the same object as far as read(2) is concerned, and
 * they differ in the device_ops_t behind them rather than in a second
 * object_type_t each. */

static int64 devobj_read(object_t *obj, void *buf, uint64 n, uint64 *offset) {
    device_t *d = (device_t *)obj->body;
    int64 got = dev_read(d, *offset, buf, n);

    if (got > 0) {
        *offset += (uint64)got;
    }
    return got;
}

static int64 devobj_write(object_t *obj, const void *buf, uint64 n,
                          uint64 *offset) {
    device_t *d = (device_t *)obj->body;
    int64 put = dev_write(d, *offset, buf, n);

    if (put > 0) {
        *offset += (uint64)put;
    }
    return put;
}

/* A synchronous device is always ready. The mouse was the first driver with
 * an interrupt-driven queue, and it answers through device_ops_t::poll; a
 * driver without that slot still gets "always ready" (dev_poll).
 *
 * A REMOVED device is also always ready, and reports both directions rather
 * than an error. poll(2) has no way to say -ENODEV; a poller told the
 * descriptor will never be readable waits forever, where one told it is
 * readable calls read, gets -ENODEV, and learns the truth on a call that can
 * express it. */
static int devobj_poll(object_t *obj, int events) {
    return dev_poll((device_t *)obj->body, events);
}

static const object_type_t device_object_type = {
    .name  = "device",
    .klass = OBJ_BLOCK,
    .read  = devobj_read,
    .write = devobj_write,
    .poll  = devobj_poll
    /* No destroy: the body is a pool entry owned by this file, and it must
     * outlive every object pointing at it - that is exactly what makes a read
     * after removal return -ENODEV instead of faulting. */
};

object_t *dev_object_create(device_t *dev) {
    if (dev == NULL) {
        return NULL;
    }
    return ob_create(&device_object_type, dev);
}

int dev_object_is(const object_t *obj) {
    return obj != NULL && obj->type == &device_object_type;
}

device_t *dev_from_object(const object_t *obj) {
    if (!dev_object_is(obj)) {
        return NULL;
    }
    return (device_t *)obj->body;
}

/* --- attach and detach --------------------------------------------------- */

int dev_attach(device_t *dev, const char *ns_name, const char *dos_name) {
    object_t *obj;
    int rc;

    if (dev == NULL || dev->ops == NULL || ns_name == NULL) {
        return -22;                          /* -EINVAL */
    }
    rc = str_copy(dev->ns_name, sizeof(dev->ns_name), ns_name);
    if (rc != 0) {
        return rc;
    }

    /* Create the containing directories first.
     *
     * ns_insert does NOT create parents - it calls split() with
     * create_parents = 0 and returns -ENOENT if any intermediate directory is
     * missing. That is the right default for a name that came from outside,
     * where auto-creating directories would turn a typo into a silently
     * successful insert at the wrong path.
     *
     * A device name is different: it is generated here from a template, not
     * typed, so the containers are part of the name rather than an assumption
     * about the namespace. \Device\Harddisk0\DR0 needs \Device\Harddisk0
     * to exist, and NT really does have that as a directory holding DR0
     * beside Partition1 and friends.
     *
     * This was found the hard way. name_for built the disk name by patching
     * two hand-counted indices into a literal and got both wrong, which
     * overwrote the separating backslash - so the name was ONE component and
     * needed no parent. Fixing the name is what first made a two-level device
     * name reach ns_insert, and every disk then failed to register with
     * -ENOENT. The bug and the thing hiding it were in the same line. */
    {
        char   parent[NS_PATH_MAX];
        uint64 i;
        int    last = -1;

        for (i = 0; dev->ns_name[i] != '\0'; i++) {
            if (dev->ns_name[i] == '\\') {
                last = (int)i;
            }
        }
        /* last > 0 skips the leading separator: a name like \Foo has its only
         * backslash at index 0 and lives directly in the root, which already
         * exists. */
        if (last > 0) {
            for (i = 0; i < (uint64)last; i++) {
                parent[i] = dev->ns_name[i];
            }
            parent[last] = '\0';
            if (ns_mkdir(parent) == NULL) {
                dev->ns_name[0] = '\0';
                return -2;                   /* -ENOENT */
            }
        }
    }

    obj = dev_object_create(dev);
    if (obj == NULL) {
        dev->ns_name[0] = '\0';
        return -23;                          /* -ENFILE */
    }

    rc = ns_insert(dev->ns_name, obj);
    ob_deref(obj);          /* the namespace holds the reference now */
    if (rc != 0) {
        dev->ns_name[0] = '\0';
        return rc;
    }

    if (dos_name != NULL && dos_name[0] != '\0') {
        char link[NS_PATH_MAX];
        uint64 n = 0;
        uint64 i;

        link[n++] = '\\'; link[n++] = '?'; link[n++] = '?'; link[n++] = '\\';
        for (i = 0; dos_name[i] != '\0'; i++) {
            if (n + 1 >= sizeof(link)) {
                break;
            }
            link[n++] = dos_name[i];
        }
        link[n] = '\0';

        /* A failed alias is not a failed attach. The device is reachable
         * under its own \Device\ name either way - that is the whole content
         * of the /dev merged view in devices.c, and treating a missing alias
         * as fatal would put us back to the state where a link is what makes
         * a device exist. */
        if (ns_link(link, dev->ns_name) == 0) {
            str_copy(dev->dos_name, sizeof(dev->dos_name), dos_name);
        }
    }
    return 0;
}

void dev_detach(device_t *dev) {
    int i;

    if (dev == NULL || !dev->in_use || (dev->flags & DEVICE_GONE)) {
        return;
    }

    /* Children first, depth first. Pulling a disk detaches the volumes on it,
     * and the volume has to go first: while it is still named, a lookup can
     * still reach it and it would go on answering from a parent that has
     * already been told to release its state. */
    for (i = 0; i < DEVICE_MAX; i++) {
        if (dev_pool[i].in_use && dev_pool[i].parent == dev) {
            dev_detach(&dev_pool[i]);
        }
    }

    /* Names before the flag before the driver. The order is the whole
     * correctness argument:
     *
     *   1. remove the names, so nothing new can resolve to this device
     *   2. set DEVICE_GONE, so anything that resolved a moment ago and is
     *      still holding a reference gets -ENODEV on its next operation
     *   3. tell the driver, which can now free its own state knowing no
     *      lookup is in flight and no future operation will reach it
     *
     * Doing 3 before 1 is a driver freeing state that a lookup completing on
     * the next line is about to hand to a read. */
    if (dev->dos_name[0] != '\0') {
        char link[NS_PATH_MAX];
        uint64 n = 0;
        uint64 k;

        link[n++] = '\\'; link[n++] = '?'; link[n++] = '?'; link[n++] = '\\';
        for (k = 0; dev->dos_name[k] != '\0' && n + 1 < sizeof(link); k++) {
            link[n++] = dev->dos_name[k];
        }
        link[n] = '\0';
        ns_remove(link);
        dev->dos_name[0] = '\0';
    }
    if (dev->ns_name[0] != '\0') {
        ns_remove(dev->ns_name);
    }

    dev->flags |= DEVICE_GONE;

    if (dev->ops != NULL && dev->ops->detach != NULL) {
        dev->ops->detach(dev);
    }

    /* And the cache, last.
     *
     * After the driver rather than before, because the driver's detach is the
     * last thing that could legitimately write, and because a block cached
     * for a device whose slot is about to be recycled is a block the NEXT
     * device would find under its own key. bcache.h has the argument in full;
     * what matters here is that it is one call in the one function that knows
     * a device has gone, rather than a rule each driver remembers - which is
     * exactly the argument that put the DEVICE_GONE flag here too. */
    bcache_invalidate_dev(dev);
}

int dev_present(const device_t *dev) {
    return dev != NULL && dev->in_use && !(dev->flags & DEVICE_GONE);
}

/* --- the wrapped operations ---------------------------------------------
 *
 * -ENODEV for a removed device, decided here and nowhere else. Every driver
 * checking for itself is every driver getting one chance to forget, and the
 * consequence of forgetting is a read from hardware that has been unplugged.
 *
 * -ENODEV rather than -EIO, and the distinction survives all the way out to
 * the shell: -EIO says the transfer was attempted and failed, which sends you
 * looking at cables. -ENODEV says there is nothing there, which is true. */

int64 dev_read_raw(device_t *dev, uint64 offset, void *buf, uint64 n) {
    if (dev == NULL || buf == NULL) {
        return -22;
    }
    if (!dev_present(dev)) {
        return -19;                          /* -ENODEV */
    }
    if (dev->ops->read == NULL) {
        return -22;
    }
    if (n == 0) {
        return 0;
    }
    return dev->ops->read(dev, offset, buf, n);
}

int64 dev_write_raw(device_t *dev, uint64 offset, const void *buf, uint64 n) {
    if (dev == NULL || buf == NULL) {
        return -22;
    }
    if (!dev_present(dev)) {
        return -19;
    }
    if (dev->ops->write == NULL) {
        /* -EROFS, matching what fs_write answers for a filesystem with no
         * write slot. The two layers give the same errno for the same shape
         * of refusal, so a caller does not have to know which one refused. */
        return -30;
    }
    if (n == 0) {
        return 0;
    }
    return dev->ops->write(dev, offset, buf, n);
}

/* The cache is entered HERE and nowhere else.
 *
 * Every guard above stays in front of it: a request to a removed device is
 * -ENODEV before the cache is asked, so a pulled disk can never be answered
 * out of blocks it left behind. That ordering is the reason the flag test
 * lives after dev_present rather than at the top - the cheap check is the one
 * that would have been wrong. */
int64 dev_read(device_t *dev, uint64 offset, void *buf, uint64 n) {
    if (dev == NULL || buf == NULL) {
        return -22;
    }
    if (!dev_present(dev)) {
        return -19;
    }
    if (n == 0) {
        return 0;
    }
    if ((dev->flags & DEVICE_CACHED) && bcache_enabled()) {
        return bcache_read(dev, offset, buf, n);
    }
    return dev_read_raw(dev, offset, buf, n);
}

int64 dev_write(device_t *dev, uint64 offset, const void *buf, uint64 n) {
    if (dev == NULL || buf == NULL) {
        return -22;
    }
    if (!dev_present(dev)) {
        return -19;
    }
    if (dev->ops->write == NULL) {
        return -30;
    }
    if (n == 0) {
        return 0;
    }
    if ((dev->flags & DEVICE_CACHED) && bcache_enabled()) {
        return bcache_write(dev, offset, buf, n);
    }
    return dev_write_raw(dev, offset, buf, n);
}

int dev_control(device_t *dev, uint32 code, void *arg, uint64 arg_size) {
    if (dev == NULL) {
        return -22;
    }
    if (!dev_present(dev)) {
        return -19;
    }
    if (dev->ops->control == NULL) {
        return -25;                          /* -ENOTTY */
    }
    return dev->ops->control(dev, code, arg, arg_size);
}

int dev_poll(device_t *dev, int events) {
    /* A removed device, and a device with no poll slot, are both ready -
     * see devobj_poll below for why that is the answer for the first. */
    if (!dev_present(dev) || dev->ops->poll == NULL) {
        return OB_POLLIN | OB_POLLOUT;
    }
    return dev->ops->poll(dev, events);
}

int dev_mmap(device_t *dev, uint64 offset, uint64 *phys, uint64 *cache_flags) {
    if (dev == NULL || phys == NULL || cache_flags == NULL) {
        return -22;
    }
    if (!dev_present(dev)) {
        return -19;
    }
    if (dev->ops->mmap == NULL) {
        return -19;                          /* -ENODEV, what Linux answers */
    }
    if ((offset & 0xFFFULL) != 0) {
        return -22;
    }
    *cache_flags = 0;
    return dev->ops->mmap(dev, offset, phys, cache_flags);
}

int dev_parse(device_t *dev, const char *remainder, uint32 access,
              object_t **out) {
    if (dev == NULL || out == NULL || remainder == NULL) {
        return -22;
    }
    *out = NULL;
    if (!dev_present(dev)) {
        return -19;
    }
    if (dev->ops->parse == NULL) {
        /* No namespace below this device. An empty remainder was the device
         * itself and the caller should not have come here; a non-empty one is
         * -ENOTDIR, which is what \Device\Console\foo means. */
        return remainder[0] == '\0' ? -22 : -20;
    }
    return dev->ops->parse(dev, remainder, access, out);
}

int dev_open_object(object_t *obj, const char *remainder, uint32 access,
                    object_t **out) {
    device_t *dev;
    int rc;

    if (obj == NULL || out == NULL) {
        return -22;
    }
    *out = NULL;

    if (remainder == NULL || remainder[0] == '\0') {
        /* Nothing followed the name: the device itself is the answer. The
         * reference passes straight through rather than being taken again -
         * see the header on why every path leaves the caller one thing to
         * release. */
        *out = obj;
        return 0;
    }

    dev = dev_from_object(obj);
    if (dev == NULL) {
        /* Something followed the name of an object that is not a device, so
         * there is nothing that could parse it. -ENOTDIR, which is what
         * \Device\Console\foo means and what a caller printing an error
         * should say. */
        ob_deref(obj);
        return -20;
    }

    rc = dev_parse(dev, remainder, access, out);
    ob_deref(obj);              /* the device object, not the parsed result */
    return rc;
}

int dev_iterate(int (*cb)(device_t *dev, void *ctx), void *ctx) {
    int i;

    if (cb == NULL) {
        return -22;
    }
    for (i = 0; i < DEVICE_MAX; i++) {
        if (dev_pool[i].in_use) {
            int rc = cb(&dev_pool[i], ctx);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

static const char *kind_name(device_kind_t k) {
    switch (k) {
        case DEVICE_KIND_DISK:   return "disk  ";
        case DEVICE_KIND_VOLUME: return "volume";
        case DEVICE_KIND_CHAR:   return "char  ";
        case DEVICE_KIND_NET:    return "net   ";
        default:                 return "?     ";
    }
}

void dev_report(void) {
    int i;

    print_string("devices:\n", 0x0F);
    for (i = 0; i < DEVICE_MAX; i++) {
        device_t *d = &dev_pool[i];

        if (!d->in_use) {
            continue;
        }
        print_string("  ", 0x07);
        print_string(kind_name(d->kind), 0x07);
        print_string(" ", 0x07);
        print_string(d->ns_name, 0x0F);
        if (d->dos_name[0] != '\0') {
            print_string("  \\??\\", 0x07);
            print_string(d->dos_name, 0x07);
        }
        if (d->flags & DEVICE_REMOVABLE) {
            print_string("  [removable]", 0x0E);
        }
        if (d->flags & DEVICE_GONE) {
            print_string("  [gone]", 0x0C);
        }
        print_string("\n", 0x07);
    }
}
