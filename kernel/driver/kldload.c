#include "bus.h"
#include "device.h"
#include "fs.h"
#include "irq.h"
#include "kheap.h"
#include "paging.h"
#include "pmm.h"
#include "vmalloc.h"
#include "kldload.h"
#include "kprintf.h"
#include "ksyms.h"
#include "modinit.h"
#include "typesk.h"

/* The LinuxKPI per-CPU area (kernel/driver/lkpi_smp.c). Declared here
 * rather than by including <linux/percpu.h>, which is a driver-side header. */
unsigned long lkpi_percpu_module_alloc(uint64 size, uint64 align);
void          lkpi_percpu_replicate(unsigned long cpu0_addr, uint64 size);
#define LKPI_PERCPU_SECTION "lkpi_percpu"

/* See kldload.h. */

/* --- the ET_REL subset of ELF -------------------------------------------
 *
 * Declared here rather than shared with elf.c on purpose: elf.c needs
 * PROGRAM headers and this needs SECTION headers, the two files have no
 * structure in common beyond the ehdr, and this tree already prefers a
 * duplicated declaration over a header two files must agree about. */

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
} __attribute__((packed)) kld_ehdr;

typedef struct {
    uint32 sh_name;
    uint32 sh_type;
    uint64 sh_flags;
    uint64 sh_addr;
    uint64 sh_offset;
    uint64 sh_size;
    uint32 sh_link;
    uint32 sh_info;
    uint64 sh_addralign;
    uint64 sh_entsize;
} __attribute__((packed)) kld_shdr;

typedef struct {
    uint32 st_name;
    uint8  st_info;
    uint8  st_other;
    uint16 st_shndx;
    uint64 st_value;
    uint64 st_size;
} __attribute__((packed)) kld_sym;

typedef struct {
    uint64 r_offset;
    uint64 r_info;
    int64  r_addend;
} __attribute__((packed)) kld_rela;

typedef char kld_assert_shdr[(sizeof(kld_shdr) == 64) ? 1 : -1];
typedef char kld_assert_sym[(sizeof(kld_sym) == 24) ? 1 : -1];
typedef char kld_assert_rela[(sizeof(kld_rela) == 24) ? 1 : -1];

#define ET_REL          1
#define EM_X86_64       0x3E

#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_NOBITS      8

#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4

#define SHN_UNDEF       0
#define SHN_ABS         0xFFF1
#define SHN_COMMON      0xFFF2

/* The relocation types gcc -c actually emits for kernel-model x86-64 code.
 * Anything else is refused rather than guessed at - a wrong relocation is a
 * jump to the wrong place, which is far harder to diagnose than a load that
 * declined. */
#define R_X86_64_64     1
#define R_X86_64_PC32   2
#define R_X86_64_PLT32  4
#define R_X86_64_32     10
#define R_X86_64_32S    11

#define ELF64_R_SYM(i)  ((uint32)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32)((i) & 0xFFFFFFFFu))

/* Concurrent modules, not modules ever loaded. It was 8 with no unload path,
 * which made it a lifetime budget; a slot is reusable now, so this is only
 * "how many at once". 16 because the table is 16 pointer-sized fields wide
 * and doubling it costs a few hundred bytes of .bss. */
#define KLD_MAX_MODULES  16

/* --- where a module's image lives, and why it is not the heap ------------
 *
 * It WAS the heap: one kmalloc_a for every SHF_ALLOC section, text and data
 * together. That is why module .text was W+X, which the ordered TODO lists as
 * the oldest open security property in the tree - the kernel had enforced W^X
 * on userspace since item 11 Part 6 and on PE drivers since item 4, and left
 * its own loadable modules writable and executable.
 *
 * The heap cannot be fixed in place. Page protections are per PAGE, and a
 * heap allocation shares its first and last pages with whatever the allocator
 * put next to it - so marking a module's text read-execute would mark some
 * other object's bytes read-execute too, and marking the rest no-execute
 * would take the execute bit off part of the heap. The image needs pages
 * nobody else is in.
 *
 * So modules get their own VA window, reserved once from the kernel VA
 * allocator exactly as the PE driver window is (kernel/exec/pe.c).
 *
 * --- and it is a BITMAP now, not a bump pointer ---------------------------
 *
 * It was a bump pointer, and the comment here said why: "a free list for
 * memory that is never freed is a data structure whose only correct value is
 * the one it starts with", because there was no unload path. There is one now
 * (kld_unload below), and a bump pointer that cannot rewind turns every
 * unload into a permanent 4MB-window leak - eight loads of a large module and
 * the window is gone, whether or not anything is still loaded.
 *
 * One bit per page: 4MB / 4KB = 1024 pages, sixteen uint64s, first fit. Not a
 * free list of extents, which is the other obvious choice and is strictly
 * worse here: a bitmap coalesces for free (adjacent freed pages are just
 * adjacent clear bits, with no merge step to get wrong) and the whole search
 * is sixteen words. Fragmentation is real - first fit over pages can refuse
 * an allocation that would fit if the holes were moved together - and it is
 * accepted, because moving a module means re-relocating it. */
#define KLD_IMAGE_WINDOW  0x00400000ULL   /* 4MB of VA for every module     */
#define KLD_IMAGE_FALLBACK 0xFFFFFFFFB0000000ULL
#define KLD_IMAGE_PAGES   (KLD_IMAGE_WINDOW / PMM_PAGE_SIZE)
#define KLD_BITMAP_WORDS  ((KLD_IMAGE_PAGES + 63) / 64)

static uint64 kld_window_base;
static uint64 kld_page_used[KLD_BITMAP_WORDS];

void kld_image_window_init(void) {
    uint64 got = kvm_alloc_range(KLD_IMAGE_WINDOW, PMM_PAGE_SIZE);

    /* The literal is a fallback, matching pe_driver_window_init: a reservation
     * that fails must not stop modules loading, it must only stop them being
     * collision-checked against the heap and the kernel stacks. */
    kld_window_base = (got != 0) ? got : KLD_IMAGE_FALLBACK;
}

/* Round to a whole number of pages. Every image starts on a page boundary so
 * that one module's text can never share a page with another's data. */
static uint64 page_up(uint64 v) {
    return (v + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
}

static int page_bit(uint64 idx) {
    return (kld_page_used[idx >> 6] >> (idx & 63)) & 1;
}

static void page_bit_set(uint64 idx, int on) {
    uint64 mask = 1ULL << (idx & 63);

    if (on) {
        kld_page_used[idx >> 6] |= mask;
    } else {
        kld_page_used[idx >> 6] &= ~mask;
    }
}

/* Map `bytes` of fresh, zeroed, WRITABLE and NON-EXECUTABLE pages.
 *
 * Writable because relocation has to patch the image, and non-executable
 * because nothing should be running out of it yet. kld_image_seal below is
 * what turns the text half into read-execute once the patching is done - and
 * the ORDER is the whole point: there is no moment at which a page is both
 * writable and executable. */
static uint8 *kld_image_alloc(uint64 bytes) {
    uint64 need = page_up(bytes) / PMM_PAGE_SIZE;
    uint64 start = 0;
    uint64 run   = 0;
    uint64 i;
    uint64 base;
    uint64 off;

    if (kld_window_base == 0) {
        kld_image_window_init();
    }
    if (need == 0 || need > KLD_IMAGE_PAGES) {
        return NULL;
    }

    /* First fit. `run` is the length of the free stretch ending at i. */
    for (i = 0; i < KLD_IMAGE_PAGES; i++) {
        if (page_bit(i)) {
            run = 0;
            continue;
        }
        if (run == 0) {
            start = i;
        }
        run++;
        if (run == need) {
            break;
        }
    }
    if (run != need) {
        return NULL;
    }
    base = kld_window_base + start * PMM_PAGE_SIZE;

    for (off = 0; off < need * PMM_PAGE_SIZE; off += PMM_PAGE_SIZE) {
        if (vmm_alloc_page_in(vmm_kernel_space(), base + off,
                              PAGE_PRESENT | PAGE_RW | PAGE_NX) == 0) {
            /* Unwind the pages already mapped, so a partial failure does not
             * leave holes in the window that the next module would load
             * across. */
            uint64 back;

            for (back = 0; back < off; back += PMM_PAGE_SIZE) {
                vmm_unmap_page_in(vmm_kernel_space(), base + back, 1);
            }
            return NULL;
        }
    }
    for (i = 0; i < need; i++) {
        page_bit_set(start + i, 1);
    }

    {
        uint8 *p = (uint8 *)base;
        uint64 n;

        for (n = 0; n < need * PMM_PAGE_SIZE; n++) {
            p[n] = 0;
        }
    }
    return (uint8 *)base;
}

/* Make [base, base+bytes) read-EXECUTE: present, not writable, not NX.
 *
 * The frame stays where it is; only the flags change. There is no
 * vmm_protect, so this reads the physical address back out of the mapping and
 * re-maps it - which is the same operation with the invalidation handled by
 * vmm_map_page_in, and is why it is not simply poking the PTE. */
static void kld_image_seal(uint64 base, uint64 bytes) {
    uint64 off;

    for (off = 0; off < page_up(bytes); off += PMM_PAGE_SIZE) {
        phys_addr_t phys = vmm_get_phys_in(vmm_kernel_space(), base + off);

        if (phys == 0) {
            continue;
        }
        (void)vmm_map_page_in(vmm_kernel_space(), base + off, phys,
                              PAGE_PRESENT);
    }
}

/* Release an image: unmap its pages and give its window back.
 *
 * Called by the failure paths in kld_load AND by kld_unload. The frames go
 * back to the PMM (vmm_unmap_page_in's second argument) and the bits go back
 * to the bitmap, so a module unloaded and reloaded lands on the same pages -
 * which is not a coincidence to rely on, but IS the thing the unload selftest
 * asserts, because a base address that moves is the signature of a window
 * that was never reclaimed. */
static void kld_image_free(uint8 *mem, uint64 bytes) {
    uint64 base = (uint64)mem;
    uint64 off;

    if (mem == NULL) {
        return;
    }
    for (off = 0; off < page_up(bytes); off += PMM_PAGE_SIZE) {
        uint64 va = base + off;

        vmm_unmap_page_in(vmm_kernel_space(), va, 1);
        if (kld_window_base != 0 && va >= kld_window_base &&
            va < kld_window_base + KLD_IMAGE_WINDOW) {
            page_bit_set((va - kld_window_base) / PMM_PAGE_SIZE, 0);
        }
    }
}

#define KLD_MAX_SECTIONS 64
#define KLD_NAME_MAX     32

typedef struct {
    char   name[KLD_NAME_MAX];
    void  *base;          /* the one allocation holding every section */
    uint64 size;
    uint64 exec_size;   /* page-aligned extent of the executable group */

    /* The placed .genesis_modexit section: an array of function pointers to
     * call on unload. Held as an address+count rather than copied into a
     * fixed array here, because the section IS the array - it lives in the
     * module's own data, it is already relocated, and copying it would be a
     * second copy to keep in step for no gain. Zero for a module with no
     * exit function, which is legal and common. */
    uint64 exit_ptrs;
    uint64 exit_count;

    int    in_use;
} kld_module_t;

static kld_module_t modules[KLD_MAX_MODULES];
static int          module_count;

const char *kld_strerror(int rc) {
    switch (rc) {
        case KLD_OK:             return "ok";
        case KLD_ERR_MAGIC:      return "not an ELF file";
        case KLD_ERR_TYPE:       return "not ET_REL (build with -c, not -shared)";
        case KLD_ERR_MACHINE:    return "wrong architecture";
        case KLD_ERR_TRUNCATED:  return "truncated";
        case KLD_ERR_NOMEM:      return "out of memory";
        case KLD_ERR_SYMBOL:     return "undefined symbol the kernel does not export";
        case KLD_ERR_RELOC:      return "unsupported relocation type";
        case KLD_ERR_NOENTRY:    return "no init function";
        case KLD_ERR_INITFAIL:   return "the module's init function declined";
        case KLD_ERR_FULL:       return "module table full";
        default:                 return "unknown";
    }
}

static int name_eq(const char *a, const char *b) {
    uint32 i = 0;

    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

/* Is section `i` the LinuxKPI per-CPU section? Such a section is not part
 * of the module image: it is placed in the per-CPU area, where CPU n's copy
 * of every variable in it is at the same offset from CPU 0's as for every
 * other per-CPU object (see <linux/percpu.h>). */
static int is_percpu_section(const uint8 *base, uint64 size, const kld_ehdr *eh,
                             const kld_shdr *sh, uint32 i) {
    const kld_shdr *strs;

    if (eh->e_shstrndx >= eh->e_shnum) {
        return 0;
    }
    strs = &sh[eh->e_shstrndx];
    if (strs->sh_offset + strs->sh_size > size || sh[i].sh_name >= strs->sh_size) {
        return 0;
    }
    return name_eq((const char *)(base + strs->sh_offset + sh[i].sh_name),
                   LKPI_PERCPU_SECTION);
}

static uint64 align_up(uint64 v, uint64 a) {
    if (a < 2) {
        return v;
    }
    return (v + a - 1) & ~(a - 1);
}

/* Where each section landed, indexed by section number. Zero means "not
 * placed" - a section with no SHF_ALLOC, which relocations may still
 * reference through a symbol but never need an address for.
 *
 * Passed down rather than file-static, which is what it was first. A module's
 * init function is called from inside kld_load, and a module that loaded
 * another module would re-enter and clobber the outer load's table - so the
 * outer relocations would resolve against the inner module's section
 * addresses. Nothing does that today; 512 bytes of stack is a cheap way for
 * it never to become possible. */

/* The address a symbol stands for. Three cases, and getting the first two
 * confused is the classic module-loader bug:
 *
 *   defined in this object  -> where its section landed, plus st_value
 *   undefined (SHN_UNDEF)   -> the kernel's, through ksyms
 *   absolute (SHN_ABS)      -> st_value verbatim, no relocation at all
 */
static int symbol_address(const kld_sym *sym, const char *strings,
                          const uint64 *placed, uint64 *out) {
    const char *name = strings + sym->st_name;

    if (sym->st_shndx == SHN_ABS) {
        *out = sym->st_value;
        return KLD_OK;
    }
    if (sym->st_shndx == SHN_UNDEF) {
        uint64 addr = ksym_resolve(name);

        if (addr == 0) {
            kprintf_c(0x0C, "kld: undefined symbol '%s'\n", name);
            return KLD_ERR_SYMBOL;
        }
        *out = addr;
        return KLD_OK;
    }
    if (sym->st_shndx >= KLD_MAX_SECTIONS || placed[sym->st_shndx] == 0) {
        kprintf_c(0x0C, "kld: symbol '%s' is in an unplaced section\n", name);
        return KLD_ERR_SYMBOL;
    }
    *out = placed[sym->st_shndx] + sym->st_value;
    return KLD_OK;
}

/* Take a module table slot. Split out because BOTH entry-point mechanisms
 * have to do it before calling anything, and the .genesis_modinit path may
 * call several functions - so "record it once, before the first one" is a
 * rule that has to hold across two call sites now rather than being a
 * straight line through one. */
static uint64 pending_exec_size;   /* set by kld_load before it records */
static uint64 pending_exit_ptrs;   /* likewise: the placed .genesis_modexit */
static uint64 pending_exit_count;

static void record_module(const char *name, void *base, uint64 size) {
    kld_module_t *mod = &modules[module_count];
    uint32 k = 0;

    while (name[k] != '\0' && k < KLD_NAME_MAX - 1) {
        mod->name[k] = name[k];
        k++;
    }
    mod->name[k] = '\0';
    mod->base      = base;
    mod->size      = size;
    mod->exec_size  = pending_exec_size;
    mod->exit_ptrs  = pending_exit_ptrs;
    mod->exit_count = pending_exit_count;
    mod->in_use     = 1;
    module_count++;
}

int kld_load(const char *name, const void *image, uint64 size) {
    const uint8 *base = (const uint8 *)image;
    const kld_ehdr *eh = (const kld_ehdr *)image;
    const kld_shdr *sh;
    uint64 total = 0;
    uint64 exec_end = 0;      /* page-aligned end of the executable group */
    int    pass;
    uint8 *mem = NULL;
    uint64 cursor;
    uint32 i;
    int rc = KLD_OK;
    uint64 init_addr = 0;
    int    modinit_calls = 0;   /* entries called out of .genesis_modinit */
    int    recorded = 0;        /* has this module taken a table slot yet? */
    uint64 placed[KLD_MAX_SECTIONS];

    if (size < sizeof(kld_ehdr)) {
        return KLD_ERR_TRUNCATED;
    }
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        return KLD_ERR_MAGIC;
    }
    if (eh->e_type != ET_REL)      return KLD_ERR_TYPE;
    if (eh->e_machine != EM_X86_64) return KLD_ERR_MACHINE;
    if (eh->e_shentsize != sizeof(kld_shdr)) return KLD_ERR_TRUNCATED;
    if (eh->e_shnum > KLD_MAX_SECTIONS) return KLD_ERR_TRUNCATED;
    if (eh->e_shoff + (uint64)eh->e_shnum * sizeof(kld_shdr) > size) {
        return KLD_ERR_TRUNCATED;
    }
    if (module_count >= KLD_MAX_MODULES) {
        return KLD_ERR_FULL;
    }

    sh    = (const kld_shdr *)(base + eh->e_shoff);

    for (i = 0; i < KLD_MAX_SECTIONS; i++) {
        placed[i] = 0;
    }

    /* --- pass 1: how much memory, honouring each section's alignment -----
     *
     * One allocation for the whole module rather than one per section. A
     * per-section allocation would be tidier to free, but PC32 relocations
     * reach BETWEEN sections - .text calling into .text.unlikely, or loading
     * from .rodata - and a 32-bit displacement only spans 2GB. Sections
     * scattered across the heap would work until two of them landed more
     * than 2GB apart, which on this kernel is never, and on a bigger one is
     * an intermittent failure nobody would connect to the loader. */
    /* EXECUTABLE SECTIONS FIRST, then a page boundary, then everything else.
     *
     * The grouping is what makes W^X possible at all. Protections are per
     * page, so text and data interleaved in address order means every page
     * that holds any text must be executable and every page that holds any
     * data must be writable - and the pages holding both must be both, which
     * is the W+X this is fixing. Two groups with a page-aligned seam means no
     * page is ever in both halves.
     *
     * Two passes over the section table with the same arithmetic, rather than
     * a sort: the table is small, and a sort would need somewhere to put the
     * order that the relocation pass could then read back by index. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < eh->e_shnum; i++) {
            int is_exec = (sh[i].sh_flags & SHF_EXECINSTR) != 0;

            if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) {
                continue;
            }
            if (is_exec != (pass == 0)) {
                continue;
            }
            if (is_percpu_section(base, size, eh, sh, i)) {
                continue;                   /* placed in the per-CPU area */
            }
            total = align_up(total, sh[i].sh_addralign);
            total += sh[i].sh_size;
        }
        if (pass == 0) {
            /* The seam. Recorded as well as rounded to, because the seal at
             * the end has to know how much to make executable. */
            total    = page_up(total);
            exec_end = total;
        }
    }
    if (total == 0) {
        return KLD_ERR_NOENTRY;
    }

    /* Its own pages, not the heap. See kld_image_alloc: a heap allocation
     * shares its first and last page with other objects, so the protections
     * this whole arrangement exists to set could not be set without setting
     * them for somebody else's bytes too. */
    pending_exec_size = exec_end;
    mem = kld_image_alloc(total);
    if (mem == NULL) {
        return KLD_ERR_NOMEM;
    }

    /* --- pass 2: place and copy ------------------------------------------
     * The same two-group order as the sizing pass above, or the addresses
     * would not match the sizes. */
    cursor = 0;
    for (pass = 0; pass < 2; pass++) {
    for (i = 0; i < eh->e_shnum; i++) {
        int is_exec = (sh[i].sh_flags & SHF_EXECINSTR) != 0;

        if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) {
            continue;
        }
        if (is_exec != (pass == 0)) {
            continue;
        }
        if (is_percpu_section(base, size, eh, sh, i)) {
            uint8 *pc;
            uint64 b;

            placed[i] = lkpi_percpu_module_alloc(sh[i].sh_size,
                                                 sh[i].sh_addralign);
            if (placed[i] == 0) {
                kprintf_c(0x0C, "kld: %s: per-CPU area exhausted (%lx bytes)\n",
                          name, sh[i].sh_size);
                kld_image_free(mem, total);
                return KLD_ERR_NOMEM;
            }
            pc = (uint8 *)placed[i];
            for (b = 0; b < sh[i].sh_size; b++) {
                pc[b] = (sh[i].sh_type == SHT_NOBITS ||
                         sh[i].sh_offset + b >= size)
                        ? 0 : base[sh[i].sh_offset + b];
            }
            continue;
        }
        cursor = align_up(cursor, sh[i].sh_addralign);
        placed[i] = (uint64)(mem + cursor);

        if (sh[i].sh_type == SHT_NOBITS) {
            /* .bss - no bytes in the file. Zeroed, for the same reason
             * elf.c zeroes it: a module's globals starting as garbage
             * presents as corruption a long way from here. */
            uint64 b;
            for (b = 0; b < sh[i].sh_size; b++) {
                mem[cursor + b] = 0;
            }
        } else {
            uint64 b;
            if (sh[i].sh_offset + sh[i].sh_size > size) {
                kld_image_free(mem, total);
                return KLD_ERR_TRUNCATED;
            }
            for (b = 0; b < sh[i].sh_size; b++) {
                mem[cursor + b] = base[sh[i].sh_offset + b];
            }
        }
        cursor += sh[i].sh_size;
    }
    if (pass == 0) {
        cursor = exec_end;
    }
    }

    /* --- pass 3: relocate ------------------------------------------------ */
    for (i = 0; i < eh->e_shnum && rc == KLD_OK; i++) {
        const kld_rela *rela;
        const kld_sym  *syms;
        const char     *strings;
        uint64 target_base;
        uint64 n, k;

        if (sh[i].sh_type != SHT_RELA) {
            continue;
        }
        /* sh_info is the section being relocated; sh_link is the symbol
         * table. Swapping those two reads relocations against the wrong
         * thing and is the other classic bug here. */
        if (sh[i].sh_info >= eh->e_shnum || sh[i].sh_link >= eh->e_shnum) {
            rc = KLD_ERR_TRUNCATED;
            break;
        }
        target_base = placed[sh[i].sh_info];
        if (target_base == 0) {
            /* Relocations against a section that was not loaded - debug
             * info, typically. Nothing to patch. */
            continue;
        }
        if (sh[i].sh_offset + sh[i].sh_size > size) {
            rc = KLD_ERR_TRUNCATED;
            break;
        }

        rela    = (const kld_rela *)(base + sh[i].sh_offset);
        syms    = (const kld_sym *)(base + sh[sh[i].sh_link].sh_offset);
        strings = (const char *)(base + sh[sh[sh[i].sh_link].sh_link].sh_offset);
        n       = sh[i].sh_size / sizeof(kld_rela);

        for (k = 0; k < n; k++) {
            uint32 type = ELF64_R_TYPE(rela[k].r_info);
            uint32 symi = ELF64_R_SYM(rela[k].r_info);
            uint64 where = target_base + rela[k].r_offset;
            uint64 value = 0;

            rc = symbol_address(&syms[symi], strings, placed, &value);
            if (rc != KLD_OK) {
                break;
            }
            value = (uint64)((int64)value + rela[k].r_addend);

            switch (type) {
                case R_X86_64_64:
                    *(uint64 *)where = value;
                    break;
                case R_X86_64_PC32:
                case R_X86_64_PLT32: {
                    /* PLT32 is treated as PC32, which is correct and is what
                     * every kernel module loader does: there is no
                     * procedure linkage table in a statically linked kernel,
                     * so a call through a "PLT entry" is just a direct call.
                     *
                     * The RANGE CHECK is the part that matters. A 32-bit
                     * displacement reaches +/-2GB; silently truncating one
                     * that does not fit produces a call into the middle of
                     * something else. */
                    int64 disp = (int64)value - (int64)where;

                    if (disp > 0x7FFFFFFFLL || disp < -0x80000000LL) {
                        kprintf_c(0x0C, "kld: PC32 displacement %lx does not "
                                        "fit in 32 bits\n", (uint64)disp);
                        rc = KLD_ERR_RELOC;
                        break;
                    }
                    *(uint32 *)where = (uint32)(int32)disp;
                    break;
                }
                case R_X86_64_32:
                    if (value > 0xFFFFFFFFULL) {
                        rc = KLD_ERR_RELOC;
                        break;
                    }
                    *(uint32 *)where = (uint32)value;
                    break;
                case R_X86_64_32S: {
                    int64 sv = (int64)value;

                    if (sv > 0x7FFFFFFFLL || sv < -0x80000000LL) {
                        rc = KLD_ERR_RELOC;
                        break;
                    }
                    *(uint32 *)where = (uint32)(int32)sv;
                    break;
                }
                default:
                    kprintf_c(0x0C, "kld: relocation type %d not supported\n",
                              type);
                    rc = KLD_ERR_RELOC;
                    break;
            }
            if (rc != KLD_OK) {
                break;
            }
        }
    }

    /* --- SEAL, before a single instruction runs out of this image --------
     *
     * The moment the module stops being writable-and-not-executable and its
     * text becomes executable-and-not-writable. There is no instant at which
     * it is both, which is the whole property: a W^X window that opens for
     * one instruction is not a W^X window.
     *
     * HERE and not after the load finishes, which is where it went first and
     * was wrong. pass 4a below CALLS INTO THE MODULE - .genesis_modinit is a
     * list of function pointers and they run immediately - so a seal placed
     * after the load completes is a seal placed after the module has already
     * executed. The symptom was exact and worth keeping: an instruction-fetch
     * page fault, at a present page, inside the second module loaded.
     *
     * Only on success. A module that failed to relocate is about to be
     * unmapped, and sealing it first would be work done to memory on its way
     * out. */
    if (rc == KLD_OK) {
        kld_image_seal((uint64)mem, exec_end);
    }

    /* --- the exit pointers, found BEFORE anything is called --------------
     *
     * .genesis_modexit is modinit's mirror (see modinit.h) and it is located
     * here, ahead of pass 4a, for a reason that is about ORDER rather than
     * tidiness: pass 4a records the module in the table and then calls its
     * init functions, and record_module copies these two fields as it goes.
     * Locating the section afterwards would record every module as having no
     * exit function and then quietly fix it up - or not, on the path where
     * record_module already ran.
     *
     * A module with no exit section is normal, not an error. Most modules
     * have nothing to undo. */
    /* The per-CPU section's relocations are done: now every CPU's copy can
     * be given its initial contents. Before the init functions run, because
     * they are the first code that may read another CPU's copy. */
    if (rc == KLD_OK) {
        for (i = 0; i < eh->e_shnum; i++) {
            if ((sh[i].sh_flags & SHF_ALLOC) && sh[i].sh_size != 0 &&
                placed[i] != 0 && is_percpu_section(base, size, eh, sh, i)) {
                lkpi_percpu_replicate((unsigned long)placed[i], sh[i].sh_size);
            }
        }
    }

    pending_exit_ptrs  = 0;
    pending_exit_count = 0;
    if (rc == KLD_OK && eh->e_shstrndx < eh->e_shnum &&
        sh[eh->e_shstrndx].sh_offset + sh[eh->e_shstrndx].sh_size <= size) {
        const char *shstr = (const char *)(base + sh[eh->e_shstrndx].sh_offset);

        for (i = 0; i < eh->e_shnum; i++) {
            if (!name_eq(shstr + sh[i].sh_name, GENESIS_MODEXIT_SECTION) ||
                placed[i] == 0) {
                continue;
            }
            pending_exit_ptrs  = placed[i];
            pending_exit_count = sh[i].sh_size / sizeof(uint64);
            break;
        }
    }

    /* --- pass 4a: the .genesis_modinit section ---------------------------
     *
     * The preferred mechanism, and the one real driver source reaches
     * through DRIVER_MODULE / module_pci_driver / module_init - see
     * modinit.h for why a section rather than a known symbol name.
     *
     * Found by name out of the section header string table, which is why
     * e_shstrndx is read here and nowhere else in this file. A module may
     * have several entries (a source file with two DRIVER_MODULEs is legal
     * and upstream does it), so every pointer in the section is called, and
     * the FIRST one that declines fails the whole load. */
    if (rc == KLD_OK && eh->e_shstrndx < eh->e_shnum) {
        const char *shstr = (const char *)(base + sh[eh->e_shstrndx].sh_offset);

        if (sh[eh->e_shstrndx].sh_offset + sh[eh->e_shstrndx].sh_size <= size) {
            for (i = 0; i < eh->e_shnum; i++) {
                const uint64 *fns;
                uint64 n, k;

                if (!name_eq(shstr + sh[i].sh_name, GENESIS_MODINIT_SECTION) ||
                    placed[i] == 0) {
                    continue;
                }
                fns = (const uint64 *)placed[i];
                n   = sh[i].sh_size / sizeof(uint64);

                for (k = 0; k < n && rc == KLD_OK; k++) {
                    int (*fn)(void) = (int (*)(void))fns[k];
                    int err;

                    if (fn == NULL) {
                        continue;
                    }
                    /* Recorded BEFORE the call, same as the by-name path
                     * below and for the same reason: an init that asks about
                     * loaded modules must see a consistent table. */
                    if (!recorded) {
                        record_module(name, mem, total);
                        recorded = 1;
                    }
                    err = fn();
                    if (err != 0) {
                        /* Honoured, not discarded. A driver whose init
                         * returns -ENODEV has said it does not want to be
                         * here; recording it as loaded means the failure is
                         * discovered later as a device that never responds. */
                        kprintf_c(0x0C, "kld: %s: init returned %d\n",
                                  name, err);
                        rc = KLD_ERR_INITFAIL;
                    }
                    modinit_calls++;
                }
            }
        }
    }

    /* --- pass 4b: the init function by NAME ------------------------------
     *
     * The fallback, for a module that predates the section mechanism or is
     * hand-written against the kernel directly (src/kmod/hello_kmod.c is
     * both). Three spellings are accepted, so a module written for either
     * upstream's convention loads unmodified.
     *
     * Called through void(*)(void) rather than int(*)(void) because that is
     * genuinely what these are declared as - kld_module_init returns void.
     * The section path above is where a return value is available and
     * checked. */
    for (i = 0; i < eh->e_shnum && rc == KLD_OK && modinit_calls == 0 &&
                init_addr == 0; i++) {
        const kld_sym *syms;
        const char *strings;
        uint64 n, k;

        if (sh[i].sh_type != SHT_SYMTAB) {
            continue;
        }
        syms    = (const kld_sym *)(base + sh[i].sh_offset);
        strings = (const char *)(base + sh[sh[i].sh_link].sh_offset);
        n       = sh[i].sh_size / sizeof(kld_sym);

        for (k = 0; k < n; k++) {
            const char *sname = strings + syms[k].st_name;

            if (name_eq(sname, "kld_module_init") ||
                name_eq(sname, "module_init") ||
                name_eq(sname, "init_module")) {
                if (syms[k].st_shndx < KLD_MAX_SECTIONS &&
                    placed[syms[k].st_shndx] != 0) {
                    init_addr = placed[syms[k].st_shndx] + syms[k].st_value;
                    break;
                }
            }
        }
    }

    /* Neither mechanism found anything to call. A relocatable object with no
     * entry point is not a module - it is an object file somebody copied
     * into /boot/kernel - and loading it would consume a module slot and a
     * heap allocation to no effect. */
    if (rc == KLD_OK && modinit_calls == 0 && init_addr == 0) {
        rc = KLD_ERR_NOENTRY;
    }
    if (rc != KLD_OK) {
        /* Unwound, including the case where the module WAS recorded before a
         * .genesis_modinit entry declined. A failed load must not leave a
         * half-registered module behind. */
        if (recorded) {
            module_count--;
            modules[module_count].in_use = 0;
        }
        kld_image_free(mem, total);
        return rc;
    }

    if (!recorded) {
        record_module(name, mem, total);
    }

    if (init_addr != 0) {
        /* Called AFTER the module is recorded, so an init that asks about
         * loaded modules - or fails and leaves something registered - sees a
         * consistent table. */
        ((void (*)(void))init_addr)();
    }
    return KLD_OK;
}

/* --- is W^X actually in force? -------------------------------------------
 *
 * ROADMAP item 4's oldest security remainder was "module .text is W+X". This
 * is what says it is not any more, and it is a CHECK rather than a report
 * because the alternative ways to find out are both bad: trusting the loader
 * to have done what its comments say, or writing to module text on purpose
 * and taking the fault, which on a kernel means taking the machine.
 *
 * So the mapping is read back. Two assertions per module, and both are
 * needed - a loader that mapped everything read-execute would pass the first
 * and fail the second, and one that changed nothing would fail the first:
 *
 *   TEXT   present, NOT writable, NOT no-execute
 *   DATA   present, writable, no-execute
 *
 * The NX half is skipped when the CPU has no NX, because paging.c strips
 * PAGE_NX in that case and asserting it would be asserting something the
 * hardware declined to do. Reported as skipped rather than passed.
 *
 * --- what this check can and cannot fail on -------------------------------
 * It cannot fail on a MISSING seal, and that is a property of the design
 * rather than a hole in the test. An image is mapped writable and
 * NON-EXECUTABLE from the start, so a load that never seals faults on the
 * first instruction fetch into the module - measured, by disabling the seal:
 * the kernel dies inside kld_scan_entry before this check is reached. The
 * arrangement is fail-closed, and forgetting to seal produces a dead module
 * rather than a writable-and-executable one.
 *
 * What it does catch is the opposite error, which is silent: a seal that
 * covers too much, leaving the data half read-only or non-writable, or one
 * that only partly applied. Those produce a module that loads, runs its init,
 * and corrupts or faults later somewhere with no connection to the loader. */
int kld_wx_selftest(void) {
    int failures = 0;
    int nx = paging_nx_enabled();
    int i;

    for (i = 0; i < module_count; i++) {
        kld_module_t *m = &modules[i];
        uint64 base = (uint64)m->base;
        uint64 off;

        if (!m->in_use || m->exec_size == 0) {
            continue;
        }
        for (off = 0; off < m->exec_size; off += PMM_PAGE_SIZE) {
            uint64 f = vmm_get_flags_in(vmm_kernel_space(), base + off);

            if (!(f & PAGE_PRESENT) || (f & PAGE_RW)) {
                kprintf_c(0x0C, "kld W^X: %s text page %u is WRITABLE\n",
                          m->name, (uint32)(off / PMM_PAGE_SIZE));
                failures++;
            }
            if (nx && (f & PAGE_NX)) {
                kprintf_c(0x0C, "kld W^X: %s text page %u is NO-EXECUTE\n",
                          m->name, (uint32)(off / PMM_PAGE_SIZE));
                failures++;
            }
        }
        for (off = m->exec_size; off < m->size; off += PMM_PAGE_SIZE) {
            uint64 f = vmm_get_flags_in(vmm_kernel_space(), base + off);

            if (!(f & PAGE_PRESENT) || !(f & PAGE_RW)) {
                kprintf_c(0x0C, "kld W^X: %s data page %u is not writable\n",
                          m->name, (uint32)(off / PMM_PAGE_SIZE));
                failures++;
            }
            if (nx && !(f & PAGE_NX)) {
                kprintf_c(0x0C, "kld W^X: %s data page %u is EXECUTABLE\n",
                          m->name, (uint32)(off / PMM_PAGE_SIZE));
                failures++;
            }
        }
    }

    if (failures == 0) {
        kprintf("kld: W^X selftest passed (%d modules%s)\n", module_count,
                nx ? "" : ", NX unavailable - execute bits not checked");
    } else {
        kprintf_c(0x0C, "kld: W^X selftest FAILED (%d)\n", failures);
    }
    return failures;
}

/* --- unloading -----------------------------------------------------------
 *
 * ROADMAP item 4's last remainder: "no module unload path at all (module_exit
 * is accepted and never called, so a driver that allocates in init leaks)".
 *
 * --- the actual hazard is not the leak ------------------------------------
 *
 * A leak is what the roadmap entry names and it is the least of it. The thing
 * that kills the machine is a POINTER LEFT BEHIND. A module's driver_t, its
 * probe and attach functions, its interrupt handlers, its callout functions,
 * its softc: all of them live inside the image, and the last step of an
 * unload is to unmap that image. Anything still holding one of those pointers
 * is holding a pointer into unmapped kernel memory, and the fault it
 * eventually takes is in an interrupt handler or a timer tick, a long way
 * from here, with nothing on the screen naming the module.
 *
 * So this does not simply free things. It ASKS, of every registry that could
 * be holding such a pointer, "do you have anything inside these bytes?", and
 * refuses the unload if the answer is yes. The question is an address range
 * rather than a list the module declares, because a declared list is a list
 * somebody forgets to add to and a forgotten entry is not a compile error -
 * see bus.c's bus_unregister_range for the same argument at more length.
 *
 * --- which registries, and the honest limit -------------------------------
 *
 * Five are swept: the newbus driver tables (bus.c), the interrupt handler
 * pool (irq.c), the callout wheel (callout.c), the taskqueue (kern_
 * taskqueue.c) and the sysctl tree (kern_devsysctl.c). Those are the five a
 * driver in this tree can actually reach, and each of them can enumerate
 * itself completely, which is what makes the answer trustworthy rather than
 * reassuring.
 *
 * The sysctl one is worth a sentence, because it was the example this comment
 * used to give of what it could NOT check. sysctl_add_oid does not copy the
 * name, so a knob added by a module names itself with a string literal in
 * that module's .rodata - and oid_arg1 points at the driver's softc, which a
 * reader would follow. Adding the sweep is half of it; bus.c calling
 * device_sysctl_fini when it detaches a driver is the half that means the
 * sweep normally finds nothing.
 *
 * It is still NOT a proof that no pointer survives. A module that stored a
 * function pointer in some structure nothing here walks - an eventhandler
 * list, a field of another driver's softc - passes every check and still
 * faults. The list grows as the registries do; the mechanism is the part that
 * generalises. What can be said is the useful half: an unload that is REFUSED
 * is certainly unsafe, and an unload that succeeds has been checked against
 * everything this kernel knows how to enumerate.
 *
 * --- order, and the state a refusal can leave behind ---------------------
 *
 * Checks first, then actions - except that the exit function is BOTH. It is
 * the thing that releases the registrations, so it has to run before the
 * residual sweep, and it cannot be un-run if the sweep then finds something.
 * A module whose exit does not release everything it registered is therefore
 * left STOPPED BUT LOADED: refused, still mapped, still in the table, with its
 * exit already called. That is a bug in the module, it is reported as one,
 * and it is the safe end of the trade - the alternative is unmapping code
 * that something is about to call. */

/* Declared rather than included. kernel/bsd/compat/sys/callout.h and
 * sys/taskqueue.h are FreeBSD headers that want -D_KERNEL and the compat
 * include path; this file is ordinary kernel source and pulling that whole
 * environment in to reach two functions would make kldload.c a BSD
 * translation unit. Same arrangement kern_taskqueue.c uses in the other
 * direction for sched.h. */
int callout_count_in_range(uint64 base, uint64 size);
int taskqueue_count_in_range(uint64 base, uint64 size);
int sysctl_count_in_range(uint64 base, uint64 size);

/* Counted so the unload selftest can assert that a module's exit function was
 * actually reached - "kld_unload returned OK" is also what a stub that did
 * nothing would return. */
static uint64 kld_exit_calls;

uint64 kld_exit_call_count(void) {
    return kld_exit_calls;
}

static int find_module(const char *name) {
    int i;

    for (i = 0; i < module_count; i++) {
        if (modules[i].in_use && name_eq(modules[i].name, name)) {
            return i;
        }
    }
    return -1;
}

int kld_is_loaded(const char *name) {
    return find_module(name) >= 0;
}

/* device_ops_t tables pointing into the image, found through dev_iterate
 * rather than by reaching into device.c's pool. A char device registered by a
 * module and never detached is the same hazard as an interrupt handler: the
 * next read(2) on it calls through a function pointer into nothing. */
struct kld_devscan { uint64 base, size; int found; };

static int kld_devscan_cb(device_t *dev, void *ctx) {
    struct kld_devscan *sc = (struct kld_devscan *)ctx;
    uint64 ops = (uint64)dev->ops;

    if (ops >= sc->base && ops < sc->base + sc->size) {
        sc->found++;
    }
    return 0;
}

static int kld_residual_refs(uint64 base, uint64 size, const char *name) {
    struct kld_devscan sc;
    int irqs, cals, tsks, ctls;
    int total = 0;

    irqs = irq_count_handlers_in_range(base, size);
    cals = callout_count_in_range(base, size);
    tsks = taskqueue_count_in_range(base, size);
    ctls = sysctl_count_in_range(base, size);

    sc.base = base;
    sc.size = size;
    sc.found = 0;
    dev_iterate(kld_devscan_cb, &sc);

    total = irqs + cals + tsks + ctls + sc.found;
    if (total != 0) {
        kprintf_c(0x0C, "kld: %s: %d reference%s still point into the image "
                        "(%d irq, %d callout, %d task, %d sysctl, "
                        "%d device)\n",
                  name, total, total == 1 ? "" : "s",
                  irqs, cals, tsks, ctls, sc.found);
    }
    return total;
}

int kld_unload(const char *name) {
    int    idx = find_module(name);
    kld_module_t *m;
    uint64 base, size;
    uint64 k;
    int    drivers, leaked = 0;
    int    i;

    if (idx < 0) {
        return KLD_ERR_NOTLOADED;
    }
    m    = &modules[idx];
    base = (uint64)m->base;
    size = m->size;

    /* Ask before doing anything at all. A driver attached to a device with no
     * detach method is a refusal, and finding that out AFTER the exit
     * function has run would leave the module stopped for no reason. */
    if (bus_can_unregister_range(base, size) != 0) {
        kprintf_c(0x0C, "kld: %s: a driver is attached and has no detach "
                        "method - refusing to unload\n", name);
        return KLD_ERR_BUSY;
    }

    /* The module's own teardown. Runs with the image still mapped and still
     * executable, which is the only moment it can. */
    for (k = 0; k < m->exit_count; k++) {
        void (*fn)(void) = (void (*)(void))((const uint64 *)m->exit_ptrs)[k];

        if (fn == NULL) {
            continue;
        }
        fn();
        kld_exit_calls++;
    }

    /* Whatever the exit function left registered on the bus. Zero for a
     * well-behaved driver that called its own unregister on the way out, and
     * this is a sweep rather than a check because a driver_t sitting in a
     * devclass list is removable without the driver's cooperation - unlike a
     * live interrupt handler, which is not. */
    drivers = bus_unregister_range(base, size, &leaked);
    if (drivers < 0) {
        /* The exit function registered something new, or attached a driver
         * with no detach. Nothing was torn down; the module stays. */
        kprintf_c(0x0C, "kld: %s: bus refused deregistration after exit\n",
                  name);
        return KLD_ERR_BUSY;
    }
    if (leaked != 0) {
        kprintf_c(0x0E, "kld: %s: reclaimed %d bus resource%s its detach "
                        "method did not release\n", name, leaked,
                  leaked == 1 ? "" : "s");
    }

    /* And the registries nothing here can clean up on the module's behalf. */
    if (kld_residual_refs(base, size, name) != 0) {
        kprintf_c(0x0C, "kld: %s: STOPPED BUT STILL LOADED - its exit "
                        "function did not release everything\n", name);
        return KLD_ERR_BUSY;
    }

    kld_image_free((uint8 *)base, size);

    /* Compact the table rather than leaving a hole. Every other loop in this
     * file walks 0..module_count, and a hole would mean auditing all of them
     * for an in_use test - which is exactly the kind of invariant that holds
     * until somebody adds the fifth loop. */
    for (i = idx; i + 1 < module_count; i++) {
        modules[i] = modules[i + 1];
    }
    module_count--;
    modules[module_count].in_use = 0;

    kprintf("kld: unloaded %s (%d driver%s deregistered, %lx bytes)\n",
            name, drivers, drivers == 1 ? "" : "s", size);
    return KLD_OK;
}

/* --- is the unload path real? --------------------------------------------
 *
 * Two halves, because they fail in different ways and a test that only did
 * the first would pass on a stub.
 *
 * HALF ONE - the four range sweeps. Each is checked in BOTH directions
 * against a TIGHT range: the address of one static object here, and nothing
 * else. Before the registration the sweep must answer 0 and after it 1, which
 * is what distinguishes a working sweep from one that returns 0 for
 * everything - and a sweep that always returns 0 is the failure mode that
 * matters, because it turns kld_unload's refusal into a rubber stamp.
 *
 * The range is one object wide rather than "the kernel", so that a handler
 * some other subsystem registered cannot make the positive case pass by
 * accident. And it is the CTX and the STRUCT that are registered in range,
 * not the function - the function pointers here are kernel text either way,
 * so a sweep that only looked at functions would fail these, which is
 * deliberate: that is the half irq.c's comment says is the quiet one.
 *
 * HALF TWO - an actual module, unloaded and reloaded. hellokm is the right
 * subject precisely because it registers nothing: it isolates the image
 * lifecycle from the deregistration logic, so a failure here means the pages
 * or the table, not the bus.
 *
 * The check that a stub cannot pass is the last one. Unload, then reload, and
 * require the module to come back AT THE SAME ADDRESS. A kld_unload that
 * cleared the table entry and forgot the bitmap would satisfy every other
 * assertion here and put the reloaded module one image further up the window.
 * The unmapped-pages assertion is the other one worth its line: it reads the
 * page tables back, the same way the W^X selftest does, rather than trusting
 * that a call to kld_image_free did anything. */

static int kld_st_irq(void *ctx) { (void)ctx; return 0; }

/* The object the irq sweep is aimed at. A static in .bss, so its address is
 * known and stable and the range below is exact. */
static int kld_st_marker;

/* The callout and taskqueue halves live in kernel/bsd/kld_sweep_selftest.c
 * and not here, for the reason the declarations above give: those are FreeBSD
 * compat types and this is not a BSD translation unit. Split by include
 * environment rather than by subject, which is not ideal - the four checks
 * are one idea - and is better than making kldload.c compile with -D_KERNEL
 * and the compat search path ahead of kernel/include. */
int kld_bsd_sweep_selftest(void);

int kld_unload_selftest(void) {
    int    failures = 0;
    uint64 before, after;
    uint64 base, size, off;
    int    idx;
    int    n;

#define KST(cond, what)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            kprintf_c(0x0C, "kld: unload selftest: %s\n", (what));          \
            failures++;                                                     \
        }                                                                   \
    } while (0)

    /* --- half one: the sweeps, both directions, one object wide ---------- */

    /* irq: the CTX in range, the handler in kernel text - so a sweep that
     * only looked at the function pointer fails this, which is the point. */
    KST(irq_count_handlers_in_range((uint64)&kld_st_marker,
                                    sizeof(kld_st_marker)) == 0,
        "irq sweep found a handler before one was registered");
    if (irq_register(9, kld_st_irq, &kld_st_marker) == 0) {
        KST(irq_count_handlers_in_range((uint64)&kld_st_marker,
                                        sizeof(kld_st_marker)) == 1,
            "irq sweep missed a handler whose ctx is in range");
        irq_unregister_handler(9, kld_st_irq, &kld_st_marker);
        KST(irq_count_handlers_in_range((uint64)&kld_st_marker,
                                        sizeof(kld_st_marker)) == 0,
            "irq sweep still finds an unregistered handler");
    } else {
        KST(0, "irq_register declined - sweep untested");
    }

    failures += kld_bsd_sweep_selftest();

    /* --- half two: a real module, out and back in ------------------------ */

    idx = find_module("hellokm.ko");
    if (idx < 0) {
        idx = find_module("HELLOKM.KO");
    }
    if (idx < 0) {
        /* Not a pass and not a failure: this image does not carry the
         * module. Said out loud, because a silent zero here would read as
         * eight checks that ran. */
        kprintf_c(0x0E, "kld: unload selftest: hellokm not loaded - image "
                        "lifecycle NOT exercised\n");
        goto done;
    }

    base = (uint64)modules[idx].base;
    size = modules[idx].size;
    KST(modules[idx].exit_count >= 1,
        "hellokm has no .genesis_modexit entry - exit path untested");

    before = kld_exit_calls;
    n = module_count;
    KST(kld_unload("hellokm.ko") == KLD_OK || kld_unload("HELLOKM.KO") == KLD_OK,
        "kld_unload refused");
    after = kld_exit_calls;

    KST(after == before + 1, "the module's exit function was not called");
    KST(!kld_is_loaded("hellokm.ko") && !kld_is_loaded("HELLOKM.KO"),
        "still in the module table after unload");
    KST(module_count == n - 1, "the module table slot was not released");

    for (off = 0; off < size; off += PMM_PAGE_SIZE) {
        if (vmm_get_phys_in(vmm_kernel_space(), base + off) != 0) {
            KST(0, "an image page is still mapped after unload");
            break;
        }
    }

    /* Reload. kld_load_directories skips what is already loaded, so this
     * brings back exactly the one that went. */
    (void)kld_load_directories();
    idx = find_module("hellokm.ko");
    if (idx < 0) {
        idx = find_module("HELLOKM.KO");
    }
    KST(idx >= 0, "the module did not reload");
    if (idx >= 0) {
        KST((uint64)modules[idx].base == base,
            "reloaded at a different address - the image window was not "
            "reclaimed");
        KST(vmm_get_phys_in(vmm_kernel_space(), base) != 0,
            "the reloaded image is not mapped");
    }

done:
    if (failures == 0) {
        kprintf("kld: unload selftest passed\n");
    } else {
        kprintf_c(0x0C, "kld: unload selftest FAILED (%d)\n", failures);
    }
    return failures;
#undef KST
}

/* --- the directories ---------------------------------------------------- */

static const char *const kld_dirs[] = {
    "/boot/kernel",
    "/boot/modules",
    "/lib/modules",
    NULL
};

static int already_loaded(const char *name) {
    int i;

    for (i = 0; i < module_count; i++) {
        if (modules[i].in_use && name_eq(modules[i].name, name)) {
            return 1;
        }
    }
    return 0;
}

/* One directory's worth of state, threaded through fs_iterate's callback. */
struct kld_scan {
    const char *dir;
    int         loaded;
};

/* Case-INSENSITIVE, because the root volume is FAT and 8.3 names come back
 * upper-cased. The first version tested for lowercase ".ko" only and found
 * nothing at all on a volume that plainly had a module on it - the loader
 * was correct and the directory scan simply never handed it anything. */
static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ends_with_ko(const char *n) {
    uint32 k = 0;

    while (n[k] != '\0') {
        k++;
    }
    return k >= 3 && n[k - 3] == '.' &&
           lower(n[k - 2]) == 'k' && lower(n[k - 1]) == 'o';
}

static int kld_scan_entry(const fs_dirent_t *ent, void *ctx) {
    struct kld_scan *scan = (struct kld_scan *)ctx;
    char   path[160];
    uint8 *buf = NULL;
    uint32 len = 0;
    uint32 j = 0, k;
    int rc;

    if (ent->is_dir || !ends_with_ko(ent->name) || already_loaded(ent->name)) {
        return 0;
    }

    while (scan->dir[j] != '\0' && j < sizeof(path) - 2) {
        path[j] = scan->dir[j];
        j++;
    }
    path[j++] = '/';
    for (k = 0; ent->name[k] != '\0' && j + k < sizeof(path) - 1; k++) {
        path[j + k] = ent->name[k];
    }
    path[j + k] = '\0';

    if (fs_read_whole(path, &buf, &len) != 0 || buf == NULL) {
        return 0;
    }
    rc = kld_load(ent->name, buf, len);
    fs_free_file(buf);

    if (rc == KLD_OK) {
        scan->loaded++;
    } else {
        /* Reported, not silent. A module that is present and does not load
         * is exactly the thing somebody needs to be told about - it is why
         * they put it there. */
        kprintf_c(0x0C, "kld: %s: %s\n", path, kld_strerror(rc));
    }
    return 0;
}

int kld_load_directories(void) {
    int loaded = 0;
    int d;

    for (d = 0; kld_dirs[d] != NULL; d++) {
        fs_node_t dir;
        struct kld_scan scan;

        /* A missing directory is not an error and is not reported. Three of
         * these are conventions borrowed from other systems; a Genesis image
         * that ships none of them is normal. */
        if (fs_lookup(kld_dirs[d], &dir) != 0 || !dir.is_dir) {
            continue;
        }
        scan.dir    = kld_dirs[d];
        scan.loaded = 0;
        fs_iterate(&dir, kld_scan_entry, &scan);
        loaded += scan.loaded;
    }
    return loaded;
}

void kld_report(uint8 color) {
    int i;

    if (module_count == 0) {
        kprintf_c(color, "kld: no loadable modules found in /boot/kernel, "
                         "/boot/modules or /lib/modules\n");
        return;
    }
    kprintf_c(color, "kld: %d module%s loaded\n", module_count,
              module_count == 1 ? "" : "s");
    for (i = 0; i < module_count; i++) {
        kprintf_c(color, "  %s  at %lx  %lx bytes\n", modules[i].name,
                  (uint64)modules[i].base, modules[i].size);
    }
}
