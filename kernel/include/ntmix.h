#ifndef NTMIX_H
#define NTMIX_H

#include "process.h"
#include "typesk.h"

/* A Linux process loading Windows DLLs - ROADMAP item 19, stage 2. See
 * kernel/exec/ntmix.c.
 *
 * Reached through prctl(PR_GENESIS_PE_LOAD, path, out): Linux's own
 * extension point, the way this tree adds every Genesis-specific Linux call.
 * The first call gives the process an NT environment (a PEB, parameters, a
 * TEB for every thread); every call maps the DLL at `path` and whatever it
 * imports that is not already there. `out` then lists the new modules
 * dependencies first, with their entry points, for ring 3 to initialise. */
#define PR_GENESIS_PE_LOAD  0x47454e10u

#define GNT_PE_LOAD_MAX 16

/* What prctl(PR_GENESIS_PE_LOAD) writes back. The same layout is declared
 * in src/libgnt/gnt.h; change both or neither. */
typedef struct {
    uint64 base;                 /* the DLL that was asked for            */
    uint32 count;                /* modules newly mapped by this call     */
    uint32 reserved;
    struct {
        uint64 base;
        uint64 size;
        uint64 entry;            /* absolute DllMain, or 0                */
    } mods[GNT_PE_LOAD_MAX];     /* dependencies first                    */
} gnt_pe_load_out_t;

uint64 nt_genesis_pe_load(uint64 path_ptr, uint64 out_ptr);

/* Give thread `t` a TEB of its own in a free slot and point its GS base at
 * it: every thread of a process with NT gets one, including those clone()
 * makes after the fact. 0 or -errno. */
int nt_thread_teb_attach(process_t *t, uint64 stack_top, uint64 stack_size);

#endif
