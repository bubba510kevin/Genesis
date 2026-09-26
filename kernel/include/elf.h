#ifndef ELF_H
#define ELF_H

#include "paging.h"
#include "typesk.h"

/* ELF64 parsing and validation.
 *
 * This stage inspects and reports only - nothing is mapped and nothing is
 * jumped to. Every number it prints has an answer you can check on the host
 * with `readelf -l`, so the parse is proved correct before any of it is used
 * to place pages. The mapping stage that follows is short; getting there with
 * a verified parse is what makes it short.
 */

#define ELF_OK              0
#define ELF_ERR_TRUNCATED  -1   /* file smaller than the headers claim     */
#define ELF_ERR_MAGIC      -2   /* not an ELF at all                       */
#define ELF_ERR_CLASS      -3   /* 32-bit ELF                              */
#define ELF_ERR_ENDIAN     -4   /* big-endian                              */
#define ELF_ERR_MACHINE    -5   /* not x86-64                              */
#define ELF_ERR_TYPE       -6   /* not ET_EXEC - see the note below        */
#define ELF_ERR_DYNAMIC    -7   /* reserved; see elf_info_t::interp        */
#define ELF_ERR_INTERP    -10   /* PT_INTERP present but unreadable        */
#define ELF_ERR_ADDRESS   -8   /* a segment lands outside user space      */
#define ELF_ERR_NOMEM     -9   /* out of physical frames                  */

/* Where a dynamic linker is loaded.
 *
 * A fixed base rather than a search, and the same simplification the PE side
 * made by fixing ImageBase at 0x140000000 and skipping relocations on day one.
 * An rtld is ET_DYN and genuinely position independent, so any aligned address
 * works; picking one means AT_BASE is a constant the loader does not have to
 * negotiate, and the first bug in dynamic linking is not "did the bias
 * arithmetic come out right".
 *
 * 0x7F0000000000 is high enough to be clear of any ET_EXEC's segments and of
 * the heap growing up from them, and low enough to stay well under
 * ELF_USER_LIMIT. It is NOT where the stack is - USER_STACK_TOP is separate
 * and lower - and the gap between them is deliberate: an rtld that runs off
 * the end of its own mapping should fault, not land on the stack. */
#define ELF_INTERP_BASE   0x00007F0000000000ULL

/* Longest PT_INTERP path accepted. Anything longer is a malformed image
 * rather than a deeply nested interpreter path. */
#define ELF_INTERP_MAX    128

/* Highest address a loaded segment may touch. Everything at or above this is
 * either the non-canonical hole or kernel space, and a binary claiming a
 * p_vaddr up there would map straight over the running kernel. */
#define ELF_USER_LIMIT    0x0000800000000000ULL

typedef struct {
    uint64 entry;          /* e_entry                                      */
    uint64 lowest_vaddr;   /* start of the lowest PT_LOAD                  */
    uint64 highest_vaddr;  /* end of the highest PT_LOAD, memsz included   */
    uint32 load_count;     /* number of PT_LOAD segments                   */
    uint64 phdr_vaddr;     /* where the program headers are mapped         */
    uint64 phnum;          /* how many, for AT_PHNUM                       */
    uint64 phentsize;

    /* The bias every vaddr in this image was loaded at. Zero for an ET_EXEC,
     * which is why nothing needed it before. Carried on the info rather than
     * recomputed by the caller, because entry and phdr_vaddr are already
     * biased when they come out of here - a caller that biased them again
     * would jump to base+base+entry, and that address is mapped often enough
     * for the failure to look like something else. */
    uint64 load_bias;

    /* PT_INTERP, if there is one. Empty for a static binary.
     *
     * A field rather than the ELF_ERR_DYNAMIC rejection that used to be here.
     * The rejection was correct as a statement about what the kernel could
     * do; it stopped being correct as a statement about the FILE, and those
     * are different things to encode in a validator. */
    char   interp[ELF_INTERP_MAX];
    int    has_interp;
} elf_info_t;

/* Validate the headers and fill in `info`. Does not touch memory outside the
 * supplied image. */
int elf_validate(const void *image, uint64 size, elf_info_t *info);

/* Print the header summary and one line per program header. Intended to be
 * diffed against `readelf -l` output. */
void elf_report(const void *image, uint64 size, uint8 color);

/* Validate, then map every PT_LOAD into the CURRENT address space and copy
 * the image in. Does not jump. On success info->entry is where to go.
 *
 * Maps into whatever CR3 currently points at, which today is the kernel's own
 * address space - so a loaded binary and the kernel share one page table
 * tree. That is fine for a jump-and-halt test and is exactly what per-process
 * PML4s exist to fix. */
int elf_load(const void *image, uint64 size, elf_info_t *info);

/* The same, into an address space that need not be the one in CR3.
 *
 * Pages are mapped in `as` and written through the direct map rather than by
 * dereferencing the target address, so the caller's own mappings stay intact
 * throughout. That is what lets execve build the replacement imagebefore
 * committing to it: if this fails, the old space is still there to return an
 * errno into. */
int elf_load_into(address_space_t *as, const void *image, uint64 size,
                  elf_info_t *info);

/* The same, biased by `bias` - which is what loading an ET_DYN means.
 *
 * elf_load_into is this with a bias of zero, and an ET_DYN is rejected there
 * for the reason it always was: an ET_DYN loaded at zero has segments at
 * page 0 and jumps into the null page. It is accepted HERE, where a non-zero
 * bias is an explicit statement by the caller about where the image goes.
 *
 * The interpreter is the only caller today. A PIE main executable is the same
 * operation with a different bias, which is the reason this takes a parameter
 * rather than hardcoding ELF_INTERP_BASE. */
int elf_load_biased(address_space_t *as, const void *image, uint64 size,
                    uint64 bias, elf_info_t *info);

/* Human-readable form of an ELF_ERR_* code. */
const char *elf_strerror(int rc);

#endif
