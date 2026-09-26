#ifndef FAT_H
#define FAT_H

#include "device.h"
#include "typesk.h"

/* FAT16, read-only.
 *
 * Read-only on purpose for a first version. Reading is a matter of computing
 * four offsets and following a chain; WRITING means allocating clusters,
 * updating every copy of the FAT, rewriting directory entries, and keeping all
 * of it consistent if you stop halfway. Those are separate problems and the
 * second set is much easier to get wrong quietly.
 *
 * FAT16 rather than 12 or 32 because its root directory is a fixed-size array
 * at a computable LBA rather than a cluster chain - one less layer between you
 * and a file listing. FAT32 moves the root into the data area; FAT12 packs
 * entries into 12 bits, so half of them straddle a byte boundary.
 */

#define FAT_OK             0
#define FAT_ERR_IO        -1   /* the ATA layer failed                     */
#define FAT_ERR_NOTFAT    -2   /* no FAT16 signature in the boot sector    */
#define FAT_ERR_NOTFOUND  -3   /* no directory entry with that name        */
#define FAT_ERR_TOOBIG    -4   /* file is larger than the buffer given     */
#define FAT_ERR_CORRUPT   -5   /* cluster chain left the volume            */
#define FAT_ERR_NOMEM     -6
#define FAT_ERR_NOTDIR    -7   /* a path component that is not a directory */
#define FAT_ERR_BADPATH   -8   /* not absolute, or a component over 8.3    */
#define FAT_ERR_ISDIR     -9   /* tried to read a directory as a file      */
#define FAT_ERR_EXISTS   -10   /* a name that is already taken              */
#define FAT_ERR_NOTEMPTY -11   /* rmdir on a directory with entries in it   */
#define FAT_ERR_FULL     -12   /* no free cluster, or no free dir slot      */
#define FAT_ERR_INVAL    -13   /* refused: "." or "..", or crossing volumes */

/* Directory entry attribute bits. */
#define FAT_ATTR_READONLY  0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
/* All four low bits together mark a long-filename fragment rather than a real
 * entry. Not skipping these is why a first FAT driver lists rows of garbage. */
#define FAT_ATTR_LFN       0x0F

/* Longest absolute path the mutation calls will split. Generous next to 8.3
 * components, and bounded because fat_split_path writes the parent into a
 * caller-supplied buffer of exactly this size. */
#define FAT_PATH_MAX       256

typedef struct {
    /* A device, not an ata_device_t.
     *
     * That one word is what lets FAT sit on a PARTITION. ata_read addresses
     * the whole disk, so a filesystem holding an ata_device_t can only ever
     * be mounted at sector 0 - which is why the current data disk has no
     * partition table. A volume device applies its own base offset (see
     * volume.c), so the same driver reads LBA 0 of the partition and the
     * partition decides where that is.
     *
     * It also means FAT works unchanged on anything block-shaped: a ramdisk,
     * a file-backed image, a USB stick. None of that is a FAT change. */
    device_t *dev;

    uint32 bytes_per_sector;
    uint32 sectors_per_cluster;

    uint32 fat_start;        /* LBA of the first FAT                       */
    uint32 fat_sectors;      /* length of one FAT                          */
    uint32 root_start;       /* LBA of the root directory                  */
    uint32 root_entries;     /* fixed capacity, not a count of files       */
    uint32 root_sectors;
    uint32 data_start;       /* LBA of cluster 2                           */
    uint32 total_clusters;

    /* How many copies of the FAT this volume has - almost always 2.
     *
     * Read but not STORED before, because nothing wrote to the FAT and a
     * reader only ever needs the first copy. Every mutation has to update
     * all of them: mkfs.fat writes two, fsck compares them, and a volume
     * whose copies disagree is one every other tool will call corrupt. */
    uint32 num_fats;

    uint8  mounted;
} fat_volume_t;

/* Callback for fat_list_root. `name` is a NUL-terminated 8.3 name with the
 * dot reinserted, so "README.TXT" rather than the raw "README  TXT". */
typedef void (*fat_dir_cb)(const char *name, uint32 size, uint8 attr);

/* One resolved directory entry. */
typedef struct {
    char   name[13];    /* 8.3 with the dot reinserted, NUL-terminated      */
    uint32 cluster;     /* first cluster; 0 for the root and for empty files */
    uint32 size;        /* bytes; 0 for a directory                         */
    uint8  attr;        /* FAT_ATTR_* bits                                  */

    /* First cluster of the DIRECTORY this entry lives in; 0 for the root.
     *
     * Added for fat_write_entry_at, and it is the field that makes writing
     * possible at all: a write that extends a file has to update the file's
     * SIZE, and the size lives in the 32-byte directory record, not in the
     * data. Without knowing which directory to look in there is no way back
     * to that record from an entry that was resolved some time ago.
     *
     * The directory cluster rather than the record's LBA and offset, which
     * would be more direct. Two reasons: a cluster is stable where an LBA is
     * only stable until the directory is rewritten, and fat_scan_dir_raw
     * already turns (directory cluster, 8.3 name) into a position - so this
     * reuses the lookup the mutation paths were already built on instead of
     * adding a second way to find the same 32 bytes. */
    uint32 dir_cluster;
} fat_entry_t;

/* Parse the boot sector and fill in the geometry. */
int fat_mount(fat_volume_t *vol, device_t *dev);

/* Walk the root directory, calling cb once per real entry. Deleted entries,
 * LFN fragments and the volume label are skipped. */
int fat_list_root(fat_volume_t *vol, fat_dir_cb cb);

/* Called once per real entry by fat_iterate. Return non-zero to stop early. */
typedef int (*fat_entry_cb)(const fat_entry_t *entry, void *ctx);

/* Walk a directory named by path, passing a context through. fat_list is the
 * same thing without the context, kept because its callers do not need one. */
int fat_iterate(fat_volume_t *vol, const char *path, fat_entry_cb cb, void *ctx);

/* The same, given an entry already resolved. An open directory holds its own
 * entry, so this avoids re-walking the path on every getdents64 call - which
 * for a deep path is most of the work of listing it. */
int fat_iterate_dir(fat_volume_t *vol, const fat_entry_t *dir,
                    fat_entry_cb cb, void *ctx);

/* Read part of a file. This is what read(2) needs and fat_read_path cannot
 * give it - whole-file reads are the wrong shape for a descriptor with a
 * position. Short reads at end of file are normal, not errors. */
int fat_read_entry_at(fat_volume_t *vol, const fat_entry_t *ent, uint64 offset,
                      void *buffer, uint32 max, uint32 *out);

/* Write part of a file, extending it and allocating clusters as it grows.
 *
 * `ent` is UPDATED in place - its size, and its first cluster if the file was
 * empty - because the caller is holding the only copy of that entry and the
 * on-disk record has just changed underneath it. An fs_node_t whose size did
 * not move after a write that extended the file reports the old size to
 * fstat and truncates the next read.
 *
 * Writing PAST the end of the file is allowed and is not sparse: FAT has no
 * concept of a hole, so the skipped clusters are really allocated and really
 * zeroed. That costs disk where a sparse filesystem would cost nothing, and
 * it is the only behaviour that can be implemented honestly here - the
 * alternative is a chain with a gap in it, which no other FAT reader would
 * understand.
 *
 * Returns FAT_OK with *out set to the bytes written. A short write means the
 * volume filled up; FAT_ERR_FULL means it was already full. */
int fat_write_entry_at(fat_volume_t *vol, fat_entry_t *ent, uint64 offset,
                       const void *buffer, uint32 max, uint32 *out);

/* Resolve an absolute path to an entry. "/" gives the root itself.
 *
 * Paths must be absolute: there is no cwd here, because a cwd belongs to a
 * process and this layer has no idea one exists. Components are 8.3 and
 * matched case-insensitively, so "/bin/busybox" finds BUSYBOX. Long filenames
 * are not read, so a host file named "busybox.elf" is reachable only by
 * whatever short name the filesystem gave it. */
int fat_lookup(fat_volume_t *vol, const char *path, fat_entry_t *out);

/* Walk a directory named by path, calling cb once per real entry. */
int fat_list(fat_volume_t *vol, const char *path, fat_dir_cb cb);

/* Read a whole file by path. Writes at most `max` bytes and stores the real
 * size in *out_size if non-NULL - so passing (NULL, 0, &size) is how you ask
 * how big something is before allocating for it. */
int fat_read_path(fat_volume_t *vol, const char *path,
                  void *buffer, uint32 max, uint32 *out_size);

/* Read a file in the root by bare 8.3 name. Equivalent to fat_read_path with
 * a leading slash; kept for existing callers. */
/* --- mutation ------------------------------------------------------------
 *
 * Everything above this line reads. These four write, and they are the first
 * thing in this file that can damage a volume rather than merely misreport
 * it - so each one validates before it touches a sector, and each one
 * updates every FAT copy.
 *
 * All take absolute 8.3 paths, the same as fat_lookup. None of them
 * implements long filenames: a component that does not fit 8.3 is refused
 * with FAT_ERR_BADPATH rather than truncated, because a silently truncated
 * name creates a file the caller cannot then find.
 */

/* Create an empty directory. Allocates one cluster for it and writes the
 * "." and ".." entries every FAT directory must have. Returns FAT_ERR_EXISTS
 * if the name is taken. */
/* Create an empty regular file. FAT_ERR_EXISTS if the name is taken,
 * FAT_ERR_BADPATH if it cannot be spelled in 8.3 - refused rather than
 * truncated, because a file created under a name the caller did not ask for
 * is one it cannot then open. */
/* Count unused clusters, by walking the whole allocation table. Not cached -
 * see the definition on why a captured value is worse than a slow one. */
int fat_free_clusters(fat_volume_t *vol, uint32 *out);

int fat_create(fat_volume_t *vol, const char *path);

/* Set a file's length: shrink by cutting the chain and freeing the tail, grow
 * by zero-filling. `ent` is updated in place. */
int fat_truncate(fat_volume_t *vol, fat_entry_t *ent, uint32 new_size);

int fat_mkdir(fat_volume_t *vol, const char *path);

/* Remove a FILE. Frees its cluster chain and marks its directory entry
 * deleted. Refuses a directory with FAT_ERR_ISDIR - that is rmdir's job, and
 * conflating them is how a directory tree gets orphaned. */
int fat_unlink(fat_volume_t *vol, const char *path);

/* Remove an EMPTY directory. Refuses a non-empty one with FAT_ERR_NOTEMPTY,
 * and refuses "." and ".." with FAT_ERR_INVAL. */
int fat_rmdir(fat_volume_t *vol, const char *path);

/* Rename, within the volume. Both a plain rename and a move between
 * directories: the same entry is rewritten in place when the parent does not
 * change, and copied-then-deleted when it does.
 *
 * Refuses to overwrite an existing name - POSIX rename replaces, but doing
 * that safely means unlinking the target only after the new entry is
 * committed, and a half-done replace loses the file. Returning
 * FAT_ERR_EXISTS lets the caller decide. */
int fat_rename(fat_volume_t *vol, const char *oldpath, const char *newpath);

int fat_read_file(fat_volume_t *vol, const char *name,
                  void *buffer, uint32 max, uint32 *out_size);

#endif
