#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "screen.h"
#include "typesk.h"

/* The on-disk structures. Marked packed so the compiler emits byte-wise
 * accesses for any member that lands misaligned - which is the correct
 * behaviour regardless of where the caller's buffer came from, and costs
 * nothing on x86. */
typedef struct {
    uint8  e_ident[16];
    uint16 e_type;
    uint16 e_machine;
    uint32 e_version;
    uint64 e_entry;
    uint64 e_phoff;
    uint64 e_shoff;
    uint32 e_flags;
    uint16 e_ehsize;
    uint16 e_phentsize;
    uint16 e_phnum;
    uint16 e_shentsize;
    uint16 e_shnum;
    uint16 e_shstrndx;
} __attribute__((packed)) elf64_ehdr;

typedef struct {
    uint32 p_type;
    uint32 p_flags;
    uint64 p_offset;
    uint64 p_vaddr;
    uint64 p_paddr;
    uint64 p_filesz;
    uint64 p_memsz;
    uint64 p_align;
} __attribute__((packed)) elf64_phdr;

/* If either of these fires, a member picked up padding and every offset past
 * it is wrong - which would present as a plausible-looking but incorrect
 * parse rather than an obvious failure. */
typedef char assert_ehdr[(sizeof(elf64_ehdr) == 64) ? 1 : -1];
typedef char assert_phdr[(sizeof(elf64_phdr) == 56) ? 1 : -1];

#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define ET_EXEC         2
#define ET_DYN          3
#define EM_X86_64       0x3E

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_PHDR         6
#define PT_TLS          7
#define PT_GNU_EH_FRAME 0x6474E550u
#define PT_GNU_STACK    0x6474E551u
#define PT_GNU_RELRO    0x6474E552u

#define PF_X 1
#define PF_W 2
#define PF_R 4

const char *elf_strerror(int rc) {
    switch (rc) {
        case ELF_OK:            return "ok";
        case ELF_ERR_TRUNCATED: return "truncated";
        case ELF_ERR_MAGIC:     return "not an ELF file";
        case ELF_ERR_CLASS:     return "32-bit ELF, need 64-bit";
        case ELF_ERR_ENDIAN:    return "big-endian";
        case ELF_ERR_MACHINE:   return "wrong architecture";
        /* Worth spelling out: modern GCC defaults to PIE, which produces
         * ET_DYN. A position-independent executable has no fixed load address
         * and needs relocations applied, so a simple loader cannot use it.
         * Build test binaries with -static -no-pie. */
        case ELF_ERR_TYPE:      return "not ET_EXEC (build with -static -no-pie)";
        case ELF_ERR_DYNAMIC:   return "dynamically linked (needs an interpreter)";
        case ELF_ERR_INTERP:    return "malformed PT_INTERP path";
        case ELF_ERR_ADDRESS:   return "segment outside user address space";
        case ELF_ERR_NOMEM:     return "out of physical memory";
        default:                return "unknown";
    }
}

static const char *phdr_type_name(uint32 type) {
    switch (type) {
        case PT_NULL:         return "NULL   ";
        case PT_LOAD:         return "LOAD   ";
        case PT_DYNAMIC:      return "DYNAMIC";
        case PT_INTERP:       return "INTERP ";
        case PT_NOTE:         return "NOTE   ";
        case PT_PHDR:         return "PHDR   ";
        case PT_TLS:          return "TLS    ";
        case PT_GNU_EH_FRAME: return "EH_FRME";
        case PT_GNU_STACK:    return "GNUSTCK";
        case PT_GNU_RELRO:    return "RELRO  ";
        default:              return "other  ";
    }
}

/* `allow_dyn` is set only by elf_load_biased, and only because that caller has
 * stated where the image goes. An ET_DYN accepted with no bias loads its
 * segments at their file vaddrs, which for a PIE start at or near zero - so it
 * maps into the null page, or fails the page-0 check below with an error that
 * says ADDRESS when the real problem is TYPE. */
static int validate_ex(const void *image, uint64 size, elf_info_t *info,
                       int allow_dyn) {
    const elf64_ehdr *eh = (const elf64_ehdr *)image;
    const uint8 *base = (const uint8 *)image;
    uint32 i;

    if (size < sizeof(elf64_ehdr)) {
        return ELF_ERR_TRUNCATED;
    }
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        return ELF_ERR_MAGIC;
    }
    if (eh->e_ident[4] != ELFCLASS64)  return ELF_ERR_CLASS;
    if (eh->e_ident[5] != ELFDATA2LSB) return ELF_ERR_ENDIAN;
    if (eh->e_machine  != EM_X86_64)   return ELF_ERR_MACHINE;
    if (eh->e_type != ET_EXEC && !(allow_dyn && eh->e_type == ET_DYN)) {
        return ELF_ERR_TYPE;
    }

    /* Every program header must lie inside the buffer. Checking before
     * dereferencing is the difference between rejecting a malformed file and
     * reading whatever follows it in memory - which for a loader that will
     * eventually take untrusted input is not a theoretical concern. */
    if (eh->e_phoff + (uint64)eh->e_phnum * eh->e_phentsize > size) {
        return ELF_ERR_TRUNCATED;
    }
    if (eh->e_phentsize != sizeof(elf64_phdr)) {
        return ELF_ERR_TRUNCATED;
    }

    info->entry         = eh->e_entry;
    info->load_bias     = 0;
    info->interp[0]     = '\0';
    info->has_interp    = 0;
    info->phnum         = eh->e_phnum;
    info->phentsize     = eh->e_phentsize;
    info->phdr_vaddr    = 0;
    info->lowest_vaddr  = 0xFFFFFFFFFFFFFFFFULL;
    info->highest_vaddr = 0;
    info->load_count    = 0;

    for (i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr *ph =
            (const elf64_phdr *)(base + eh->e_phoff + (uint64)i * eh->e_phentsize);

        if (ph->p_type == PT_INTERP) {
            uint64 k;

            /* Recorded, not rejected. This was ELF_ERR_DYNAMIC, and that was
             * a true statement about what the kernel could do rather than
             * about the file - which is the wrong thing for a validator to
             * encode. execve decides what to do with an interpreter; this
             * function's job is to say what is in the image.
             *
             * The path is bounds-checked against the BUFFER before it is
             * read, and NUL-termination is checked rather than assumed: a
             * p_filesz that runs to the end of the file with no terminator
             * would otherwise leave interp[] unterminated and every later
             * strcmp reading off the end of it. */
            if (ph->p_offset + ph->p_filesz > size) {
                return ELF_ERR_TRUNCATED;
            }
            if (ph->p_filesz == 0 || ph->p_filesz > ELF_INTERP_MAX) {
                return ELF_ERR_INTERP;
            }
            if (base[ph->p_offset + ph->p_filesz - 1] != '\0') {
                return ELF_ERR_INTERP;
            }
            for (k = 0; k < ph->p_filesz; k++) {
                info->interp[k] = (char)base[ph->p_offset + k];
            }
            info->has_interp = 1;
            continue;
        }
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > size) {
            return ELF_ERR_TRUNCATED;
        }

        /* The auxiliary vector must tell libc where the program headers are
         * IN THE PROCESS's address space. They are not a segment of their own
         * here (no PT_PHDR), so find the PT_LOAD whose file range covers
         * e_phoff and translate the offset through it. */
        if (info->phdr_vaddr == 0 &&
            eh->e_phoff >= ph->p_offset &&
            eh->e_phoff < ph->p_offset + ph->p_filesz) {
            info->phdr_vaddr = ph->p_vaddr + (eh->e_phoff - ph->p_offset);
        }

        info->load_count++;
        if (ph->p_vaddr < info->lowest_vaddr) {
            info->lowest_vaddr = ph->p_vaddr;
        }
        if (ph->p_vaddr + ph->p_memsz > info->highest_vaddr) {
            info->highest_vaddr = ph->p_vaddr + ph->p_memsz;
        }
    }

    if (info->load_count == 0) {
        return ELF_ERR_TRUNCATED;
    }
    return ELF_OK;
}

int elf_validate(const void *image, uint64 size, elf_info_t *info) {
    return validate_ex(image, size, info, 0);
}

void elf_report(const void *image, uint64 size, uint8 color) {
    const elf64_ehdr *eh = (const elf64_ehdr *)image;
    const uint8 *base = (const uint8 *)image;
    elf_info_t info;
    uint32 i;
    int rc;

    rc = elf_validate(image, size, &info);
    if (rc != ELF_OK) {
        print_string("elf: ", 0x0C);
        print_string(elf_strerror(rc), 0x0C);
        print_string("\n", 0x0C);
        return;
    }

    print_string("elf: ET_EXEC x86-64  entry ", color);
    print_hex64(info.entry, color);
    print_string("\n", color);

    print_string("  type     vaddr             filesz     memsz      flags\n", 0x07);

    for (i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr *ph =
            (const elf64_phdr *)(base + eh->e_phoff + (uint64)i * eh->e_phentsize);
        char flags[4];

        print_string("  ", color);
        print_string(phdr_type_name(ph->p_type), color);
        print_string("  ", color);
        print_hex64(ph->p_vaddr, color);
        print_string("  ", color);
        print_hex((uint32)ph->p_filesz, color);
        print_string(" ", color);
        print_hex((uint32)ph->p_memsz, color);
        print_string("  ", color);

        flags[0] = (ph->p_flags & PF_R) ? 'R' : '-';
        flags[1] = (ph->p_flags & PF_W) ? 'W' : '-';
        flags[2] = (ph->p_flags & PF_X) ? 'X' : '-';
        flags[3] = '\0';
        print_string(flags, color);

        /* memsz beyond filesz is .bss: pages that exist but have no bytes in
         * the file. The mapping stage MUST zero that tail. Skip it and a
         * static binary's globals start as whatever the frame last held,
         * which fails in ways that look like heap corruption. */
        if (ph->p_type == PT_LOAD && ph->p_memsz > ph->p_filesz) {
            print_string("  bss ", 0x0E);
            print_hex((uint32)(ph->p_memsz - ph->p_filesz), 0x0E);
        }
        print_string("\n", color);
    }

    print_string("  span ", 0x07);
    print_hex64(info.lowest_vaddr, 0x07);
    print_string(" - ", 0x07);
    print_hex64(info.highest_vaddr, 0x07);
    print_string("\n", 0x07);
}

/* Copy `n` bytes to a user virtual address in a space that may not be loaded.
 *
 * This is the function the direct map exists for. `dst` is an address in
 * `as`, which is generally not the space in CR3 - so it cannot simply be
 * dereferenced. Instead each page is translated through that space's own
 * tables and written through the direct map, where every frame in RAM is
 * already addressable regardless of whose page tables describe it.
 *
 * Under recursive mapping this was impossible without switching CR3 to the
 * target space first, which meant execve had to abandon the caller's mappings
 * before it knew the new image would load - and had nothing to go back to if
 * it failed. */
static int copy_to_space(address_space_t *as, uint64 dst,
                         const void *src, uint64 n) {
    const uint8 *in = (const uint8 *)src;
    uint64 done = 0;

    while (done < n) {
        uint64 page   = (dst + done) & ~0xFFFULL;
        uint64 offset = (dst + done) & 0xFFFULL;
        uint64 chunk  = PMM_PAGE_SIZE - offset;
        phys_addr_t phys;
        uint8 *win;
        uint64 i;

        if (chunk > n - done) {
            chunk = n - done;
        }
        phys = vmm_get_phys_in(as, page);
        if (phys == 0) {
            return ELF_ERR_NOMEM;
        }
        win = (uint8 *)phys_to_virt(phys & ~0xFFFULL);
        for (i = 0; i < chunk; i++) {
            win[offset + i] = in[done + i];
        }
        done += chunk;
    }
    return ELF_OK;
}

static int zero_in_space(address_space_t *as, uint64 dst, uint64 n) {
    uint64 done = 0;

    while (done < n) {
        uint64 page   = (dst + done) & ~0xFFFULL;
        uint64 offset = (dst + done) & 0xFFFULL;
        uint64 chunk  = PMM_PAGE_SIZE - offset;
        phys_addr_t phys;
        uint8 *win;
        uint64 i;

        if (chunk > n - done) {
            chunk = n - done;
        }
        phys = vmm_get_phys_in(as, page);
        if (phys == 0) {
            return ELF_ERR_NOMEM;
        }
        win = (uint8 *)phys_to_virt(phys & ~0xFFFULL);
        for (i = 0; i < chunk; i++) {
            win[offset + i] = 0;
        }
        done += chunk;
    }
    return ELF_OK;
}

int elf_load_biased(address_space_t *as, const void *image, uint64 size,
                    uint64 bias, elf_info_t *info) {
    const elf64_ehdr *eh = (const elf64_ehdr *)image;
    const uint8 *base = (const uint8 *)image;
    uint32 i;
    int rc;

    if (as == NULL) {
        return ELF_ERR_ADDRESS;
    }
    /* A bias must be page aligned. An unaligned one would put a segment's
     * page-aligned start at an unaligned address, and every mapping below
     * would silently round it back down - loading the image somewhere other
     * than where the caller was told it went, and where AT_BASE says it is. */
    if ((bias & 0xFFFULL) != 0) {
        return ELF_ERR_ADDRESS;
    }

    rc = validate_ex(image, size, info, bias != 0);
    if (rc != ELF_OK) {
        return rc;
    }

    /* Bias the reported addresses ONCE, here, before anything reads them.
     * The mapping loops below add the bias to each p_vaddr themselves, and
     * doing it to entry/phdr here rather than at the call site is what stops
     * a caller biasing an already-biased entry - base+base+entry is an
     * address that is mapped often enough for the failure to look like
     * something else entirely. */
    info->load_bias = bias;
    if (bias != 0) {
        info->entry         += bias;
        info->lowest_vaddr  += bias;
        info->highest_vaddr += bias;
        if (info->phdr_vaddr != 0) {
            info->phdr_vaddr += bias;
        }
    }

    for (i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr *ph =
            (const elf64_phdr *)(base + eh->e_phoff + (uint64)i * eh->e_phentsize);
        uint64 page, page_start, page_end;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        /* Nothing may land in kernel space. Without this, an ELF claiming
         * p_vaddr = 0xFFFFFFFF80000000 maps straight over the running kernel -
         * and the loader will eventually take input it did not produce.
         * Refusing page 0 keeps null dereferences faulting. */
        if (ph->p_vaddr + bias < 0x1000ULL ||
            ph->p_vaddr + bias + ph->p_memsz > ELF_USER_LIMIT) {
            return ELF_ERR_ADDRESS;
        }

        page_start = (ph->p_vaddr + bias) & ~0xFFFULL;
        page_end   = (ph->p_vaddr + bias + ph->p_memsz + 0xFFFULL) & ~0xFFFULL;

        for (page = page_start; page < page_end; page += PMM_PAGE_SIZE) {
            phys_addr_t phys;

            /* Rounding to page boundaries makes adjacent segments' spans
             * overlap. Allocating a second frame for a page a previous segment
             * already filled would discard its data - silently, since the
             * mapping succeeds. Reuse instead.
             *
             * Physical 0 cannot be a real mapping here: pmm_mark_region_used
             * reserved it at boot, so vmm_get_phys returning 0 unambiguously
             * means "not mapped". */
            if (vmm_get_phys_in(as, page) != 0) {
                continue;
            }
            /* Writable and non-executable while the image is being loaded,
             * which is not the permission the page ends up with - the pass
             * below recomputes both bits from the segment table and remaps.
             * PAGE_RW here is what lets copy_to_space and zero_in_space
             * write the image in at all; a page destined to be read-only
             * .text is read-only only once there is something in it.
             *
             * NX rather than executable for the transient state, so that a
             * failure between here and the permission pass leaves pages that
             * fault on a fetch rather than pages that silently run. */
            phys = vmm_alloc_page_in(as, page,
                                     PAGE_PRESENT | PAGE_RW | PAGE_USER |
                                     PAGE_NX);
            if (phys == 0) {
                return ELF_ERR_NOMEM;
            }
            /* Zero on allocation: covers the padding between page_start and
             * p_vaddr, which belongs to no segment and would otherwise hold
             * whatever the frame last contained. */
            {
                uint8 *win = (uint8 *)phys_to_virt(phys);
                uint64 b;
                for (b = 0; b < PMM_PAGE_SIZE; b++) {
                    win[b] = 0;
                }
            }
        }

        rc = copy_to_space(as, ph->p_vaddr + bias, base + ph->p_offset,
                           ph->p_filesz);
        if (rc != ELF_OK) {
            return rc;
        }

        /* memsz beyond filesz is .bss - pages that exist with no bytes behind
         * them in the file. Skip this and a static binary's globals start as
         * garbage, which presents as heap corruption a long way from here. */
        if (ph->p_memsz > ph->p_filesz) {
            rc = zero_in_space(as, ph->p_vaddr + bias + ph->p_filesz,
                               ph->p_memsz - ph->p_filesz);
            if (rc != ELF_OK) {
                return rc;
            }
        }
    }

    /* --- the per-page permission union ----------------------------------
     *
     * A second pass, and it has to be a second pass. Rounding p_vaddr and
     * p_memsz out to page boundaries makes adjacent segments SHARE their
     * boundary page: the tail of .rodata and the head of .text land in one
     * 4KB frame all the time, because ld only aligns segments to
     * p_align, and the loader above keeps whichever mapping arrived first.
     *
     * So permission cannot be decided while walking segments in order. The
     * first segment to touch a page would win, and if that was the
     * non-executable one the binary faults on a fetch it was entitled to
     * make - a #PF at a valid address inside .text, with nothing in the
     * segment table that looks wrong. This is why the PE loader did not need
     * the same treatment: pe.c validates that SectionAlignment is at least a
     * page, so no two sections ever share one.
     *
     * Both bits are COMPUTED here, per page, as a union over every segment
     * covering that page:
     *
     *     executable  =  any covering segment has PF_X
     *     writable    =  any covering segment has PF_W
     *
     * An earlier version built the executable half by construction instead -
     * map everything NX in the pass above, then clear NX for PF_X segments -
     * and deferred the write half entirely, leaving every page PAGE_RW so
     * .text stayed writable. Doing the write half the same way does not
     * work, and that is why this is a rewrite rather than a third pass: the
     * X union needs its second sub-pass to SET, the W union needs its second
     * sub-pass to CLEAR, and a shared page has to come out both executable
     * and writable-if-either-side-says-so. Two incremental passes in
     * opposite directions have to run in an order neither one states, and
     * vmm_map_page_in writes flags absolutely - it has no way to preserve
     * the bit the other pass just decided. Computing both from the segment
     * table has no ordering to get wrong and is idempotent, so a page
     * revisited through a second covering segment lands on the same answer.
     *
     * The inner scan is O(segments^2) in the worst case. Segments are single
     * digits - four for everything this tree builds - so this is cheaper
     * than the bookkeeping that would avoid it.
     *
     * The frame is reused rather than reallocated: vmm_map_page_in overwrites
     * the leaf entry and nothing else, so the bytes copy_to_space already
     * wrote are untouched. Which is also why this runs after ALL copying:
     * dropping PAGE_RW before copy_to_space would fault on the loader's own
     * write into .text. */
    for (i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr *ph =
            (const elf64_phdr *)(base + eh->e_phoff + (uint64)i * eh->e_phentsize);
        uint64 page, page_start, page_end;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        page_start = (ph->p_vaddr + bias) & ~0xFFFULL;
        page_end   = (ph->p_vaddr + bias + ph->p_memsz + 0xFFFULL) & ~0xFFFULL;

        for (page = page_start; page < page_end; page += PMM_PAGE_SIZE) {
            phys_addr_t phys = vmm_get_phys_in(as, page);
            uint64      flags;
            int         exec  = 0;
            int         write = 0;
            int         j;

            if (phys == 0) {
                /* Cannot happen: the pass above mapped every page of every
                 * PT_LOAD segment. Checked anyway, because the alternative is
                 * mapping physical zero over a page of the image. */
                return ELF_ERR_NOMEM;
            }

            for (j = 0; j < eh->e_phnum; j++) {
                const elf64_phdr *q =
                    (const elf64_phdr *)(base + eh->e_phoff +
                                         (uint64)j * eh->e_phentsize);
                uint64 q_start, q_end;

                if (q->p_type != PT_LOAD || q->p_memsz == 0) {
                    continue;
                }
                q_start = (q->p_vaddr + bias) & ~0xFFFULL;
                q_end   = (q->p_vaddr + bias + q->p_memsz + 0xFFFULL)
                          & ~0xFFFULL;

                if (page < q_start || page >= q_end) {
                    continue;
                }
                if (q->p_flags & PF_X) {
                    exec = 1;
                }
                if (q->p_flags & PF_W) {
                    write = 1;
                }
            }

            /* paging.c strips PAGE_NX when the CPU cannot honour it, so this
             * asks unconditionally and a machine without NX simply does not
             * enforce the execute half. The write half is enforced by the
             * present/RW bits, which every long-mode CPU honours - so on
             * such a machine W^X degrades to W-only rather than to
             * nothing. */
            flags = PAGE_PRESENT | PAGE_USER;
            if (write) {
                flags |= PAGE_RW;
            }
            if (!exec) {
                flags |= PAGE_NX;
            }

            if (!vmm_map_page_in(as, page, phys & ~0xFFFULL, flags)) {
                return ELF_ERR_NOMEM;
            }
        }
    }

    return ELF_OK;
}

int elf_load_into(address_space_t *as, const void *image, uint64 size,
                  elf_info_t *info) {
    return elf_load_biased(as, image, size, 0, info);
}

int elf_load(const void *image, uint64 size, elf_info_t *info) {
    return elf_load_into(vmm_current_space(), image, size, info);
}
