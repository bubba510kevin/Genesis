#ifndef ELFSO_H
#define ELFSO_H

#include "paging.h"
#include "typesk.h"

/* Linux shared objects loaded into a running process - ROADMAP item 19,
 * stage 3. See kernel/exec/elfso.c. */

#define ELFSO_OK    0
#define ELFSO_ERR  -1

/* Where shared objects go: 256GB up, well clear of everything else a
 * Genesis process maps (images at 5-8GB, the NT blocks under 1.5GB, the
 * user stack at 1GB) and far below PE_USER_LIMIT. */
#define ELFSO_BASE        0x0000004000000000ULL
#define ELFSO_LIMIT       0x0000008000000000ULL
#define ELFSO_STEP        0x0000000001000000ULL
#define ELFSO_MAX_PAGES   4096                    /* 16MB per object */

/* New objects one load may bring in, and objects the process may hold. */
#define ELFSO_MAX_OBJECTS 8

typedef struct {
    uint64 base;
    uint64 size;
} elfso_loaded_t;

typedef struct {
    uint64 base;             /* the load bias: the ELF header is here       */
    uint64 size;
    uint64 init;             /* DT_INIT, absolute, or 0                     */
    uint64 init_array;       /* DT_INIT_ARRAY, absolute, or 0               */
    uint32 init_count;
} elfso_module_t;

typedef struct {
    uint64 base;             /* the object asked for                        */
    int    count;            /* newly mapped, dependencies first            */
    elfso_module_t mods[ELFSO_MAX_OBJECTS];
} elfso_result_t;

typedef int  (*elfso_reader_t)(const char *path, uint8 **out, uint32 *size);
typedef void (*elfso_release_t)(uint8 *buf);

/* Map the shared object at `path` into `as` (the CALLING process's own
 * space), with every DT_NEEDED not already loaded - /lib first, then the
 * first object's directory - and relocate them all eagerly against one
 * scope: the new objects in load order, then `loaded` (what the process
 * already has). An object already loaded maps nothing and returns its base.
 * On failure the reason is printed and nothing new stays mapped. */
int elfso_load_library(address_space_t *as, const char *path,
                       const elfso_loaded_t *loaded, int nloaded,
                       elfso_reader_t reader, elfso_release_t release,
                       elfso_result_t *out);

#endif
