#include "device.h"
#include "fileobj.h"
#include "fs.h"
#include "ns.h"
#include "object.h"
#include "part.h"
#include "screen.h"
#include "typesk.h"
#include "volume.h"

/* See volume.h for the two spellings and the drive-letter rule. What follows
 * is how.
 *
 * There is no filesystem knowledge in this file. Which format a volume holds
 * is answered by the probe table, and a filesystem that wants to be findable
 * registers itself - the same standard vfs.c is held to one layer up, and the
 * same reason: an if-chain naming every filesystem is the file that has to be
 * edited to add one. */

#define VOLUME_MAX     8
#define FS_PROBE_MAX   4
#define SECTOR_SIZE  512

struct volume {
    device_t    *dev;        /* this volume's own device object            */
    device_t    *disk;       /* the disk it is a partition of              */

    uint64       start_lba;
    uint64       sectors;

    fs_volume_t *fs;         /* NULL until volume_probe identifies it      */
    char         letter;     /* 'C'..'Z', or 0                             */
    char         mount_point[64];
    uint32       number;     /* the N in \Device\HarddiskVolumeN           */
    int          in_use;

    /* Detached, but the object is deliberately still alive - a handle held
     * across the removal has to fail, not fault, and clearing in_use here
     * would hand this volume_t to the next disk while somebody still points
     * at it.
     *
     * So the slot is RETIRED, and volume_alloc takes a retired one only when
     * there is no free one left. That is exactly the policy vol_detach has
     * always described - "slots are recovered when the pool is exhausted, not
     * when a device goes away" - and it was not true of anything, because
     * nothing recovered them at all: eight insert/remove cycles exhausted the
     * pool for the life of the machine. Same shape as the filesystem-slot
     * leak fs_ops_t::unmount fixes, one layer up. */
    int          retired;
};

static volume_t volume_pool[VOLUME_MAX];

/* Volume NUMBERS are never reused; drive LETTERS are.
 *
 * The asymmetry is the point. \Device\HarddiskVolume2 is an identity - a
 * diagnostic that says "volume 2 failed" should never be ambiguous about
 * which medium it meant, and a boot that has seen five sticks should be able
 * to say so. A drive letter is a policy alias handed out to whatever is
 * present, and never reusing one runs out after twenty-three insertions for
 * no benefit: a stale handle holds a reference to the volume OBJECT, which
 * stays alive answering -ENODEV no matter what the letter is later given to. */
static uint32 next_volume_number = 1;

static struct {
    const char *name;
    fs_probe_fn probe;
} fs_probes[FS_PROBE_MAX];
static int fs_probe_count;

/* --- the volume's own I/O -----------------------------------------------
 *
 * Offsets are relative to the partition and every one of them is bounds
 * checked against it. That check is the entire safety property of a
 * partition: without it a filesystem with a wrong geometry reads and, worse,
 * WRITES into the partition next door, and the damage looks like corruption
 * on a volume nothing touched. */

static int64 vol_read(device_t *dev, uint64 offset, void *buf, uint64 n) {
    volume_t *v = (volume_t *)dev->body;
    uint64    span;

    if (v == NULL || v->disk == NULL) {
        return -5;                       /* -EIO */
    }
    span = v->sectors * SECTOR_SIZE;
    if (offset >= span) {
        return 0;                        /* end of volume, not an error */
    }
    if (n > span - offset) {
        n = span - offset;
    }
    /* Straight through to the disk device, which does the sector bouncing.
     * Doing it again here would be the second buffering path item 5 is
     * explicitly trying not to end up with - the block cache goes above the
     * disk, and this layer only ever adds a base. */
    return dev_read(v->disk, v->start_lba * SECTOR_SIZE + offset, buf, n);
}

static int64 vol_write(device_t *dev, uint64 offset, const void *buf,
                       uint64 n) {
    volume_t *v = (volume_t *)dev->body;
    uint64    span;

    if (v == NULL || v->disk == NULL) {
        return -5;
    }
    span = v->sectors * SECTOR_SIZE;
    /* -ENOSPC rather than a short write, matching the raw disk. A caller told
     * it wrote fewer bytes than it asked for retries the remainder forever. */
    if (offset >= span) {
        return -28;
    }
    if (n > span - offset) {
        n = span - offset;
    }
    return dev_write(v->disk, v->start_lba * SECTOR_SIZE + offset, buf, n);
}

/* --- the parse ----------------------------------------------------------
 *
 * The operation that made device.h necessary. ns_lookup stops at this
 * device's name and hands over everything after it, untouched and in
 * namespace spelling; turning that into a file is this function.
 *
 * The conversion is backslash to slash and nothing else. In particular no
 * normalization: "." and ".." and repeated separators are path_normalize's
 * job, and doing a second, subtly different version of it here is how two
 * spellings of one path come to resolve to two different files. A remainder
 * arriving here unnormalized is a caller that skipped a step, and it will
 * fail at the filesystem as a name with a dot in it - which is a legible
 * failure, unlike silently agreeing with a normalization nobody performed. */
static int vol_parse(device_t *dev, const char *remainder, uint32 access,
                     object_t **out) {
    volume_t *v = (volume_t *)dev->body;
    char      rel[256];
    uint64    i = 0;
    int       err = 0;
    object_t *obj;

    if (v == NULL) {
        return -5;
    }
    if (v->fs == NULL) {
        /* A volume with no filesystem identified. -ENODEV rather than -ENOENT:
         * the path does not exist because nothing here can say what exists,
         * which is a different problem from a missing file and a different
         * thing to go and fix. The device is still readable raw. */
        return -19;
    }

    /* An empty remainder is the volume root. Not an error and not the raw
     * device: \??\D: with nothing after it is the root directory of D:, which
     * is what `dir D:` opens. */
    if (remainder[0] == '\0') {
        rel[0] = '/';
        rel[1] = '\0';
    } else {
        for (i = 0; remainder[i] != '\0'; i++) {
            if (i + 1 >= sizeof(rel)) {
                return -36;              /* -ENAMETOOLONG */
            }
            rel[i] = (remainder[i] == '\\') ? '/' : remainder[i];
        }
        rel[i] = '\0';
        /* The namespace's remainder starts with its separator, so this is
         * already absolute-from-the-volume. A remainder that somehow does not
         * is not silently prefixed: it would mean the namespace changed shape
         * underneath this, and inventing a leading slash hides that. */
        if (rel[0] != '/') {
            return -22;
        }
    }

    obj = fileobj_open_on(v->fs, rel, access, &err);
    if (obj == NULL) {
        return err;
    }
    *out = obj;
    return 0;
}

static void vol_detach(device_t *dev) {
    volume_t *v = (volume_t *)dev->body;

    if (v == NULL) {
        return;
    }
    /* Every mount of this volume, wherever it went. fs_unmount_volume rather
     * than fs_unmount_at(v->mount_point) because a volume can be mounted more
     * than once and the remembered point is only the first - and because at
     * this point -EBUSY is not an answer the hardware will accept. */
    if (v->fs != NULL) {
        fs_unmount_volume(v->fs);
        v->fs->mounted = 0;
        v->fs = NULL;
    }
    v->mount_point[0] = '\0';
    v->letter = 0;
    v->retired = 1;

    /* The volume_t itself is NOT freed and neither is its device_t. Handles
     * on files that lived here are still out there holding fs_node_t bodies
     * whose ->vol points at the fs_volume_t above; clearing `mounted` is what
     * turns their next read into -ENODEV, and freeing the slot is what would
     * turn it into a read of a recycled pool entry. Slots are recovered when
     * the pool is exhausted, not when a device goes away. */
}

static const device_ops_t volume_ops = {
    .name    = "volume",
    .parse   = vol_parse,
    .read    = vol_read,
    .write   = vol_write,
    .control = NULL,
    .detach  = vol_detach
};

/* --- drive letters ------------------------------------------------------- */

static int letter_taken(char c) {
    int i;

    for (i = 0; i < VOLUME_MAX; i++) {
        if (volume_pool[i].in_use && volume_pool[i].letter == c) {
            return 1;
        }
    }
    /* The namespace is asked too, not just the pool. \??\C: could have been
     * created by something that is not a volume at all - and a second inserter
     * getting -EEXIST from ns_link after this function said the letter was
     * free is a failure with no good place to report it. */
    {
        char path[8];

        path[0] = '\\'; path[1] = '?'; path[2] = '?'; path[3] = '\\';
        path[4] = c;    path[5] = ':'; path[6] = '\0';
        if (ns_lookup_entry(path) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* Assign and link. `want_c` is set for the volume that will be the root.
 *
 * A: and B: are never handed out. They meant floppies, and every DOS-era
 * program that special-cases a drive letter special-cases those two - two
 * letters is a cheap price for not discovering which ones the hard way. */
static char assign_letter(volume_t *v, int want_c) {
    char c;

    if (v->letter != 0) {
        return v->letter;
    }
    if (want_c && !letter_taken('C')) {
        c = 'C';
    } else {
        for (c = 'D'; c <= 'Z'; c++) {
            if (!letter_taken(c)) {
                break;
            }
        }
        if (c > 'Z') {
            /* Out of letters is not a failure to create the volume. It is
             * reachable by its \Device\ name and mountable on the POSIX side;
             * only the DOS alias is missing. Refusing the volume over a
             * missing alias would put us back where devices.c started, with a
             * link being what makes a device exist. */
            return 0;
        }
    }

    {
        char link[8];

        link[0] = '\\'; link[1] = '?'; link[2] = '?'; link[3] = '\\';
        link[4] = c;    link[5] = ':'; link[6] = '\0';
        if (ns_link(link, v->dev->ns_name) != 0) {
            return 0;
        }
    }
    v->letter = c;
    /* Recorded on the device too, so dev_detach removes the letter as one of
     * the two names it inserted rather than leaving it to this file to
     * remember. One remover, not two. */
    v->dev->dos_name[0] = c;
    v->dev->dos_name[1] = ':';
    v->dev->dos_name[2] = '\0';
    return c;
}

/* --- creation ------------------------------------------------------------ */

static void number_to_name(uint32 n, char *out, uint64 cap) {
    const char *prefix = "\\Device\\HarddiskVolume";
    uint64 i = 0;
    char   digits[8];
    int    d = 0;

    while (prefix[i] != '\0' && i + 1 < cap) {
        out[i] = prefix[i];
        i++;
    }
    if (n == 0) {
        digits[d++] = '0';
    }
    while (n > 0 && d < (int)sizeof(digits)) {
        digits[d++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (d > 0 && i + 1 < cap) {
        out[i++] = digits[--d];
    }
    out[i] = '\0';
}

static volume_t *volume_alloc(void) {
    int i;
    int slot = -1;

    /* A never-used slot first; a retired one only if there is none. Two
     * passes rather than one test, so that reuse - the case where a stale
     * handle could be shown a different medium - happens only when there is
     * genuinely no alternative, rather than whenever a retired slot happens
     * to sit earlier in the array than a free one. */
    for (i = 0; i < VOLUME_MAX; i++) {
        if (!volume_pool[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (i = 0; i < VOLUME_MAX; i++) {
            if (volume_pool[i].retired) {
                slot = i;
                break;
            }
        }
    }
    {
        if (slot >= 0) {
            volume_t *v = &volume_pool[slot];
            int k;

            v->dev       = NULL;
            v->disk      = NULL;
            v->start_lba = 0;
            v->sectors   = 0;
            v->fs        = NULL;
            v->letter    = 0;
            v->number    = 0;
            for (k = 0; k < (int)sizeof(v->mount_point); k++) {
                v->mount_point[k] = '\0';
            }
            v->in_use  = 1;
            v->retired = 0;
            return v;
        }
    }
    return NULL;
}

static volume_t *volume_create(device_t *disk, uint64 start_lba,
                               uint64 sectors) {
    volume_t *v = volume_alloc();
    device_t *d;
    char      name[64];

    if (v == NULL) {
        return NULL;
    }
    d = dev_alloc();
    if (d == NULL) {
        v->in_use = 0;
        return NULL;
    }

    v->disk      = disk;
    v->start_lba = start_lba;
    v->sectors   = sectors;
    v->number    = next_volume_number++;

    d->ops        = &volume_ops;
    d->kind       = DEVICE_KIND_VOLUME;
    d->body       = v;
    d->size       = sectors * SECTOR_SIZE;
    d->block_size = SECTOR_SIZE;
    /* The parent, which is what makes pulling the disk detach every volume on
     * it without this file keeping a second list of which volumes came from
     * where. dev_detach walks children depth first. */
    d->parent     = disk;
    /* Removability is inherited. A partition on a removable medium is
     * removable; deriving it here rather than storing a second copy means the
     * two cannot disagree about a stick that was hot-plugged. */
    d->flags      = disk->flags & DEVICE_REMOVABLE;

    number_to_name(v->number, name, sizeof(name));
    if (dev_attach(d, name, NULL) != 0) {
        dev_free(d);
        v->in_use = 0;
        return NULL;
    }
    v->dev = d;
    return v;
}

/* --- probing ------------------------------------------------------------- */

int volume_register_fs(const char *name, fs_probe_fn probe) {
    if (name == NULL || probe == NULL) {
        return -22;
    }
    if (fs_probe_count >= FS_PROBE_MAX) {
        return -28;                          /* -ENOSPC */
    }
    fs_probes[fs_probe_count].name  = name;
    fs_probes[fs_probe_count].probe = probe;
    fs_probe_count++;
    return 0;
}

int volume_probe(volume_t *vol) {
    int i;

    if (vol == NULL || vol->dev == NULL) {
        return -22;
    }
    if (vol->fs != NULL) {
        return 0;
    }
    for (i = 0; i < fs_probe_count; i++) {
        fs_volume_t *fs = fs_probes[i].probe(vol->dev);

        if (fs != NULL) {
            vol->fs = fs;
            return 0;
        }
    }
    /* Every prober declined. Not an error about the volume - an unrecognised
     * format is a fact, and the volume still exists and still reads raw. */
    return -19;                              /* -ENODEV */
}

int volume_scan_disk(device_t *disk) {
    part_entry_t  parts[PART_MAX];
    part_table_t  table;
    int           n;
    int           made = 0;
    int           i;

    if (disk == NULL) {
        return -22;
    }
    n = part_scan(disk, parts, PART_MAX, &table);
    if (n < 0) {
        return n;
    }

    /* No partition table at all: the whole disk is one volume.
     *
     * This is not a fallback for tidiness, it is the disk this kernel boots
     * from - tools/fatfs.py writes a filesystem at sector 0 with no table.
     * Reporting zero volumes for it would mean the change that added GPT is
     * the change that stopped the machine booting. */
    if (n == 0 && table == PART_TABLE_NONE) {
        if (volume_create(disk, 0, disk->size / SECTOR_SIZE) != NULL) {
            made = 1;
        }
        return made;
    }

    for (i = 0; i < n; i++) {
        if (!part_type_is_data(&parts[i])) {
            continue;
        }
        if (volume_create(disk, parts[i].start_lba, parts[i].sectors) != NULL) {
            made++;
        }
    }
    return made;
}

/* --- mounting ------------------------------------------------------------ */

int volume_mount(volume_t *vol, const char *mount_point) {
    int rc;

    if (vol == NULL || mount_point == NULL) {
        return -22;
    }
    if (vol->fs == NULL) {
        return -19;                          /* -ENODEV: nothing to mount */
    }
    rc = fs_mount_at(mount_point, vol->fs);
    if (rc != 0) {
        return rc;
    }
    {
        uint64 i;
        for (i = 0; i + 1 < sizeof(vol->mount_point) &&
                    mount_point[i] != '\0'; i++) {
            vol->mount_point[i] = mount_point[i];
        }
        vol->mount_point[i] = '\0';
    }
    return 0;
}

void volume_remove(volume_t *vol) {
    if (vol == NULL || vol->dev == NULL) {
        return;
    }
    /* Everything is dev_detach's, deliberately. Eject and surprise removal
     * differ in what happens BEFORE this call - eject gets to refuse when a
     * file is open, surprise removal does not - and once the medium is
     * declared gone the teardown is identical. Two teardown paths that are
     * supposed to be identical is the pair that drifts. */
    dev_detach(vol->dev);
}

volume_t *volume_from_device(const device_t *dev) {
    if (dev == NULL || dev->kind != DEVICE_KIND_VOLUME ||
        dev->ops != &volume_ops) {
        return NULL;
    }
    return (volume_t *)dev->body;
}

fs_volume_t *volume_fs(const volume_t *vol) {
    return vol != NULL ? vol->fs : NULL;
}

char volume_letter(const volume_t *vol) {
    return vol != NULL ? vol->letter : 0;
}

/* --- boot ---------------------------------------------------------------- */

static int scan_one_disk(device_t *dev, void *ctx) {
    (void)ctx;
    if (dev->kind == DEVICE_KIND_DISK) {
        volume_scan_disk(dev);
    }
    return 0;
}

void volume_init(void) {
    volume_t *root = NULL;
    int i;

    /* Two passes over the disks, not one: dev_iterate is walking the same
     * pool that volume_create inserts into, and creating a device inside the
     * walk means the new entry is visited by the walk that made it. It would
     * work today - a volume is not a disk, so scan_one_disk skips it - and it
     * is a loop that depends on a property of a different file. */
    dev_iterate(scan_one_disk, NULL);

    for (i = 0; i < VOLUME_MAX; i++) {
        volume_t *v = &volume_pool[i];

        if (v->retired) {
            /* Detached. Probing it would read through a device that answers
             * -ENODEV, which fails harmlessly and prints nothing - but it is
             * a read per pass per dead volume, and "the probe ran and found
             * nothing" is indistinguishable in a log from "the medium is
             * unreadable". Skipped explicitly. */
            continue;
        }
        if (!v->in_use || v->fs == NULL) {
            if (v->in_use) {
                volume_probe(v);
            }
        }
        if (!v->in_use || v->fs == NULL) {
            continue;
        }
        /* The first volume that mounts becomes the root, and gets C:. First
         * rather than "the bootable one" because the MBR boot flag says which
         * partition the BIOS was told to chain to, which on this build is
         * neither disk - and a rule that reads a flag nothing sets is a rule
         * that has never been tested. When there is a real boot volume to
         * identify, this is the line that changes. */
        if (root == NULL) {
            root = v;
            fs_set_root(v->fs);
            v->mount_point[0] = '/';
            v->mount_point[1] = '\0';
            assign_letter(v, 1);
        } else {
            assign_letter(v, 0);

            /* A volume that is not the root gets mounted under /mnt, named by
             * its drive letter: /mnt/d, /mnt/e. That is the POSIX half of the
             * same volume the NT side reaches as \\??\\D:, and it is what
             * makes a second filesystem visible to a process at all - without
             * it a mounted-and-identified volume is reachable only through
             * the namespace, and nothing in ring 3 goes that way yet.
             *
             * Mounting over a path that does not exist on the root volume is
             * allowed and deliberate (see fs.h): requiring /mnt to exist first
             * would mean a read-only root can never mount anything. */
            if (v->letter != 0) {
                char point[8];

                point[0] = '/'; point[1] = 'm'; point[2] = 'n'; point[3] = 't';
                point[4] = '/';
                point[5] = (char)(v->letter - 'A' + 'a');
                point[6] = '\0';
                volume_mount(v, point);
            }
        }
    }
}

void volume_report(void) {
    int i;

    print_string("volumes:\n", 0x0F);
    for (i = 0; i < VOLUME_MAX; i++) {
        volume_t *v = &volume_pool[i];

        if (!v->in_use) {
            continue;
        }
        print_string("  ", 0x07);
        print_string(v->dev != NULL ? v->dev->ns_name : "(unnamed)", 0x0F);
        if (v->letter != 0) {
            char l[4];
            l[0] = ' '; l[1] = v->letter; l[2] = ':'; l[3] = '\0';
            print_string(l, 0x0A);
        }
        print_string("  lba ", 0x07);
        print_hex((uint32)v->start_lba, 0x07);
        print_string("  sectors ", 0x07);
        print_hex((uint32)v->sectors, 0x07);
        if (v->fs != NULL && v->fs->ops != NULL) {
            print_string("  ", 0x07);
            print_string(v->fs->ops->name, 0x0A);
        } else {
            print_string("  (unrecognised)", 0x0E);
        }
        if (v->mount_point[0] != '\0') {
            print_string("  on ", 0x07);
            print_string(v->mount_point, 0x0A);
        }
        print_string("\n", 0x07);
    }
}
