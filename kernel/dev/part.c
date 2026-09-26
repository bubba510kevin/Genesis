#include "device.h"
#include "part.h"
#include "typesk.h"

/* See part.h for why the MBR parse lives here rather than in disk.c.
 *
 * Everything below reads little-endian by hand, byte at a time, rather than
 * casting into the sector buffer. The GPT entry array is 128 bytes per entry
 * starting at an arbitrary LBA, and an MBR entry sits at 446 + 16i - neither
 * is guaranteed to land 8-byte aligned, and an unaligned load is undefined
 * even where x86 tolerates it: the compiler is entitled to assume it cannot
 * happen and vectorise on that assumption. disk.c already made this argument
 * for the MBR; it applies twice as hard to a 64-bit field. */

#define SECTOR_SIZE   512
#define GPT_HEADER_LBA 1

static uint16 rd16(const uint8 *p) {
    return (uint16)((uint16)p[0] | ((uint16)p[1] << 8));
}

static uint32 rd32(const uint8 *p) {
    return (uint32)p[0]        | ((uint32)p[1] << 8) |
           ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static uint64 rd64(const uint8 *p) {
    return (uint64)rd32(p) | ((uint64)rd32(p + 4) << 32);
}

/* --- CRC-32 -------------------------------------------------------------
 *
 * Computed bit by bit rather than from a 1KB table. This runs a handful of
 * times at boot over at most 16KB, so the table would buy nothing measurable
 * and cost a page of static data in a kernel with a 128KB image cap. */
uint32 part_crc32(const void *data, uint64 len) {
    const uint8 *p = (const uint8 *)data;
    uint32 crc = 0xFFFFFFFFu;
    uint64 i;
    int    k;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++) {
            uint32 mask = (uint32)-(int32)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* --- GUIDs --------------------------------------------------------------- */

static int guid_eq(const part_guid_t *a, const uint8 *b) {
    int i;

    for (i = 0; i < 16; i++) {
        if (a->b[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int guid_is_zero(const part_guid_t *g) {
    int i;

    for (i = 0; i < 16; i++) {
        if (g->b[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* The three GUIDs worth recognising by name, in on-disk byte order.
 *
 * Written out as bytes rather than as the {12345678-1234-...} text form,
 * because the text form's first three groups are little-endian on disk and
 * the last two are not - and a table written from the text form by hand is
 * the single most common way to get a GUID comparison silently wrong. These
 * were transcribed field by field and are checked against a real disk by the
 * verification test, which is the only way to know. */

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B - EFI System Partition */
static const uint8 GUID_EFI_SYSTEM[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* 024DEE41-33E7-11D3-9D69-0008C781F39F - MBR partition scheme (reserved) */
static const uint8 GUID_MBR_SCHEME[16] = {
    0x41, 0xEE, 0x4D, 0x02, 0xE7, 0x33, 0xD3, 0x11,
    0x9D, 0x69, 0x00, 0x08, 0xC7, 0x81, 0xF3, 0x9F
};

/* 21686148-6449-6E6F-744E-656564454649 - BIOS boot partition */
static const uint8 GUID_BIOS_BOOT[16] = {
    0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E,
    0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49
};

int part_type_is_data(const part_entry_t *e) {
    if (e == NULL) {
        return 0;
    }
    if (e->mbr_type != 0) {
        /* MBR: extended containers hold other partitions rather than a
         * filesystem, and 0xEE is a protective entry that should never have
         * reached a caller (part_scan drops it). Everything else is offered. */
        return e->mbr_type != 0x05 && e->mbr_type != 0x0F &&
               e->mbr_type != 0xEE;
    }
    if (guid_is_zero(&e->type_guid)) {
        return 0;
    }
    /* A whitelist would be wrong here. There are dozens of filesystem type
     * GUIDs and new ones appear; refusing everything unrecognised means a
     * disk partitioned by a tool this kernel has not heard of has no volumes
     * at all. Refusing the three that are definitively NOT filesystems is the
     * claim that can actually be justified - and if a mount attempt on an
     * unknown type fails, it fails as "no filesystem recognised", which is a
     * far better message than the partition not existing. */
    return !guid_eq(&e->type_guid, GUID_EFI_SYSTEM) &&
           !guid_eq(&e->type_guid, GUID_MBR_SCHEME) &&
           !guid_eq(&e->type_guid, GUID_BIOS_BOOT);
}

const char *part_table_name(part_table_t t) {
    switch (t) {
        case PART_TABLE_MBR: return "MBR";
        case PART_TABLE_GPT: return "GPT";
        default:             return "none";
    }
}

/* --- is sector 0 a partition table, or a filesystem? ---------------------
 *
 * The question this file got WRONG on its first run, and the failure is worth
 * recording because the reasoning that produced it sounded fine.
 *
 * The rule was "no 0xAA55 signature means no partition table, so the whole
 * disk is one volume". That is true of the negative and says nothing about
 * the positive: a FAT boot sector carries 0xAA55 too - tools/fatfs.py writes
 * it, mkfs.fat writes it, every FAT volume ever made has it. So the data
 * disk, which is a filesystem starting at sector 0 with no table at all, was
 * treated as partitioned; bytes 446..510 of its BPB and boot code were parsed
 * as four partition entries; none of them validated; and the disk reported
 * ZERO volumes. Nothing was mounted and init could not be read.
 *
 * The signature does not distinguish the two cases because BOTH have it. What
 * distinguishes them is what else is in the sector.
 *
 * --- Two independent tests ------------------------------------------------
 * A BIOS parameter block is decisive when present: a jump instruction at byte
 * 0, a sane bytes-per-sector, and a power-of-two sectors-per-cluster do not
 * occur together by chance in MBR boot code, because the bytes at 11-13 of an
 * MBR are in the middle of executable code rather than a header.
 *
 * The partition entries are the second test, and it is the weaker one: a boot
 * flag must be 0x00 or 0x80, so any other value means these sixteen-byte
 * groups are not entries. It catches the disks whose sector 0 has no BPB and
 * no table either.
 *
 * Neither test is a proof and both are what every other OS uses. What makes
 * the pair acceptable is the direction they fail in: a false "this is a
 * filesystem" gives one whole-disk volume that a filesystem probe then
 * rejects, which is recoverable and visible. A false "this is a table" is
 * what just happened - silent, and it loses the disk. */

static int looks_like_bpb(const uint8 *sector) {
    uint16 bytes_per_sector;
    uint8  sectors_per_cluster;

    /* A short jump followed by a NOP, or a near jump. Every BPB-bearing boot
     * sector starts with one of these two because the BPB itself sits where
     * code would otherwise be, and the first instruction has to jump over
     * it. */
    if (!((sector[0] == 0xEB && sector[2] == 0x90) || sector[0] == 0xE9)) {
        return 0;
    }

    bytes_per_sector    = rd16(sector + 11);
    sectors_per_cluster = sector[13];

    if (bytes_per_sector != 512  && bytes_per_sector != 1024 &&
        bytes_per_sector != 2048 && bytes_per_sector != 4096) {
        return 0;
    }
    /* A power of two from 1 to 128. The bit trick rejects zero as well, which
     * matters: a zero here would be a divide by zero in any filesystem that
     * trusted it. */
    if (sectors_per_cluster == 0 || sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0) {
        return 0;
    }
    /* Reserved sectors is never zero on FAT - the boot sector itself is one.
     * Cheap, and it rejects a sector that passed the tests above by accident. */
    if (rd16(sector + 14) == 0) {
        return 0;
    }
    return 1;
}

static int entries_look_like_table(const uint8 *mbr) {
    int i;

    for (i = 0; i < 4; i++) {
        uint8 flag = mbr[446 + i * 16];

        /* The boot-indicator byte is 0x00 or 0x80 and nothing else. Any other
         * value means these bytes are code or data, not a partition table. */
        if (flag != 0x00 && flag != 0x80) {
            return 0;
        }
    }
    return 1;
}

/* --- MBR ---------------------------------------------------------------- */

/* Fills `out`, returns the count. `protective` is set when the table is a
 * single 0xEE entry - the marker a GPT disk leaves so that an MBR-only tool
 * sees one partition covering everything and declines to touch it. */
static int scan_mbr(device_t *disk, const uint8 *mbr, part_entry_t *out,
                    int max, int *protective) {
    uint64 disk_sectors = disk->size / SECTOR_SIZE;
    int found = 0;
    int ee    = 0;
    int live  = 0;
    int i;

    for (i = 0; i < 4; i++) {
        const uint8 *e = &mbr[446 + i * 16];
        uint64 start, count;

        if (e[4] == 0) {
            continue;                  /* an empty slot, not a bad one */
        }
        live++;
        if (e[4] == 0xEE) {
            ee++;
            continue;                  /* never handed back; see part.h */
        }

        start = rd32(e + 8);
        count = rd32(e + 12);

        if (count == 0 || (disk_sectors != 0 && start >= disk_sectors)) {
            continue;
        }
        /* Clamp rather than reject, as disk.c did: a table claiming a
         * partition runs past the end of the disk is wrong, and exposing the
         * part that exists beats losing the three good entries over one bad
         * one. */
        if (disk_sectors != 0 && start + count > disk_sectors) {
            count = disk_sectors - start;
        }
        if (found < max) {
            int k;

            out[found].start_lba = start;
            out[found].sectors   = count;
            out[found].mbr_type  = e[4];
            out[found].bootable  = (e[0] == 0x80) ? 1 : 0;
            out[found].index     = (uint32)(i + 1);
            for (k = 0; k < 16; k++) {
                out[found].type_guid.b[k]   = 0;
                out[found].unique_guid.b[k] = 0;
            }
            found++;
        }
    }

    /* Protective only when 0xEE is the ONLY live entry. A disk with a 0xEE
     * beside three real partitions is a hybrid MBR - the layout a dual-boot
     * installer writes - and treating it as protective would discard three
     * partitions that are genuinely there. */
    *protective = (ee > 0 && live == ee);

    /* Extended partitions (0x05, 0x0F) are still not followed. Walking a
     * linked list of sectors scattered across the disk, each entry relative
     * to a different base, is the classic place for an off-by-one that
     * reports a plausible partition that is not there - and GPT, which is
     * here now, is the answer to needing more than four. */
    return found;
}

/* --- GPT ----------------------------------------------------------------- */

/* Read and validate a GPT header from `lba`. Returns 0 and fills the four
 * out-parameters, or a negative errno. */
static int read_gpt_header(device_t *disk, uint64 lba,
                           uint64 *entry_lba, uint32 *entry_count,
                           uint32 *entry_size, uint32 *array_crc) {
    uint8  sector[SECTOR_SIZE];
    uint32 header_size;
    uint32 stored_crc;
    uint32 computed;
    uint8  copy[SECTOR_SIZE];
    int64  got;
    uint32 i;

    got = dev_read(disk, lba * SECTOR_SIZE, sector, SECTOR_SIZE);
    if (got < 0) {
        return (int)got;
    }
    if (got != SECTOR_SIZE) {
        return -5;                               /* -EIO */
    }

    /* "EFI PART" */
    if (sector[0] != 'E' || sector[1] != 'F' || sector[2] != 'I' ||
        sector[3] != ' ' || sector[4] != 'P' || sector[5] != 'A' ||
        sector[6] != 'R' || sector[7] != 'T') {
        return -22;                              /* -EINVAL: not a header */
    }

    header_size = rd32(sector + 12);
    /* 92 is the size the spec fixes for revision 1.0 and every real header
     * uses it. Larger is allowed by the spec, smaller is not; a header
     * claiming more than a sector is a corrupt field being used to size a
     * CRC over memory past the buffer, which is the actual reason this bound
     * is checked rather than assumed. */
    if (header_size < 92 || header_size > SECTOR_SIZE) {
        return -22;
    }

    /* The header CRC is computed with its own field zeroed. Done on a COPY:
     * zeroing the live buffer and restoring afterwards works right up until
     * an early return leaves it zeroed, and then the caller is looking at a
     * header that fails its own check for reasons that have nothing to do
     * with the disk. */
    stored_crc = rd32(sector + 16);
    for (i = 0; i < header_size; i++) {
        copy[i] = sector[i];
    }
    copy[16] = 0; copy[17] = 0; copy[18] = 0; copy[19] = 0;
    computed = part_crc32(copy, header_size);
    if (computed != stored_crc) {
        return -5;                               /* -EIO: header corrupt */
    }

    *entry_lba   = rd64(sector + 72);
    *entry_count = rd32(sector + 80);
    *entry_size  = rd32(sector + 84);
    *array_crc   = rd32(sector + 88);

    /* 128 is the spec minimum and the only value in practice, but the field
     * is honoured rather than assumed - what is rejected is a size that is
     * not a multiple of 8, or one so large the array cannot be walked. An
     * entry_size of zero would make the walk below loop forever. */
    if (*entry_size < 128 || *entry_size > 4096 || (*entry_size % 8) != 0) {
        return -22;
    }
    if (*entry_count == 0 || *entry_count > 1024) {
        return -22;
    }
    return 0;
}

static int scan_gpt(device_t *disk, part_entry_t *out, int max) {
    uint64 entry_lba = 0, alt_lba;
    uint32 entry_count = 0, entry_size = 0, array_crc = 0;
    uint32 running = 0xFFFFFFFFu;      /* CRC state, folded across reads */
    uint8  buf[SECTOR_SIZE];
    uint64 total_bytes;
    uint64 done;
    uint64 disk_sectors = disk->size / SECTOR_SIZE;
    int    found = 0;
    int    rc;

    rc = read_gpt_header(disk, GPT_HEADER_LBA, &entry_lba, &entry_count,
                         &entry_size, &array_crc);
    if (rc != 0) {
        /* The backup header at the last LBA, which is the entire reason GPT
         * writes one. A primary destroyed by a tool that wrote an MBR over
         * LBA 1 is exactly the case this recovers, and it is common enough
         * to be worth the twenty lines. */
        if (disk_sectors < 2) {
            return rc;
        }
        alt_lba = disk_sectors - 1;
        rc = read_gpt_header(disk, alt_lba, &entry_lba, &entry_count,
                             &entry_size, &array_crc);
        if (rc != 0) {
            return rc;
        }
    }

    total_bytes = (uint64)entry_count * entry_size;
    if (entry_lba == 0 || (disk_sectors != 0 && entry_lba >= disk_sectors)) {
        return -22;
    }

    /* Two passes over the array would mean reading it twice or buffering
     * 16KB the kernel has no spare page for. Instead the CRC is folded
     * sector by sector as the entries are parsed, and the RESULT is checked
     * at the end - so a corrupt array is rejected after the parse rather
     * than before it, and the parse output is discarded when it is.
     *
     * Discarding is the part that matters. Returning the entries and
     * mentioning the CRC failed would leave the decision to a caller that has
     * no way to make it, and "mount the partitions from a table that failed
     * its checksum" is not a decision anyone should be offered. */
    for (done = 0; done < total_bytes; done += SECTOR_SIZE) {
        uint64 chunk = total_bytes - done;
        uint64 off;
        int64  got;

        if (chunk > SECTOR_SIZE) {
            chunk = SECTOR_SIZE;
        }
        got = dev_read(disk, entry_lba * SECTOR_SIZE + done, buf, SECTOR_SIZE);
        if (got < 0) {
            return (int)got;
        }
        if (got != SECTOR_SIZE) {
            return -5;
        }

        {   /* fold this chunk into the running CRC */
            uint64 i;
            int    k;
            for (i = 0; i < chunk; i++) {
                running ^= buf[i];
                for (k = 0; k < 8; k++) {
                    uint32 mask = (uint32)-(int32)(running & 1u);
                    running = (running >> 1) ^ (0xEDB88320u & mask);
                }
            }
        }

        for (off = 0; off + entry_size <= chunk; off += entry_size) {
            const uint8 *e = buf + off;
            part_entry_t *p;
            uint64 first, last;
            int    k;
            int    zero = 1;

            for (k = 0; k < 16; k++) {
                if (e[k] != 0) {
                    zero = 0;
                    break;
                }
            }
            if (zero) {
                /* An all-zero type GUID is an unused slot. NOT a stopping
                 * point: the array is allowed to be sparse, and stopping at
                 * the first hole loses every partition after a deleted one. */
                continue;
            }

            first = rd64(e + 32);
            last  = rd64(e + 40);
            if (last < first) {
                continue;
            }
            if (disk_sectors != 0 && first >= disk_sectors) {
                continue;
            }
            if (disk_sectors != 0 && last >= disk_sectors) {
                last = disk_sectors - 1;
            }

            if (found >= max) {
                continue;      /* keep folding the CRC over the rest */
            }
            p = &out[found];
            p->start_lba = first;
            /* Inclusive last LBA, hence the +1. Off by one here produces a
             * volume one sector short, which a filesystem notices only when
             * something lands in its final cluster - a bug that appears
             * months after the disk was partitioned. */
            p->sectors   = last - first + 1;
            p->mbr_type  = 0;
            p->bootable  = 0;
            p->index     = (uint32)((done + off) / entry_size) + 1;
            for (k = 0; k < 16; k++) {
                p->type_guid.b[k]   = e[k];
                p->unique_guid.b[k] = e[16 + k];
            }
            found++;
        }
    }

    if (~running != array_crc) {
        return -5;                               /* -EIO: array corrupt */
    }
    return found;
}

/* --- the decision -------------------------------------------------------- */

int part_scan(device_t *disk, part_entry_t *out, int max,
              part_table_t *table) {
    uint8 mbr[SECTOR_SIZE];
    int   protective = 0;
    int   n;
    int64 got;

    if (disk == NULL || out == NULL || max <= 0 || table == NULL) {
        return -22;
    }
    *table = PART_TABLE_NONE;

    got = dev_read(disk, 0, mbr, SECTOR_SIZE);
    if (got < 0) {
        return (int)got;
    }
    if (got != SECTOR_SIZE) {
        return -5;
    }

    /* No boot signature at all: certainly no partition table. */
    if (rd16(mbr + 510) != 0xAA55u) {
        return 0;
    }

    /* Signature present, which by itself proves nothing - see the note above
     * looks_like_bpb. A filesystem at sector 0 has the signature too, and
     * treating it as partitioned is what lost the data disk entirely.
     *
     * PART_TABLE_NONE with zero partitions is not a failure: volume.c reads
     * it as "the whole disk is one volume", which is what tools/fatfs.py
     * produces and what this kernel boots from today. */
    if (looks_like_bpb(mbr) || !entries_look_like_table(mbr)) {
        return 0;
    }

    n = scan_mbr(disk, mbr, out, max, &protective);

    /* A protective MBR means the real table is a GPT, and the 0xEE entry must
     * not be offered as a partition - mounting it would mean mounting the
     * disk including its own partition table.
     *
     * A GPT is also tried when the MBR is NOT protective and found nothing.
     * That covers a disk whose protective entry was overwritten by a tool
     * that only understands MBR, which leaves a valid GPT that nothing would
     * otherwise look for. */
    if (protective || n == 0) {
        int g = scan_gpt(disk, out, max);

        if (g >= 0) {
            *table = PART_TABLE_GPT;
            return g;
        }
        if (protective) {
            /* Protective MBR, unreadable GPT. Zero partitions rather than the
             * 0xEE entry: the disk says "do not interpret me as MBR" and the
             * honest answer to a GPT we cannot read is that we found nothing,
             * not that we found one partition spanning everything. */
            *table = PART_TABLE_GPT;
            return 0;
        }
    }

    *table = PART_TABLE_MBR;
    return n;
}
