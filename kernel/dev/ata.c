#include "ata.h"
#include "io.h"
#include "screen.h"
#include "typesk.h"

/* Register offsets from io_base. */
#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_SECCOUNT    0x02
#define ATA_REG_LBA_LO      0x03
#define ATA_REG_LBA_MID     0x04
#define ATA_REG_LBA_HI      0x05
#define ATA_REG_DRIVE       0x06
#define ATA_REG_STATUS      0x07   /* read  */
#define ATA_REG_COMMAND     0x07   /* write */

/* Status bits. BSY and everything else are mutually exclusive: while BSY is
 * set the other bits are meaningless and must not be tested. */
#define ATA_SR_BSY          0x80
#define ATA_SR_DRDY         0x40
#define ATA_SR_DF           0x20
#define ATA_SR_DRQ          0x08
#define ATA_SR_ERR          0x01

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY    0xEC

#define ATA_POLL_LIMIT      1000000u

static ata_device_t devices[4];

/* Reading the regular status register at io_base+7 acknowledges a pending
 * interrupt as a side effect. The alternate status register on the control
 * port returns the same value with no side effects, so all polling goes
 * through here. It matters the moment you convert this to IRQ-driven, and
 * it costs nothing now. */
static uint8 ata_status(ata_device_t *dev) {
    return inb(dev->ctrl_base);
}

/* After selecting a drive the bus needs ~400ns before status is valid. Four
 * reads of the alternate status register is the conventional way to burn it -
 * each ISA I/O cycle is roughly 100ns. Skip this and you read the PREVIOUS
 * drive's status and conclude the wrong thing about which drives exist. */
static void ata_delay_400ns(ata_device_t *dev) {
    inb(dev->ctrl_base);
    inb(dev->ctrl_base);
    inb(dev->ctrl_base);
    inb(dev->ctrl_base);
}

/* Wait for BSY to clear, then check the error bits. */
static int ata_wait_not_busy(ata_device_t *dev) {
    uint32 spins = ATA_POLL_LIMIT;

    while (spins--) {
        uint8 status = ata_status(dev);

        if (status == 0xFF) {
            /* All lines floating high: nothing is driving the bus, so there
             * is no drive here at all. Distinct from a drive that is merely
             * busy, and worth catching early - otherwise you spin the full
             * timeout on every empty channel at boot. */
            return ATA_ERR_NODEV;
        }
        if (!(status & ATA_SR_BSY)) {
            if (status & ATA_SR_ERR) return ATA_ERR_ERR;
            if (status & ATA_SR_DF)  return ATA_ERR_FAULT;
            return ATA_OK;
        }
    }
    return ATA_ERR_TIMEOUT;
}

/* Wait for BSY to clear AND DRQ to come up: the drive has a full sector of
 * data waiting. */
static int ata_wait_drq(ata_device_t *dev) {
    uint32 spins = ATA_POLL_LIMIT;

    while (spins--) {
        uint8 status = ata_status(dev);

        if (status == 0xFF) return ATA_ERR_NODEV;
        if (status & ATA_SR_BSY) continue;
        if (status & ATA_SR_ERR) return ATA_ERR_ERR;
        if (status & ATA_SR_DF)  return ATA_ERR_FAULT;
        if (status & ATA_SR_DRQ) return ATA_OK;
    }
    return ATA_ERR_TIMEOUT;
}

static void ata_select(ata_device_t *dev, uint32 lba) {
    /* 0xE0 = LBA mode, master. Bit 4 selects slave. The low nibble carries
     * LBA bits 27:24 - the reason this is "28-bit LBA" and caps out at
     * 128GB. LBA48 uses a different command and a second register write. */
    outb(dev->io_base + ATA_REG_DRIVE,
         (uint8)(0xE0 | (dev->slave << 4) | ((lba >> 24) & 0x0F)));
    ata_delay_400ns(dev);
}

/* --- probing ------------------------------------------------------------ */

static void ata_identify(ata_device_t *dev) {
    uint16 id[256];
    int i, rc;

    dev->present = 0;

    outb(dev->io_base + ATA_REG_DRIVE, (uint8)(0xA0 | (dev->slave << 4)));
    ata_delay_400ns(dev);

    outb(dev->io_base + ATA_REG_SECCOUNT, 0);
    outb(dev->io_base + ATA_REG_LBA_LO,   0);
    outb(dev->io_base + ATA_REG_LBA_MID,  0);
    outb(dev->io_base + ATA_REG_LBA_HI,   0);
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns(dev);

    if (ata_status(dev) == 0) {
        return;   /* status 0 means no device on this slot */
    }

    rc = ata_wait_not_busy(dev);
    if (rc != ATA_OK) {
        return;
    }

    /* An ATAPI or SATA device answers IDENTIFY by putting a signature in the
     * LBA mid/hi registers instead of returning data. Reading 256 words from
     * one of those would hang waiting for a DRQ that never comes. */
    if (inb(dev->io_base + ATA_REG_LBA_MID) != 0 ||
        inb(dev->io_base + ATA_REG_LBA_HI)  != 0) {
        return;
    }

    if (ata_wait_drq(dev) != ATA_OK) {
        return;
    }

    insw(dev->io_base + ATA_REG_DATA, id, 256);

    /* Words 60-61: LBA28 sector count, low word first. */
    dev->sectors = (uint32)id[60] | ((uint32)id[61] << 16);

    /* Words 27-46: model, 40 characters with each BYTE PAIR SWAPPED - a
     * big-endian artefact of the original spec. Unswap or every drive appears
     * to be made by "TQMEU". */
    for (i = 0; i < 20; i++) {
        dev->model[i * 2]     = (char)(id[27 + i] >> 8);
        dev->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    dev->model[40] = '\0';
    for (i = 39; i >= 0 && dev->model[i] == ' '; i--) {
        dev->model[i] = '\0';
    }

    dev->present = 1;
}

void ata_init(void) {
    static const uint16 io[2]   = { 0x1F0, 0x170 };
    static const uint16 ctrl[2] = { 0x3F6, 0x376 };
    int channel, slave, i;

    for (i = 0; i < 4; i++) {
        devices[i].present = 0;
        devices[i].sectors = 0;
        devices[i].model[0] = '\0';
    }

    for (channel = 0; channel < 2; channel++) {
        for (slave = 0; slave < 2; slave++) {
            ata_device_t *dev = &devices[channel * 2 + slave];

            dev->io_base   = io[channel];
            dev->ctrl_base = ctrl[channel];
            dev->slave     = (uint8)slave;

            /* Clear nIEN so the device would raise interrupts. Harmless while
             * polling, and means the IRQ path works the day you enable it. */
            outb(dev->ctrl_base, 0x00);

            ata_identify(dev);
        }
    }
}

ata_device_t *ata_get(int index) {
    if (index < 0 || index > 3 || !devices[index].present) {
        return NULL;
    }
    return &devices[index];
}

/* --- transfers ---------------------------------------------------------- */

int ata_read(ata_device_t *dev, uint32 lba, uint8 count, void *buffer) {
    uint16 *out = (uint16 *)buffer;
    int rc, i;

    if (dev == NULL || !dev->present) return ATA_ERR_NODEV;
    if (count == 0)                   return ATA_ERR_RANGE;
    if (lba + count > dev->sectors)   return ATA_ERR_RANGE;

    ata_select(dev, lba);

    outb(dev->io_base + ATA_REG_SECCOUNT, count);
    outb(dev->io_base + ATA_REG_LBA_LO,  (uint8)(lba & 0xFF));
    outb(dev->io_base + ATA_REG_LBA_MID, (uint8)((lba >> 8) & 0xFF));
    outb(dev->io_base + ATA_REG_LBA_HI,  (uint8)((lba >> 16) & 0xFF));
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    /* One DRQ handshake PER SECTOR. It is tempting to issue a single
     * rep insw for count*256 words, and it will even appear to work on an
     * emulator - but the drive raises DRQ once per sector and the data is not
     * continuously available. On real hardware that reads garbage. */
    for (i = 0; i < count; i++) {
        rc = ata_wait_drq(dev);
        if (rc != ATA_OK) {
            return rc;
        }
        insw(dev->io_base + ATA_REG_DATA, out, 256);
        out += 256;
    }

    return ATA_OK;
}

int ata_write(ata_device_t *dev, uint32 lba, uint8 count, const void *buffer) {
    const uint16 *in = (const uint16 *)buffer;
    int rc, i;

    if (dev == NULL || !dev->present) return ATA_ERR_NODEV;
    if (count == 0)                   return ATA_ERR_RANGE;
    if (lba + count > dev->sectors)   return ATA_ERR_RANGE;

    ata_select(dev, lba);

    outb(dev->io_base + ATA_REG_SECCOUNT, count);
    outb(dev->io_base + ATA_REG_LBA_LO,  (uint8)(lba & 0xFF));
    outb(dev->io_base + ATA_REG_LBA_MID, (uint8)((lba >> 8) & 0xFF));
    outb(dev->io_base + ATA_REG_LBA_HI,  (uint8)((lba >> 16) & 0xFF));
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    for (i = 0; i < count; i++) {
        rc = ata_wait_drq(dev);
        if (rc != ATA_OK) {
            return rc;
        }
        outsw(dev->io_base + ATA_REG_DATA, in, 256);
        in += 256;
    }

    /* Without this the data may sit in the drive's write cache indefinitely.
     * It is not optional: a write that is never flushed is a write that
     * silently did not happen if power is lost, and on an emulator it can mean
     * a read-back returns stale data. */
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy(dev);
}

/* --- reporting ---------------------------------------------------------- */

static void print_u32(uint32 value, uint8 color) {
    char buf[11];
    int i = 10;

    buf[10] = '\0';
    if (value == 0) {
        print_string("0", color);
        return;
    }
    while (value && i > 0) {
        buf[--i] = (char)('0' + (value % 10));
        value /= 10;
    }
    print_string(&buf[i], color);
}

void ata_report(uint8 color) {
    int i, found = 0;

    for (i = 0; i < 4; i++) {
        if (!devices[i].present) {
            continue;
        }
        found++;
        print_string("ata", color);
        print_u32((uint32)i, color);
        print_string(": ", color);
        print_string(devices[i].model, color);
        print_string("  ", color);
        print_u32(devices[i].sectors, color);
        print_string(" sectors (", color);
        /* Report KB below 1MB: a 100-sector boot image is a legitimate disk
         * and "0 MB" tells you nothing about whether IDENTIFY worked. */
        if (devices[i].sectors < 2048) {
            print_u32(devices[i].sectors / 2, color);
            print_string(" KB)\n", color);
        } else {
            print_u32(devices[i].sectors / 2048, color);
            print_string(" MB)\n", color);
        }
    }

    if (!found) {
        print_string("ata: no drives found\n", 0x0C);
    }
}
