/* Genesis: the single translation unit for the vendored ZFS reader.
 *
 * kernel/zfs/vendor/zfsimpl.c is FreeBSD's standalone reader, unmodified, and
 * it includes zfssubr.c which in turn includes the checksum and compression
 * files beside it. Upstream builds it exactly this way - one object - which
 * is why there is one .c here and not fifteen.
 *
 * The includes below are the environment that upstream's stand/libsa/zfs/zfs.c
 * has already pulled in by the time it includes zfsimpl.c. Putting them here
 * rather than editing the vendored file is the whole point: the vendored
 * files are byte-for-byte what was fetched, and everything this port had to
 * decide is in kernel/zfs/compat/ and kernel/zfs/zfs_shim.c. */

/* Upstream's own knob for accepting a vdev of type "file".
 *
 * The loader refuses one because a boot device is never a file. Genesis is
 * not booting from the pool: the pools it will see are images - one created
 * with `zpool create tank /path/to/img` on a host and staged onto a disk, and
 * the three OpenZFS test-suite pools the host tests read. Every one of those
 * has a top-level vdev of type "file", and without this they are all refused
 * with "can only boot from disk, mirror, raidz1...".
 *
 * Defined here rather than in build.py so that the kernel and the host tests
 * cannot disagree about it - a flag that is on in the tests and off in the
 * kernel is a test suite that proves the wrong binary works. */
#define ZFS_TEST 1

#include <sys/endian.h>         /* BYTE_ORDER - see the note below          */
#include <sys/param.h>          /* NBBY, MIN, roundup                      */
#include <sys/types.h>
#include <errno.h>
#include <stdbool.h>
#include <zfs_libsa.h>          /* malloc, printf, the string routines     */
#include "vendor/nvlist.h"      /* the nvlist parser zfsimpl.c calls into  */
#include "vendor/zfsimpl.h"      /* guarded; zfsimpl.c includes it again    */

/* --- the byte-order trap, which this port fell into ----------------------
 *
 * zfsimpl.h decides the host's byte order with
 *
 *     #if BYTE_ORDER == _BIG_ENDIAN
 *
 * and an UNDEFINED macro in an #if is zero. So if <sys/endian.h> has not been
 * included by the time zfsimpl.h is read, that test is 0 == 0, it is TRUE,
 * and the reader concludes it is running on a big-endian machine. Every
 * checksum is then computed byteswapped, every block fails verification, and
 * the failure arrives as
 *
 *     zio_read error: 5
 *     ZFS: i/o error - all block copies unavailable
 *
 * which reads as a corrupt pool. It cost an afternoon: the labels parsed, the
 * uberblocks verified (they are SHA-256, which has no word order to get
 * wrong), the pool name and GUID came out right, and then every single data
 * block was "unreadable". A one-line include order.
 *
 * Hence the include above, and the assertion below - the value is knowable at
 * compile time, so it is checked at compile time rather than left to a test
 * that would only fail on a pool nobody had. */
#if ZFS_HOST_BYTEORDER != 1ULL
#error "the vendored reader thinks this machine is big-endian: <sys/endian.h> must be included before zfsimpl.h, or every checksum will be computed byteswapped and every block will read as corrupt"
#endif

/* One prototype that upstream gets from stand/libsa/zfs/libzfs.h, which is
 * the boot loader's own header and pulls in a boot loader. zfsimpl.c calls
 * this function above the line that defines it, so without the declaration
 * the call is implicitly int-returning and the definition conflicts. */
nvlist_t *vdev_read_bootenv(vdev_t *vdev);

/* Same story: vdev_probe calls ldi_get_size to find the labels at the end of
 * the medium, and upstream declares it in the loader's own headers. Genesis
 * defines it in zfs_glue.inc. */
uint64_t ldi_get_size(void *priv);

#include "vendor/zfsimpl.c"

/* Genesis's equivalent of stand/libsa/zfs/zfs.c: the glue that turns the
 * vendored reader's static functions into something the rest of the kernel
 * can call. Included rather than compiled separately because those functions
 * ARE static, which is upstream's arrangement and not an accident. */
#include "zfs_glue.inc"
