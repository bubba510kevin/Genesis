/* A device_t backed by a host file or buffer.
 *
 * Shared by fat_test.c and part_test.c, and shared deliberately rather than
 * copied: both need "a block device that is really a file", and two copies of
 * that would be two chances for one of them to disagree with dev_read's actual
 * contract - a short read is a short read, past the end is zero, and neither is
 * an error.
 *
 * This is the payoff of the device layer for testing, and it is worth naming.
 * fat.c used to call ata_read directly, so the only way to test it was to stub
 * ata_read - which meant the stub had to imitate ATA's sector-count-and-LBA
 * interface, and part.c could not be tested at all because it would have needed
 * a second, different stub at the same layer. With both of them reading through
 * dev_read, one stub serves both, and it imitates an interface that is three
 * functions wide instead of a hardware protocol.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_stub.h"

/* A device_ops_t, not a replacement dev_read.
 *
 * This started as a stub for dev_read itself, which worked while device.c was
 * outside the host build. Once volume_test.c pulled device.c in, the two
 * collided - and the collision was worth resolving this way round rather than
 * by dropping device.c: the tests now go through the REAL dev_read, so they
 * exercise its bounds handling and its DEVICE_GONE check instead of a
 * second implementation that only resembles it.
 *
 * A stub that reimplements the function under test is a stub that can agree
 * with the test and disagree with the kernel. */
static int64 stub_read(device_t *dev, uint64 offset, void *buf, uint64 n) {
    dev_stub_t *s;

    if (dev == NULL || dev->body == NULL) {
        return -5;
    }
    s = (dev_stub_t *)dev->body;
    s->reads++;

    if (offset >= s->len) {
        return 0;                 /* past the end is EOF, not an error */
    }
    if (n > s->len - offset) {
        n = s->len - offset;
    }
    memcpy(buf, s->data + offset, (size_t)n);
    return (int64)n;
}

/* Writes go to the in-memory copy, and to the host file as well ONLY for a
 * stub opened with dev_stub_load_rw.
 *
 * Memory-only is the default and the reason has not changed: a test that
 * modified the shared fixture would make the next test depend on whether this
 * one ran, which is the failure where a suite passes in order and fails when
 * one test is run alone. What changed is that there is now a test which has
 * to check what reached the MEDIUM - a write is only written if it survives
 * the volume being torn down and mounted again - and it gets its own private
 * copy of the image so the guarantee above still holds for everyone else. */
static int64 stub_write(device_t *dev, uint64 offset, const void *buf,
                        uint64 n) {
    dev_stub_t *s;

    if (dev == NULL || dev->body == NULL) {
        return -5;
    }
    s = (dev_stub_t *)dev->body;
    s->writes++;
    if (offset >= s->len) {
        return -28;               /* -ENOSPC, matching disk.c */
    }
    if (n > s->len - offset) {
        n = s->len - offset;
    }
    memcpy(s->data + offset, buf, (size_t)n);

    if (s->file != NULL) {
        FILE *f = (FILE *)s->file;

        if (fseek(f, (long)offset, SEEK_SET) != 0 ||
            fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            return -5;
        }
        /* Flushed per write rather than at close. A test that mounts the
         * image again mid-run has to see the bytes, and stdio would otherwise
         * still be holding them. */
        fflush(f);
    }
    return (int64)n;
}

static const device_ops_t stub_ops = {
    .name  = "hostfile",
    .read  = stub_read,
    .write = stub_write
};

void dev_stub_attach(device_t *dev, dev_stub_t *stub, void *data, uint64 len) {
    stub->data      = (unsigned char *)data;
    stub->len       = len;
    stub->reads     = 0;
    stub->writes    = 0;
    stub->owns_data = 0;
    /* NULL, not left whatever a caller's dev_stub_t happened to hold before
     * this call. stub_write's own "only a stub opened with dev_stub_load_rw
     * writes through" guarantee is `s->file != NULL`, and every OTHER caller
     * of that check gets there through stub_load, which always sets this
     * field explicitly (to a real FILE* or to NULL) - this was the one path
     * that did not, and a dev_stub_t declared as an ordinary local rather
     * than `static` (which zero-initialises) turns the gap into a garbage
     * pointer stub_write dereferences as a FILE*. Found by gnfs_test.c,
     * which is the first caller of this function to use a non-static
     * dev_stub_t. */
    stub->file      = NULL;

    memset(dev, 0, sizeof(*dev));
    /* in_use, because the real dev_read checks dev_present() before touching
     * the driver - and a zeroed device_t is, correctly, not present. A test
     * device built by hand rather than by dev_alloc has to say so itself, and
     * omitting it makes every read -ENODEV: the exact failure a pulled disk
     * produces, arriving here for a completely different reason. */
    dev->in_use     = 1;
    dev->ops        = &stub_ops;
    dev->body       = stub;
    dev->kind       = DEVICE_KIND_DISK;
    dev->size       = len;
    dev->block_size = 512;
}

static int stub_load(device_t *dev, dev_stub_t *stub, const char *path,
                     int writeback) {
    FILE *f = fopen(path, writeback ? "r+b" : "rb");
    long  size;

    if (f == NULL) {
        return -1;
    }
    /* A stub being reloaded may still hold the previous file open. Closed
     * here rather than left to the caller, because every caller reloads in a
     * loop and one leaked descriptor per iteration is a test that fails at
     * the open-file limit for reasons that have nothing to do with what it
     * is testing. */
    if (stub->file != NULL) {
        fclose((FILE *)stub->file);
        stub->file = NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    free(stub->data);
    stub->data = malloc((size_t)size);
    if (stub->data == NULL || fread(stub->data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return -1;
    }
    dev_stub_attach(dev, stub, stub->data, (uint64)size);
    stub->owns_data = 1;

    if (writeback) {
        stub->file = f;          /* kept open; stub_write writes through it */
    } else {
        fclose(f);
        stub->file = NULL;
    }
    return 0;
}

int dev_stub_load(device_t *dev, dev_stub_t *stub, const char *path) {
    return stub_load(dev, stub, path, 0);
}

int dev_stub_load_rw(device_t *dev, dev_stub_t *stub, const char *path) {
    return stub_load(dev, stub, path, 1);
}
