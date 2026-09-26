#ifndef AHCI_H
#define AHCI_H

#include "typesk.h"

/* AHCI (SATA) driver - polling, no interrupts, one command slot.
 *
 * --- why this is native rather than vendored -------------------------------
 * vendsrc/sys/dev/ahci/ahci.c is 2,911 lines and is a CAM SIM driver: it
 * includes cam/cam_ccb.h, cam/cam_sim.h and cam/cam_xpt_sim.h, and it never
 * touches a disk directly - it registers a SIM and services CCBs. Vendoring it
 * means vendoring CAM, which is 104,469 lines, or writing a shim for the
 * twenty-four xpt_ and cam_sim_ entry points it uses. AHCI's own programming model is small
 * enough that writing it is cheaper than either, and the result fits the shape
 * kernel/dev/ata.c already established: raw hardware talk here, a device_ops_t
 * adapter in disk.c.
 *
 * The DRIVER MODEL is still Newbus - this attaches through the PCI bus layer
 * the same way, and nothing here is Linux- or WDM-shaped. What is rejected is
 * FreeBSD's ahci.c specifically, not FreeBSD's driver model.
 *
 * --- polled, like ata.c, and for the same reason ---------------------------
 * ata.h says it: polling has no concurrency, so when it misbehaves the bug is
 * in the storage protocol rather than in interrupt handling. AHCI can raise an
 * interrupt per command and should eventually; a first cut that polls PxCI is
 * one that can be debugged.
 *
 * --- DMA, which PIO did not need -------------------------------------------
 * This is the real difference from ata.c and it is worth stating loudly. PIO
 * moves data through an I/O port, so the CPU does the address translation and
 * any kernel virtual address works. AHCI is DMA: the controller walks a
 * physical-address scatter list with the MMU bypassed entirely. Every address
 * handed to the hardware here is PHYSICAL, and a buffer that is contiguous in
 * kernel virtual space is not necessarily contiguous in physical space.
 *
 * That is why read/write below bounce through a dedicated frame rather than
 * mapping the caller's buffer: a caller's pointer may straddle two pages that
 * are not adjacent in RAM, and handing its physical start address to the
 * controller would transfer the right number of bytes to the wrong place.
 */

#define AHCI_OK             0
#define AHCI_ERR_NODEV     -1   /* no device on this port                   */
#define AHCI_ERR_TIMEOUT   -2   /* the controller never completed           */
#define AHCI_ERR_TFD       -3   /* task file error: the drive refused it    */
#define AHCI_ERR_RANGE     -5   /* LBA past the end of the device           */
#define AHCI_ERR_NOTSATA   -6   /* the port answered, but it is not a disk  */

#define AHCI_SECTOR_SIZE  512

/* Sixteen, because CAP.NP is five bits and a real HBA can implement 32 - but
 * every port costs a frame of DMA structures whether or not anything is
 * plugged into it, and no machine this targets has more than a handful. Ports
 * past this are REPORTED and skipped rather than silently ignored. */
#define AHCI_MAX_PORTS 16

typedef struct {
    void  *hba;          /* the HBA's mapped registers (opaque here)      */
    int    port;         /* which port on that HBA                        */
    uint8  present;
    uint8  atapi;        /* answered, but as a packet device              */
    uint64 sectors;      /* LBA48 addressable count                       */
    char   model[41];
} ahci_device_t;

/* Find every AHCI controller on the PCI bus, reset it, and probe its ports.
 * Safe to call when there is no AHCI controller at all - the machine this
 * currently boots on under QEMU has one, and the FreeBSD-idiom loadable
 * module already claims it, so see the note in ahci.c about who wins. */
void ahci_init(void);

/* index into the flat list of devices found across all controllers, or NULL. */
ahci_device_t *ahci_get(int index);

/* Read/write `count` sectors. `buffer` is an ordinary kernel pointer and need
 * not be physically contiguous - see the DMA note above. Returns AHCI_OK or a
 * negative code. */
int ahci_read(ahci_device_t *dev, uint64 lba, uint32 count, void *buffer);
int ahci_write(ahci_device_t *dev, uint64 lba, uint32 count,
               const void *buffer);

/* One line per detected device, matching ata_report's posture: report what was
 * found whether or not anything uses it. */
void ahci_report(uint8 color);

/* Boot selftest: IDENTIFY, then a write/read round trip against a scratch
 * sector. Returns 0 when it passed, non-zero otherwise; prints its own
 * result like the other selftests. Skips loudly when there is no device. */
int ahci_selftest(void);

#endif
