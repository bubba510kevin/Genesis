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

void gnfs_onode_init(gnfs_onode_t *o, uint32 mode, uint32 uid, uint32 gid) {
    uint64 i;

    o->mode      = mode;
    o->uid       = uid;
    o->gid       = gid;
    o->nlink     = 1;
    o->size      = 0;
    o->nblocks   = 0;
    o->acl_block = 0;
    for (i = 0; i < GNFS_OBJ_DIRECT; i++) {
        o->direct[i] = 0;
    }
}

gnfs_onode_t *gnfs_onode_at(uint8 *table, uint64 objnum) {
    if (objnum == GNFS_OBJNUM_NONE || objnum >= GNFS_MAX_OBJECTS) {
        return NULL;
    }
    return &((gnfs_onode_t *)table)[objnum];
}

int gnfs_onode_alloc(gnfs_root_t *r, uint64 *out) {
    if (r->next_objnum >= GNFS_MAX_OBJECTS) {
        return -ENOSPC;
    }
    *out = r->next_objnum;
    r->next_objnum++;
    return 0;
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

void gnfs_dir_init_block(uint8 *block) {
    uint64 i;
    gnfs_dirent_t *ents = (gnfs_dirent_t *)block;

    for (i = 0; i < GNFS_DIRENTS_PER_BLOCK; i++) {
        ents[i].objnum   = GNFS_OBJNUM_NONE;
        ents[i].is_dir   = 0;
        ents[i].name_len = 0;
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
            ents[i].objnum   = GNFS_OBJNUM_NONE;
            ents[i].is_dir   = 0;
            ents[i].name_len = 0;
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
