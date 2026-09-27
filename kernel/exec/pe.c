#include "ntoskrnl_exports.h"
#include "paging.h"
#include "pe.h"
#include "pmm.h"
#include "screen.h"
#include "typesk.h"
#include "vmalloc.h"

/* See pe.h's PE_DRIVER_BASE comment: this is the actual window
 * load_image uses for a kernel-mode load, reserved from the kernel VA
 * allocator by pe_driver_window_init() (ROADMAP item 11). Defaults to the
 * literal constant so a load before that call, or after a failed
 * reservation, still works. */
static uint64 driver_window_base = PE_DRIVER_BASE;

void pe_driver_window_init(void) {
    uint64 allocated = kvm_alloc_range(DRIVER_IMAGE_MAX_SIZE, PMM_PAGE_SIZE);
    if (allocated != 0) {
        driver_window_base = allocated;
    }
}

/* See pe.h for what this is and where the structures come from. */

static const pe_coff_header_t *coff_of(const void *image, uint64 size) {
    const uint8 *base = (const uint8 *)image;
    const pe_dos_header_t *dos;
    uint64 off;

    if (image == NULL || size < sizeof(pe_dos_header_t)) {
        return NULL;
    }
    dos = (const pe_dos_header_t *)image;
    if (dos->e_magic != PE_DOS_MAGIC) {
        return NULL;
    }
    off = dos->e_lfanew;

    /* e_lfanew is attacker-controlled in exactly the same sense e_phoff is:
     * it is a file offset read out of the file. Check it before adding it to
     * a pointer, not after. */
    if (off + 4 + sizeof(pe_coff_header_t) > size) {
        return NULL;
    }
    if (*(const uint32 *)(base + off) != PE_NT_SIGNATURE) {
        return NULL;
    }
    return (const pe_coff_header_t *)(base + off + 4);
}

int pe_is_pe(const void *image, uint64 size) {
    return coff_of(image, size) != NULL;
}

static const pe_opt_header64_t *opt_of(const pe_coff_header_t *coff) {
    return (const pe_opt_header64_t *)((const uint8 *)coff +
                                       sizeof(pe_coff_header_t));
}

static const pe_section_header_t *sections_of(const pe_coff_header_t *coff) {
    /* After the optional header, whose length the COFF header states rather
     * than the reader assuming. A linker may emit a longer one than this
     * kernel knows about, and walking past it by sizeof() rather than by
     * SizeOfOptionalHeader is how a section table gets read as garbage. */
    return (const pe_section_header_t *)((const uint8 *)coff +
                                         sizeof(pe_coff_header_t) +
                                         coff->size_of_optional_header);
}

static int validate(const void *image, uint64 size, pe_info_t *info,
                    int allow_dll, int kernel_mode) {
    const pe_coff_header_t *coff = coff_of(image, size);
    const pe_opt_header64_t *opt;
    const pe_section_header_t *sec;
    uint64 sec_off;
    uint32 i;

    if (info == NULL) {
        return PE_ERR_MAGIC;
    }
    if (coff == NULL) {
        return PE_ERR_MAGIC;
    }
    if (coff->machine != PE_MACHINE_AMD64) {
        return PE_ERR_MACHINE;
    }
    if (coff->size_of_optional_header < sizeof(pe_opt_header64_t)) {
        return PE_ERR_TRUNCATED;
    }
    if ((const uint8 *)coff + sizeof(pe_coff_header_t) +
        coff->size_of_optional_header > (const uint8 *)image + size) {
        return PE_ERR_TRUNCATED;
    }

    opt = opt_of(coff);
    if (opt->magic != PE_OPT_MAGIC_64) {
        /* PE32 rather than PE32+. The difference is not cosmetic: ImageBase
         * is 4 bytes there and every field after it shifts, so reading this
         * struct off a PE32 image gives plausible nonsense rather than an
         * obvious failure. */
        return PE_ERR_OPTMAGIC;
    }
    if ((coff->characteristics & PE_FILE_EXECUTABLE_IMAGE) == 0) {
        return PE_ERR_NOTEXE;
    }
    /* A DLL is a perfectly good image and a bad thing to execve. The
     * difference is who is asking: the loader resolving an import wants one,
     * a process calling execve does not. */
    if (!allow_dll && (coff->characteristics & PE_FILE_DLL) != 0) {
        return PE_ERR_NOTEXE;
    }
    /* A kernel-mode driver image is required to say so explicitly - the
     * one check pe_validate/pe_load_into/pe_load_executable never make,
     * since `subsystem` otherwise goes unread by this file entirely.
     * Refusing a normal GUI/CUI-subsystem EXE/DLL here is what stops
     * pe_load_driver from ever mapping an ordinary userspace image into
     * kernel address space by mistake. */
    if (kernel_mode && opt->subsystem != PE_SUBSYSTEM_NATIVE) {
        return PE_ERR_SUBSYSTEM;
    }

    /* Sections must be page-granular for their characteristics to mean
     * anything: two sections sharing a page can only have one set of
     * permissions, and silently granting the union is how a read-only section
     * becomes writable. Refusing is honest; 0x1000 is what every linker
     * emits for an image anyway. */
    if (opt->section_alignment < PMM_PAGE_SIZE ||
        (opt->section_alignment & (PMM_PAGE_SIZE - 1)) != 0) {
        return PE_ERR_ALIGN;
    }
    if (opt->size_of_image == 0) {
        return PE_ERR_TRUNCATED;
    }
    if (opt->size_of_headers > size) {
        return PE_ERR_TRUNCATED;
    }

    sec_off = (uint64)((const uint8 *)sections_of(coff) - (const uint8 *)image);
    if (sec_off + (uint64)coff->number_of_sections *
        sizeof(pe_section_header_t) > size) {
        return PE_ERR_TRUNCATED;
    }

    sec = sections_of(coff);
    for (i = 0; i < coff->number_of_sections; i++) {
        uint32 j;

        /* Raw ranges must not overlap each other.
         *
         * The spec does not spell this out, because no linker has ever needed
         * to say it. A generator can produce it easily though: hardcode the
         * file offsets, let one section outgrow its slot, and the header now
         * describes a .rdata that begins in the middle of .text. Everything
         * downstream then works perfectly - this loader maps exactly what it
         * was told, and the image reads its own string constants as machine
         * code. What comes back is STATUS_ACCESS_VIOLATION from a pointer
         * check several layers away, with nothing pointing here.
         *
         * The loader knows enough to say so, so it says so. Refusing is the
         * same call as for forwarder exports and ordinal imports: an image
         * this one cannot honestly load is rejected by name rather than
         * loaded approximately. */
        if (sec[i].size_of_raw_data == 0) {
            continue;
        }
        for (j = 0; j < i; j++) {
            uint64 a_start, a_end, b_start, b_end;

            if (sec[j].size_of_raw_data == 0) {
                continue;
            }
            a_start = sec[i].pointer_to_raw_data;
            a_end   = a_start + sec[i].size_of_raw_data;
            b_start = sec[j].pointer_to_raw_data;
            b_end   = b_start + sec[j].size_of_raw_data;
            if (a_start < b_end && b_start < a_end) {
                return PE_ERR_SECTION;
            }
        }
    }

    for (i = 0; i < coff->number_of_sections; i++) {
        /* A section with no bytes in the file - .bss - legitimately has
         * PointerToRawData meaningless. Only check the extent of sections
         * that claim to have content. */
        if (sec[i].size_of_raw_data != 0) {
            if ((uint64)sec[i].pointer_to_raw_data +
                (uint64)sec[i].size_of_raw_data > size) {
                return PE_ERR_SECTION;
            }
        }
        /* The RVA has to land inside the image the header describes, or the
         * section maps outside the range every other check was made against. */
        if ((uint64)sec[i].virtual_address +
            (uint64)sec[i].virtual_size > (uint64)opt->size_of_image +
                                          opt->section_alignment) {
            return PE_ERR_SECTION;
        }
        /* And it has to sit ON a SectionAlignment boundary. The check above
         * on SectionAlignment itself is necessary and not sufficient: it
         * constrains the spacing between sections, not where any one of them
         * begins. An RVA that is not a multiple of it puts two sections in
         * one page after all, and map_range keeps whichever mapping arrived
         * first - so the second section would silently run under the first
         * one's permissions.
         *
         * That was a cosmetic complaint while only the write bit was
         * enforced. With NX it is the difference between a data page and an
         * executable one, and it is the reason this loader can use one
         * flags word per section where the ELF loader will need a per-page
         * union. Every linker emits aligned RVAs; refusing one that does not
         * costs nothing and keeps the guarantee true rather than assumed. */
        if ((sec[i].virtual_address & (opt->section_alignment - 1)) != 0) {
            return PE_ERR_ALIGN;
        }
    }

    /* An entry point of zero is legal for a DLL - it simply has no
     * initialisation to run - and meaningless for an executable, where it
     * would mean jumping at the DOS header. */
    if (opt->address_of_entry_point >= opt->size_of_image ||
        (!allow_dll && opt->address_of_entry_point == 0)) {
        return PE_ERR_NOTEXE;
    }

    info->preferred_base = opt->image_base;
    info->image_base     = opt->image_base;
    info->image_size     = opt->size_of_image;
    info->entry          = opt->image_base + opt->address_of_entry_point;
    info->highest_vaddr  = opt->image_base + opt->size_of_image;
    info->section_count  = coff->number_of_sections;
    info->relocated      = 0;
    info->has_imports    = opt->number_of_rva_and_sizes > PE_DIR_IMPORT &&
                           opt->directory[PE_DIR_IMPORT].size != 0;
    info->thread_start   = 0;    /* set only by pe_load_executable's link */
    info->apc_dispatcher = 0;
    info->exception_dispatcher = 0;
    return PE_OK;
}

int pe_validate(const void *image, uint64 size, pe_info_t *info) {
    return validate(image, size, info, 0, 0);
}

/* --- writing into a space that is not in CR3 -----------------------------
 * The same technique elf.c uses, and for the same reason: execve builds the
 * replacement image before committing to it, so a malformed file has to stay
 * an errno rather than a half-destroyed address space. */

static int copy_in(address_space_t *as, uint64 dst, const void *src, uint64 n) {
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
            return PE_ERR_NOMEM;
        }
        win = (uint8 *)phys_to_virt(phys & ~0xFFFULL);
        for (i = 0; i < chunk; i++) {
            win[offset + i] = in[done + i];
        }
        done += chunk;
    }
    return PE_OK;
}

/* Read back out of the target space. Only base relocation needs this - it is
 * a read-modify-write of an address the image already contains. Byte-wise
 * because a 64-bit value can straddle a page boundary and the two halves are
 * not adjacent in physical memory. */
static int read_out(address_space_t *as, uint64 src, void *dst, uint64 n) {
    uint8 *out = (uint8 *)dst;
    uint64 done = 0;

    while (done < n) {
        uint64 page   = (src + done) & ~0xFFFULL;
        uint64 offset = (src + done) & 0xFFFULL;
        uint64 chunk  = PMM_PAGE_SIZE - offset;
        phys_addr_t phys;
        const uint8 *win;
        uint64 i;

        if (chunk > n - done) {
            chunk = n - done;
        }
        phys = vmm_get_phys_in(as, page);
        if (phys == 0) {
            return PE_ERR_NOMEM;
        }
        win = (const uint8 *)phys_to_virt(phys & ~0xFFFULL);
        for (i = 0; i < chunk; i++) {
            out[done + i] = win[offset + i];
        }
        done += chunk;
    }
    return PE_OK;
}

/* Map and zero [start, end), skipping pages another section already made.
 * Sections are page-aligned by the check in pe_validate, so an overlap can
 * only be the header page - but reusing rather than reallocating is what
 * stops a second mapping from discarding the first one's contents. */
static int map_range(address_space_t *as, uint64 start, uint64 end,
                     uint64 flags) {
    uint64 page;

    for (page = start; page < end; page += PMM_PAGE_SIZE) {
        phys_addr_t phys;
        uint8 *win;
        uint64 b;

        if (vmm_get_phys_in(as, page) != 0) {
            continue;
        }
        phys = vmm_alloc_page_in(as, page, flags);
        if (phys == 0) {
            return PE_ERR_NOMEM;
        }
        /* Zero on allocation. A section's VirtualSize routinely exceeds its
         * SizeOfRawData - that difference IS .bss for a PE - and uninitialised
         * globals presenting as heap corruption is a long walk back to here. */
        win = (uint8 *)phys_to_virt(phys);
        for (b = 0; b < PMM_PAGE_SIZE; b++) {
            win[b] = 0;
        }
    }
    return PE_OK;
}

/* Page flags from the section's characteristics.
 *
 * Both halves are enforced now. The write bit was always straightforward; the
 * execute bit waited on EFER.NXE, because bit 63 of a PTE is no-execute only
 * once NXE is set and is a RESERVED bit before that - which faults on every
 * access rather than on a fetch, and reports a cause that reads like a
 * corrupt page table. paging.c strips PAGE_NX when the CPU cannot honour it,
 * so this asks for it unconditionally and a machine without NX simply does
 * not enforce.
 *
 * Note which way round the question is asked. A section is non-executable
 * unless it says otherwise, so a characteristics word this loader does not
 * understand yields a page that faults on fetch rather than one that runs.
 * The other default is the one that turns a malformed section table into an
 * executable heap.
 *
 * The ELF loader still maps every segment executable. That is a separate
 * change and a harder one: rounding to page boundaries makes adjacent
 * segments share a boundary page, elf_load_into keeps whichever mapping got
 * there first, and a page holding the tail of .rodata and the head of .text
 * would inherit NX and fault on a fetch the binary was entitled to make. It
 * needs a per-page union of the segments' permissions, not this function's
 * shape. PE gets away without that because section_alignment is validated to
 * be at least a page, so no two sections ever share one. */
static uint64 flags_for(uint32 characteristics, int kernel_mode) {
    uint64 flags = PAGE_PRESENT | (kernel_mode ? 0 : PAGE_USER);

    if (characteristics & PE_SCN_MEM_WRITE) {
        flags |= PAGE_RW;
    }
    if ((characteristics & PE_SCN_MEM_EXECUTE) == 0) {
        flags |= PAGE_NX;
    }
    return flags;
}

/* --- base relocation -----------------------------------------------------
 *
 * .reloc is a list of blocks. Each is an RVA, a byte length covering the
 * whole block including its own 8-byte head, and then 16-bit entries: four
 * bits of type, twelve of offset within the 4KB page the RVA names. For
 * x86-64 the only type that does anything is DIR64 - add the delta to the
 * 64-bit value there. ABSOLUTE is padding to a 4-byte boundary and is
 * skipped, which is not an optimisation: treating it as an offset of zero
 * would add the delta to the first eight bytes of the page. */
static int apply_relocations(address_space_t *as, const void *image,
                             uint64 size, const pe_opt_header64_t *opt,
                             uint64 load_base, int64 delta) {
    const uint8 *file = (const uint8 *)image;
    uint32 dir_rva, dir_size, walked = 0;

    if (delta == 0) {
        return PE_OK;
    }
    if (opt->number_of_rva_and_sizes <= PE_DIR_BASERELOC) {
        return PE_ERR_RELOC;
    }
    dir_rva  = opt->directory[PE_DIR_BASERELOC].virtual_address;
    dir_size = opt->directory[PE_DIR_BASERELOC].size;
    if (dir_rva == 0 || dir_size == 0) {
        /* Stripped. The image can only run where it asked to, and it cannot
         * have that address - so this is a hard failure and not something to
         * paper over by loading it anyway. */
        return PE_ERR_RELOC;
    }

    /* The directory is read from the MAPPED image rather than the file,
     * because .reloc is a section like any other and its file offset and its
     * RVA are different numbers. */
    while (walked + 8 <= dir_size) {
        uint32 block_rva, block_size, entries, e;
        uint8  head[8];

        if (read_out(as, load_base + dir_rva + walked, head, 8) != PE_OK) {
            return PE_ERR_RELOC;
        }
        block_rva  = *(const uint32 *)(head + 0);
        block_size = *(const uint32 *)(head + 4);

        if (block_size < 8 || walked + block_size > dir_size) {
            return PE_ERR_RELOC;
        }
        entries = (block_size - 8) / 2;

        for (e = 0; e < entries; e++) {
            uint8  raw[2];
            uint16 entry;
            uint32 type, offset;
            uint64 target;
            uint64 value;

            if (read_out(as, load_base + dir_rva + walked + 8 + (uint64)e * 2,
                         raw, 2) != PE_OK) {
                return PE_ERR_RELOC;
            }
            entry  = (uint16)(raw[0] | ((uint16)raw[1] << 8));
            type   = (uint32)(entry >> 12);
            offset = (uint32)(entry & 0x0FFFu);

            if (type == PE_REL_ABSOLUTE) {
                continue;
            }
            if (type != PE_REL_DIR64) {
                /* Anything else on x86-64 means the file was built for a
                 * machine this loader already rejected, or is malformed.
                 * Guessing is worse than refusing. */
                return PE_ERR_RELOC;
            }
            if ((uint64)block_rva + offset + 8 > (uint64)opt->size_of_image) {
                return PE_ERR_RELOC;
            }
            target = load_base + block_rva + offset;
            if (read_out(as, target, &value, 8) != PE_OK) {
                return PE_ERR_RELOC;
            }
            value = (uint64)((int64)value + delta);
            if (copy_in(as, target, &value, 8) != PE_OK) {
                return PE_ERR_RELOC;
            }
        }
        walked += block_size;
    }
    (void)file;
    (void)size;
    return PE_OK;
}

static int load_image(address_space_t *as, const void *image, uint64 size,
                      pe_info_t *info, int allow_dll, int kernel_mode) {
    const pe_coff_header_t *coff;
    const pe_opt_header64_t *opt;
    const pe_section_header_t *sec;
    const uint8 *file = (const uint8 *)image;
    uint64 load_base, align, addr_limit;
    int64  delta;
    uint32 i;
    int rc;

    if (as == NULL) {
        return PE_ERR_ADDRESS;
    }
    rc = validate(image, size, info, allow_dll, kernel_mode);
    if (rc != PE_OK) {
        return rc;
    }
    coff = coff_of(image, size);
    opt  = opt_of(coff);
    sec  = sections_of(coff);

    /* Where it goes. The preferred base is used when it fits - for a
     * userspace image that's the 0x140000000 every 64-bit linker defaults
     * to, 5GB, above the user stack at 1GB and far below PE_USER_LIMIT.
     * For a kernel-mode driver, "fits" means falling inside
     * PE_DRIVER_BASE's window instead - a real .sys's preferred ImageBase
     * is never a kernel-half address, so this branch takes the fallback
     * almost every time, which is the routine case here, not an edge one
     * (see pe.h's PE_DRIVER_BASE comment). Page 0 stays unmapped either
     * way so a null dereference still faults. */
    addr_limit = kernel_mode ? (driver_window_base + DRIVER_IMAGE_MAX_SIZE)
                             : PE_USER_LIMIT;
    load_base = info->preferred_base;
    if (kernel_mode) {
        if (load_base < driver_window_base || load_base + info->image_size > addr_limit ||
            (load_base & (PMM_PAGE_SIZE - 1)) != 0) {
            load_base = driver_window_base;
            if (load_base + info->image_size > addr_limit) {
                return PE_ERR_ADDRESS;
            }
        }
    } else {
        if (load_base < PMM_PAGE_SIZE || load_base + info->image_size > addr_limit ||
            (load_base & (PMM_PAGE_SIZE - 1)) != 0) {
            load_base = PE_FALLBACK_BASE;
            if (load_base + info->image_size > addr_limit) {
                return PE_ERR_ADDRESS;
            }
        }
    }
    delta = (int64)load_base - (int64)info->preferred_base;

    /* The headers are mapped, read-only, at the base. Nothing in a no-import
     * image reads them - but a PE is entitled to find its own headers at
     * ImageBase, and LdrInitializeThunk will walk them the moment there is
     * an ntdll to run. Cheap now, awkward to retrofit. PAGE_USER is dropped
     * entirely for a kernel-mode load - ring 3 has no business touching a
     * driver's headers. */
    align = opt->section_alignment;
    rc = map_range(as, load_base,
                   load_base + ((opt->size_of_headers + align - 1) & ~(align - 1)),
                   PAGE_PRESENT | (kernel_mode ? 0 : PAGE_USER) | PAGE_NX);
    if (rc != PE_OK) {
        return rc;
    }
    rc = copy_in(as, load_base, file, opt->size_of_headers);
    if (rc != PE_OK) {
        return rc;
    }

    for (i = 0; i < coff->number_of_sections; i++) {
        uint64 va    = load_base + sec[i].virtual_address;
        uint64 vsize = sec[i].virtual_size;
        uint64 fsize = sec[i].size_of_raw_data;
        uint64 start, end;

        /* VirtualSize of zero happens in object files and in sections a
         * linker emitted without meaning to; the raw size is then the only
         * length there is. */
        if (vsize == 0) {
            vsize = fsize;
        }
        if (vsize == 0) {
            continue;
        }
        /* Never copy more than the section will occupy in memory. A raw size
         * larger than the virtual size is normal - it is FileAlignment
         * padding - and copying it would spill into the next section. */
        if (fsize > vsize) {
            fsize = vsize;
        }
        if (sec[i].characteristics & PE_SCN_CNT_UNINITIALIZED) {
            fsize = 0;                   /* .bss: nothing in the file       */
        }

        if (va < PMM_PAGE_SIZE || va + vsize > addr_limit) {
            return PE_ERR_ADDRESS;
        }

        start = va & ~0xFFFULL;
        end   = (va + vsize + 0xFFFULL) & ~0xFFFULL;
        rc = map_range(as, start, end,
                       flags_for(sec[i].characteristics, kernel_mode));
        if (rc != PE_OK) {
            return rc;
        }
        if (fsize > 0) {
            rc = copy_in(as, va, file + sec[i].pointer_to_raw_data, fsize);
            if (rc != PE_OK) {
                return rc;
            }
        }
        /* The tail beyond the raw data is already zero: map_range zeroes
         * every frame it allocates, and nothing has written there since. */
    }

    rc = apply_relocations(as, image, size, opt, load_base, delta);
    if (rc != PE_OK) {
        return rc;
    }

    info->image_base    = load_base;
    info->entry         = load_base + opt->address_of_entry_point;
    info->highest_vaddr = load_base + info->image_size;
    info->relocated     = (delta != 0);
    return PE_OK;
}


/* --- linking -------------------------------------------------------------
 *
 * An image with no imports is finished the moment its sections are mapped.
 * One with imports has a table of addresses that are still zero and code
 * that will jump through them, so loading it without resolving them produces
 * a program that runs until its first library call and then jumps to zero.
 *
 * The shape, from the spec: the import directory is an array of descriptors
 * terminated by an all-zero entry, one per DLL. Each names a DLL and holds
 * two parallel arrays of thunks - OriginalFirstThunk, the hint/name table,
 * and FirstThunk, the IAT. They start identical; the loader overwrites the
 * IAT with real addresses and leaves the name table alone, which is what
 * makes a second pass possible and what lets a debugger still say which
 * function an entry was.
 *
 * OriginalFirstThunk can be zero, in which case the IAT itself is still the
 * name table when the loader arrives - so read names from it and write
 * addresses over them, in that order, one entry at a time. Reading all the
 * names first and then writing works too and needs a buffer; this does not.
 */

struct module {
    char   name[32];
    uint64 base;
};

struct link_ctx {
    address_space_t  *as;
    pe_file_reader_t  reader;
    pe_file_release_t release;
    struct module     modules[PE_MAX_MODULES];
    int               count;
    int               depth;
    int               kernel_mode;  /* see pe_load_driver */
};

/* An ASCII string out of the target address space. Bounded by the buffer,
 * because the length comes from the file. */
static int read_str(address_space_t *as, uint64 va, char *out, uint64 cap) {
    uint64 i;

    for (i = 0; i + 1 < cap; i++) {
        uint8 c;

        if (read_out(as, va + i, &c, 1) != PE_OK) {
            return 0;
        }
        out[i] = (char)c;
        if (c == 0) {
            return 1;
        }
    }
    out[cap - 1] = '\0';
    return 0;                            /* longer than anything we name */
}

static int name_eq_ci(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;

        if (x != y) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int str_copy_n(char *dst, const char *src, uint64 cap) {
    uint64 i;

    for (i = 0; i + 1 < cap && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return src[i] == '\0';
}

static uint64 module_lookup(const struct link_ctx *ctx, const char *name) {
    int i;

    for (i = 0; i < ctx->count; i++) {
        if (name_eq_ci(ctx->modules[i].name, name)) {
            return ctx->modules[i].base;
        }
    }
    return 0;
}

/* The export directory. Two indirections and not one: AddressOfNames[i] gives
 * a name, AddressOfNameOrdinals[i] gives the index into AddressOfFunctions
 * for that name. Indexing the function table with i directly works for a DLL
 * that exports everything by name in order and quietly returns a different
 * function for one that does not. */
static int load_dependency(struct link_ctx *ctx, const char *name,
                           uint64 *base_out);

/* Join a bare module name and an extension: "NTOSKRNL" + ".exe". Refuses
 * rather than truncating - a truncated DLL name would be looked up, not
 * found, and reported as a missing dependency whose name is subtly wrong. */
static int dll_name_with_suffix(char *dst, uint32 dst_size, const char *name,
                                const char *suffix) {
    uint32 n = 0, k;

    while (name[n] != '\0') {
        n++;
    }
    for (k = 0; suffix[k] != '\0'; k++) {
        /* counted, not assumed */
    }
    if (n + k + 1 > dst_size) {
        return 0;
    }
    for (k = 0; k < n; k++) {
        dst[k] = name[k];
    }
    for (n = 0; suffix[n] != '\0'; n++) {
        dst[k + n] = suffix[n];
    }
    dst[k + n] = '\0';
    return 1;
}

/* One lookup for both keys. `want` names the export, or is NULL for a lookup
 * by `want_ordinal` - which is an ORDINAL, not an index: the index into
 * AddressOfFunctions is ordinal - ordinal_base, and a DLL whose ordinal_base
 * is not 1 (they exist) resolves to the wrong function every time if that
 * subtraction is skipped.
 *
 * `depth` bounds forwarder chains. A forwarder may point at a DLL that
 * forwards again, legitimately - but it may also point back at itself, and
 * there is no rule in the format forbidding it.
 */
static int find_export_ex(struct link_ctx *ctx, uint64 dll_base,
                          const char *want, uint32 want_ordinal, uint64 *out,
                          int depth);

/* Follow "OTHERDLL.Function" or "OTHERDLL.#123".
 *
 * The string sits inside the export directory where a function RVA would be,
 * which is the only thing that distinguishes a forwarder from a real export -
 * there is no flag. That is why the range test below is the whole detection
 * mechanism, and why it has to use the directory's SIZE and not just its
 * start.
 *
 * Note the forwarded-to name carries no extension: NTDLL exports forwarded to
 * "ntoskrnl.RtlUnwind" say "NTOSKRNL". Windows appends ".dll"; ntoskrnl is
 * ".exe" and is special-cased there too. Both suffixes are tried here rather
 * than picking one, because guessing wrong produces "DLL not found" for a
 * name that is right, which is a confusing way to fail. */
static int follow_forwarder(struct link_ctx *ctx, const char *fwd, uint64 *out,
                            int depth) {
    char   dll[64];
    char   sym[128];
    uint64 target_base = 0;
    uint32 i = 0, j;
    int    rc;

    while (fwd[i] != '\0' && fwd[i] != '.') {
        if (i + 1 >= sizeof(dll)) {
            return 0;
        }
        dll[i] = fwd[i];
        i++;
    }
    if (fwd[i] != '.') {
        return 0;                        /* no separator - not a forwarder */
    }
    dll[i] = '\0';
    i++;                                 /* past the '.' */

    for (j = 0; fwd[i + j] != '\0'; j++) {
        if (j + 1 >= sizeof(sym)) {
            return 0;
        }
        sym[j] = fwd[i + j];
    }
    if (j == 0) {
        return 0;                        /* "DLL." with nothing after it */
    }
    sym[j] = '\0';

    /* "#123" - forwarded to an ordinal rather than a name. */
    if (sym[0] == '#') {
        uint32 ord = 0;

        for (j = 1; sym[j] != '\0'; j++) {
            if (sym[j] < '0' || sym[j] > '9') {
                return 0;
            }
            ord = ord * 10 + (uint32)(sym[j] - '0');
        }
        if (j == 1) {
            return 0;                    /* a bare '#' */
        }
        {
            char cand[68];
            if (!dll_name_with_suffix(cand, sizeof(cand), dll, ".dll")) {
                return 0;
            }
            if (ctx->kernel_mode && nt_is_synthetic_dll(cand)) {
                return 0;                /* see the ordinal note below */
            }
            if (load_dependency(ctx, cand, &target_base) != PE_OK) {
                if (!dll_name_with_suffix(cand, sizeof(cand), dll, ".exe") ||
                    load_dependency(ctx, cand, &target_base) != PE_OK) {
                    return 0;
                }
            }
        }
        return find_export_ex(ctx, target_base, NULL, ord, out, depth + 1);
    }

    {
        char cand[68];

        if (!dll_name_with_suffix(cand, sizeof(cand), dll, ".dll")) {
            return 0;
        }
        /* A forwarder INTO the synthetic kernel exports resolves against the
         * table rather than a mapped image - the same fork resolve_imports
         * takes, and the reason this cannot just call load_dependency. */
        if (ctx->kernel_mode && nt_is_synthetic_dll(cand)) {
            return nt_resolve_import(cand, sym, out);
        }
        if (!dll_name_with_suffix(cand, sizeof(cand), dll, ".exe")) {
            return 0;
        }
        if (ctx->kernel_mode && nt_is_synthetic_dll(cand)) {
            return nt_resolve_import(cand, sym, out);
        }

        if (!dll_name_with_suffix(cand, sizeof(cand), dll, ".dll") ||
            load_dependency(ctx, cand, &target_base) != PE_OK) {
            if (!dll_name_with_suffix(cand, sizeof(cand), dll, ".exe") ||
                load_dependency(ctx, cand, &target_base) != PE_OK) {
                return 0;
            }
        }
    }
    rc = find_export_ex(ctx, target_base, sym, 0, out, depth + 1);
    return rc;
}

static int find_export_ex(struct link_ctx *ctx, uint64 dll_base,
                          const char *want, uint32 want_ordinal, uint64 *out,
                          int depth) {
    address_space_t *as = ctx->as;
    pe_export_dir_t dir;
    uint64 dir_rva, dir_size, i;
    uint32 index;
    uint32 func_rva;
    const pe_coff_header_t *coff;
    const pe_opt_header64_t *opt;
    uint8 hdr[0x400];

    /* Four is arbitrary but not unbounded, which is the property that
     * matters: a forwarder cycle would otherwise recurse until the kernel
     * stack ran out, and this runs on the loader's stack. */
    if (depth > 4) {
        return 0;
    }

    /* The headers were mapped at the base, so the directory table can be
     * read back from the image itself rather than kept alongside it. */
    if (read_out(as, dll_base, hdr, sizeof(hdr)) != PE_OK) {
        return 0;
    }
    coff = coff_of(hdr, sizeof(hdr));
    if (coff == NULL) {
        return 0;
    }
    opt = opt_of(coff);
    if (opt->number_of_rva_and_sizes <= PE_DIR_EXPORT) {
        return 0;
    }
    dir_rva  = opt->directory[PE_DIR_EXPORT].virtual_address;
    dir_size = opt->directory[PE_DIR_EXPORT].size;
    if (dir_rva == 0) {
        return 0;                        /* exports nothing at all */
    }
    if (read_out(as, dll_base + dir_rva, &dir, sizeof(dir)) != PE_OK) {
        return 0;
    }

    if (want == NULL) {
        /* By ordinal: straight into AddressOfFunctions, no name table at all.
         * That is the whole point of an ordinal import - the DLL need not
         * export the function by name, and many system DLLs have entries
         * reachable no other way. */
        if (want_ordinal < dir.ordinal_base) {
            return 0;
        }
        index = want_ordinal - dir.ordinal_base;
        if (index >= dir.number_of_functions) {
            return 0;
        }
    } else {
        uint16 name_ordinal;

        for (i = 0; i < dir.number_of_names; i++) {
            char   name[128];
            uint32 name_rva;

            if (read_out(as, dll_base + dir.address_of_names + i * 4,
                         &name_rva, 4) != PE_OK) {
                return 0;
            }
            if (!read_str(as, dll_base + name_rva, name, sizeof(name))) {
                continue;
            }
            if (name_eq_ci(name, want)) {
                break;
            }
        }
        if (i >= dir.number_of_names) {
            return 0;
        }
        if (read_out(as, dll_base + dir.address_of_name_ordinals + i * 2,
                     &name_ordinal, 2) != PE_OK) {
            return 0;
        }
        /* AddressOfNameOrdinals holds an INDEX already biased by
         * ordinal_base - unlike the ordinal in an import thunk. So this one
         * is used directly and the ordinal path above subtracts. Getting
         * these two the same way round is the bug this comment exists to
         * prevent. */
        index = name_ordinal;
        if (index >= dir.number_of_functions) {
            return 0;
        }
    }

    if (read_out(as, dll_base + dir.address_of_functions + index * 4,
                 &func_rva, 4) != PE_OK) {
        return 0;
    }
    if (func_rva == 0) {
        return 0;                        /* a hole in the ordinal space */
    }

    /* A forwarder - an export whose RVA lands inside the export directory
     * itself, where it names another DLL's function as a string instead of
     * pointing at code. Followed now rather than refused; before this it
     * returned 0 and the load failed. */
    if (func_rva >= dir_rva && func_rva < dir_rva + dir_size) {
        char fwd[192];

        if (!read_str(as, dll_base + func_rva, fwd, sizeof(fwd))) {
            return 0;
        }
        return follow_forwarder(ctx, fwd, out, depth);
    }

    *out = dll_base + func_rva;
    return 1;
}

static int find_export(struct link_ctx *ctx, uint64 dll_base,
                       const char *want, uint64 *out) {
    return find_export_ex(ctx, dll_base, want, 0, out, 0);
}

static int resolve_imports(struct link_ctx *ctx, uint64 base);

/* Load a DLL named by an import descriptor, once. */
static int load_dependency(struct link_ctx *ctx, const char *name,
                           uint64 *base_out) {
    char path[128];
    uint8 *file = NULL;
    uint32 size = 0;
    pe_info_t info;
    uint64 base;
    int rc;

    base = module_lookup(ctx, name);
    if (base != 0) {
        *base_out = base;
        return PE_OK;                    /* already loaded */
    }
    if (ctx->count >= PE_MAX_MODULES) {
        return PE_ERR_IMPORT;
    }
    if (ctx->depth >= 4) {
        /* A dependency chain this deep is a cycle or a mistake. Bounded
         * rather than trusted, because the depth comes from the files. */
        return PE_ERR_IMPORT;
    }

    if (!str_copy_n(path, PE_SYSTEM_DIR, sizeof(path))) {
        return PE_ERR_IMPORT;
    }
    {
        uint64 at = 0;

        while (path[at] != '\0') {
            at++;
        }
        if (!str_copy_n(path + at, name, sizeof(path) - at)) {
            return PE_ERR_IMPORT;
        }
    }

    if (ctx->reader == NULL || ctx->reader(path, &file, &size) != 0) {
        print_string("pe: cannot open ", 0x0C);
        print_string(path, 0x0C);
        print_string("\n", 0x0C);
        return PE_ERR_IMPORT;
    }

    rc = load_image(ctx->as, file, size, &info, 1, ctx->kernel_mode);
    if (ctx->release != NULL) {
        ctx->release(file);
    }
    if (rc != PE_OK) {
        return rc;
    }

    str_copy_n(ctx->modules[ctx->count].name, name,
               sizeof(ctx->modules[ctx->count].name));
    ctx->modules[ctx->count].base = info.image_base;
    ctx->count++;

    /* A dependency may have dependencies. ntdll has none, but the loader
     * should not be the reason that stays true. */
    ctx->depth++;
    rc = resolve_imports(ctx, info.image_base);
    ctx->depth--;
    if (rc != PE_OK) {
        return rc;
    }

    *base_out = info.image_base;
    return PE_OK;
}

static int resolve_imports(struct link_ctx *ctx, uint64 base) {
    address_space_t *as = ctx->as;
    const pe_coff_header_t *coff;
    const pe_opt_header64_t *opt;
    uint8 hdr[0x400];
    uint64 dir_rva, dir_size, walked;

    if (read_out(as, base, hdr, sizeof(hdr)) != PE_OK) {
        return PE_ERR_IMPORT;
    }
    coff = coff_of(hdr, sizeof(hdr));
    if (coff == NULL) {
        return PE_ERR_IMPORT;
    }
    opt = opt_of(coff);
    if (opt->number_of_rva_and_sizes <= PE_DIR_IMPORT) {
        return PE_OK;
    }
    dir_rva  = opt->directory[PE_DIR_IMPORT].virtual_address;
    dir_size = opt->directory[PE_DIR_IMPORT].size;
    if (dir_rva == 0 || dir_size == 0) {
        return PE_OK;                    /* imports nothing */
    }

    for (walked = 0; walked + sizeof(pe_import_desc_t) <= dir_size;
         walked += sizeof(pe_import_desc_t)) {
        pe_import_desc_t desc;
        char   dll[64];
        uint64 dll_base = 0;
        uint64 names, iat, slot;
        int rc, synthetic;

        if (read_out(as, base + dir_rva + walked, &desc, sizeof(desc))
            != PE_OK) {
            return PE_ERR_IMPORT;
        }
        if (desc.name == 0 && desc.first_thunk == 0) {
            break;                       /* the all-zero terminator */
        }
        if (!read_str(as, base + desc.name, dll, sizeof(dll))) {
            return PE_ERR_IMPORT;
        }

        /* A kernel-mode load resolving against "ntoskrnl.exe"/"hal.dll"
         * has no real DLL to map - see ntoskrnl_exports.h. Every other
         * dependency, kernel-mode or not, still goes through the normal
         * load_dependency/find_export path unchanged. */
        synthetic = ctx->kernel_mode && nt_is_synthetic_dll(dll);
        if (!synthetic) {
            rc = load_dependency(ctx, dll, &dll_base);
            if (rc != PE_OK) {
                return rc;
            }
        }

        /* OriginalFirstThunk when there is one, the IAT itself when there is
         * not - in which case the names are being overwritten as they are
         * read, so read and write one entry at a time. */
        names = desc.original_first_thunk != 0 ? desc.original_first_thunk
                                               : desc.first_thunk;
        iat   = desc.first_thunk;
        if (iat == 0) {
            return PE_ERR_IMPORT;
        }

        for (slot = 0; ; slot++) {
            uint64 thunk, addr = 0;
            char   func[128];

            if (read_out(as, base + names + slot * 8, &thunk, 8) != PE_OK) {
                return PE_ERR_IMPORT;
            }
            if (thunk == 0) {
                break;                   /* end of this DLL's imports */
            }
            if (thunk & 0x8000000000000000ULL) {
                /* Import by ordinal: the name table is skipped entirely and
                 * the low 16 bits ARE the ordinal. Only the low 16 - the
                 * rest of the thunk below bit 63 is reserved and a real
                 * linker leaves it zero, but masking is free and trusting it
                 * is not.
                 *
                 * A synthetic DLL has no export directory to index, only the
                 * name table in ntoskrnl_exports.c, so an ordinal import of
                 * one still cannot be resolved. That is refused explicitly
                 * here rather than falling through to find_export_ex with a
                 * dll_base of zero. */
                uint32 ord = (uint32)(thunk & 0xFFFFu);

                if (synthetic) {
                    print_string("pe: ordinal import from synthetic ", 0x0C);
                    print_string(dll, 0x0C);
                    print_string(" - it has no export directory\n", 0x0C);
                    return PE_ERR_IMPORT;
                }
                if (!find_export_ex(ctx, dll_base, NULL, ord, &addr, 0)) {
                    print_string("pe: ", 0x0C);
                    print_string(dll, 0x0C);
                    print_string(" has no export at that ordinal\n", 0x0C);
                    return PE_ERR_IMPORT;
                }
                if (copy_in(as, base + iat + slot * 8, &addr, 8) != PE_OK) {
                    return PE_ERR_IMPORT;
                }
                continue;
            }
            /* IMAGE_IMPORT_BY_NAME: a 2-byte hint, then the name. The hint is
             * a guess at the export index and is advisory - checking it would
             * be an optimisation, trusting it would be a bug. */
            if (!read_str(as, base + thunk + 2, func, sizeof(func))) {
                return PE_ERR_IMPORT;
            }
            if (synthetic ? !nt_resolve_import(dll, func, &addr)
                          : !find_export(ctx, dll_base, func, &addr)) {
                print_string("pe: ", 0x0C);
                print_string(dll, 0x0C);
                print_string(" does not export ", 0x0C);
                print_string(func, 0x0C);
                print_string("\n", 0x0C);
                return PE_ERR_IMPORT;
            }
            if (copy_in(as, base + iat + slot * 8, &addr, 8) != PE_OK) {
                return PE_ERR_IMPORT;
            }
        }
    }
    return PE_OK;
}

/* --- implicit TLS ----------------------------------------------------------
 *
 * IMAGE_TLS_DIRECTORY64, read out of a module that is already mapped: the
 * template range, the zero fill after it, where the module wants its index
 * written, and its callback list. Every module with one gets the next index
 * - the executable first, as on Windows, so a program's own _tls_index is 0 -
 * and the index is written into the module now, before any code of it runs.
 * The per-thread copies are the kernel's business at thread creation (see
 * nt_thread_tls_init). */
typedef struct __attribute__((packed)) {
    uint64 start_address_of_raw_data;
    uint64 end_address_of_raw_data;
    uint64 address_of_index;
    uint64 address_of_callbacks;
    uint32 size_of_zero_fill;
    uint32 characteristics;
} pe_tls_directory64_t;

static int collect_tls(address_space_t *as, uint64 base, pe_info_t *info) {
    uint32 lfanew = 0;
    pe_opt_header64_t opt;
    pe_tls_directory64_t td;
    uint32 index;
    uint64 first_cb = 0;

    if (read_out(as, base + 0x3C, &lfanew, 4) != PE_OK ||
        read_out(as, base + lfanew + 24, &opt, sizeof(opt)) != PE_OK) {
        return PE_ERR_TLS;
    }
    if (opt.number_of_rva_and_sizes <= PE_DIR_TLS ||
        opt.directory[PE_DIR_TLS].virtual_address == 0 ||
        opt.directory[PE_DIR_TLS].size < sizeof(td)) {
        return PE_OK;                      /* no TLS: normal */
    }
    if (read_out(as, base + opt.directory[PE_DIR_TLS].virtual_address, &td,
                 sizeof(td)) != PE_OK) {
        return PE_ERR_TLS;
    }
    if (info->tls_count >= 8) {
        print_string("pe: more than 8 modules with TLS\n", 0x0C);
        return PE_ERR_TLS;
    }
    if (td.end_address_of_raw_data < td.start_address_of_raw_data ||
        td.end_address_of_raw_data - td.start_address_of_raw_data > 0x8000 ||
        td.size_of_zero_fill > 0x8000) {
        print_string("pe: TLS template out of range\n", 0x0C);
        return PE_ERR_TLS;
    }
    index = (uint32)info->tls_count;
    if (td.address_of_index != 0 &&
        copy_in(as, td.address_of_index, &index, 4) != PE_OK) {
        return PE_ERR_TLS;
    }
    info->tls[index].module_base = base;
    info->tls[index].start       = td.start_address_of_raw_data;
    info->tls[index].end         = td.end_address_of_raw_data;
    info->tls[index].zero_fill   = td.size_of_zero_fill;
    info->tls[index].index_addr  = td.address_of_index;
    info->tls[index].callbacks   = td.address_of_callbacks;
    if (td.address_of_callbacks != 0 &&
        read_out(as, td.address_of_callbacks, &first_cb, 8) == PE_OK &&
        first_cb != 0) {
        info->has_tls_callbacks = 1;
    }
    info->tls_count++;
    return PE_OK;
}

int pe_load_executable(address_space_t *as, const void *image, uint64 size,
                       pe_info_t *info, pe_file_reader_t reader,
                       pe_file_release_t release) {
    struct link_ctx ctx;
    int rc, i;

    rc = load_image(as, image, size, info, 0, 0);
    if (rc != PE_OK) {
        return rc;
    }
    info->thread_start = 0;
    info->apc_dispatcher = 0;
    info->exception_dispatcher = 0;
    info->tls_count = 0;
    info->has_tls_callbacks = 0;
    rc = collect_tls(as, info->image_base, info);
    if (rc != PE_OK) {
        return rc;
    }
    if (!info->has_imports) {
        return PE_OK;                    /* nothing to link */
    }

    ctx.as          = as;
    ctx.reader      = reader;
    ctx.release     = release;
    ctx.count       = 0;
    ctx.depth       = 0;
    ctx.kernel_mode = 0;
    for (i = 0; i < PE_MAX_MODULES; i++) {
        ctx.modules[i].name[0] = '\0';
        ctx.modules[i].base = 0;
    }
    rc = resolve_imports(&ctx, info->image_base);
    if (rc != PE_OK) {
        return rc;
    }
    /* Every DLL that came in with it, in load order, after the executable. */
    for (i = 0; i < ctx.count; i++) {
        if (ctx.modules[i].base != 0 && ctx.modules[i].base != info->image_base) {
            rc = collect_tls(as, ctx.modules[i].base, info);
            if (rc != PE_OK) {
                return rc;
            }
        }
    }
    /* Absent is not an error: a program that imports no ntdll, or an ntdll
     * too old to export it, links and runs - it just cannot create threads,
     * and NtCreateThreadEx says so when asked. */
    {
        uint64 ntdll = module_lookup(&ctx, "ntdll.dll");
        uint64 at = 0;

        if (ntdll != 0 && find_export(&ctx, ntdll, "RtlUserThreadStart", &at)) {
            info->thread_start = at;
        }
        if (ntdll != 0 && find_export(&ctx, ntdll, "KiUserApcDispatcher", &at)) {
            info->apc_dispatcher = at;
        }
        if (ntdll != 0 &&
            find_export(&ctx, ntdll, "KiUserExceptionDispatcher", &at)) {
            info->exception_dispatcher = at;
        }
    }
    return PE_OK;
}

/* Kernel-mode driver load - see pe.h. Mirrors pe_load_executable exactly,
 * except allow_dll/kernel_mode are forced on for load_image (a .sys is
 * DLL-characteristics + IMAGE_SUBSYSTEM_NATIVE) and resolve_imports takes
 * the synthetic ntoskrnl.exe/hal.dll branch for those two DLL names. */
int pe_load_driver(address_space_t *as, const void *image, uint64 size,
                   pe_info_t *info, pe_file_reader_t reader,
                   pe_file_release_t release) {
    struct link_ctx ctx;
    int rc, i;

    rc = load_image(as, image, size, info, 1, 1);
    if (rc != PE_OK) {
        return rc;
    }
    if (!info->has_imports) {
        return PE_OK;
    }

    ctx.as          = as;
    ctx.reader      = reader;
    ctx.release     = release;
    ctx.count       = 0;
    ctx.depth       = 0;
    ctx.kernel_mode = 1;
    for (i = 0; i < PE_MAX_MODULES; i++) {
        ctx.modules[i].name[0] = '\0';
        ctx.modules[i].base = 0;
    }
    return resolve_imports(&ctx, info->image_base);
}

/* --- reporting ----------------------------------------------------------- */

static void print_name(const char *name, uint8 color) {
    char buf[9];
    int i;

    for (i = 0; i < 8 && name[i] != '\0'; i++) {
        buf[i] = name[i];
    }
    buf[i] = '\0';
    print_string(buf, color);
}

void pe_report(const void *image, uint64 size, uint8 color) {
    const pe_coff_header_t *coff = coff_of(image, size);
    const pe_opt_header64_t *opt;
    const pe_section_header_t *sec;
    pe_info_t info;
    uint32 i;
    int rc;

    if (coff == NULL) {
        print_string("pe: not a PE image\n", 0x0C);
        return;
    }
    rc = pe_validate(image, size, &info);
    if (rc != PE_OK) {
        print_string("pe: ", 0x0C);
        print_string(pe_strerror(rc), 0x0C);
        print_string("\n", 0x0C);
        return;
    }
    opt = opt_of(coff);
    sec = sections_of(coff);

    print_string("PE32+ x86-64, ImageBase ", color);
    print_hex64(opt->image_base, color);
    print_string(" entry +", color);
    print_hex(opt->address_of_entry_point, color);
    print_string(" size ", color);
    print_hex(opt->size_of_image, color);
    print_string(info.has_imports ? " (imports)\n" : " (no imports)\n", color);

    for (i = 0; i < coff->number_of_sections; i++) {
        print_string("  ", color);
        print_name(sec[i].name, 0x0F);
        print_string("  rva ", 0x07);
        print_hex(sec[i].virtual_address, 0x07);
        print_string("  vsz ", 0x07);
        print_hex(sec[i].virtual_size, 0x07);
        print_string("  raw ", 0x07);
        print_hex(sec[i].pointer_to_raw_data, 0x07);
        print_string("  rsz ", 0x07);
        print_hex(sec[i].size_of_raw_data, 0x07);
        print_string("  ", 0x07);
        print_string((sec[i].characteristics & PE_SCN_MEM_READ) ? "r" : "-", 0x0B);
        print_string((sec[i].characteristics & PE_SCN_MEM_WRITE) ? "w" : "-", 0x0B);
        print_string((sec[i].characteristics & PE_SCN_MEM_EXECUTE) ? "x" : "-", 0x0B);
        print_string("\n", 0x07);
    }
}

int pe_load_into(address_space_t *as, const void *image, uint64 size,
                 pe_info_t *info) {
    return load_image(as, image, size, info, 0, 0);
}

const char *pe_strerror(int rc) {
    switch (rc) {
        case PE_OK:             return "ok";
        case PE_ERR_TRUNCATED:  return "truncated: the file is smaller than its headers claim";
        case PE_ERR_MAGIC:      return "not a PE image";
        case PE_ERR_MACHINE:    return "not x86-64";
        case PE_ERR_OPTMAGIC:   return "PE32, not PE32+";
        case PE_ERR_NOTEXE:     return "not an executable image";
        case PE_ERR_ALIGN:      return "SectionAlignment is finer than a page";
        case PE_ERR_ADDRESS:    return "does not fit in user address space";
        case PE_ERR_SECTION:    return "a section lies outside the file";
        case PE_ERR_RELOC:      return "must move and has no usable .reloc";
        case PE_ERR_NOMEM:      return "out of physical frames";
        case PE_ERR_IMPORT:     return "an imported name could not be resolved";
        case PE_ERR_SUBSYSTEM:  return "kernel-mode load requires IMAGE_SUBSYSTEM_NATIVE";
        case PE_ERR_TLS:        return "a TLS directory out of range (or too many modules with one)";
        default:                return "unknown error";
    }
}
