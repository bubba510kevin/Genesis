#include "fat.h"
#include "ata.h"
#include "kheap.h"
#include "typesk.h"

#define DIR_ENTRY_SIZE   32

/* FAT16 cluster values. Clusters are numbered from 2 - 0 and 1 are reserved
 * as markers - which is the reason for the "- 2" in fat_cluster_lba below and
 * the single most common off-by-two in a first FAT driver. */
#define CLUSTER_FIRST    2u
#define CLUSTER_BAD      0xFFF7u
#define CLUSTER_EOC      0xFFF8u   /* 0xFFF8-0xFFFF all mean end of chain */

/* BPB fields are little-endian AND unaligned - bytes_per_sector sits at
 * offset 11, an odd address. Casting a uint16 * at it is undefined behaviour
 * even though x86 tolerates the access, and GCC is entitled to optimise on the
 * assumption it cannot happen. Assembling bytewise costs nothing. */
static uint16 rd16(const uint8 *p) {
    return (uint16)((uint16)p[0] | ((uint16)p[1] << 8));
}

static uint32 rd32(const uint8 *p) {
    return (uint32)p[0] | ((uint32)p[1] << 8)
         | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static uint32 fat_cluster_lba(fat_volume_t *vol, uint32 cluster) {
    return vol->data_start + (cluster - CLUSTER_FIRST) * vol->sectors_per_cluster;
}

/* Sectors, through the device layer.
 *
 * One helper rather than six ata_read call sites, and that is what made the
 * move off ata_device_t a ten-line change instead of a rewrite: the LBA-to-
 * byte-offset conversion and the "a short read is an error here, unlike in
 * read(2)" rule are each written once.
 *
 * Short IS an error at this level. A caller of read(2) handles a short read
 * as end of file; a filesystem asking for a directory sector and getting half
 * of one has a corrupt volume, and treating that as success hands the parser
 * a buffer whose second half is whatever was there before. */
static int fat_sectors(fat_volume_t *vol, uint32 lba, uint32 count,
                       void *buf) {
    uint64 want = (uint64)count * ATA_SECTOR_SIZE;
    int64  got  = dev_read(vol->dev, (uint64)lba * ATA_SECTOR_SIZE, buf, want);

    if (got < 0) {
        return FAT_ERR_IO;
    }
    return ((uint64)got == want) ? FAT_OK : FAT_ERR_IO;
}

int fat_mount(fat_volume_t *vol, device_t *dev) {
    uint8 *boot;
    uint32 total_sectors, data_sectors, num_fats;
    int rc, i;

    vol->mounted = 0;
    vol->dev = dev;

    boot = (uint8 *)kmalloc(ATA_SECTOR_SIZE);
    if (boot == NULL) {
        return FAT_ERR_NOMEM;
    }

    /* vol->dev was set above, so this goes through the same helper as every
     * other read here. Reading the boot sector by a different route than the
     * rest of the driver is how a filesystem comes to mount successfully and
     * then read from the wrong offset. */
    rc = fat_sectors(vol, 0, 1, boot);
    if (rc != FAT_OK) {
        kfree(boot);
        return FAT_ERR_IO;
    }

    for (i = 0; i < 5; i++) {
        if (boot[54 + i] != (uint8)"FAT16"[i]) {
            kfree(boot);
            return FAT_ERR_NOTFAT;
        }
    }

    vol->bytes_per_sector    = rd16(boot + 11);
    vol->sectors_per_cluster = boot[13];
    vol->fat_start           = rd16(boot + 14);   /* == reserved sectors */
    num_fats                 = boot[16];
    vol->root_entries        = rd16(boot + 17);
    vol->fat_sectors         = rd16(boot + 22);

    /* Total sector count lives in one of two fields: the 16-bit one at 19, or,
     * if that is zero because the volume is too large for it, the 32-bit one
     * at 32. Reading only the first gives 0 on any disk above 32MB. */
    total_sectors = rd16(boot + 19);
    if (total_sectors == 0) {
        total_sectors = rd32(boot + 32);
    }

    vol->num_fats     = num_fats;
    vol->root_start   = vol->fat_start + num_fats * vol->fat_sectors;
    vol->root_sectors = (vol->root_entries * DIR_ENTRY_SIZE
                         + vol->bytes_per_sector - 1) / vol->bytes_per_sector;
    vol->data_start   = vol->root_start + vol->root_sectors;

    data_sectors = total_sectors - vol->data_start;
    vol->total_clusters = data_sectors / vol->sectors_per_cluster;

    kfree(boot);

    if (vol->bytes_per_sector != ATA_SECTOR_SIZE ||
        vol->sectors_per_cluster == 0) {
        return FAT_ERR_NOTFAT;
    }

    vol->mounted = 1;
    return FAT_OK;
}

/* Follow one link in the chain. Each FAT16 entry is 2 bytes, so the entry for
 * cluster N is at byte offset N*2 into the FAT - which may land in any sector
 * of it, hence the read per lookup. Caching the FAT in memory is the obvious
 * optimisation and deliberately not done yet: a 32MB volume has a 128KB FAT,
 * and correctness first. */
static int fat_next_cluster(fat_volume_t *vol, uint32 cluster, uint32 *next) {
    uint8 *sector;
    uint32 offset, lba, index;
    int rc;

    offset = cluster * 2u;
    lba    = vol->fat_start + (offset / vol->bytes_per_sector);
    index  = offset % vol->bytes_per_sector;

    sector = (uint8 *)kmalloc(ATA_SECTOR_SIZE);
    if (sector == NULL) {
        return FAT_ERR_NOMEM;
    }

    rc = fat_sectors(vol, lba, 1, sector);
    if (rc != FAT_OK) {
        kfree(sector);
        return FAT_ERR_IO;
    }

    *next = rd16(sector + index);
    kfree(sector);
    return FAT_OK;
}

/* "README  TXT" -> "README.TXT". The stored form is 8 characters of name and
 * 3 of extension, space-padded, with no dot: the dot is implied by position.
 * Compare raw entries and nothing ever matches. */
static void fat_name_from_entry(const uint8 *entry, char *out) {
    int i, n = 0;

    for (i = 0; i < 8 && entry[i] != ' '; i++) {
        out[n++] = (char)entry[i];
    }
    if (entry[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && entry[i] != ' '; i++) {
            out[n++] = (char)entry[i];
        }
    }
    out[n] = '\0';
}

static char fat_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* "test.txt" -> "TEST    TXT", the 11-byte on-disk form, so a caller can pass
 * a name the way a human writes it.
 *
 * Returns 0, or -1 if the name does not fit 8.3.
 *
 * It used to return nothing and TRUNCATE. The path resolver already bounds a
 * component at 12 characters - 8.3 plus the dot - which closes most of that,
 * and the comment there says why. What it does NOT close is a component that
 * is short enough to pass and still has a stem longer than eight:
 *
 *   lookup "/wsr/system321"   ->  9 characters, passes the 12-char bound
 *                            ->  truncated key "SYSTEM32"
 *                            ->  matches the system32 DIRECTORY that is
 *                                really there
 *
 * A caller asking for one name gets a different object that happens to be on
 * the volume, with no error at any layer. It needs the target to have no
 * extension (with one, the truncated key needs 9+1+3 = 13 characters and the
 * bound catches it first), which is exactly the case a directory hits.
 *
 * So this is the narrow residual of a gap that was mostly already closed -
 * worth closing properly because the failure mode is a wrong answer rather
 * than an error, and because tools/fatfs.py REFUSES to write what this side
 * quietly accepted. Two halves of one format disagreeing about what is
 * representable is the shape that produces a wrong answer instead of a
 * failure.
 *
 * A name that does not fit cannot be ON the volume, so -1 becomes
 * FAT_ERR_NOTFOUND at the caller. That is the honest answer: not "this is
 * malformed", just "no such file", which is true. */
static int fat_name_to_entry(const char *name, char *out11) {
    int i, n = 0;

    for (i = 0; i < 11; i++) {
        out11[i] = ' ';
    }
    for (i = 0; name[i] != '\0' && name[i] != '.'; i++) {
        if (n >= 8) {
            return -1;                  /* stem longer than 8 */
        }
        out11[n++] = fat_upper(name[i]);
    }
    if (name[i] == '.') {
        i++;
        for (n = 8; name[i] != '\0'; i++) {
            /* A second dot cannot appear in a short name: the extension is
             * three characters of name, and a dot is not one of them. Caught
             * by the length bound below, but only by accident - a name like
             * "a.b.c" would otherwise build the key "A       B.C". */
            if (n >= 11 || name[i] == '.') {
                return -1;
            }
            out11[n++] = fat_upper(name[i]);
        }
    }
    return 0;
}

/* Hand every real entry of one directory sector to `visit`.
 *
 * Returns 0 to keep going, 1 if `visit` asked to stop, and 2 for the
 * never-used marker that ends a directory. Three outcomes rather than two
 * because the caller has two different loops to break out of. */
static int fat_scan_sector(fat_volume_t *vol, const uint8 *sector,
                           int (*visit)(const uint8 *entry, void *ctx),
                           void *ctx) {
    uint32 e;

    for (e = 0; e < vol->bytes_per_sector / DIR_ENTRY_SIZE; e++) {
        const uint8 *entry = sector + e * DIR_ENTRY_SIZE;

        /* 0x00 means this slot has never been used, and so has no slot after
         * it. Stopping here rather than scanning the whole table is the
         * difference between listing a directory and listing its capacity. */
        if (entry[0] == 0x00) {
            return 2;
        }
        if (entry[0] == 0xE5) {
            continue;   /* deleted */
        }
        if ((entry[11] & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
            continue;   /* long-filename fragment, not a file */
        }
        if (entry[11] & FAT_ATTR_VOLUME_ID) {
            continue;   /* the volume label wears a directory entry */
        }
        if (visit(entry, ctx)) {
            return 1;
        }
    }
    return 0;
}

/* Walk any directory, handing each real entry to `visit`.
 *
 * dir_cluster == 0 means the root, which on FAT16 is not a cluster chain at
 * all but a fixed-size array at a computed LBA. That asymmetry is the whole
 * reason this function has two loops in it: subdirectories are ordinary files
 * whose contents happen to be directory entries, and are followed through the
 * FAT like any other file. FAT32 removes the special case by making the root
 * a chain like everything else - which is worth remembering as the thing that
 * gets simpler, not harder, if this is ever ported.
 *
 * Returns FAT_OK, or 1 if `visit` stopped the walk early. */
static int fat_walk_dir(fat_volume_t *vol, uint32 dir_cluster,
                        int (*visit)(const uint8 *entry, void *ctx),
                        void *ctx) {
    uint8 *sector;
    uint32 s;
    int rc, scan;

    if (!vol->mounted) {
        return FAT_ERR_NOTFAT;
    }

    sector = (uint8 *)kmalloc(ATA_SECTOR_SIZE);
    if (sector == NULL) {
        return FAT_ERR_NOMEM;
    }

    /* --- a subdirectory: follow the chain --- */
    if (dir_cluster != 0) {
        uint32 cluster = dir_cluster;

        for (;;) {
            if (cluster < CLUSTER_FIRST || cluster >= CLUSTER_BAD ||
                cluster >= vol->total_clusters + CLUSTER_FIRST) {
                kfree(sector);
                return FAT_ERR_CORRUPT;
            }

            for (s = 0; s < vol->sectors_per_cluster; s++) {
                rc = fat_sectors(vol, fat_cluster_lba(vol, cluster) + s, 1,
                                 sector);
                if (rc != FAT_OK) {
                    kfree(sector);
                    return FAT_ERR_IO;
                }
                scan = fat_scan_sector(vol, sector, visit, ctx);
                if (scan != 0) {
                    kfree(sector);
                    return scan == 1 ? 1 : FAT_OK;
                }
            }

            rc = fat_next_cluster(vol, cluster, &cluster);
            if (rc != FAT_OK) {
                kfree(sector);
                return rc;
            }
            if (cluster >= CLUSTER_EOC) {
                kfree(sector);
                return FAT_OK;
            }
        }
    }

    /* --- the root: a flat array --- */
    for (s = 0; s < vol->root_sectors; s++) {
        rc = fat_sectors(vol, vol->root_start + s, 1, sector);
        if (rc != FAT_OK) {
            kfree(sector);
            return FAT_ERR_IO;
        }
        scan = fat_scan_sector(vol, sector, visit, ctx);
        if (scan != 0) {
            kfree(sector);
            return scan == 1 ? 1 : FAT_OK;
        }
    }

    kfree(sector);
    return FAT_OK;
}

/* --- listing ------------------------------------------------------------ */

struct list_ctx { fat_dir_cb cb; };

static int list_visit(const uint8 *entry, void *ctx) {
    struct list_ctx *lc = (struct list_ctx *)ctx;
    char name[13];

    fat_name_from_entry(entry, name);
    lc->cb(name, rd32(entry + 28), entry[11]);
    return 0;
}

/* --- finding one name in one directory ---------------------------------- */

struct find_ctx {
    const char *want11;
    fat_entry_t *out;
    int found;
};

static int find_visit(const uint8 *entry, void *ctx) {
    struct find_ctx *fc = (struct find_ctx *)ctx;
    int i;

    for (i = 0; i < 11; i++) {
        if ((char)entry[i] != fc->want11[i]) {
            return 0;
        }
    }

    fat_name_from_entry(entry, fc->out->name);
    /* Cluster number is split: the high half at offset 20 is always zero on
     * FAT16 and only meaningful on FAT32. Reading only the low half here is
     * correct, and is exactly what breaks if this is ever pointed at FAT32. */
    fc->out->cluster = (uint32)rd16(entry + 26);
    fc->out->size    = rd32(entry + 28);
    fc->out->attr    = entry[11];
    fc->found = 1;
    return 1;
}

static int fat_find_in_dir(fat_volume_t *vol, uint32 dir_cluster,
                           const char *name, fat_entry_t *out) {
    char want[11];
    struct find_ctx fc;
    int rc;

    /* A name that cannot be represented in 8.3 is not on the volume, so this
     * is -ENOENT rather than a scan for a truncated key that might match some
     * other file. */
    if (fat_name_to_entry(name, want) != 0) {
        return FAT_ERR_NOTFOUND;
    }
    fc.want11 = want;
    fc.out    = out;
    fc.found  = 0;

    rc = fat_walk_dir(vol, dir_cluster, find_visit, &fc);
    if (rc < 0) {
        return rc;
    }
    if (fc.found) {
        /* Set HERE rather than in find_visit, because this is the only level
         * that knows which directory was searched - find_visit is handed a
         * raw 32-byte record and nothing else. One assignment covers every
         * entry fat_lookup resolves, which is every entry a write can reach. */
        out->dir_cluster = dir_cluster;
    }
    return fc.found ? FAT_OK : FAT_ERR_NOTFOUND;
}

/* --- path resolution ----------------------------------------------------
 * Absolute paths only, split on '/', one component at a time, starting at the
 * root. Repeated and trailing slashes are skipped rather than rejected, so
 * "/bin/", "//bin" and "/bin" all name the same directory - which matters
 * because a PATH search concatenates a directory and a name without caring
 * whether the directory already ended in a slash.
 *
 * Every component except the last must be a directory. Reporting that as
 * -ENOTDIR rather than -ENOENT is worth the extra error code: "/bin/sh/x"
 * where sh is a file is a different mistake from a name that is simply
 * absent, and a shell reports them differently.
 *
 * No "." or ".." handling beyond what the on-disk entries give for free - the
 * entries genuinely exist in every subdirectory, so ".." resolves by lookup
 * rather than by string manipulation. It does NOT work in the root, which has
 * no such entries. Relative paths and cwd belong with the process object, not
 * here. */
int fat_lookup(fat_volume_t *vol, const char *path, fat_entry_t *out) {
    fat_entry_t cur;
    uint32 i = 0;

    if (!vol->mounted) {
        return FAT_ERR_NOTFAT;
    }
    if (path == NULL || path[0] != '/') {
        return FAT_ERR_BADPATH;
    }

    /* The root itself: cluster 0 is the sentinel fat_walk_dir understands. */
    cur.name[0] = '/';
    cur.name[1] = '\0';
    cur.cluster = 0;
    cur.size    = 0;
    cur.attr    = FAT_ATTR_DIRECTORY;
    /* The root has no directory record anywhere - it IS the directory. Zero
     * is the same sentinel fat_walk_dir uses, and a write to it is refused by
     * the FAT_ATTR_DIRECTORY check long before this would be consulted. */
    cur.dir_cluster = 0;

    for (;;) {
        char component[13];
        uint32 n = 0;

        while (path[i] == '/') {
            i++;
        }
        if (path[i] == '\0') {
            break;
        }

        while (path[i] != '\0' && path[i] != '/') {
            /* 8.3 plus the dot is 12 characters. Anything longer cannot name
             * a file on this volume, and truncating would silently open the
             * wrong one. */
            if (n >= sizeof(component) - 1) {
                return FAT_ERR_BADPATH;
            }
            component[n++] = path[i++];
        }
        component[n] = '\0';

        if (!(cur.attr & FAT_ATTR_DIRECTORY)) {
            return FAT_ERR_NOTDIR;
        }

        {
            int rc = fat_find_in_dir(vol, cur.cluster, component, &cur);
            if (rc != FAT_OK) {
                return rc;
            }
        }
    }

    *out = cur;
    return FAT_OK;
}

int fat_list(fat_volume_t *vol, const char *path, fat_dir_cb cb) {
    fat_entry_t dir;
    struct list_ctx lc;
    int rc;

    rc = fat_lookup(vol, path, &dir);
    if (rc != FAT_OK) {
        return rc;
    }
    if (!(dir.attr & FAT_ATTR_DIRECTORY)) {
        return FAT_ERR_NOTDIR;
    }

    lc.cb = cb;
    rc = fat_walk_dir(vol, dir.cluster, list_visit, &lc);
    return rc < 0 ? rc : FAT_OK;
}

int fat_list_root(fat_volume_t *vol, fat_dir_cb cb) {
    return fat_list(vol, "/", cb);
}

/* --- reading ------------------------------------------------------------ */

/* Copy a file's contents, given the entry that describes it. Split out of
 * fat_read_path so the chain-following logic has exactly one home. */
static int fat_read_entry(fat_volume_t *vol, const fat_entry_t *ent,
                          void *buffer, uint32 max, uint32 *out_size) {
    uint8 *out = (uint8 *)buffer;
    uint8 *cluster_buf;
    uint32 cluster, cluster_bytes, copied;
    int rc;

    if (out_size != NULL) {
        *out_size = ent->size;
    }
    if (ent->attr & FAT_ATTR_DIRECTORY) {
        return FAT_ERR_ISDIR;
    }
    if (ent->size > max) {
        return FAT_ERR_TOOBIG;
    }
    /* An empty file has no chain, and its cluster field is 0 - the same value
     * that means "the root directory" elsewhere. Returning before following
     * anything keeps those two meanings from ever meeting. */
    if (ent->size == 0) {
        return FAT_OK;
    }

    cluster_bytes = vol->sectors_per_cluster * vol->bytes_per_sector;
    cluster_buf = (uint8 *)kmalloc(cluster_bytes);
    if (cluster_buf == NULL) {
        return FAT_ERR_NOMEM;
    }

    cluster = ent->cluster;
    copied  = 0;

    while (copied < ent->size) {
        uint32 chunk, i;

        /* A chain that leaves the volume means a corrupt FAT. Checking is
         * cheap and the alternative is reading arbitrary sectors, or looping
         * forever on a chain that points at itself. */
        if (cluster < CLUSTER_FIRST ||
            cluster >= CLUSTER_BAD ||
            cluster >= vol->total_clusters + CLUSTER_FIRST) {
            kfree(cluster_buf);
            return FAT_ERR_CORRUPT;
        }

        rc = fat_sectors(vol, fat_cluster_lba(vol, cluster),
                         vol->sectors_per_cluster, cluster_buf);
        if (rc != FAT_OK) {
            kfree(cluster_buf);
            return FAT_ERR_IO;
        }

        chunk = ent->size - copied;
        if (chunk > cluster_bytes) {
            chunk = cluster_bytes;
        }
        for (i = 0; i < chunk; i++) {
            out[copied + i] = cluster_buf[i];
        }
        copied += chunk;

        if (copied >= ent->size) {
            break;
        }

        rc = fat_next_cluster(vol, cluster, &cluster);
        if (rc != FAT_OK) {
            kfree(cluster_buf);
            return rc;
        }
    }

    kfree(cluster_buf);
    return FAT_OK;
}

/* Read `n` bytes starting at `offset` within a file.
 *
 * read(2) needs this and fat_read_entry cannot provide it: a cluster chain is
 * a singly linked list, so reaching byte N means walking from the start every
 * time. That is O(offset) per call and it is the reason real drivers cache
 * the last cluster visited. Correct first; a two-field cache on the open
 * instance is the obvious next move if sequential reads feel slow.
 *
 * Returns FAT_OK with *out set to the bytes actually copied, which is short
 * at end of file and zero past it. */
int fat_read_entry_at(fat_volume_t *vol, const fat_entry_t *ent, uint64 offset,
                      void *buffer, uint32 max, uint32 *out) {
    uint8 *dst = (uint8 *)buffer;
    uint8 *cluster_buf;
    uint32 cluster_bytes, copied = 0;
    uint32 cluster, skip;
    int rc;

    if (out != NULL) {
        *out = 0;
    }
    if (ent->attr & FAT_ATTR_DIRECTORY) {
        return FAT_ERR_ISDIR;
    }
    if (offset >= ent->size || max == 0 || ent->size == 0) {
        return FAT_OK;              /* at or past EOF: zero bytes, not an error */
    }
    if (max > ent->size - offset) {
        max = (uint32)(ent->size - offset);
    }

    cluster_bytes = vol->sectors_per_cluster * vol->bytes_per_sector;
    cluster_buf = (uint8 *)kmalloc(cluster_bytes);
    if (cluster_buf == NULL) {
        return FAT_ERR_NOMEM;
    }

    cluster = ent->cluster;
    skip    = (uint32)(offset / cluster_bytes);
    while (skip-- > 0) {
        if (cluster < CLUSTER_FIRST || cluster >= CLUSTER_BAD) {
            kfree(cluster_buf);
            return FAT_ERR_CORRUPT;
        }
        rc = fat_next_cluster(vol, cluster, &cluster);
        if (rc != FAT_OK || cluster >= CLUSTER_EOC) {
            kfree(cluster_buf);
            return rc == FAT_OK ? FAT_ERR_CORRUPT : rc;
        }
    }

    {
        uint32 within = (uint32)(offset % cluster_bytes);

        while (copied < max) {
            uint32 chunk, i;

            if (cluster < CLUSTER_FIRST || cluster >= CLUSTER_BAD ||
                cluster >= vol->total_clusters + CLUSTER_FIRST) {
                kfree(cluster_buf);
                return FAT_ERR_CORRUPT;
            }
            rc = fat_sectors(vol, fat_cluster_lba(vol, cluster),
                             vol->sectors_per_cluster, cluster_buf);
            if (rc != FAT_OK) {
                kfree(cluster_buf);
                return FAT_ERR_IO;
            }

            chunk = cluster_bytes - within;
            if (chunk > max - copied) {
                chunk = max - copied;
            }
            for (i = 0; i < chunk; i++) {
                dst[copied + i] = cluster_buf[within + i];
            }
            copied += chunk;
            within  = 0;

            if (copied >= max) {
                break;
            }
            rc = fat_next_cluster(vol, cluster, &cluster);
            if (rc != FAT_OK) {
                kfree(cluster_buf);
                return rc;
            }
            if (cluster >= CLUSTER_EOC) {
                break;              /* chain ended early: short read */
            }
        }
    }

    kfree(cluster_buf);
    if (out != NULL) {
        *out = copied;
    }
    return FAT_OK;
}

/* --- iteration with a context ------------------------------------------- */

struct iter_ctx {
    fat_entry_cb cb;
    void        *user;
    uint32       dir_cluster;
};

static int iter_visit(const uint8 *entry, void *ctx) {
    struct iter_ctx *ic = (struct iter_ctx *)ctx;
    fat_entry_t e;

    fat_name_from_entry(entry, e.name);
    e.cluster = (uint32)rd16(entry + 26);
    e.size    = rd32(entry + 28);
    e.attr    = entry[11];
    /* Filled from the walk's own directory, so an entry handed to a caller by
     * iteration is as writable as one from a lookup. Nothing writes through
     * one today; leaving it uninitialised would be a field that is correct
     * only depending on how the entry was obtained, which is the kind of
     * distinction nobody remembers at the call site. */
    e.dir_cluster = ic->dir_cluster;
    return ic->cb(&e, ic->user);
}

int fat_iterate_dir(fat_volume_t *vol, const fat_entry_t *dir,
                    fat_entry_cb cb, void *user) {
    struct iter_ctx ic;
    int rc;

    if (!(dir->attr & FAT_ATTR_DIRECTORY)) {
        return FAT_ERR_NOTDIR;
    }
    ic.cb          = cb;
    ic.user        = user;
    ic.dir_cluster = dir->cluster;
    rc = fat_walk_dir(vol, dir->cluster, iter_visit, &ic);
    return rc < 0 ? rc : FAT_OK;
}

int fat_iterate(fat_volume_t *vol, const char *path, fat_entry_cb cb, void *user) {
    fat_entry_t dir;
    struct iter_ctx ic;
    int rc;

    rc = fat_lookup(vol, path, &dir);
    if (rc != FAT_OK) {
        return rc;
    }
    if (!(dir.attr & FAT_ATTR_DIRECTORY)) {
        return FAT_ERR_NOTDIR;
    }
    ic.cb          = cb;
    ic.user        = user;
    ic.dir_cluster = dir.cluster;
    rc = fat_walk_dir(vol, dir.cluster, iter_visit, &ic);
    return rc < 0 ? rc : FAT_OK;
}

int fat_read_path(fat_volume_t *vol, const char *path,
                  void *buffer, uint32 max, uint32 *out_size) {
    fat_entry_t ent;
    int rc;

    rc = fat_lookup(vol, path, &ent);
    if (rc != FAT_OK) {
        return rc;
    }
    return fat_read_entry(vol, &ent, buffer, max, out_size);
}

/* Root-relative by bare name, the original interface. Kept because "size the
 * buffer, then fill it" callers exist; new code should use fat_read_path. */
int fat_read_file(fat_volume_t *vol, const char *name,
                  void *buffer, uint32 max, uint32 *out_size) {
    fat_entry_t ent;
    int rc;

    rc = fat_find_in_dir(vol, 0, name, &ent);
    if (rc != FAT_OK) {
        return rc;
    }
    return fat_read_entry(vol, &ent, buffer, max, out_size);
}

/* =========================================================================
 * MUTATION
 * =========================================================================
 *
 * Everything above reads. From here down, a bug damages a volume rather than
 * misreporting one, so the shape of every operation is the same: validate
 * completely, then write - and write the FAT copies before the directory
 * entry that refers to them, so a failure between the two leaks a cluster
 * rather than leaving a directory entry pointing at a chain that is not
 * marked allocated. A leaked cluster is invisible until fsck; a dangling
 * chain is a file that eats another file.
 */

static void wr16(uint8 *p, uint16 v) {
    p[0] = (uint8)(v & 0xFF);
    p[1] = (uint8)(v >> 8);
}

static void wr32(uint8 *p, uint32 v) {
    p[0] = (uint8)(v & 0xFF);
    p[1] = (uint8)((v >> 8) & 0xFF);
    p[2] = (uint8)((v >> 16) & 0xFF);
    p[3] = (uint8)((v >> 24) & 0xFF);
}

/* The mirror of fat_sectors. Short is an error here for the same reason. */
static int fat_write_sectors(fat_volume_t *vol, uint32 lba, uint32 count,
                             const void *buf) {
    uint64 want = (uint64)count * ATA_SECTOR_SIZE;
    int64  got  = dev_write(vol->dev, (uint64)lba * ATA_SECTOR_SIZE, buf, want);

    if (got < 0) {
        return FAT_ERR_IO;
    }
    return ((uint64)got == want) ? FAT_OK : FAT_ERR_IO;
}

/* Write one FAT entry, into EVERY copy of the FAT.
 *
 * num_fats is almost always 2. Updating only the first leaves a volume that
 * this driver reads back correctly and that fsck, mtools and every other
 * implementation call corrupt - the worst kind of wrong, because the machine
 * that made the mess is the one that cannot see it. */
static int fat_set_entry(fat_volume_t *vol, uint32 cluster, uint16 value) {
    uint8 *sector;
    uint32 offset, index, copy;
    int rc = FAT_OK;

    if (cluster < CLUSTER_FIRST ||
        cluster >= vol->total_clusters + CLUSTER_FIRST) {
        return FAT_ERR_CORRUPT;
    }

    sector = (uint8 *)kmalloc(ATA_SECTOR_SIZE);
    if (sector == NULL) {
        return FAT_ERR_NOMEM;
    }
    offset = cluster * 2u;
    index  = offset % vol->bytes_per_sector;

    for (copy = 0; copy < vol->num_fats; copy++) {
        uint32 lba = vol->fat_start + copy * vol->fat_sectors
                   + (offset / vol->bytes_per_sector);

        /* Read-modify-write: a FAT sector holds 256 entries and the other 255
         * belong to other files. */
        rc = fat_sectors(vol, lba, 1, sector);
        if (rc != FAT_OK) {
            break;
        }
        wr16(sector + index, value);
        rc = fat_write_sectors(vol, lba, 1, sector);
        if (rc != FAT_OK) {
            break;
        }
    }
    kfree(sector);
    return rc;
}

/* Find a free cluster and claim it as a one-cluster chain.
 *
 * Linear from the start every time, with no "next free" hint. That is O(n)
 * per allocation and it is the right trade here: a hint has to be persisted
 * to be worth anything (FAT32 has a field for it, FAT16 does not), and an
 * unpersisted one is just a cache that is wrong after every reboot. */
static int fat_alloc_cluster(fat_volume_t *vol, uint32 *out) {
    uint8 *sector;
    uint32 cluster;
    int rc;

    sector = (uint8 *)kmalloc(ATA_SECTOR_SIZE);
    if (sector == NULL) {
        return FAT_ERR_NOMEM;
    }

    for (cluster = CLUSTER_FIRST;
         cluster < vol->total_clusters + CLUSTER_FIRST; cluster++) {
        uint32 offset = cluster * 2u;
        uint32 lba    = vol->fat_start + (offset / vol->bytes_per_sector);

        rc = fat_sectors(vol, lba, 1, sector);
        if (rc != FAT_OK) {
            kfree(sector);
            return FAT_ERR_IO;
        }
        if (rd16(sector + (offset % vol->bytes_per_sector)) != 0) {
            continue;
        }
        kfree(sector);
        /* Marked as the END of a chain, not merely non-zero: an allocated
         * cluster whose FAT entry stayed 0 is one the next allocation hands
         * out again. */
        rc = fat_set_entry(vol, cluster, 0xFFFF);
        if (rc != FAT_OK) {
            return rc;
        }
        *out = cluster;
        return FAT_OK;
    }
    kfree(sector);
    return FAT_ERR_FULL;
}

/* Free a whole chain. Bounded by the cluster count, because a corrupt volume
 * can contain a loop and this must not become one. */
static int fat_free_chain(fat_volume_t *vol, uint32 first) {
    uint32 cluster = first;
    uint32 guard = 0;

    while (cluster >= CLUSTER_FIRST && cluster < CLUSTER_BAD &&
           cluster < vol->total_clusters + CLUSTER_FIRST) {
        uint32 next;
        int rc;

        if (guard++ > vol->total_clusters) {
            return FAT_ERR_CORRUPT;
        }
        rc = fat_next_cluster(vol, cluster, &next);
        if (rc != FAT_OK) {
            return rc;
        }
        /* The link is read BEFORE the entry is cleared, or the rest of the
         * chain is unreachable and leaks. */
        rc = fat_set_entry(vol, cluster, 0);
        if (rc != FAT_OK) {
            return rc;
        }
        cluster = next;
    }
    return FAT_OK;
}

/* Fill a cluster with zeroes. A new directory's cluster MUST be zeroed: a
 * directory ends at the first 0x00 entry, and an unzeroed cluster ends
 * wherever the previous occupant's data happened to have one. */
static int fat_zero_cluster(fat_volume_t *vol, uint32 cluster) {
    uint8 *sector;
    uint32 s;
    int rc = FAT_OK;

    sector = (uint8 *)kmalloc(ATA_SECTOR_SIZE);
    if (sector == NULL) {
        return FAT_ERR_NOMEM;
    }
    for (s = 0; s < ATA_SECTOR_SIZE; s++) {
        sector[s] = 0;
    }
    for (s = 0; s < vol->sectors_per_cluster; s++) {
        rc = fat_write_sectors(vol, fat_cluster_lba(vol, cluster) + s, 1,
                               sector);
        if (rc != FAT_OK) {
            break;
        }
    }
    kfree(sector);
    return rc;
}

/* --- locating a directory entry ON DISK ---------------------------------
 *
 * fat_walk_dir hands out a const pointer into a sector buffer, which is
 * everything a reader needs and nothing a writer does: to delete or rename
 * an entry you have to know which sector it came from and where in it.
 *
 * Rather than change that walker's callback signature and every visitor with
 * it, this is a separate scan that reports POSITION. The duplication is
 * deliberate and bounded - it is the same two loops, and keeping the read
 * path untouched means a bug here cannot break reading. */
struct dir_pos {
    uint32 lba;
    uint32 offset;     /* byte offset within the sector */
};

/* `want11` is the 11-byte packed name to find, or NULL to find a FREE slot.
 * Returns FAT_OK and fills *pos, or FAT_ERR_NOTFOUND. */
static int fat_scan_dir_raw(fat_volume_t *vol, uint32 dir_cluster,
                            const char *want11, struct dir_pos *pos,
                            uint32 *out_cluster_last) {
    uint8 *sector;
    uint32 cluster = dir_cluster;
    uint32 s, e;
    int rc;

    sector = (uint8 *)kmalloc(ATA_SECTOR_SIZE);
    if (sector == NULL) {
        return FAT_ERR_NOMEM;
    }

    for (;;) {
        uint32 first_lba, nsectors;

        if (dir_cluster == 0) {
            first_lba = vol->root_start;
            nsectors  = vol->root_sectors;
        } else {
            if (cluster < CLUSTER_FIRST || cluster >= CLUSTER_BAD ||
                cluster >= vol->total_clusters + CLUSTER_FIRST) {
                kfree(sector);
                return FAT_ERR_CORRUPT;
            }
            first_lba = fat_cluster_lba(vol, cluster);
            nsectors  = vol->sectors_per_cluster;
            if (out_cluster_last != NULL) {
                *out_cluster_last = cluster;
            }
        }

        for (s = 0; s < nsectors; s++) {
            rc = fat_sectors(vol, first_lba + s, 1, sector);
            if (rc != FAT_OK) {
                kfree(sector);
                return FAT_ERR_IO;
            }
            for (e = 0; e < vol->bytes_per_sector / DIR_ENTRY_SIZE; e++) {
                const uint8 *entry = sector + e * DIR_ENTRY_SIZE;
                int i, match;

                if (want11 == NULL) {
                    /* Looking for somewhere to put a new entry. Both the
                     * never-used marker and a deleted one will do. */
                    if (entry[0] == 0x00 || entry[0] == 0xE5) {
                        pos->lba    = first_lba + s;
                        pos->offset = e * DIR_ENTRY_SIZE;
                        kfree(sector);
                        return FAT_OK;
                    }
                    continue;
                }

                if (entry[0] == 0x00) {
                    kfree(sector);
                    return FAT_ERR_NOTFOUND;   /* no entry past this one */
                }
                if (entry[0] == 0xE5 ||
                    (entry[11] & FAT_ATTR_LFN) == FAT_ATTR_LFN ||
                    (entry[11] & FAT_ATTR_VOLUME_ID)) {
                    continue;
                }
                match = 1;
                for (i = 0; i < 11; i++) {
                    if ((char)entry[i] != want11[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    pos->lba    = first_lba + s;
                    pos->offset = e * DIR_ENTRY_SIZE;
                    kfree(sector);
                    return FAT_OK;
                }
            }
        }

        if (dir_cluster == 0) {
            kfree(sector);
            return FAT_ERR_NOTFOUND;    /* the root does not grow */
        }
        rc = fat_next_cluster(vol, cluster, &cluster);
        if (rc != FAT_OK) {
            kfree(sector);
            return rc;
        }
        if (cluster >= CLUSTER_EOC) {
            kfree(sector);
            return FAT_ERR_NOTFOUND;
        }
    }
}

/* Read, modify and write back the 32 bytes at `pos`. Every mutation of an
 * existing entry goes through this, so the read-modify-write of the other
 * 15 entries in the sector is written once. */
static int fat_edit_entry(fat_volume_t *vol, const struct dir_pos *pos,
                          void (*edit)(uint8 *entry, void *ctx), void *ctx) {
    uint8 *sector;
    int rc;

    sector = (uint8 *)kmalloc(ATA_SECTOR_SIZE);
    if (sector == NULL) {
        return FAT_ERR_NOMEM;
    }
    rc = fat_sectors(vol, pos->lba, 1, sector);
    if (rc != FAT_OK) {
        kfree(sector);
        return FAT_ERR_IO;
    }
    edit(sector + pos->offset, ctx);
    rc = fat_write_sectors(vol, pos->lba, 1, sector);
    kfree(sector);
    return rc;
}

struct entry_init {
    const char *name11;
    uint8       attr;
    uint32      cluster;
    uint32      size;
};

static void edit_write_entry(uint8 *entry, void *ctx) {
    const struct entry_init *ei = (const struct entry_init *)ctx;
    int i;

    for (i = 0; i < DIR_ENTRY_SIZE; i++) {
        entry[i] = 0;
    }
    for (i = 0; i < 11; i++) {
        entry[i] = (uint8)ei->name11[i];
    }
    entry[11] = ei->attr;
    wr16(entry + 26, (uint16)ei->cluster);   /* first cluster low  */
    wr16(entry + 20, 0);                     /* first cluster high - FAT16 */
    wr32(entry + 28, ei->size);
}

static void edit_delete_entry(uint8 *entry, void *ctx) {
    (void)ctx;
    /* 0xE5 rather than 0x00. Zero means "never used" and ends the directory
     * scan, so zeroing an entry in the middle hides every entry after it -
     * the files are still there and become invisible, which is worse than
     * losing them because backups keep the invisible version. */
    entry[0] = 0xE5;
}

static void edit_rename_entry(uint8 *entry, void *ctx) {
    const char *name11 = (const char *)ctx;
    int i;

    for (i = 0; i < 11; i++) {
        entry[i] = (uint8)name11[i];
    }
}

/* Split "/a/b/c" into the parent path "/a/b" and the final component "c".
 * The parent buffer is the caller's. */
static int fat_split_path(const char *path, char *parent, uint32 parent_max,
                          char *leaf, uint32 leaf_max) {
    uint32 len = 0, cut = 0, i;

    if (path == NULL || path[0] != '/') {
        return FAT_ERR_BADPATH;
    }
    while (path[len] != '\0') {
        if (path[len] == '/') {
            cut = len;
        }
        len++;
    }
    if (len == 1) {
        return FAT_ERR_INVAL;           /* "/" has no parent */
    }
    if (len - cut - 1 == 0 || len - cut > leaf_max) {
        return FAT_ERR_BADPATH;         /* trailing slash, or too long */
    }
    if (cut + 1 > parent_max) {
        return FAT_ERR_BADPATH;
    }

    for (i = 0; i < cut; i++) {
        parent[i] = path[i];
    }
    /* The parent of "/x" is "/", not "" - and an empty string is not an
     * absolute path, so fat_lookup would reject it. */
    if (cut == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        parent[cut] = '\0';
    }
    for (i = 0; i < len - cut - 1; i++) {
        leaf[i] = path[cut + 1 + i];
    }
    leaf[len - cut - 1] = '\0';
    return FAT_OK;
}

/* The first cluster of the directory `path` names, with 0 meaning the root.
 * Refuses anything that is not a directory. */
static int fat_dir_cluster_of(fat_volume_t *vol, const char *path,
                              uint32 *out) {
    fat_entry_t ent;
    int rc;

    if (path[0] == '/' && path[1] == '\0') {
        *out = 0;
        return FAT_OK;
    }
    rc = fat_lookup(vol, path, &ent);
    if (rc != FAT_OK) {
        return rc;
    }
    if (!(ent.attr & FAT_ATTR_DIRECTORY)) {
        return FAT_ERR_NOTDIR;
    }
    *out = ent.cluster;
    return FAT_OK;
}

/* Is this directory empty? "." and ".." do not count. */
struct empty_ctx { int empty; };

static int empty_visit(const uint8 *entry, void *ctx) {
    struct empty_ctx *ec = (struct empty_ctx *)ctx;
    char name[13];

    fat_name_from_entry(entry, name);
    if (name[0] == '.' && (name[1] == '\0' ||
                           (name[1] == '.' && name[2] == '\0'))) {
        return 0;
    }
    ec->empty = 0;
    return 1;                            /* one is enough - stop */
}

int fat_mkdir(fat_volume_t *vol, const char *path) {
    char parent[FAT_PATH_MAX];
    char leaf[16];
    char name11[11];
    uint32 parent_cluster, new_cluster;
    struct dir_pos pos;
    struct entry_init ei;
    int rc;

    if (!vol->mounted) {
        return FAT_ERR_NOTFAT;
    }
    rc = fat_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (rc != FAT_OK) {
        return rc;
    }
    if (fat_name_to_entry(leaf, name11) != 0) {
        return FAT_ERR_BADPATH;
    }
    rc = fat_dir_cluster_of(vol, parent, &parent_cluster);
    if (rc != FAT_OK) {
        return rc;
    }

    /* Existence check BEFORE anything is allocated, so a refused mkdir
     * leaves no trace. */
    rc = fat_scan_dir_raw(vol, parent_cluster, name11, &pos, NULL);
    if (rc == FAT_OK) {
        return FAT_ERR_EXISTS;
    }
    if (rc != FAT_ERR_NOTFOUND) {
        return rc;
    }
    /* And a free slot BEFORE allocating a cluster, so a full directory does
     * not leak one. */
    rc = fat_scan_dir_raw(vol, parent_cluster, NULL, &pos, NULL);
    if (rc != FAT_OK) {
        return rc == FAT_ERR_NOTFOUND ? FAT_ERR_FULL : rc;
    }

    rc = fat_alloc_cluster(vol, &new_cluster);
    if (rc != FAT_OK) {
        return rc;
    }
    rc = fat_zero_cluster(vol, new_cluster);
    if (rc != FAT_OK) {
        fat_free_chain(vol, new_cluster);
        return rc;
    }

    /* "." and ".." first, then the entry in the parent. This order is what
     * makes a failure safe: a directory that exists but nothing points at is
     * a leaked cluster, where a parent entry pointing at a directory with no
     * "." is a structure every tool will refuse to walk.
     *
     * ".." of a directory whose parent is the ROOT is written as cluster 0,
     * which is the FAT convention for "the root" and not an error. */
    {
        uint8 *sector = (uint8 *)kmalloc(ATA_SECTOR_SIZE);
        uint32 i;

        if (sector == NULL) {
            fat_free_chain(vol, new_cluster);
            return FAT_ERR_NOMEM;
        }
        for (i = 0; i < ATA_SECTOR_SIZE; i++) {
            sector[i] = 0;
        }
        {
            struct entry_init dot = { ".          ", FAT_ATTR_DIRECTORY,
                                      new_cluster, 0 };
            struct entry_init dotdot = { "..         ", FAT_ATTR_DIRECTORY,
                                         parent_cluster, 0 };
            edit_write_entry(sector, &dot);
            edit_write_entry(sector + DIR_ENTRY_SIZE, &dotdot);
        }
        rc = fat_write_sectors(vol, fat_cluster_lba(vol, new_cluster), 1,
                               sector);
        kfree(sector);
        if (rc != FAT_OK) {
            fat_free_chain(vol, new_cluster);
            return rc;
        }
    }

    ei.name11  = name11;
    ei.attr    = FAT_ATTR_DIRECTORY;
    ei.cluster = new_cluster;
    ei.size    = 0;                      /* a directory's size is always 0 */
    rc = fat_edit_entry(vol, &pos, edit_write_entry, &ei);
    if (rc != FAT_OK) {
        fat_free_chain(vol, new_cluster);
        return rc;
    }
    return FAT_OK;
}

int fat_unlink(fat_volume_t *vol, const char *path) {
    char parent[FAT_PATH_MAX];
    char leaf[16];
    char name11[11];
    uint32 parent_cluster;
    struct dir_pos pos;
    fat_entry_t ent;
    int rc;

    if (!vol->mounted) {
        return FAT_ERR_NOTFAT;
    }
    rc = fat_lookup(vol, path, &ent);
    if (rc != FAT_OK) {
        return rc;
    }
    if (ent.attr & FAT_ATTR_DIRECTORY) {
        return FAT_ERR_ISDIR;
    }

    rc = fat_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (rc != FAT_OK) {
        return rc;
    }
    if (fat_name_to_entry(leaf, name11) != 0) {
        return FAT_ERR_BADPATH;
    }
    rc = fat_dir_cluster_of(vol, parent, &parent_cluster);
    if (rc != FAT_OK) {
        return rc;
    }
    rc = fat_scan_dir_raw(vol, parent_cluster, name11, &pos, NULL);
    if (rc != FAT_OK) {
        return rc;
    }

    /* The DIRECTORY ENTRY goes first, then the chain.
     *
     * The opposite order has a window in which the entry names a chain that
     * is already free - so a concurrent allocation hands those clusters to
     * another file while this one still points at them, and two files share
     * blocks. Doing it this way leaks the chain if the second step fails,
     * which fsck reclaims. Losing space is recoverable; sharing it is not. */
    rc = fat_edit_entry(vol, &pos, edit_delete_entry, NULL);
    if (rc != FAT_OK) {
        return rc;
    }
    if (ent.cluster != 0) {
        return fat_free_chain(vol, ent.cluster);
    }
    return FAT_OK;
}

int fat_rmdir(fat_volume_t *vol, const char *path) {
    char parent[FAT_PATH_MAX];
    char leaf[16];
    char name11[11];
    uint32 parent_cluster;
    struct dir_pos pos;
    fat_entry_t ent;
    struct empty_ctx ec;
    int rc;

    if (!vol->mounted) {
        return FAT_ERR_NOTFAT;
    }
    rc = fat_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (rc != FAT_OK) {
        return rc;
    }
    if (leaf[0] == '.' && (leaf[1] == '\0' ||
                           (leaf[1] == '.' && leaf[2] == '\0'))) {
        return FAT_ERR_INVAL;
    }
    rc = fat_lookup(vol, path, &ent);
    if (rc != FAT_OK) {
        return rc;
    }
    if (!(ent.attr & FAT_ATTR_DIRECTORY)) {
        return FAT_ERR_NOTDIR;
    }

    ec.empty = 1;
    rc = fat_walk_dir(vol, ent.cluster, empty_visit, &ec);
    if (rc != FAT_OK && rc != 1) {
        return rc;
    }
    if (!ec.empty) {
        return FAT_ERR_NOTEMPTY;
    }

    if (fat_name_to_entry(leaf, name11) != 0) {
        return FAT_ERR_BADPATH;
    }
    rc = fat_dir_cluster_of(vol, parent, &parent_cluster);
    if (rc != FAT_OK) {
        return rc;
    }
    rc = fat_scan_dir_raw(vol, parent_cluster, name11, &pos, NULL);
    if (rc != FAT_OK) {
        return rc;
    }
    rc = fat_edit_entry(vol, &pos, edit_delete_entry, NULL);
    if (rc != FAT_OK) {
        return rc;
    }
    if (ent.cluster != 0) {
        return fat_free_chain(vol, ent.cluster);
    }
    return FAT_OK;
}

int fat_rename(fat_volume_t *vol, const char *oldpath, const char *newpath) {
    char old_parent[FAT_PATH_MAX], new_parent[FAT_PATH_MAX];
    char old_leaf[16], new_leaf[16];
    char old11[11], new11[11];
    uint32 old_dir, new_dir;
    struct dir_pos old_pos, new_pos;
    fat_entry_t ent;
    int rc, same_dir, i;

    if (!vol->mounted) {
        return FAT_ERR_NOTFAT;
    }
    rc = fat_lookup(vol, oldpath, &ent);
    if (rc != FAT_OK) {
        return rc;
    }
    rc = fat_split_path(oldpath, old_parent, sizeof(old_parent),
                        old_leaf, sizeof(old_leaf));
    if (rc != FAT_OK) {
        return rc;
    }
    rc = fat_split_path(newpath, new_parent, sizeof(new_parent),
                        new_leaf, sizeof(new_leaf));
    if (rc != FAT_OK) {
        return rc;
    }
    if (fat_name_to_entry(old_leaf, old11) != 0 ||
        fat_name_to_entry(new_leaf, new11) != 0) {
        return FAT_ERR_BADPATH;
    }
    rc = fat_dir_cluster_of(vol, old_parent, &old_dir);
    if (rc != FAT_OK) {
        return rc;
    }
    rc = fat_dir_cluster_of(vol, new_parent, &new_dir);
    if (rc != FAT_OK) {
        return rc;
    }

    /* Refuse an existing target rather than replacing it - see fat.h. */
    rc = fat_scan_dir_raw(vol, new_dir, new11, &new_pos, NULL);
    if (rc == FAT_OK) {
        return FAT_ERR_EXISTS;
    }
    if (rc != FAT_ERR_NOTFOUND) {
        return rc;
    }

    rc = fat_scan_dir_raw(vol, old_dir, old11, &old_pos, NULL);
    if (rc != FAT_OK) {
        return rc;
    }

    same_dir = (old_dir == new_dir);
    for (i = 0; same_dir && i < 11; i++) {
        /* Same directory AND same packed name means nothing to do. Two
         * spellings of one 8.3 name - "a.txt" and "A.TXT" - pack
         * identically, and rewriting the entry with itself is a write for
         * no reason. */
        if (old11[i] != new11[i]) {
            same_dir = 2;                /* different name, same directory */
        }
    }
    if (same_dir == 1) {
        return FAT_OK;
    }

    if (old_dir == new_dir) {
        /* In place: rewrite the eleven name bytes and nothing else. The
         * cluster, size and attributes are already right, and not touching
         * them means a rename cannot corrupt them. */
        return fat_edit_entry(vol, &old_pos, edit_rename_entry, new11);
    }

    /* Across directories: write the new entry FIRST, then delete the old.
     *
     * A failure between the two leaves the file reachable under BOTH names,
     * which is confusing and recoverable. The other order leaves it
     * reachable under neither, which is data loss. */
    {
        struct entry_init ei;

        rc = fat_scan_dir_raw(vol, new_dir, NULL, &new_pos, NULL);
        if (rc != FAT_OK) {
            return rc == FAT_ERR_NOTFOUND ? FAT_ERR_FULL : rc;
        }
        ei.name11  = new11;
        ei.attr    = ent.attr;
        ei.cluster = ent.cluster;
        ei.size    = ent.size;
        rc = fat_edit_entry(vol, &new_pos, edit_write_entry, &ei);
        if (rc != FAT_OK) {
            return rc;
        }
        return fat_edit_entry(vol, &old_pos, edit_delete_entry, NULL);
    }
}


/* --- writing file CONTENT ------------------------------------------------
 *
 * The last NULL slot in the vtable, and the oldest entry in the ordered
 * TODO's Owed list: the four namespace operations above (mkdir, rmdir,
 * unlink, rename) landed together and left this one, because rewriting a
 * directory record is a different problem from growing a cluster chain.
 *
 * Everything it needs already existed - fat_alloc_cluster, fat_set_entry,
 * fat_zero_cluster, fat_write_sectors, fat_edit_entry - which is why this is
 * one function rather than a layer. What it adds is the SEQUENCE, and the
 * order of the four steps is the whole of the correctness argument:
 *
 *   1. Locate the directory record FIRST, before touching anything. If the
 *      entry cannot be found there is nowhere to record the new size, and a
 *      write that lands on disk with no size update is data the filesystem
 *      cannot see - worse than a write that never happened.
 *   2. Allocate and LINK each new cluster before writing into it, so a
 *      failure part-way leaves a shorter file rather than a chain whose tail
 *      is unreachable.
 *   3. Write the data.
 *   4. Update the record last, with the size actually achieved. A size
 *      written before the data would describe bytes that are not there yet,
 *      and a crash in between would leave a file reporting garbage as
 *      content. This way a crash leaves allocated-but-unreferenced clusters,
 *      which is a leak fsck can find and fix, not corruption a reader
 *      believes.
 */

/* Set both halves of the record a write can change. Deliberately one edit
 * rather than two: the size and the first cluster are the only fields that
 * move, they move together on the first write to an empty file, and
 * fat_edit_entry is a read-modify-write of a whole sector each time. */
struct entry_grow {
    uint32 cluster;
    uint32 size;
};

static void edit_grow_entry(uint8 *entry, void *ctx) {
    const struct entry_grow *g = (const struct entry_grow *)ctx;

    wr16(entry + 26, (uint16)g->cluster);
    wr16(entry + 20, 0);                 /* high half: always 0 on FAT16 */
    wr32(entry + 28, g->size);
}

/* Advance to the next cluster in the chain, ALLOCATING and linking one when
 * the chain ends here. `*cluster` is updated in place. */
static int fat_next_or_grow(fat_volume_t *vol, uint32 *cluster) {
    uint32 next;
    int rc;

    rc = fat_next_cluster(vol, *cluster, &next);
    if (rc != FAT_OK) {
        return rc;
    }
    if (next < CLUSTER_EOC) {
        *cluster = next;
        return FAT_OK;
    }

    rc = fat_alloc_cluster(vol, &next);
    if (rc != FAT_OK) {
        return rc;
    }
    /* Zeroed before it is linked. A cluster that is reachable and full of
     * whatever the last file left there is a file that reads back somebody
     * else's deleted data - which is a disclosure bug, not an untidiness. */
    rc = fat_zero_cluster(vol, next);
    if (rc != FAT_OK) {
        return rc;
    }
    rc = fat_set_entry(vol, *cluster, (uint16)next);
    if (rc != FAT_OK) {
        return rc;
    }
    *cluster = next;
    return FAT_OK;
}

/* A position inside a file's cluster chain, for a write in progress. */
struct fat_wcursor {
    uint32 cluster;   /* the cluster the next byte goes in            */
    uint32 within;    /* byte offset inside that cluster              */
};

/* Write `len` bytes at the cursor, from `src` or - when `src` is NULL - as
 * ZEROES, growing the chain as it goes. Advances the cursor and reports how
 * much actually landed.
 *
 * The NULL-means-zero case is not a convenience. Extending a file past its old
 * end has to make the skipped bytes read as zero, and those bytes are in two
 * different kinds of place: clusters allocated by this write (which
 * fat_next_or_grow has already zeroed) and the TAIL OF THE CLUSTER THAT HELD
 * THE OLD END OF FILE, which was allocated long ago and still holds whatever
 * was there. Zeroing only the new clusters leaves that tail, and it reads back
 * as somebody's deleted data appearing inside a file that never wrote it.
 *
 * Writing the gap through the same path as the data is what makes both cases
 * correct without either of them being a special case - which is the version
 * of this that was written first and was wrong in exactly that spot. */
static int fat_put_bytes(fat_volume_t *vol, struct fat_wcursor *cur,
                         const uint8 *src, uint32 len,
                         uint8 *cbuf, uint32 cbytes, uint32 *done) {
    uint32 put = 0;
    int rc = FAT_OK;

    while (put < len) {
        uint32 chunk = cbytes - cur->within;
        uint32 lba, i;

        if (chunk > len - put) {
            chunk = len - put;
        }
        if (cur->cluster < CLUSTER_FIRST || cur->cluster >= CLUSTER_BAD ||
            cur->cluster >= vol->total_clusters + CLUSTER_FIRST) {
            rc = FAT_ERR_CORRUPT;
            break;
        }
        lba = fat_cluster_lba(vol, cur->cluster);

        /* Read-modify-write, unless this pass replaces the whole cluster.
         * Skipping the read for a full cluster is what makes a large
         * sequential write one I/O per cluster instead of two. */
        if (chunk != cbytes) {
            rc = fat_sectors(vol, lba, vol->sectors_per_cluster, cbuf);
            if (rc != FAT_OK) {
                break;
            }
        }
        for (i = 0; i < chunk; i++) {
            cbuf[cur->within + i] = (src != NULL) ? src[put + i] : 0;
        }
        rc = fat_write_sectors(vol, lba, vol->sectors_per_cluster, cbuf);
        if (rc != FAT_OK) {
            break;
        }

        put         += chunk;
        cur->within += chunk;
        if (cur->within >= cbytes) {
            if (put >= len) {
                /* Exactly at a cluster boundary with nothing left to write.
                 * Do NOT grow here - that would allocate a cluster the file
                 * has no byte in, which is a leak on every write whose length
                 * happens to be a multiple of the cluster size. */
                cur->within = 0;
                break;
            }
            rc = fat_next_or_grow(vol, &cur->cluster);
            if (rc != FAT_OK) {
                break;              /* out of space: a short write */
            }
            cur->within = 0;
        }
    }

    if (done != NULL) {
        *done = put;
    }
    return put == len ? FAT_OK : rc;
}

/* Move the cursor to absolute byte `pos`, growing the chain to reach it. */
static int fat_seek_cursor(fat_volume_t *vol, struct fat_wcursor *cur,
                           uint64 pos, uint32 cbytes) {
    uint32 skip = (uint32)(pos / cbytes);
    int rc;

    while (skip-- > 0) {
        rc = fat_next_or_grow(vol, &cur->cluster);
        if (rc != FAT_OK) {
            return rc;
        }
    }
    cur->within = (uint32)(pos % cbytes);
    return FAT_OK;
}

int fat_write_entry_at(fat_volume_t *vol, fat_entry_t *ent, uint64 offset,
                       const void *buffer, uint32 max, uint32 *out) {
    struct fat_wcursor cur;
    struct dir_pos pos;
    uint8 *cluster_buf;
    char want11[11];
    uint32 cluster_bytes;
    uint32 written = 0;
    uint32 old_size;
    int rc;

    if (out != NULL) {
        *out = 0;
    }
    if (!vol->mounted) {
        return FAT_ERR_NOTFAT;
    }
    if (ent->attr & FAT_ATTR_DIRECTORY) {
        return FAT_ERR_ISDIR;
    }
    if (max == 0) {
        return FAT_OK;
    }
    /* The on-disk size field is 32 bits. Refused rather than wrapped: a file
     * whose recorded length is (length mod 4G) is one every other FAT reader
     * will believe. */
    if (offset >= 0xFFFFFFFFULL || offset + max > 0xFFFFFFFFULL) {
        return FAT_ERR_TOOBIG;
    }

    /* Step 1: the directory record, before anything is modified. */
    if (fat_name_to_entry(ent->name, want11) != 0) {
        return FAT_ERR_NOTFOUND;
    }
    rc = fat_scan_dir_raw(vol, ent->dir_cluster, want11, &pos, NULL);
    if (rc != FAT_OK) {
        return rc;
    }

    cluster_bytes = vol->sectors_per_cluster * vol->bytes_per_sector;
    cluster_buf = (uint8 *)kmalloc(cluster_bytes);
    if (cluster_buf == NULL) {
        return FAT_ERR_NOMEM;
    }
    old_size = ent->size;

    /* An empty file has no chain at all. The first cluster is allocated here
     * and recorded in step 4 along with the size - not now, because a record
     * pointing at a cluster the write then failed to fill is a file whose
     * contents are whatever that cluster held. */
    cur.cluster = ent->cluster;
    cur.within  = 0;
    if (cur.cluster < CLUSTER_FIRST || cur.cluster >= CLUSTER_BAD) {
        rc = fat_alloc_cluster(vol, &cur.cluster);
        if (rc == FAT_OK) {
            rc = fat_zero_cluster(vol, cur.cluster);
        }
        if (rc != FAT_OK) {
            kfree(cluster_buf);
            return rc;
        }
        ent->cluster = cur.cluster;
        old_size = 0;
    }

    /* Step 2: the GAP, when the write starts past the old end of file. Real
     * zeroes over the real range - FAT has no holes, and the tail of the
     * cluster that held the old EOF is stale data that must not become part
     * of this file. See fat_put_bytes. */
    if (offset > old_size) {
        uint32 gap_done = 0;

        rc = fat_seek_cursor(vol, &cur, old_size, cluster_bytes);
        if (rc == FAT_OK) {
            rc = fat_put_bytes(vol, &cur, NULL,
                               (uint32)(offset - old_size), cluster_buf,
                               cluster_bytes, &gap_done);
        }
        if (rc != FAT_OK) {
            /* The gap could not be filled, so the data cannot go where it was
             * asked to go. Nothing is written, and the size is left alone -
             * the clusters that were allocated leak until fsck, which is the
             * failure this ordering chooses over a file with unreadable
             * bytes in the middle of it. */
            kfree(cluster_buf);
            return rc;
        }
    } else {
        rc = fat_seek_cursor(vol, &cur, offset, cluster_bytes);
        if (rc != FAT_OK) {
            kfree(cluster_buf);
            return rc;
        }
    }

    /* Step 3: the data. */
    rc = fat_put_bytes(vol, &cur, (const uint8 *)buffer, max, cluster_buf,
                       cluster_bytes, &written);
    kfree(cluster_buf);

    /* Step 4: the record, last, with the size actually achieved. A size
     * written before the data would describe bytes that are not there yet,
     * and a crash in between would leave a file reporting garbage as content.
     * This way a crash leaves allocated-but-unreferenced clusters, which is a
     * leak fsck can find, not corruption a reader believes. */
    {
        struct entry_grow g;
        int erc;

        if (offset + written > ent->size) {
            ent->size = (uint32)(offset + written);
        }
        g.cluster = ent->cluster;
        g.size    = ent->size;
        erc = fat_edit_entry(vol, &pos, edit_grow_entry, &g);
        if (erc != FAT_OK) {
            return erc;
        }
    }

    if (written > 0) {
        if (out != NULL) {
            *out = written;
        }
        return FAT_OK;
    }
    return rc;
}

/* --- making and shortening files ----------------------------------------
 *
 * The two operations the write path was unusable without. write(2) could
 * rewrite a file that already existed and do nothing else: a file could not
 * be MADE and could not be SHORTENED, so O_CREAT and O_TRUNC were refused and
 * every program that opens a file for output was refused with them.
 */

/* Create an empty regular file. FAT_ERR_EXISTS if the name is taken.
 *
 * No cluster is allocated. A FAT file with first cluster 0 and size 0 is the
 * on-disk representation of "empty" - not a special case this invents, and
 * exactly what fat_write_entry_at already expects to find when it allocates
 * the first cluster on the first write. Allocating one here would give every
 * created-and-never-written file a cluster it does not use. */
int fat_create(fat_volume_t *vol, const char *path) {
    char parent[FAT_PATH_MAX];
    char leaf[16];
    char name11[11];
    uint32 parent_cluster;
    struct dir_pos pos;
    struct entry_init ei;
    int rc;

    if (!vol->mounted) {
        return FAT_ERR_NOTFAT;
    }
    rc = fat_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf));
    if (rc != FAT_OK) {
        return rc;
    }
    if (fat_name_to_entry(leaf, name11) != 0) {
        /* A name that cannot be spelled in 8.3 is refused rather than
         * truncated. Truncating would create a file under a name the caller
         * did not ask for, which it then cannot open by the name it used. */
        return FAT_ERR_BADPATH;
    }
    rc = fat_dir_cluster_of(vol, parent, &parent_cluster);
    if (rc != FAT_OK) {
        return rc;
    }

    /* Existence check before the free-slot search, so an O_CREAT on a file
     * that is already there costs one scan and changes nothing. */
    rc = fat_scan_dir_raw(vol, parent_cluster, name11, &pos, NULL);
    if (rc == FAT_OK) {
        return FAT_ERR_EXISTS;
    }
    if (rc != FAT_ERR_NOTFOUND) {
        return rc;
    }
    rc = fat_scan_dir_raw(vol, parent_cluster, NULL, &pos, NULL);
    if (rc != FAT_OK) {
        return rc == FAT_ERR_NOTFOUND ? FAT_ERR_FULL : rc;
    }

    ei.name11  = name11;
    ei.attr    = 0;              /* a regular file: no attribute bits at all */
    ei.cluster = 0;
    ei.size    = 0;
    return fat_edit_entry(vol, &pos, edit_write_entry, &ei);
}

/* Set a file's length. Shrinks by cutting the cluster chain and freeing the
 * tail; grows by zero-filling, which is what a write past the end already
 * does.
 *
 * `ent` is updated in place, for fat_write_entry_at's reason: the caller
 * holds the only copy and the on-disk record has just changed under it.
 */
int fat_truncate(fat_volume_t *vol, fat_entry_t *ent, uint32 new_size) {
    struct dir_pos pos;
    struct entry_grow g;
    char want11[11];
    uint32 cluster_bytes, keep, cluster, tail;
    uint32 i;
    int rc;

    if (!vol->mounted) {
        return FAT_ERR_NOTFAT;
    }
    if (ent->attr & FAT_ATTR_DIRECTORY) {
        return FAT_ERR_ISDIR;
    }
    if (new_size == ent->size) {
        return FAT_OK;
    }

    if (new_size > ent->size) {
        /* Growing is a hole of zeroes, and fat_write_entry_at already fills
         * one correctly - including the tail of the cluster that held the old
         * end of file, which is the part that is easy to miss. Writing one
         * zero byte at the far end is enough to move the size, and reusing
         * that path means there is one implementation of "the gap reads as
         * zero" rather than two that can disagree. */
        static const char zero = 0;
        uint32 wrote = 0;

        return fat_write_entry_at(vol, ent, (uint64)new_size - 1, &zero, 1,
                                  &wrote);
    }

    /* --- shrinking ------------------------------------------------------- */
    if (fat_name_to_entry(ent->name, want11) != 0) {
        return FAT_ERR_NOTFOUND;
    }
    rc = fat_scan_dir_raw(vol, ent->dir_cluster, want11, &pos, NULL);
    if (rc != FAT_OK) {
        return rc;
    }

    cluster_bytes = vol->sectors_per_cluster * vol->bytes_per_sector;
    keep = (new_size + cluster_bytes - 1) / cluster_bytes;
    cluster = ent->cluster;
    tail = 0;

    if (keep == 0) {
        /* Truncated to nothing: the whole chain goes and the record's first
         * cluster becomes 0, which is FAT's representation of an empty
         * file. */
        tail = cluster;
        ent->cluster = 0;
    } else if (cluster >= CLUSTER_FIRST && cluster < CLUSTER_BAD) {
        for (i = 1; i < keep; i++) {
            uint32 next;

            rc = fat_next_cluster(vol, cluster, &next);
            if (rc != FAT_OK) {
                return rc;
            }
            if (next >= CLUSTER_EOC) {
                /* The chain is already shorter than the recorded size - a
                 * volume that was not shut down cleanly. Nothing to free, and
                 * writing the smaller size is the repair. */
                keep = i;
                break;
            }
            cluster = next;
        }
        rc = fat_next_cluster(vol, cluster, &tail);
        if (rc != FAT_OK) {
            return rc;
        }
    }

    /* THE RECORD FIRST, then the chain, and this order is the opposite of
     * fat_unlink's for a reason worth stating.
     *
     * unlink removes the entry before freeing the chain, because an entry
     * that names freed clusters can have them handed to another file - two
     * files sharing blocks, which is unrecoverable. Here the entry SURVIVES,
     * so the danger is the mirror image: a chain cut and freed before the
     * size is written leaves a record claiming more bytes than the file has,
     * and reads run off the end of the chain into whatever the FAT says next.
     *
     * Both orders choose the same way when they cannot have both: leak space
     * rather than describe space that is not yours. */
    ent->size = new_size;
    g.cluster = ent->cluster;
    g.size    = ent->size;
    rc = fat_edit_entry(vol, &pos, edit_grow_entry, &g);
    if (rc != FAT_OK) {
        return rc;
    }

    if (keep > 0 && cluster >= CLUSTER_FIRST && cluster < CLUSTER_BAD) {
        /* Terminate the kept chain BEFORE freeing what followed it. */
        rc = fat_set_entry(vol, cluster, 0xFFFF);
        if (rc != FAT_OK) {
            return rc;
        }
    }
    if (tail >= CLUSTER_FIRST && tail < CLUSTER_BAD) {
        return fat_free_chain(vol, tail);
    }
    return FAT_OK;
}

/* --- how full is the volume? ---------------------------------------------
 *
 * Counting free clusters means reading the whole FAT, which is a disk read
 * per FAT sector. That is why this is not cached: the number changes on every
 * allocation, and a value captured at mount would be right once and confident
 * forever after - df reporting a full disk as empty is worse than df being
 * slow.
 *
 * The BLOCK CACHE takes most of the sting out of it in practice, since the
 * FAT is exactly the region every allocation already touches. */
int fat_free_clusters(fat_volume_t *vol, uint32 *out) {
    uint8 *sector;
    uint32 cluster;
    uint32 free_count = 0;
    uint32 cached_lba = 0xFFFFFFFFu;
    int rc = FAT_OK;

    if (out == NULL) {
        return FAT_ERR_BADPATH;
    }
    *out = 0;
    if (!vol->mounted) {
        return FAT_ERR_NOTFAT;
    }

    sector = (uint8 *)kmalloc(ATA_SECTOR_SIZE);
    if (sector == NULL) {
        return FAT_ERR_NOMEM;
    }

    /* One sector held across the whole walk, rather than a read per entry.
     * A FAT16 sector holds 256 entries, so this is 256 times fewer reads than
     * the obvious loop - and the obvious loop is what makes people cache the
     * answer instead of fixing the scan. */
    for (cluster = CLUSTER_FIRST;
         cluster < vol->total_clusters + CLUSTER_FIRST; cluster++) {
        uint32 offset = cluster * 2u;
        uint32 lba    = vol->fat_start + (offset / vol->bytes_per_sector);

        if (lba != cached_lba) {
            rc = fat_sectors(vol, lba, 1, sector);
            if (rc != FAT_OK) {
                rc = FAT_ERR_IO;
                break;
            }
            cached_lba = lba;
        }
        if (rd16(sector + (offset % vol->bytes_per_sector)) == 0) {
            free_count++;
        }
    }

    kfree(sector);
    if (rc == FAT_OK) {
        *out = free_count;
    }
    return rc;
}
