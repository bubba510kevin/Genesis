#ifndef SECTION_H
#define SECTION_H

#include "object.h"
#include "paging.h"
#include "typesk.h"

/* NT section objects (ROADMAP 16(l), item 14(d)): memory that exists apart
 * from any address space and is mapped into one or more as VIEWS -
 * CreateFileMapping / MapViewOfFile. The views are ntvm's business
 * (ntvm_map_view); this is the object.
 *
 * A section owns its frames outright, allocated and zeroed when it is
 * created (SEC_COMMIT; SEC_RESERVE is treated the same - the eager-commit
 * rule ntvm.h explains). Every view maps those same frames, so two views -
 * in one process today, in two once there is a second NT process - see one
 * another's writes. A view holds a reference on the section, so the frames
 * outlive the section's last handle for as long as anything maps them.
 *
 * FILE-BACKED sections are filled from the file at creation. A writable one
 * writes its contents back to the file when a view is flushed
 * (FlushViewOfFile) and when the section is destroyed - there is no page
 * cache for a mapping to share, so the file and the mapping agree at those
 * two moments and not continuously. A writable section larger than its file
 * extends the file to the section's size, as NT does. SEC_IMAGE (mapping a
 * PE as an image) is not supported: the PE loader maps images itself. */

#define SEC_IMAGE    0x01000000u
#define SEC_RESERVE  0x04000000u
#define SEC_COMMIT   0x08000000u

/* Make a section of `size` bytes with page protection `protect` (one of
 * READONLY, READWRITE, WRITECOPY, EXECUTE_READ, EXECUTE_READWRITE,
 * EXECUTE_WRITECOPY). With `file` (an object a read can be issued on), the
 * contents come from it and `size` 0 means the file's size. Returns the
 * object (one reference) or NULL with *status set. */
object_t *section_create(uint64 size, uint32 protect, object_t *file,
                         int file_writable, uint32 *status);

/* The section's size and protection; 0, or -22 when obj is not a section. */
int section_info(object_t *obj, uint64 *size, uint32 *protect);

/* The frame behind page `index` of the section, 0 past its end. */
phys_addr_t section_frame(object_t *obj, uint64 index);

/* Write [offset, offset + len) back to the backing file, if the section has
 * one and is writable. 0, or a negative errno from the file. */
int section_flush(object_t *obj, uint64 offset, uint64 len);

#endif
