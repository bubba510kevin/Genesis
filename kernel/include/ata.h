#ifndef ATA_H
#define ATA_H

#include "typesk.h"

/* ATA PIO driver - polling, no interrupts.
 *
 * PIO moves data a word at a time through an I/O port, which makes it slow
 * (roughly 3-16 MB/s) but means the driver never needs a physical address.
 * That matters here: every buffer you hand it is a kernel virtual address and
 * the CPU does the translation. DMA is the opposite - the controller bypasses
 * the MMU entirely, so every buffer would need virt_to_phys() and would have
 * to be physically contiguous. Worth knowing before you optimise this later.
 *
 * Polling rather than IRQ-driven for the same reason a first allocator is
 * first-fit: it has no concurrency, so when it misbehaves the bug is in the
 * ATA protocol rather than in your interrupt handling. Convert once FAT works.
 */

#define ATA_OK             0
#define ATA_ERR_NODEV     -1   /* nothing on this channel                  */
#define ATA_ERR_TIMEOUT   -2   /* BSY never cleared, or DRQ never came     */
#define ATA_ERR_FAULT     -3   /* DF: device fault                         */
#define ATA_ERR_ERR       -4   /* ERR: read the error register for detail  */
#define ATA_ERR_RANGE     -5   /* LBA past the end of the device           */
#define ATA_ERR_NOTATA    -6   /* responded, but it is ATAPI or SATA       */

#define ATA_SECTOR_SIZE   512

typedef struct {
    uint16 io_base;      /* 0x1F0 primary, 0x170 secondary        */
    uint16 ctrl_base;    /* 0x3F6 primary, 0x376 secondary        */
    uint8  slave;        /* 0 master, 1 slave                     */
    uint8  present;
    uint32 sectors;      /* LBA28 addressable count               */
    char   model[41];
} ata_device_t;

/* Probe all four possible drives (primary/secondary x master/slave). */
void ata_init(void);

/* index 0-3, or NULL if that slot has no drive. */
ata_device_t *ata_get(int index);

/* Read/write `count` sectors (1-255; 0 would mean 256 to the hardware, which
 * this driver does not support). `buffer` must have room for
 * count * ATA_SECTOR_SIZE bytes. Returns ATA_OK or a negative code. */
int ata_read(ata_device_t *dev, uint32 lba, uint8 count, void *buffer);
int ata_write(ata_device_t *dev, uint32 lba, uint8 count, const void *buffer);

/* One line per detected drive. */
void ata_report(uint8 color);

#endif
