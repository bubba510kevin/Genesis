#ifndef DEV_STUB_H
#define DEV_STUB_H

#include "device.h"
#include "typesk.h"

/* A block device backed by a host buffer. See dev_stub.c. */
typedef struct {
    unsigned char *data;
    uint64         len;
    unsigned long  reads;    /* counted, so a test can assert a scan did not
                              * read the whole disk to find four entries */
    unsigned long  writes;   /* likewise, for the block cache: a write-through
                              * cache has to be shown reaching the device, and
                              * a read-caching one has to be shown NOT reaching
                              * it twice */
    int            owns_data;

    /* Non-NULL only for a stub opened with dev_stub_load_rw. See there and
     * stub_write: writes go through to the host file as well as to memory,
     * which is what lets a test tear the volume down, mount the IMAGE again
     * and check what actually landed. */
    void          *file;
} dev_stub_t;

/* Load `path` into `stub` and point `dev` at it. Returns 0, or -1 if the file
 * could not be read - which for these tests means run.sh did not build the
 * fixture, and is worth reporting as a failure rather than a skip. */
int dev_stub_load(device_t *dev, dev_stub_t *stub, const char *path);

/* dev_stub_load, but WRITE-THROUGH: writes reach the host file too.
 *
 * A separate entry point rather than a flag on the one above, because the
 * memory-only behaviour is a deliberate guarantee for every other suite here
 * - see stub_write - and the file that opts out of it should have to say so.
 * Only safe on an image nothing else reads; run.sh makes a private copy
 * (test-rw.img) for exactly this. */
int dev_stub_load_rw(device_t *dev, dev_stub_t *stub, const char *path);

/* The same device over a buffer the caller already has.
 *
 * dev_stub_load is this plus a file read, and it is written that way round so
 * that a test with a generated pattern and a test with a real filesystem image
 * go through the SAME device_ops_t. The alternative - a second one-off ops
 * table inside whichever test needs a synthetic disk - is a second thing that
 * can drift from dev_read's contract, which is the entire argument for this
 * file existing. */
void dev_stub_attach(device_t *dev, dev_stub_t *stub, void *data, uint64 len);

#endif
