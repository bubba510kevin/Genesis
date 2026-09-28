#include "elfso.h"
#include "kheap.h"
#include "paging.h"
#include "pmm.h"
#include "screen.h"
#include "typesk.h"

/* Linux shared objects loaded into a running process - ROADMAP item 19,
 * stage 3: a Windows program's LoadLibrary("libfoo.so").
 *
 * The shape is src/rtld/rtld.c's, which is this tree's own ELF dynamic
 * linker: map every object breadth-first from the one asked for, then
 * relocate each against one global scope searched in load order, eagerly,
 * PLT included. What differs is where it runs and who it serves. rtld runs
 * in ring 3 at a program's start and links the program; this runs in the
 * kernel, into a process that already exists, for a program that is not an
 * ELF image at all - so there is no main executable in the scope, only the
 * new objects and the ones an earlier load brought in.
 *
 * Writes go through the direct map, never through the user mapping. A
 * read-only segment is mapped read-only from the start, and a relocation
 * inside one (text relocations) still has to land; the direct-map write is
 * what makes both true at once.
 *
 * Refused, loudly, rather than approximated:
 *   - TLS (PT_TLS, R_X86_64_DTPMOD64/DTPOFF64/TPOFF64): a Windows process
 *     has no ELF thread pointer in %fs to put a module's block behind;
 *   - COPY relocations: they only exist for a main executable, and there is
 *     none;
 *   - IRELATIVE: it means calling the object's resolver, which is ring-3
 *     code, from here;
 *   - an undefined non-weak symbol. */

/* --- the ELF structures (the subset this reads) ---------------------------- */

typedef struct __attribute__((packed)) {
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
} so_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32 p_type;
    uint32 p_flags;
    uint64 p_offset;
    uint64 p_vaddr;
    uint64 p_paddr;
    uint64 p_filesz;
    uint64 p_memsz;
    uint64 p_align;
} so_phdr_t;

typedef struct __attribute__((packed)) {
    uint32 st_name;
    uint8  st_info;
    uint8  st_other;
    uint16 st_shndx;
    uint64 st_value;
    uint64 st_size;
} so_sym_t;

typedef struct __attribute__((packed)) {
    uint64 r_offset;
    uint64 r_info;
    int64  r_addend;
} so_rela_t;

#define SO_ET_DYN      3
#define SO_EM_X86_64   0x3E
#define SO_PT_LOAD     1
#define SO_PT_DYNAMIC  2
#define SO_PT_TLS      7
#define SO_PF_X        1
#define SO_PF_W        2

#define DT_NULL          0
#define DT_NEEDED        1
#define DT_PLTRELSZ      2
#define DT_HASH          4
#define DT_STRTAB        5
#define DT_SYMTAB        6
#define DT_RELA          7
#define DT_RELASZ        8
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_SONAME       14
#define DT_JMPREL       23
#define DT_INIT_ARRAY   25
#define DT_INIT_ARRAYSZ 27
#define DT_GNU_HASH     0x6ffffef5ULL

#define R_NONE        0
#define R_64          1
#define R_COPY        5
#define R_GLOB_DAT    6
#define R_JUMP_SLOT   7
#define R_RELATIVE    8
#define R_DTPMOD64   16
#define R_DTPOFF64   17
#define R_TPOFF64    18
#define R_IRELATIVE  37

#define STB_WEAK      2

#define SO_MAX_NEEDED 16

typedef struct {
    char   name[64];
    uint64 base;          /* load bias: vaddr 0 of the object            */
    uint64 lo, hi;        /* the mapped span, page-aligned               */
    uint64 dynamic;
    uint64 strtab, symtab, syment, nsyms;
    uint64 rela, relasz, jmprel, jmprelsz;
    uint64 init, init_array, init_arraysz;
    uint64 hash, gnu_hash;           /* DT_HASH / DT_GNU_HASH, absolute */
    uint64 needed[SO_MAX_NEEDED];   /* string-table offsets */
    int    nneeded;
    int    fresh;         /* mapped by this call                          */
} so_obj_t;

static int fail(const char *what, const char *detail) {
    print_string("elfso: ", 0x0C);
    print_string(what, 0x0C);
    if (detail != NULL) {
        print_string(": ", 0x0C);
        print_string(detail, 0x0C);
    }
    print_string("\n", 0x0C);
    return ELFSO_ERR;
}

/* --- reading and writing the target space through the direct map ----------- */

/* Reads go straight through the user mapping: `as` is always the calling
 * process's own space (NtGenesisLoadImage loads into the caller), so its
 * pages are the ones CR3 maps - only whether each page IS mapped needs
 * checking, once per page, so a bad pointer in a malformed object is an
 * error rather than a kernel fault. */
static int rd(address_space_t *as, uint64 va, void *dst, uint64 n) {
    uint8 *out = (uint8 *)dst;
    uint64 done = 0;

    while (done < n) {
        uint64 at = va + done;
        uint64 chunk = PMM_PAGE_SIZE - (at & 0xFFF);
        uint64 i;

        if (chunk > n - done) {
            chunk = n - done;
        }
        if (at >= ELFSO_LIMIT || vmm_get_phys_in(as, at & ~0xFFFULL) == 0) {
            return -1;
        }
        for (i = 0; i < chunk; i++) {
            out[done + i] = ((const uint8 *)at)[i];
        }
        done += chunk;
    }
    return 0;
}

static int wr(address_space_t *as, uint64 va, const void *src, uint64 n) {
    const uint8 *in = (const uint8 *)src;
    uint64 i;

    for (i = 0; i < n; i++) {
        phys_addr_t phys = vmm_get_phys_in(as, (va + i) & ~0xFFFULL);

        if (phys == 0) {
            return -1;
        }
        ((uint8 *)phys_to_virt(phys & ~0xFFFULL))[(va + i) & 0xFFF] = in[i];
    }
    return 0;
}

static uint64 rd64(address_space_t *as, uint64 va) {
    uint64 v = 0;

    rd(as, va, &v, 8);
    return v;
}

static int rd_str(address_space_t *as, uint64 va, char *out, uint64 cap) {
    uint64 i;

    for (i = 0; i + 1 < cap; i++) {
        if (rd(as, va + i, &out[i], 1) != 0) {
            return 0;
        }
        if (out[i] == '\0') {
            return 1;
        }
    }
    out[cap - 1] = '\0';
    return 0;
}

static int streq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void scopy(char *dst, const char *src, uint64 cap) {
    uint64 i;

    for (i = 0; i + 1 < cap && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static const char *basename_of(const char *path) {
    const char *b = path, *c;

    for (c = path; *c != '\0'; c++) {
        if (*c == '/') {
            b = c + 1;
        }
    }
    return b;
}

/* --- the dynamic section -------------------------------------------------- */

/* Symbol count from DT_HASH's nchain, or by walking DT_GNU_HASH to the end
 * of its last chain - the same arithmetic as rtld.c's symbol_count. */
static uint64 count_symbols(address_space_t *as, so_obj_t *o, uint64 hash,
                            uint64 gnu) {
    uint32 v32;

    if (hash != 0) {
        rd(as, hash + 4, &v32, 4);
        return v32;
    }
    if (gnu != 0) {
        uint32 nbuckets = 0, symoffset = 0, bloom = 0, last = 0, b, chain;
        uint64 buckets, chains, i;

        rd(as, gnu, &nbuckets, 4);
        rd(as, gnu + 4, &symoffset, 4);
        rd(as, gnu + 8, &bloom, 4);
        buckets = gnu + 16 + 8ULL * bloom;
        chains  = buckets + 4ULL * nbuckets;
        for (i = 0; i < nbuckets; i++) {
            rd(as, buckets + 4 * i, &b, 4);
            if (b > last) {
                last = b;
            }
        }
        if (last < symoffset) {
            return symoffset;
        }
        for (;;) {
            if (rd(as, chains + 4ULL * (last - symoffset), &chain, 4) != 0) {
                return 0;
            }
            if (chain & 1) {
                return (uint64)last + 1;
            }
            last++;
        }
    }
    (void)o;
    return 0;
}

static int parse_dynamic(address_space_t *as, so_obj_t *o) {
    uint64 i, hash = 0, gnu = 0, soname = 0, has_soname = 0;

    o->syment = sizeof(so_sym_t);
    o->nneeded = 0;
    for (i = 0; ; i++) {
        uint64 tag = rd64(as, o->dynamic + 16 * i);
        uint64 val = rd64(as, o->dynamic + 16 * i + 8);

        if (tag == DT_NULL || i > 512) {
            break;
        }
        switch (tag) {
            case DT_NEEDED:
                if (o->nneeded < SO_MAX_NEEDED) {
                    o->needed[o->nneeded++] = val;
                }
                break;
            case DT_STRTAB:       o->strtab = o->base + val; break;
            case DT_SYMTAB:       o->symtab = o->base + val; break;
            case DT_SYMENT:       o->syment = val; break;
            case DT_HASH:         hash = o->base + val; break;
            case DT_GNU_HASH:     gnu = o->base + val; break;
            case DT_RELA:         o->rela = o->base + val; break;
            case DT_RELASZ:       o->relasz = val; break;
            case DT_JMPREL:       o->jmprel = o->base + val; break;
            case DT_PLTRELSZ:     o->jmprelsz = val; break;
            case DT_INIT:         o->init = o->base + val; break;
            case DT_INIT_ARRAY:   o->init_array = o->base + val; break;
            case DT_INIT_ARRAYSZ: o->init_arraysz = val; break;
            case DT_SONAME:       soname = val; has_soname = 1; break;
            default:              break;
        }
    }
    if (o->strtab == 0 || o->symtab == 0) {
        return fail("no dynamic symbol table", o->name);
    }
    o->hash = hash;
    o->gnu_hash = gnu;
    o->nsyms = count_symbols(as, o, hash, gnu);
    if (has_soname) {
        rd_str(as, o->strtab + soname, o->name, sizeof(o->name));
    }
    return ELFSO_OK;
}

/* An object already in the process, from its ELF header in memory. */
static int adopt(address_space_t *as, uint64 base, uint64 size, so_obj_t *o) {
    so_ehdr_t eh;
    so_phdr_t ph;
    uint16 i;

    o->name[0] = '\0';
    o->base = base;
    o->lo = base;
    o->hi = base + size;
    o->dynamic = 0;
    o->strtab = o->symtab = o->rela = o->relasz = o->jmprel = o->jmprelsz = 0;
    o->init = o->init_array = o->init_arraysz = 0;
    o->hash = o->gnu_hash = 0;
    o->fresh = 0;
    if (rd(as, base, &eh, sizeof(eh)) != 0) {
        return ELFSO_ERR;
    }
    for (i = 0; i < eh.e_phnum; i++) {
        if (rd(as, base + eh.e_phoff + (uint64)i * eh.e_phentsize, &ph,
               sizeof(ph)) != 0) {
            return ELFSO_ERR;
        }
        if (ph.p_type == SO_PT_DYNAMIC) {
            o->dynamic = base + ph.p_vaddr;
        }
    }
    if (o->dynamic == 0) {
        return ELFSO_ERR;
    }
    return parse_dynamic(as, o);
}

/* --- mapping -------------------------------------------------------------- */

static int range_free(address_space_t *as, uint64 lo, uint64 hi) {
    uint64 va;

    for (va = lo; va < hi; va += PMM_PAGE_SIZE) {
        if (vmm_get_phys_in(as, va) != 0) {
            return 0;
        }
    }
    return 1;
}

static void unmap(address_space_t *as, so_obj_t *o) {
    uint64 va;

    for (va = o->lo; va < o->hi; va += PMM_PAGE_SIZE) {
        if (vmm_get_phys_in(as, va) != 0) {
            vmm_unmap_page_in(as, va, VMM_FREE_FRAME);
        }
    }
}

static int map_object(address_space_t *as, const uint8 *file, uint32 size,
                      so_obj_t *o) {
    const so_ehdr_t *eh = (const so_ehdr_t *)file;
    const so_phdr_t *ph;
    uint64 minv = ~0ULL, maxv = 0, span, base, va, npages, i;
    uint8 *perm;
    uint16 k;

    if (size < sizeof(so_ehdr_t) || eh->e_ident[0] != 0x7F ||
        eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        return fail("not an ELF file", o->name);
    }
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1 ||
        eh->e_machine != SO_EM_X86_64) {
        return fail("not a 64-bit little-endian x86-64 object", o->name);
    }
    if (eh->e_type != SO_ET_DYN) {
        return fail("not a shared object (ET_DYN)", o->name);
    }
    if (eh->e_phoff + (uint64)eh->e_phnum * sizeof(so_phdr_t) > size ||
        eh->e_phentsize != sizeof(so_phdr_t)) {
        return fail("program headers outside the file", o->name);
    }
    ph = (const so_phdr_t *)(file + eh->e_phoff);

    for (k = 0; k < eh->e_phnum; k++) {
        if (ph[k].p_type == SO_PT_TLS) {
            return fail("thread-local storage is not supported here", o->name);
        }
        if (ph[k].p_type != SO_PT_LOAD) {
            continue;
        }
        if (ph[k].p_offset + ph[k].p_filesz > size ||
            ph[k].p_filesz > ph[k].p_memsz) {
            return fail("a segment lies outside the file", o->name);
        }
        if (ph[k].p_vaddr < minv) {
            minv = ph[k].p_vaddr;
        }
        if (ph[k].p_vaddr + ph[k].p_memsz > maxv) {
            maxv = ph[k].p_vaddr + ph[k].p_memsz;
        }
    }
    if (maxv == 0) {
        return fail("no loadable segments", o->name);
    }
    if (minv != 0) {
        /* The ELF header then is not at the load bias, and the module
         * handle (the address GetProcAddress reads the header from) would
         * not be the start of the image. Every linker puts the first
         * segment at 0 for a shared object; refuse the one that does not. */
        return fail("first segment is not at address 0", o->name);
    }
    maxv = (maxv + 0xFFFULL) & ~0xFFFULL;
    span = maxv - minv;
    npages = span / PMM_PAGE_SIZE;
    if (npages > ELFSO_MAX_PAGES) {
        return fail("too large", o->name);
    }

    for (base = ELFSO_BASE; base + span <= ELFSO_LIMIT; base += ELFSO_STEP) {
        if (range_free(as, base, base + span)) {
            break;
        }
    }
    if (base + span > ELFSO_LIMIT) {
        return fail("no room in the address space", o->name);
    }

    /* Permissions per PAGE, as the union of every segment that touches it:
     * the tail of .text and the head of .rodata routinely share a page, and
     * mapping it with only the first segment's rights would fault a fetch or
     * a read the object is entitled to. */
    perm = (uint8 *)kmalloc(npages);
    if (perm == NULL) {
        return fail("out of memory", o->name);
    }
    for (i = 0; i < npages; i++) {
        perm[i] = 0;
    }
    for (k = 0; k < eh->e_phnum; k++) {
        uint64 s, e;

        if (ph[k].p_type != SO_PT_LOAD || ph[k].p_memsz == 0) {
            continue;
        }
        s = (ph[k].p_vaddr & ~0xFFFULL) - minv;
        e = ((ph[k].p_vaddr + ph[k].p_memsz + 0xFFFULL) & ~0xFFFULL) - minv;
        for (va = s; va < e; va += PMM_PAGE_SIZE) {
            perm[va / PMM_PAGE_SIZE] |= 0x80 | (uint8)(ph[k].p_flags & 7);
        }
    }

    o->base = base - minv;
    o->lo = base;
    o->hi = base + span;
    o->fresh = 1;
    for (i = 0; i < npages; i++) {
        uint64 flags = PAGE_PRESENT | PAGE_USER;
        phys_addr_t phys;
        uint8 *win;
        uint64 b;

        if (!(perm[i] & 0x80)) {
            continue;                    /* a hole between segments */
        }
        if (perm[i] & SO_PF_W) {
            flags |= PAGE_RW;
        }
        if (!(perm[i] & SO_PF_X)) {
            flags |= PAGE_NX;
        }
        phys = vmm_alloc_page_in(as, base + i * PMM_PAGE_SIZE, flags);
        if (phys == 0) {
            kfree(perm);
            unmap(as, o);
            return fail("out of memory", o->name);
        }
        win = (uint8 *)phys_to_virt(phys);
        for (b = 0; b < PMM_PAGE_SIZE; b++) {
            win[b] = 0;                  /* .bss, and the gaps */
        }
    }
    kfree(perm);

    for (k = 0; k < eh->e_phnum; k++) {
        if (ph[k].p_type == SO_PT_LOAD && ph[k].p_filesz != 0 &&
            wr(as, o->base + ph[k].p_vaddr, file + ph[k].p_offset,
               ph[k].p_filesz) != 0) {
            unmap(as, o);
            return fail("cannot write a segment", o->name);
        }
        if (ph[k].p_type == SO_PT_DYNAMIC) {
            o->dynamic = o->base + ph[k].p_vaddr;
        }
    }
    if (o->dynamic == 0) {
        unmap(as, o);
        return fail("no PT_DYNAMIC", o->name);
    }
    if (parse_dynamic(as, o) != ELFSO_OK) {
        unmap(as, o);
        return ELFSO_ERR;
    }
    return ELFSO_OK;
}

/* --- symbols and relocation ------------------------------------------------ */

/* Is symbol i of `o` a DEFINITION named `name`? */
static int sym_is(address_space_t *as, so_obj_t *o, uint64 i, const char *name,
                  uint64 *value) {
    so_sym_t s;
    char nm[128];

    if (rd(as, o->symtab + i * o->syment, &s, sizeof(s)) != 0) {
        return 0;
    }
    if (s.st_shndx == 0 || ((s.st_info >> 4) != 1 && (s.st_info >> 4) != STB_WEAK)) {
        return 0;                        /* undefined here, or local */
    }
    if (!rd_str(as, o->strtab + s.st_name, nm, sizeof(nm)) || !streq(nm, name)) {
        return 0;
    }
    *value = o->base + s.st_value;
    return 1;
}

/* Through the object's own hash table - DT_GNU_HASH if it has one (every
 * current toolchain's default), else DT_HASH - so a lookup is a bucket and
 * a short chain, not a walk of every symbol. A linear walk is the fallback
 * for an object with neither. */
static int find_def(address_space_t *as, so_obj_t *o, const char *name,
                    uint64 *value) {
    uint64 i;

    if (o->gnu_hash != 0) {
        uint32 nbuckets = 0, symoffset = 0, bloom = 0, idx = 0, ch = 0, h = 5381;
        const char *c;

        for (c = name; *c != '\0'; c++) {
            h = h * 33 + (uint8)*c;
        }
        rd(as, o->gnu_hash, &nbuckets, 4);
        rd(as, o->gnu_hash + 4, &symoffset, 4);
        rd(as, o->gnu_hash + 8, &bloom, 4);
        if (nbuckets == 0) {
            return 0;
        }
        {
            uint64 buckets = o->gnu_hash + 16 + 8ULL * bloom;
            uint64 chains  = buckets + 4ULL * nbuckets;

            rd(as, buckets + 4ULL * (h % nbuckets), &idx, 4);
            if (idx == 0 || idx < symoffset) {
                return 0;
            }
            for (;; idx++) {
                if (rd(as, chains + 4ULL * (idx - symoffset), &ch, 4) != 0) {
                    return 0;
                }
                if ((ch | 1) == (h | 1) && sym_is(as, o, idx, name, value)) {
                    return 1;
                }
                if (ch & 1) {
                    return 0;
                }
            }
        }
    }
    if (o->hash != 0) {
        uint32 nbucket = 0, idx = 0, h = 0, g;
        const char *c;

        for (c = name; *c != '\0'; c++) {
            h = (h << 4) + (uint8)*c;
            g = h & 0xF0000000u;
            if (g != 0) {
                h ^= g >> 24;
            }
            h &= ~g;
        }
        rd(as, o->hash, &nbucket, 4);
        if (nbucket == 0) {
            return 0;
        }
        rd(as, o->hash + 8 + 4ULL * (h % nbucket), &idx, 4);
        while (idx != 0 && idx < o->nsyms) {
            if (sym_is(as, o, idx, name, value)) {
                return 1;
            }
            if (rd(as, o->hash + 8 + 4ULL * nbucket + 4ULL * idx, &idx, 4) != 0) {
                return 0;
            }
        }
        return 0;
    }
    for (i = 1; i < o->nsyms; i++) {
        if (sym_is(as, o, i, name, value)) {
            return 1;
        }
    }
    return 0;
}

static int lookup(address_space_t *as, so_obj_t *scope, int n, const char *name,
                  uint64 *value) {
    int i;

    for (i = 0; i < n; i++) {
        if (find_def(as, &scope[i], name, value)) {
            return 1;
        }
    }
    return 0;
}

static int apply(address_space_t *as, so_obj_t *o, uint64 rela, uint64 bytes,
                 so_obj_t *scope, int n) {
    uint64 i, count = bytes / sizeof(so_rela_t);

    for (i = 0; i < count; i++) {
        so_rela_t r;
        uint32 type, symi;
        uint64 value = 0, where;
        char name[128];
        so_sym_t s;

        if (rd(as, rela + i * sizeof(so_rela_t), &r, sizeof(r)) != 0) {
            return fail("cannot read a relocation", o->name);
        }
        type  = (uint32)(r.r_info & 0xFFFFFFFFu);
        symi  = (uint32)(r.r_info >> 32);
        where = o->base + r.r_offset;
        name[0] = '\0';
        s.st_info = 0;
        if (symi != 0) {
            rd(as, o->symtab + symi * o->syment, &s, sizeof(s));
            rd_str(as, o->strtab + s.st_name, name, sizeof(name));
        }

        switch (type) {
            case R_NONE:
                continue;
            case R_RELATIVE:
                value = o->base + (uint64)r.r_addend;
                break;
            case R_64:
            case R_GLOB_DAT:
            case R_JUMP_SLOT:
                if (!lookup(as, scope, n, name, &value)) {
                    if ((s.st_info >> 4) == STB_WEAK) {
                        value = 0;       /* a weak undefined reference is 0 */
                        break;
                    }
                    return fail("undefined symbol", name);
                }
                if (type == R_64) {
                    value += (uint64)r.r_addend;
                }
                break;
            case R_COPY:
                return fail("COPY relocation (there is no main executable)", name);
            case R_DTPMOD64:
            case R_DTPOFF64:
            case R_TPOFF64:
                return fail("thread-local storage is not supported here", o->name);
            case R_IRELATIVE:
                return fail("IRELATIVE (an ifunc resolver would run in ring 0)", o->name);
            default:
                return fail("unhandled relocation type", o->name);
        }
        if (wr(as, where, &value, 8) != 0) {
            return fail("relocation outside the image", o->name);
        }
    }
    return ELFSO_OK;
}

/* Read `path`, map it, and name it `name` - the SONAME if it has one wins
 * in parse_dynamic, and DT_NEEDED lookups compare against that. */
static int load_one(address_space_t *as, const char *path, const char *name,
                    elfso_reader_t reader, elfso_release_t release,
                    so_obj_t *o) {
    uint8 *file = NULL;
    uint32 size = 0;
    int rc;

    if (reader(path, &file, &size) != 0) {
        return fail("cannot open", path);
    }
    scopy(o->name, name, sizeof(o->name));
    o->strtab = o->symtab = o->rela = o->relasz = o->jmprel = o->jmprelsz = 0;
    o->init = o->init_array = o->init_arraysz = 0;
    o->hash = o->gnu_hash = 0;
    o->dynamic = 0;
    rc = map_object(as, file, size, o);
    release(file);
    /* Asked for by a DT_NEEDED under a name that is not its SONAME: keep
     * the asked-for name, so the search stops asking. */
    if (rc == ELFSO_OK && !streq(o->name, name) && name != basename_of(path)) {
        scopy(o->name, name, sizeof(o->name));
    }
    return rc;
}

/* --- the entry point ------------------------------------------------------ */

int elfso_load_library(address_space_t *as, const char *path,
                       const elfso_loaded_t *loaded, int nloaded,
                       elfso_reader_t reader, elfso_release_t release,
                       elfso_result_t *out) {
    so_obj_t *objs;
    int n = 0, first_new, i, j, q, rc = ELFSO_OK;
    char needed[64], dir[160];

    out->base = 0;
    out->count = 0;
    if (nloaded < 0 || nloaded > ELFSO_MAX_OBJECTS) {
        return ELFSO_ERR;
    }
    objs = (so_obj_t *)kmalloc(sizeof(so_obj_t) * (ELFSO_MAX_OBJECTS * 2 + 1));
    if (objs == NULL) {
        return fail("out of memory", path);
    }

    /* What the process already has - the end of the scope, and what a
     * DT_NEEDED is answered from first. */
    for (i = 0; i < nloaded; i++) {
        if (adopt(as, loaded[i].base, loaded[i].size, &objs[n]) == ELFSO_OK) {
            n++;
        }
    }
    first_new = n;

    /* Already loaded: its base, nothing mapped. Matched by SONAME, which is
     * what DT_NEEDED names too, falling back on the file's own name. */
    for (i = 0; i < first_new; i++) {
        if (streq(objs[i].name, basename_of(path))) {
            out->base = objs[i].base;
            kfree(objs);
            return ELFSO_OK;
        }
    }

    /* The directory the first object came from: where a DT_NEEDED is looked
     * for after /lib. */
    scopy(dir, path, sizeof(dir));
    for (i = 0, j = -1; dir[i] != '\0'; i++) {
        if (dir[i] == '/') {
            j = i;
        }
    }
    dir[j + 1] = '\0';

    /* The object asked for, then breadth-first through what each new object
     * needs that is not in the process yet. */
    rc = load_one(as, path, basename_of(path), reader, release, &objs[n]);
    if (rc == ELFSO_OK) {
        n++;
    }
    for (q = first_new; rc == ELFSO_OK && q < n; q++) {
        int k;

        for (k = 0; rc == ELFSO_OK && k < objs[q].nneeded; k++) {
            char full[224];
            int l, have = 0, has_slash = 0;

            if (!rd_str(as, objs[q].strtab + objs[q].needed[k], needed,
                        sizeof(needed))) {
                rc = fail("bad DT_NEEDED", objs[q].name);
                break;
            }
            for (l = 0; l < n; l++) {
                if (streq(objs[l].name, needed)) {
                    have = 1;
                }
            }
            if (have) {
                continue;
            }
            if (n - first_new >= ELFSO_MAX_OBJECTS) {
                rc = fail("too many objects", needed);
                break;
            }
            for (l = 0; needed[l] != '\0'; l++) {
                if (needed[l] == '/') {
                    has_slash = 1;
                }
            }
            /* A name with a slash is a path; otherwise /lib, then the
             * directory of the object asked for. */
            if (has_slash) {
                scopy(full, needed, sizeof(full));
            } else {
                uint64 dl = 0;
                uint8 *probe = NULL;
                uint32 psize = 0;

                scopy(full, "/lib/", sizeof(full));
                scopy(full + 5, needed, sizeof(full) - 5);
                if (reader(full, &probe, &psize) != 0) {
                    while (dir[dl] != '\0') {
                        dl++;
                    }
                    scopy(full, dir, sizeof(full));
                    scopy(full + dl, needed, sizeof(full) - dl);
                } else {
                    release(probe);
                }
            }
            rc = load_one(as, full, needed, reader, release, &objs[n]);
            if (rc == ELFSO_OK) {
                n++;
            }
        }
    }

    /* Relocate every new object against the whole scope: the new ones in
     * load order, then what was already there. */
    if (rc == ELFSO_OK) {
        for (i = first_new; i < n && rc == ELFSO_OK; i++) {
            so_obj_t *scope = (so_obj_t *)kmalloc(sizeof(so_obj_t) * n);
            int s = 0;

            if (scope == NULL) {
                rc = fail("out of memory", objs[i].name);
                break;
            }
            for (j = first_new; j < n; j++) {
                scope[s++] = objs[j];
            }
            for (j = 0; j < first_new; j++) {
                scope[s++] = objs[j];
            }
            if (objs[i].rela != 0) {
                rc = apply(as, &objs[i], objs[i].rela, objs[i].relasz, scope, s);
            }
            if (rc == ELFSO_OK && objs[i].jmprel != 0) {
                rc = apply(as, &objs[i], objs[i].jmprel, objs[i].jmprelsz, scope, s);
            }
            kfree(scope);
        }
    }

    if (rc != ELFSO_OK) {
        for (i = first_new; i < n; i++) {
            unmap(as, &objs[i]);
        }
        kfree(objs);
        return ELFSO_ERR;
    }

    /* Dependencies first: the reverse of breadth-first load order. */
    for (i = n - 1; i >= first_new; i--) {
        elfso_module_t *m = &out->mods[out->count++];

        m->base       = objs[i].base;
        m->size       = objs[i].hi - objs[i].lo;
        m->init       = objs[i].init;
        m->init_array = objs[i].init_array;
        m->init_count = (uint32)(objs[i].init_arraysz / 8);
    }
    out->base = objs[first_new].base;
    kfree(objs);
    return ELFSO_OK;
}
