#include "ahci.h"
#include "ata.h"
#include "device.h"
#include "disk.h"
#include "ns.h"
#include "object.h"
#include "part.h"
#include "screen.h"
#include "typesk.h"

/* See disk.h for why a disk and a volume are two objects. What follows is the
 * raw disk half: bytes at an offset.
 *
 * --- What changed, and what did not ---------------------------------------
 * The bounce-buffer read-modify-write below is unchanged - it was correct and
 * it is still the only hard thing in this file. What changed is around it:
 *
 *   The partition parse LEFT, to part.c, together with the GPT one. A disk
 *   with a protective MBR has BOTH tables, and two scanners each believing
 *   their own answer is the shape of bug this tree has hit before.
 *
 *   The object_type_t LEFT, to device.c. Every device now reaches ring 3
 *   through one object type whose body is a device_t. That is the payoff of
 *   the vtable: adding a serial port or a ramdisk adds a device_ops_t, not a
 *   second object type with its own read/write/poll to keep in step.
 *
 * What is left is an ATA disk expressed as a device_ops_t, which is as much
 * of this file as was ever really about ATA.
 *
 * --- Reading at a byte offset over a sector device ------------------------
 * A block device is addressed in sectors and read(2) is addressed in bytes,
 * and the whole of this file's I/O is that mismatch. A read that starts
 * mid-sector or ends mid-sector has to go through a bounce buffer, because
 * the hardware cannot transfer a partial sector - and getting that wrong
 * produces a read that returns the right COUNT with the wrong bytes, which no
 * caller can detect. */

#define SECTOR_SIZE 512
/* Eight, not four. Four was one per ATA channel/drive combination, which was
 * the whole of the storage this kernel had. A machine can now present ATA
 * drives AND AHCI ports at once - QEMU does, deliberately, so that a boot
 * proves the two paths coexist rather than one having replaced the other. */
#define MAX_DISKS   8

/* Exactly one of `ata` and `ahci` is non-NULL.
 *
 * A tagged pair rather than a void* plus a kind enum, because the compiler
 * then checks each branch against the right type - and the two drivers take
 * genuinely different arguments. ata_read takes a uint32 LBA and a uint8
 * count, because ATA PIO's LBA28 addressing cannot express more; ahci_read
 * takes uint64 and uint32, because it issues the EXT commands. Flattening
 * those into one signature would mean truncating one of them at the call
 * site, silently, for disks larger than 128GB. */
typedef struct disk {
    ata_device_t  *ata;
    ahci_device_t *ahci;
    int            in_use;
} disk_t;

/* The one place the two storage drivers are told apart. Everything below this
 * point is written in sectors and does not care which controller answers. */
static int disk_dev_read(const disk_t *d, uint64 lba, uint32 count,
                         void *buf) {
    if (d->ahci != NULL) {
        return ahci_read(d->ahci, lba, count, buf);
    }
    /* The casts are safe because disk_bytes reports this device's real size
     * and every caller has already been bounded by it - an ATA device's
     * sector count cannot exceed what a uint32 LBA can name. */
    return ata_read(d->ata, (uint32)lba, (uint8)count, buf);
}

static int disk_dev_write(const disk_t *d, uint64 lba, uint32 count,
                          const void *buf) {
    if (d->ahci != NULL) {
        return ahci_write(d->ahci, lba, count, buf);
    }
    return ata_write(d->ata, (uint32)lba, (uint8)count, buf);
}

static disk_t disk_pool[MAX_DISKS];

static uint64 disk_bytes(const disk_t *d) {
    if (d->ahci != NULL) {
        return d->ahci->sectors * SECTOR_SIZE;
    }
    return (uint64)d->ata->sectors * SECTOR_SIZE;
}

/* --- byte-addressed I/O over a sector device ---------------------------- */

static int64 disk_read_op(device_t *dev, uint64 offset, void *buf, uint64 n) {
    disk_t *d = (disk_t *)dev->body;
    uint8  *dst = (uint8 *)buf;
    uint8   sector[SECTOR_SIZE];
    uint64  pos = offset, end, done = 0;

    if (d == NULL || (d->ata == NULL && d->ahci == NULL)) {
        return -5;                       /* -EIO */
    }
    end = disk_bytes(d);

    /* Past the end is end of file, not an error. A caller reading the whole
     * disk in a loop stops on the zero; returning -EIO would make a complete
     * image look like a failed one. */
    if (pos >= end) {
        return 0;
    }
    if (n > end - pos) {
        n = end - pos;
    }

    while (done < n) {
        uint64 lba    = (pos + done) / SECTOR_SIZE;
        uint64 within = (pos + done) % SECTOR_SIZE;
        uint64 chunk  = SECTOR_SIZE - within;
        uint64 i;

        /* Whole sectors, straight into the caller's buffer.
         *
         * This path exists because the block cache above only ever issues
         * aligned, 4096-byte requests, so what used to be the rare case is now
         * the only case that runs at boot - and eight ATA commands plus eight
         * 512-byte copies per 4KB block is a cost paid on every miss.
         *
         * The overrun the old comment warned about is avoided by construction
         * rather than by avoiding the case: `whole` is computed from the bytes
         * REMAINING, so the transfer is whole * SECTOR_SIZE bytes into a
         * region of at least that size. A short tail never enters here at all;
         * it falls through to the bounce buffer below on the next iteration.
         *
         * The count register is one byte and 0 means 256; the cap is 64 (32KB)
         * so the encoding edge is never reached and one failed transfer never
         * costs more than 32KB of progress. */
        if (within == 0 && n - done >= SECTOR_SIZE) {
            uint64 whole = (n - done) / SECTOR_SIZE;

            if (whole > 64) {
                whole = 64;
            }
            if (disk_dev_read(d, lba, (uint32)whole, dst + done) != 0) {
                return done > 0 ? (int64)done : -5;
            }
            done += whole * SECTOR_SIZE;
            continue;
        }

        if (chunk > n - done) {
            chunk = n - done;
        }

        /* Through the bounce buffer for anything that does not start and end
         * on a sector boundary. Reading a full sector straight into the
         * caller's buffer would write SECTOR_SIZE bytes when the caller asked
         * for fewer - a buffer overrun that only appears for certain lengths,
         * which is why the fast path above is written in terms of the bytes
         * remaining and not in terms of the sector. */
        if (disk_dev_read(d, lba, 1, sector) != 0) {
            return done > 0 ? (int64)done : -5;
        }
        for (i = 0; i < chunk; i++) {
            dst[done + i] = sector[within + i];
        }
        done += chunk;
    }
    /* No position update. dev_read takes an offset and returns a count; the
     * descriptor's position is advanced one layer up, in device.c's object
     * wrapper. Advancing it here as well was the same byte counted twice. */
    return (int64)done;
}

static int64 disk_write_op(device_t *dev, uint64 offset, const void *buf,
                           uint64 n) {
    disk_t      *d = (disk_t *)dev->body;
    const uint8 *src = (const uint8 *)buf;
    uint8        sector[SECTOR_SIZE];
    uint64       pos = offset, end, done = 0;

    if (d == NULL || (d->ata == NULL && d->ahci == NULL)) {
        return -5;
    }
    end = disk_bytes(d);

    /* A write past the end is -ENOSPC, not a short write and not EOF. A disk
     * does not grow, and a caller told it wrote fewer bytes than it asked for
     * would retry the remainder forever. */
    if (pos >= end) {
        return -28;                      /* -ENOSPC */
    }
    if (n > end - pos) {
        n = end - pos;
    }

    while (done < n) {
        uint64 lba    = (pos + done) / SECTOR_SIZE;
        uint64 within = (pos + done) % SECTOR_SIZE;
        uint64 chunk  = SECTOR_SIZE - within;
        uint64 i;

        /* Whole sectors go straight out, with NO read first.
         *
         * The read in the partial path below is there to preserve the bytes
         * the caller did not address. When the transfer covers entire sectors
         * there are no such bytes, and reading them back to overwrite them is
         * a transfer that cannot change the outcome. The cache above turns
         * this into the common case for the same reason as on the read side:
         * a full-block write from the cache never has a partial sector in it.
         *
         * Same construction as the read: `whole` comes from the bytes
         * remaining, so nothing outside the caller's buffer is ever read. */
        if (within == 0 && n - done >= SECTOR_SIZE) {
            uint64 whole = (n - done) / SECTOR_SIZE;

            if (whole > 64) {
                whole = 64;
            }
            if (disk_dev_write(d, lba, (uint32)whole, src + done) != 0) {
                return done > 0 ? (int64)done : -5;
            }
            done += whole * SECTOR_SIZE;
            continue;
        }

        if (chunk > n - done) {
            chunk = n - done;
        }

        /* Read-modify-write for any partial sector. Skipping the read and
         * writing a half-filled buffer would zero the rest of the sector -
         * destroying data the caller never addressed, which is the worst
         * possible failure for a raw device because it looks like it worked.
         *
         * A whole sector still reads first. It costs one transfer and removes
         * the branch where a caller writing exactly SECTOR_SIZE bytes at an
         * unaligned offset takes the wrong path. */
        if (disk_dev_read(d, lba, 1, sector) != 0) {
            return done > 0 ? (int64)done : -5;
        }
        for (i = 0; i < chunk; i++) {
            sector[within + i] = src[done + i];
        }
        if (disk_dev_write(d, lba, 1, sector) != 0) {
            return done > 0 ? (int64)done : -5;
        }
        done += chunk;
    }
    return (int64)done;
}

static void disk_detach_op(device_t *dev) {
    disk_t *d = (disk_t *)dev->body;

    /* The pool entry is released; the ata_device_t is not touched. It is
     * owned by ata.c and describes a controller PORT, which still exists
     * after the medium behind it does not. Freeing it here would be this file
     * disposing of another file's state on a path that file never heard of. */
    if (d != NULL) {
        d->ata    = NULL;
        d->ahci   = NULL;
        d->in_use = 0;
    }
}

static const device_ops_t disk_ops = {
    .name    = "disk",
    /* No parse: a raw disk has no namespace below it, so
     * \Device\Harddisk0\DR0\anything is -ENOTDIR - and that answer comes
     * from this NULL rather than from a stub every driver remembers to write.
     * A VOLUME is the device that parses; see volume.c. */
    .parse   = NULL,
    .read    = disk_read_op,
    .write   = disk_write_op,
    .control = NULL,
    .detach  = disk_detach_op
};

int disk_is_block(const object_t *obj) {
    /* Asked of the DEVICE rather than of a disk-specific object type. A
     * volume is a block device too and stat must report S_IFBLK for it, which
     * a type-pointer comparison against one static disk_type could not do
     * without a second comparison beside it - and two answers to "is this a
     * block device" is how a partition tool comes to refuse a partition. */
    const device_t *d = dev_from_object(obj);

    return d != NULL && (d->kind == DEVICE_KIND_DISK ||
                         d->kind == DEVICE_KIND_VOLUME);
}

uint64 disk_size(const object_t *obj) {
    /* The kind is checked before the body is read, and that check is the
     * whole point of this function existing rather than stat calling
     * fileobj_size(). fileobj_size answers by reading obj->body as an
     * fs_node_t, which is true for a file object and nonsense for anything
     * else - handed a disk object it would read a pointer as a size and
     * report a number in the terabytes.
     *
     * That is not hypothetical: it is the same shape as the bug that made
     * opening /dev with O_DIRECTORY return -ENOTDIR, where fileobj_is_dir
     * read byte 24 of a namespace entry's NAME as an attribute byte. */
    const device_t *d = dev_from_object(obj);

    if (d == NULL || !disk_is_block(obj)) {
        return 0;
    }
    return d->size;
}

/* The partition table parse that used to be here is in part.c, beside the GPT
 * one. See the note at the top of this file, and part.h for why the two could
 * not stay separate. */

/* --- registration ------------------------------------------------------- */

static void name_for(int index, char *ns_out, char *posix_out) {
    /* \Device\Harddisk<N>\DR<N> is NT's spelling for the raw disk, and sda,
     * sdb... is POSIX's. Two names for one object, which is the entire reason
     * the namespace exists - a program using either gets the same device with
     * the same state, rather than two objects kept in step.
     *
     * Built by appending rather than by patching two indices into a template.
     * The template version had the digits at 17 and 21; they are at 16 and 20,
     * so it wrote a '0' over the separating backslash AND over the NUL
     * terminator - producing \Device\Harddisk00DR00 followed by whatever was
     * on the stack, a name that is one namespace component instead of two.
     *
     * The lesson is not "count more carefully". A literal with hand-counted
     * offsets into it has no way to be checked and no way to fail loudly: the
     * result was a plausible-looking device that registered successfully. */
    const char *prefix = "\\Device\\Harddisk";
    const char *middle = "\\DR";
    int n = 0;
    int i;

    for (i = 0; prefix[i] != '\0'; i++) {
        ns_out[n++] = prefix[i];
    }
    ns_out[n++] = (char)('0' + index);
    for (i = 0; middle[i] != '\0'; i++) {
        ns_out[n++] = middle[i];
    }
    ns_out[n++] = (char)('0' + index);
    ns_out[n]   = '\0';

    posix_out[0] = 's';
    posix_out[1] = 'd';
    posix_out[2] = (char)('a' + index);
    posix_out[3] = '\0';
}

/* One registration path for both kinds.
 *
 * `ata` and `ahci` are mutually exclusive and exactly one is non-NULL - the
 * caller decides which, and this function never asks again. Splitting it into
 * two near-identical loops was the obvious first shape and the wrong one: the
 * device naming, the pool bookkeeping and the DEVICE_REMOVABLE reasoning below
 * are the same for both, and two copies of that is two places for them to
 * drift. */
static void disk_register_one(int index, ata_device_t *ata,
                              ahci_device_t *ahci) {
    device_t *dev;
    char ns_name[40];
    char posix_name[8];

    disk_pool[index].ata    = ata;
    disk_pool[index].ahci   = ahci;
    disk_pool[index].in_use = 1;

    dev = dev_alloc();
    if (dev == NULL) {
        print_string("disk: no device slot\n", 0x0C);
        disk_pool[index].in_use = 0;
        return;
    }
    dev->ops        = &disk_ops;
    dev->kind       = DEVICE_KIND_DISK;
    dev->body       = &disk_pool[index];
    dev->size       = disk_bytes(&disk_pool[index]);
    dev->block_size = SECTOR_SIZE;

    name_for(index, ns_name, posix_name);
    dev_attach(dev, ns_name, posix_name);
}

void disk_register(void) {
    int i, index = 0;

    for (i = 0; i < MAX_DISKS && index < MAX_DISKS; i++) {
        ata_device_t *ata = ata_get(i);

        if (ata == NULL || !ata->present) {
            continue;
        }
        disk_register_one(index++, ata, NULL);
    }

    /* AHCI after ATA, sharing one index space so that sda/sdb/... number
     * continuously across both controllers. A machine with only AHCI - which
     * is what the bare-metal target is - gets sda from port 0, exactly as a
     * machine with only IDE gets sda from the primary master. */
    for (i = 0; i < MAX_DISKS && index < MAX_DISKS; i++) {
        ahci_device_t *sata = ahci_get(i);

        if (sata == NULL || !sata->present) {
            continue;
        }
        disk_register_one(index++, NULL, sata);
    }
}
