#include "fileobj.h"
#include "fs.h"
#include "kheap.h"
#include "nt.h"
#include "object.h"
#include "paging.h"
#include "pmm.h"
#include "section.h"
#include "typesk.h"

/* See section.h. */

#define PAGE 0x1000ULL

typedef struct {
    uint64       size;           /* bytes, as created                      */
    uint64       pages;
    phys_addr_t *frames;
    uint32       protect;
    object_t    *file;           /* referenced; NULL for the pagefile kind */
    int          writeback;      /* writable and file-backed               */
} section_t;

static void section_destroy(object_t *obj) {
    section_t *s = obj->body;
    uint64 i;

    if (s->writeback) {
        (void)section_flush(obj, 0, s->size);
    }
    for (i = 0; i < s->pages; i++) {
        if (s->frames[i] != 0) {
            pmm_free_frame(s->frames[i]);      /* the section's reference */
        }
    }
    if (s->file != NULL) {
        ob_deref(s->file);
    }
    kfree(s->frames);
    kfree(s);
}

static const object_type_t section_type = {
    .name    = "Section",
    .klass   = OBJ_SECTION,
    .destroy = section_destroy,
};

static section_t *sec_of(object_t *obj) {
    return (obj != NULL && obj->type == &section_type) ? obj->body : NULL;
}

static int writable(uint32 p) {
    return p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE;
}

object_t *section_create(uint64 size, uint32 protect, object_t *file,
                         int file_writable, uint32 *status) {
    section_t *s;
    object_t *obj;
    uint64 i, file_size = 0;

    switch (protect) {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        break;
    default:
        *status = STATUS_INVALID_PAGE_PROTECTION;
        return NULL;
    }
    if (file != NULL) {
        const fs_node_t *n = fileobj_node(file);

        if (n == NULL || file->type == NULL || file->type->read == NULL) {
            *status = STATUS_INVALID_PARAMETER;   /* not a file */
            return NULL;
        }
        file_size = n->size;
        if (writable(protect) && !file_writable) {
            *status = STATUS_ACCESS_DENIED;
            return NULL;
        }
        if (size == 0) {
            size = file_size;
        }
        if (size == 0) {
            /* NT refuses to map an empty file: there is nothing to see. */
            *status = 0xC000011Eu;            /* STATUS_MAPPED_FILE_SIZE_ZERO */
            return NULL;
        }
        if (size > file_size && !writable(protect)) {
            *status = 0xC0000040u;            /* STATUS_SECTION_TOO_BIG */
            return NULL;
        }
    }
    if (size == 0) {
        *status = STATUS_INVALID_PARAMETER;
        return NULL;
    }

    s = kmalloc(sizeof(*s));
    if (s == NULL) {
        *status = STATUS_NO_MEMORY;
        return NULL;
    }
    s->size = size;
    s->pages = (size + PAGE - 1) / PAGE;
    s->protect = protect;
    s->file = NULL;
    s->writeback = 0;
    if (s->pages + 64 > pmm_free_frames()) {
        kfree(s);
        *status = STATUS_NO_MEMORY;            /* the commit cannot be met */
        return NULL;
    }
    s->frames = kmalloc(sizeof(phys_addr_t) * s->pages);
    if (s->frames == NULL) {
        kfree(s);
        *status = STATUS_NO_MEMORY;
        return NULL;
    }
    for (i = 0; i < s->pages; i++) {
        uint64 b;
        uint8 *z;

        s->frames[i] = pmm_alloc_frame();
        if (s->frames[i] == 0) {
            while (i-- > 0) {
                pmm_free_frame(s->frames[i]);
            }
            kfree(s->frames);
            kfree(s);
            *status = STATUS_NO_MEMORY;
            return NULL;
        }
        z = (uint8 *)phys_to_virt(s->frames[i]);
        for (b = 0; b < PAGE; b++) {
            z[b] = 0;
        }
    }

    if (file != NULL) {
        uint64 off = 0, want = file_size < size ? file_size : size;

        /* Page by page into the frames, straight through the direct map. */
        while (off < want) {
            uint64 chunk = want - off < PAGE - (off % PAGE) ? want - off
                                                            : PAGE - (off % PAGE);
            uint64 pos = off;
            int64 got = file->type->read(file,
                                         (uint8 *)phys_to_virt(
                                             s->frames[off / PAGE]) + off % PAGE,
                                         chunk, &pos);

            if (got <= 0) {
                break;                         /* short file: zeroes after */
            }
            off += (uint64)got;
        }
        ob_ref(file);
        s->file = file;
        s->writeback = writable(protect);
    }

    obj = ob_create(&section_type, s);
    if (obj == NULL) {
        s->writeback = 0;
        for (i = 0; i < s->pages; i++) {
            pmm_free_frame(s->frames[i]);
        }
        if (s->file != NULL) {
            ob_deref(s->file);
        }
        kfree(s->frames);
        kfree(s);
        *status = STATUS_INSUFFICIENT_RESOURCES;
        return NULL;
    }
    /* A writable mapping larger than its file makes the file that large -
     * now, so the file's size agrees with the section's from the start. */
    if (s->writeback && size > file_size) {
        (void)section_flush(obj, file_size, size - file_size);
    }
    *status = STATUS_SUCCESS;
    return obj;
}

int section_info(object_t *obj, uint64 *size, uint32 *protect) {
    section_t *s = sec_of(obj);

    if (s == NULL) {
        return -22;
    }
    *size = s->size;
    *protect = s->protect;
    return 0;
}

phys_addr_t section_frame(object_t *obj, uint64 index) {
    section_t *s = sec_of(obj);

    return (s == NULL || index >= s->pages) ? 0 : s->frames[index];
}

int section_flush(object_t *obj, uint64 offset, uint64 len) {
    section_t *s = sec_of(obj);
    uint64 end;

    if (s == NULL) {
        return -22;
    }
    if (!s->writeback || s->file == NULL || s->file->type->write == NULL) {
        return 0;
    }
    end = offset + len > s->size ? s->size : offset + len;
    while (offset < end) {
        uint64 chunk = end - offset < PAGE - (offset % PAGE)
                     ? end - offset : PAGE - (offset % PAGE);
        uint64 pos = offset;
        int64 put = s->file->type->write(s->file,
                                         (const uint8 *)phys_to_virt(
                                             s->frames[offset / PAGE]) +
                                             offset % PAGE,
                                         chunk, &pos);

        if (put <= 0) {
            return put < 0 ? (int)put : -5;
        }
        offset += (uint64)put;
    }
    return 0;
}
