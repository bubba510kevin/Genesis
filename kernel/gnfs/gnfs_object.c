#include "gnfs_layout.h"
#include "typesk.h"

/* The object layer's pure half: onode access, allocation bookkeeping, and
 * directory entries, all as functions over caller-owned buffers - no I/O, no
 * allocation of memory, no device. Same split as kernel/gnfs/gnfs_format.c
 * and kernel/fs/acl.c before it, and for the same reason: tests/host/
 * gnfs_test.c checks every rule here with no disk underneath it.
 */

#define ENOENT  2
#define ENOSPC  28
#define EEXIST  17

uint64 gnfs_obj_table_blocks_for(uint64 max_objects) {
    return (max_objects + GNFS_ONODES_PER_BLOCK - 1) / GNFS_ONODES_PER_BLOCK;
}

void gnfs_onode_free(gnfs_onode_t *o) {
    uint8 *b = (uint8 *)o;
    uint64 i;

    for (i = 0; i < sizeof(*o); i++) {
        b[i] = 0;
    }
}

void gnfs_onode_init(gnfs_onode_t *o, uint32 mode, uint32 uid, uint32 gid) {
    /* Zeroed first, so every field this does not name - the block map,
     * the indirect pointers, the ACL block - starts as "nothing", and so no
     * byte of a reused slot's previous occupant survives into the new one. */
    gnfs_onode_free(o);
    o->mode  = mode;
    o->uid   = uid;
    o->gid   = gid;
    o->nlink = 1;
}

gnfs_onode_t *gnfs_onode_at(uint8 *table, uint64 max_objects, uint64 objnum) {
    if (objnum == GNFS_OBJNUM_NONE || objnum >= max_objects) {
        return NULL;
    }
    return &((gnfs_onode_t *)table)[objnum];
}

/* First free slot at or after the hint, wrapping once. The hint moves past
 * each allocation so a burst of creates does not rescan the same prefix,
 * and because the scan wraps, a slot freed behind the hint is found again
 * the moment the table has nothing free ahead of it - which is what
 * "object numbers are reused" has to mean. */
int gnfs_onode_alloc(gnfs_root_t *r, uint8 *table, uint64 *out) {
    uint64 max = r->max_objects;
    uint64 start = r->next_objnum;
    uint64 k;

    if (start == GNFS_OBJNUM_NONE || start >= max) {
        start = 1;
    }
    for (k = 0; k < max; k++) {
        uint64 objnum = start + k;
        gnfs_onode_t *o;

        if (objnum >= max) {
            objnum -= max - 1;       /* wrap to 1, skipping objnum 0 */
        }
        o = gnfs_onode_at(table, max, objnum);
        if (o != NULL && o->mode == 0) {
            *out = objnum;
            r->next_objnum = objnum + 1;
            return 0;
        }
    }
    return -ENOSPC;
}

uint64 gnfs_objects_in_use(const uint8 *table, uint64 max_objects) {
    const gnfs_onode_t *t = (const gnfs_onode_t *)table;
    uint64 n = 0;
    uint64 i;

    for (i = 1; i < max_objects; i++) {
        if (t[i].mode != 0) {
            n++;
        }
    }
    return n;
}

/* --- directory data ---------------------------------------------------------
 *
 * A name is compared by length first, then bytes - not by scanning for a NUL
 * in `name[]`, because name_len is the stored length and a name is not
 * guaranteed (or required) to be NUL-terminated within the fixed field. */
static int names_equal(const char *a, uint64 alen, const char *b, uint8 blen) {
    uint64 i;

    if (alen != (uint64)blen) {
        return 0;
    }
    for (i = 0; i < alen; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* Every byte, not just the three fields a reader looks at. The block goes to
 * disk whole, and it used to be built in a stack buffer with only objnum,
 * is_dir and name_len set per entry - so the name bytes, the reserved bytes
 * and the tail past the last whole entry (4096 is not a multiple of the
 * entry size) carried whatever the kernel stack last held onto the medium:
 * kernel memory leaking into a filesystem any user can read raw. Found when
 * the gnfs ACL fixture refused to come out byte-identical twice. */
void gnfs_dir_init_block(uint8 *block) {
    uint64 i;

    for (i = 0; i < GNFS_BLOCK_SIZE; i++) {
        block[i] = 0;
    }
}

/* One entry back to all-zero - what a never-used slot looks like - so a
 * removed name does not linger on disk after its entry is gone. */
static void dirent_clear(gnfs_dirent_t *e) {
    uint8 *b = (uint8 *)e;
    uint64 i;

    for (i = 0; i < sizeof(*e); i++) {
        b[i] = 0;
    }
}

int gnfs_dir_find(const uint8 *block, const char *name, uint64 name_len,
                  uint64 *objnum_out, int *is_dir_out) {
    const gnfs_dirent_t *ents = (const gnfs_dirent_t *)block;
    uint64 i;

    for (i = 0; i < GNFS_DIRENTS_PER_BLOCK; i++) {
        if (ents[i].objnum == GNFS_OBJNUM_NONE) {
            continue;
        }
        if (names_equal(name, name_len, ents[i].name, ents[i].name_len)) {
            *objnum_out = ents[i].objnum;
            *is_dir_out = ents[i].is_dir;
            return 0;
        }
    }
    return -ENOENT;
}

int gnfs_dir_add(uint8 *block, const char *name, uint64 name_len,
                uint64 objnum, int is_dir) {
    gnfs_dirent_t *ents = (gnfs_dirent_t *)block;
    uint64 i;
    int64 free_slot = -1;

    if (name_len == 0 || name_len > GNFS_NAME_MAX) {
        return -22;                /* -EINVAL */
    }

    for (i = 0; i < GNFS_DIRENTS_PER_BLOCK; i++) {
        if (ents[i].objnum == GNFS_OBJNUM_NONE) {
            if (free_slot < 0) {
                free_slot = (int64)i;
            }
            continue;
        }
        if (names_equal(name, name_len, ents[i].name, ents[i].name_len)) {
            return -EEXIST;
        }
    }
    if (free_slot < 0) {
        return -ENOSPC;
    }

    dirent_clear(&ents[free_slot]);
    ents[free_slot].objnum   = objnum;
    ents[free_slot].is_dir   = (uint8)(is_dir ? 1 : 0);
    ents[free_slot].name_len = (uint8)name_len;
    for (i = 0; i < name_len; i++) {
        ents[free_slot].name[i] = name[i];
    }
    return 0;
}

int gnfs_dir_remove(uint8 *block, const char *name, uint64 name_len) {
    gnfs_dirent_t *ents = (gnfs_dirent_t *)block;
    uint64 i;

    for (i = 0; i < GNFS_DIRENTS_PER_BLOCK; i++) {
        if (ents[i].objnum == GNFS_OBJNUM_NONE) {
            continue;
        }
        if (names_equal(name, name_len, ents[i].name, ents[i].name_len)) {
            dirent_clear(&ents[i]);
            return 0;
        }
    }
    return -ENOENT;
}

int gnfs_dir_iterate(const uint8 *block, gnfs_dir_cb cb, void *ctx) {
    const gnfs_dirent_t *ents = (const gnfs_dirent_t *)block;
    uint64 i;
    int rc;

    for (i = 0; i < GNFS_DIRENTS_PER_BLOCK; i++) {
        if (ents[i].objnum == GNFS_OBJNUM_NONE) {
            continue;
        }
        rc = cb(ents[i].name, ents[i].name_len, ents[i].objnum,
               ents[i].is_dir, ctx);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}
