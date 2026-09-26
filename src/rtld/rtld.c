/* rtld - the ELF dynamic linker.
 *
 * Written from scratch rather than vendored, and that decision is worth
 * defending because the plan said "vendor rtld-elf (BSD)".
 *
 * FreeBSD's rtld-elf is about 6000 lines and it is not 6000 lines of
 * relocation processing. It is thread-local storage across four models,
 * libmap.conf, symbol versioning, dl_iterate_phdr, filters, DT_AUXILIARY,
 * lazy binding with a hand-written PLT trampoline per architecture, and a
 * private allocator - all of it wired into FreeBSD libc internals that do not
 * exist here. Vendoring it means writing a shim for each of those before one
 * relocation is applied, and the shim is bigger than this file. The BSD
 * licence was never the obstacle; the coupling is.
 *
 * What IS taken from rtld-elf is its shape: a self-relocation bootstrap, a
 * breadth-first load order, a global symbol scope searched in that order, and
 * relocation last. That is the part that is hard to get right by reasoning
 * about it, and it is the part that is free to copy because it is a design
 * rather than code.
 *
 * --- Eager only ----------------------------------------------------------
 * Every relocation, including every PLT slot, is resolved before the program
 * starts. There is no lazy binding, no runtime-resolve trampoline, and
 * DT_BIND_NOW is treated as already true.
 *
 * This is the same simplification the PE side made by fixing ImageBase and
 * skipping relocations on day one, and it costs the same thing: startup time,
 * proportional to the number of symbols rather than to the number actually
 * called. What it buys is that there is no assembly trampoline, no
 * _dl_runtime_resolve, no reentrancy question about resolving a symbol while
 * inside a resolver, and - the part that matters for a first version - every
 * failure happens at load time, where there is a place to report it, instead
 * of at an arbitrary first call.
 *
 * --- What this deliberately does not do ----------------------------------
 *   Symbol versioning (DT_VERNEED / DT_VERSYM). Versions are IGNORED, not
 *   half-implemented: the first definition found wins. That is wrong for a
 *   library that exports two versions of one symbol and right for everything
 *   else, and a half-checked version is worse than an unchecked one because
 *   it fails on the libraries that need it most.
 *
 *   General-dynamic TLS (__tls_get_addr). Initial-exec already works for one
 *   module; per-module TLS blocks across several shared objects is a
 *   different problem and its own item. A general-dynamic relocation is
 *   REFUSED here rather than resolved to something plausible.
 *
 *   dlopen. Nothing needs it yet, and the load-time scope this builds is not
 *   the scope dlopen needs.
 *
 * --- The name --------------------------------------------------------------
 * /lib/ld-gen.so, not the ld-genesis-x86_64.so.1 that the musl/glibc
 * convention would suggest. The volume is FAT16 with no long-filename
 * support, so every staged name has to fit 8.3 - one dot, at most eight
 * characters of stem and three of extension. The conventional name has two
 * dots and a seventeen-character stem, and tools/fatfs.py refuses to write
 * it.
 *
 * Refusing is the right behaviour and it is worth saying why, because the two
 * halves of this disagree: fatfs.py ERRORS on a name that does not fit, while
 * the kernel's fat_name_to_entry silently TRUNCATES the stem at eight
 * characters. A long name would therefore have produced a file that could not
 * be written and, had it been written by some other tool, a lookup that
 * quietly searched for a different name. The build-time error is the only
 * place that failure is legible.
 *
 * Build: src/rtld/build.sh, staged to /lib/ld-gen.so
 */

typedef unsigned long  u64;
typedef long           i64;
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned short u16;
typedef unsigned char  u8;

#define NULL ((void *)0)

/* --- syscalls ------------------------------------------------------------
 *
 * Raw, because the linker runs before any libc is usable - including the libc
 * it is about to relocate. Calling into libc.so here would mean calling
 * through a GOT this file has not filled in yet. */

static i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    i64 ret;
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    register i64 r9  __asm__("r9")  = f;

    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c),
                        "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}

#define sc1(n,a)         sc6((n),(i64)(a),0,0,0,0,0)
#define sc2(n,a,b)       sc6((n),(i64)(a),(i64)(b),0,0,0,0)
#define sc3(n,a,b,c)     sc6((n),(i64)(a),(i64)(b),(i64)(c),0,0,0)
#define sc6m(n,a,b,c,d,e,f) sc6((n),(i64)(a),(i64)(b),(i64)(c),(i64)(d),(i64)(e),(i64)(f))

#define SYS_write        1
#define SYS_open         2
#define SYS_close        3
#define SYS_mmap         9
#define SYS_exit_group 231

#define O_RDONLY   0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED     0x10

/* --- ELF structures ------------------------------------------------------ */

typedef struct {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} Ehdr;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} Phdr;

typedef struct {
    i64 d_tag;
    u64 d_val;
} Dyn;

typedef struct {
    u32 st_name;
    u8  st_info;
    u8  st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
} Sym;

typedef struct {
    u64 r_offset;
    u64 r_info;
    i64 r_addend;
} Rela;

#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_TLS      7

#define DT_NULL      0
#define DT_NEEDED    1
#define DT_PLTRELSZ  2
#define DT_PLTGOT    3
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_INIT     12
#define DT_FINI     13
#define DT_SONAME   14
#define DT_RPATH    15
#define DT_JMPREL   23
#define DT_INIT_ARRAY   25
#define DT_INIT_ARRAYSZ 27
#define DT_PLTREL   20
#define DT_GNU_HASH 0x6FFFFEF5

#define ELF64_R_SYM(i)  ((u32)((i) >> 32))
#define ELF64_R_TYPE(i) ((u32)((i) & 0xFFFFFFFF))

#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_COPY       5
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8
#define R_X86_64_TPOFF64   18
#define R_X86_64_DTPMOD64  16
#define R_X86_64_DTPOFF64  17
#define R_X86_64_IRELATIVE 37

#define STB_WEAK   2
#define SHN_UNDEF  0

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_BASE   7
#define AT_ENTRY  9

/* --- output --------------------------------------------------------------
 *
 * Everything goes to fd 2 and every fatal path exits non-zero. A dynamic
 * linker that fails silently produces a process that dies at an address with
 * no symbol, and the whole reason for resolving eagerly is to make failures
 * arrive somewhere they can be described. */

static u64 slen(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

static void put(const char *s) {
    sc3(SYS_write, 2, s, slen(s));
}

static void puthex(u64 v) {
    char buf[19];
    int i;

    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        u64 nib = (v >> ((15 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    sc3(SYS_write, 2, buf, 18);
}

static void die(const char *what, const char *detail) {
    put("rtld: ");
    put(what);
    if (detail) {
        put(": ");
        put(detail);
    }
    put("\n");
    sc1(SYS_exit_group, 127);
    for (;;) { }
}

static int seq(const char *a, const char *b) {
    u64 i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* --- loaded objects ------------------------------------------------------ */

#define MAX_OBJECTS   16
#define MAX_NEEDED    16
#define RTLD_PATH_MAX 256

typedef struct {
    char        name[64];
    u64         base;           /* load bias                              */
    const Dyn  *dynamic;

    const char *strtab;
    const Sym  *symtab;
    u64         syment;

    /* GNU hash. The only hash this reads: DT_HASH is the SysV table and every
     * toolchain that produces a Genesis binary emits DT_GNU_HASH beside or
     * instead of it. Supporting one and falling back to a linear scan when it
     * is absent is smaller than supporting both, and the linear scan is
     * exercised by anything old enough to matter. */
    const u32  *gnu_hash;

    const Rela *rela;
    u64         relasz;
    const Rela *jmprel;
    u64         jmprelsz;

    void      (*init)(void);
    void      (**init_array)(void);
    u64         init_arraysz;

    int         relocated;
    int         initialised;

    const char *needed[MAX_NEEDED];
    int         needed_count;
} obj_t;

static obj_t objects[MAX_OBJECTS];
static int   object_count;

/* --- symbol lookup -------------------------------------------------------
 *
 * Linear over the symbol table, not through the hash.
 *
 * The hash tables exist to make this fast and the correctness does not depend
 * on them: a linear scan finds exactly the symbols a hash lookup would. With
 * eager binding the cost is real - every relocation scans every table - and
 * it is still the right first version, because a wrong GNU-hash bloom filter
 * produces a symbol that is silently NOT FOUND, which presents as a null call
 * at run time rather than as a lookup failure. Getting correct-and-slow
 * working first means the hash can be added later with something to check it
 * against.
 *
 * DT_GNU_HASH is still read, for one thing only: symndx, the index of the
 * first dynamic symbol. Below it are the undefined symbols, which can never
 * be a definition, and skipping them is what stops an undefined reference in
 * one library resolving to the undefined reference in another. */

static u64 gnu_hash_symndx(const obj_t *o) {
    if (o->gnu_hash == NULL) {
        return 0;
    }
    return o->gnu_hash[1];
}

/* Number of symbols, derived from the GNU hash chain array.
 *
 * There is no DT_SYMTABSZ - the ELF format simply does not record how many
 * dynamic symbols there are, on the assumption that you always arrive through
 * a hash table. Walking off the end of symtab is therefore the default
 * failure mode of any linear scan, and it reads whatever follows in the
 * mapping as symbol entries: plausible st_name offsets into strtab, plausible
 * st_value addresses. It does not crash, it resolves to garbage.
 *
 * The bound comes from the GNU hash: the last chain entry with its low bit
 * set marks the final symbol in the last bucket. */
static u64 symbol_count(const obj_t *o) {
    const u32 *h = o->gnu_hash;
    u32 nbuckets, symndx, maskwords;
    const u32 *buckets, *chain;
    u32 last = 0;
    u32 i;

    if (h == NULL) {
        return 0;
    }
    nbuckets  = h[0];
    symndx    = h[1];
    maskwords = h[2];
    if (nbuckets == 0) {
        return symndx;
    }
    buckets = h + 4 + maskwords * 2;          /* 64-bit bloom words */
    chain   = buckets + nbuckets;

    for (i = 0; i < nbuckets; i++) {
        if (buckets[i] > last) {
            last = buckets[i];
        }
    }
    if (last < symndx) {
        return symndx;
    }
    while ((chain[last - symndx] & 1) == 0) {
        last++;
    }
    return last + 1;
}

/* Find `name` in the global scope.
 *
 * Breadth-first in load order, which is what makes the main executable's
 * definition win over a library's - the property that lets a program override
 * malloc. A depth-first search would find the library's first.
 *
 * `skip` is the object NOT to search, and it exists for exactly one caller:
 * R_X86_64_COPY. A copy relocation asks "where is the ORIGINAL of this
 * variable", and the executable holds a same-named allocation in its own .bss
 * that is the destination. Searching it would find that destination, copy it
 * onto itself, and leave the variable holding whatever .bss started as -
 * zero, silently, with the library's initialiser never arriving. Pass NULL to
 * search everything. */
static const Sym *lookup_skip(const char *name, obj_t **found_in,
                              const obj_t *skip) {
    const Sym *weak = NULL;
    obj_t     *weak_obj = NULL;
    int i;

    for (i = 0; i < object_count; i++) {
        obj_t *o = &objects[i];
        u64 n, start, k;

        if (o == skip) {
            continue;
        }
        n     = symbol_count(o);
        start = gnu_hash_symndx(o);
        if (o->symtab == NULL || o->strtab == NULL || n == 0) {
            continue;
        }
        for (k = start; k < n; k++) {
            const Sym *s = (const Sym *)((const u8 *)o->symtab + k * o->syment);

            if (s->st_shndx == SHN_UNDEF) {
                continue;
            }
            if (!seq(o->strtab + s->st_name, name)) {
                continue;
            }
            if ((s->st_info >> 4) == STB_WEAK) {
                /* A weak definition is remembered but does not stop the
                 * search: a strong definition in a LATER object still wins
                 * over a weak one in an earlier. Returning the weak
                 * immediately is the bug where a libc weak stub shadows the
                 * real implementation that was going to be found next. */
                if (weak == NULL) {
                    weak = s;
                    weak_obj = o;
                }
                continue;
            }
            *found_in = o;
            return s;
        }
    }
    if (weak != NULL) {
        *found_in = weak_obj;
        return weak;
    }
    return NULL;
}

static const Sym *lookup(const char *name, obj_t **found_in) {
    return lookup_skip(name, found_in, NULL);
}

/* --- parsing the dynamic section ----------------------------------------- */

static void parse_dynamic(obj_t *o) {
    const Dyn *d;
    u64 pltrel = 0;

    o->syment = sizeof(Sym);

    for (d = o->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            /* Every pointer-valued tag is biased by the load base.
             *
             * For an ET_DYN this is required; for an ET_EXEC the base is zero
             * and it is a no-op. Writing it unconditionally rather than
             * branching on the type is what makes the main executable and its
             * libraries take the same path here - and a branch would be one
             * more place for the PIE case to differ from the fixed one. */
            case DT_STRTAB:  o->strtab   = (const char *)(o->base + d->d_val); break;
            case DT_SYMTAB:  o->symtab   = (const Sym *)(o->base + d->d_val);  break;
            case DT_GNU_HASH:o->gnu_hash = (const u32 *)(o->base + d->d_val);  break;
            case DT_RELA:    o->rela     = (const Rela *)(o->base + d->d_val); break;
            case DT_RELASZ:  o->relasz   = d->d_val; break;
            case DT_JMPREL:  o->jmprel   = (const Rela *)(o->base + d->d_val); break;
            case DT_PLTRELSZ:o->jmprelsz = d->d_val; break;
            case DT_PLTREL:  pltrel      = d->d_val; break;
            case DT_SYMENT:  o->syment   = d->d_val; break;
            case DT_INIT:
                o->init = (void (*)(void))(o->base + d->d_val);
                break;
            case DT_INIT_ARRAY:
                o->init_array = (void (**)(void))(o->base + d->d_val);
                break;
            case DT_INIT_ARRAYSZ:
                o->init_arraysz = d->d_val;
                break;
            default: break;
        }
    }

    /* DT_REL (without addends) is refused rather than ignored. It does not
     * occur on x86-64 - the ABI mandates RELA - so a binary carrying it was
     * built for something else, and processing its entries as Rela would read
     * an addend out of the next relocation's r_offset. */
    if (o->jmprel != NULL && pltrel != DT_RELA) {
        die("PLT relocations are not RELA", o->name);
    }

    /* DT_NEEDED is collected in a second pass, because it indexes into strtab
     * and the DT_STRTAB entry is not guaranteed to come first. A single pass
     * that dereferenced strtab as it went would read through a null pointer
     * for any object whose tags happen to be ordered the other way - and the
     * ordering is a linker's choice, not a rule. */
    for (d = o->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED && o->needed_count < MAX_NEEDED) {
            o->needed[o->needed_count++] = o->strtab + d->d_val;
        }
    }
}

/* --- self-relocation -----------------------------------------------------
 *
 * The bootstrap problem, and the one genuinely delicate function here.
 *
 * This file is itself a shared object with a GOT, and every global variable
 * access above compiles to a load through it. Until the RELATIVE relocations
 * in this object's own .rela.dyn have been applied, those loads return
 * unbiased addresses - so `objects` points somewhere near zero, and writing
 * to it faults or, worse, does not.
 *
 * Which means this function may not touch a single global, may not call
 * anything that does, and may not use a string literal (a literal is a
 * relocated address). It takes everything as arguments and uses only locals.
 * The compiler must also not turn its loop into a memcpy call, which is what
 * -fno-builtin in the build script is for.
 *
 * It processes ONLY R_X86_64_RELATIVE. Anything else in the linker's own
 * relocations would need a symbol lookup, which needs the globals this cannot
 * touch - and -Bsymbolic in the build makes sure there is nothing else. */
/* Not static: start.S calls it before anything else, and it is the only
 * symbol in this file that the assembly stub reaches. */
void self_relocate(u64 base, const Dyn *dyn) {
    const Rela *r = NULL;
    u64 sz = 0;
    u64 i;

    for (; dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_RELA) {
            r = (const Rela *)(base + dyn->d_val);
        } else if (dyn->d_tag == DT_RELASZ) {
            sz = dyn->d_val;
        }
    }
    if (r == NULL) {
        return;
    }
    for (i = 0; i < sz / sizeof(Rela); i++) {
        if (ELF64_R_TYPE(r[i].r_info) == R_X86_64_RELATIVE) {
            *(u64 *)(base + r[i].r_offset) = base + (u64)r[i].r_addend;
        }
    }
}

/* --- loading ------------------------------------------------------------- */

static u64 page_down(u64 v) { return v & ~0xFFFULL; }
static u64 page_up(u64 v)   { return (v + 0xFFF) & ~0xFFFULL; }

/* Where the next library goes. Bumped by each load.
 *
 * A fixed arena walked upward rather than letting mmap choose, for the same
 * reason the kernel fixes ELF_INTERP_BASE: an address the linker chose is one
 * it can print, and "libc.so at 0x7E0000000000" in a fault message is worth
 * more than an allocator that is one line shorter. */
static u64 next_base = 0x00007E0000000000ULL;

static int read_all(const char *path, u8 *buf, u64 max, u64 *out_size) {
    i64 fd = sc3(SYS_open, path, O_RDONLY, 0);
    u64 got = 0;

    if (fd < 0) {
        return -1;
    }
    for (;;) {
        i64 n = sc3(0 /* SYS_read */, fd, buf + got, max - got);
        if (n < 0) {
            sc1(SYS_close, fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        got += (u64)n;
        if (got >= max) {
            sc1(SYS_close, fd);
            return -1;
        }
    }
    sc1(SYS_close, fd);
    *out_size = got;
    return 0;
}

/* One staging buffer, reused per library.
 *
 * Static rather than mmap'd: this runs before any allocator exists, and an
 * mmap for the staging buffer is one more failure path in the part of startup
 * with the fewest ways to report anything. 1MB caps how large a library can
 * be, which is a real limit and a visible one - it fails with a message
 * rather than by overwriting something. */
static u8 stage[1024 * 1024];

static obj_t *load_object(const char *soname) {
    char  path[RTLD_PATH_MAX];
    u64   size = 0;
    const Ehdr *eh;
    const Phdr *ph;
    obj_t *o;
    u64    lo = ~0ULL, hi = 0, base;
    int    i;

    /* Already loaded? Checked by SONAME before the file is opened, which is
     * what stops a diamond dependency loading libc twice - and two copies of
     * libc is two copies of its data, so errno set through one is invisible
     * through the other. */
    for (i = 0; i < object_count; i++) {
        if (seq(objects[i].name, soname)) {
            return &objects[i];
        }
    }
    if (object_count >= MAX_OBJECTS) {
        die("too many shared objects", soname);
    }

    /* One search directory. No DT_RPATH, no LD_LIBRARY_PATH, no ld.so.cache:
     * a search path is policy, and every one of those is a way for a program
     * to load a library the system did not choose. When there is a reason for
     * more than /lib, it goes here as a list. */
    {
        const char *dir = "/lib/";
        u64 n = 0, k;

        for (k = 0; dir[k]; k++) path[n++] = dir[k];
        for (k = 0; soname[k]; k++) {
            if (n + 1 >= sizeof(path)) {
                die("library path too long", soname);
            }
            path[n++] = soname[k];
        }
        path[n] = '\0';
    }

    if (read_all(path, stage, sizeof(stage), &size) != 0) {
        die("cannot open", path);
    }
    if (size < sizeof(Ehdr)) {
        die("truncated", path);
    }
    eh = (const Ehdr *)stage;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        die("not an ELF", path);
    }
    if (eh->e_phoff + (u64)eh->e_phnum * eh->e_phentsize > size) {
        die("program headers outside the file", path);
    }

    ph = (const Phdr *)(stage + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (lo > hi) {
        die("no loadable segments", path);
    }

    base = next_base;
    next_base += page_up(hi - page_down(lo)) + 0x10000;

    /* Segments are mapped PROT_READ|PROT_WRITE for the copy, and .text is
     * left writable. That is the same half-done W^X the ELF loader has on the
     * kernel side, and it is deliberately the same shape rather than a
     * different one: when the kernel gains an mprotect that can drop write on
     * a mapped text segment, both halves change together and one test covers
     * them. Marking it here while the kernel cannot enforce it would be a
     * claim with nothing behind it. */
    for (i = 0; i < eh->e_phnum; i++) {
        u64 start, end, k;

        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;

        start = base + page_down(ph[i].p_vaddr);
        end   = base + page_up(ph[i].p_vaddr + ph[i].p_memsz);

        if ((i64)sc6m(SYS_mmap, start, end - start,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) < 0) {
            die("mmap failed for", path);
        }
        for (k = 0; k < ph[i].p_filesz; k++) {
            ((u8 *)(base + ph[i].p_vaddr))[k] = stage[ph[i].p_offset + k];
        }
        /* .bss. MAP_ANONYMOUS already gives zeroed pages, so this loop only
         * matters for the tail of a partly-filled page that filesz shares
         * with memsz - and that tail is exactly where a stale byte from the
         * previous segment's copy would land. */
        for (k = ph[i].p_filesz; k < ph[i].p_memsz; k++) {
            ((u8 *)(base + ph[i].p_vaddr))[k] = 0;
        }
    }

    o = &objects[object_count++];
    {
        u64 k;
        for (k = 0; k + 1 < sizeof(o->name) && soname[k]; k++) {
            o->name[k] = soname[k];
        }
        o->name[k] = '\0';
    }
    o->base = base;
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            o->dynamic = (const Dyn *)(base + ph[i].p_vaddr);
        }
    }
    if (o->dynamic == NULL) {
        die("no PT_DYNAMIC", path);
    }
    parse_dynamic(o);
    return o;
}

/* --- relocation ---------------------------------------------------------- */

static void apply(obj_t *o, const Rela *r, u64 count) {
    u64 i;

    for (i = 0; i < count; i++) {
        u64  *where = (u64 *)(o->base + r[i].r_offset);
        u32   type  = ELF64_R_TYPE(r[i].r_info);
        u32   symi  = ELF64_R_SYM(r[i].r_info);
        const Sym  *sym = NULL;
        const char *name = NULL;

        if (symi != 0 && o->symtab != NULL) {
            sym  = (const Sym *)((const u8 *)o->symtab + symi * o->syment);
            name = o->strtab + sym->st_name;
        }

        switch (type) {
            case R_X86_64_NONE:
                break;

            case R_X86_64_RELATIVE:
                /* No symbol involved: the base plus a constant. This is the
                 * bulk of a PIE's relocations and the only kind
                 * self_relocate above handles. */
                *where = o->base + (u64)r[i].r_addend;
                break;

            case R_X86_64_64:
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: {
                obj_t *in = NULL;
                const Sym *def;

                if (name == NULL) {
                    die("relocation with no symbol", o->name);
                }
                def = lookup(name, &in);
                if (def == NULL) {
                    /* A weak UNDEFINED reference resolves to zero rather than
                     * failing. That is the format's own rule and it is load
                     * bearing: libc tests __pthread_initialize against null
                     * to find out whether threads were linked in, and failing
                     * the link here would break every single-threaded
                     * program. */
                    if (sym != NULL && (sym->st_info >> 4) == STB_WEAK) {
                        *where = 0;
                        break;
                    }
                    die("undefined symbol", name);
                }
                *where = in->base + def->st_value +
                         (type == R_X86_64_64 ? (u64)r[i].r_addend : 0);
                break;
            }

            case R_X86_64_COPY: {
                /* The main executable read a DATA symbol out of a library.
                 *
                 * The static linker cannot make a data reference indirect the
                 * way it makes a call indirect through the PLT, so instead it
                 * allocates space for the variable in the EXECUTABLE's .bss
                 * and emits this: at load time, copy the library's initialised
                 * value into that space. Every reference in the executable
                 * then points at its own copy, and the library's own
                 * references still point at the library's - which is why a
                 * copy relocation is a hazard for any variable both sides
                 * write, and why libc keeps its mutable state behind
                 * functions.
                 *
                 * Found by a negative test rather than by reading the ABI:
                 * this was an unhandled type, and a program that merely READ
                 * a library's global died at load with type 0x5 and no
                 * further explanation. Every dynamic program that touches a
                 * library's data needs it. */
                obj_t *in = NULL;
                const Sym *def;
                u64 k;

                if (name == NULL || sym == NULL) {
                    die("copy relocation with no symbol", o->name);
                }
                /* Skipping `o` is the whole correctness argument. The
                 * executable's own symbol table has this name too - it is the
                 * .bss allocation being written - and finding it would copy
                 * the destination onto itself, leaving the variable zero with
                 * no error anywhere. */
                def = lookup_skip(name, &in, o);
                if (def == NULL) {
                    die("undefined symbol for copy relocation", name);
                }
                if (def->st_size != sym->st_size) {
                    /* Sizes must agree or the copy writes past the
                     * executable's allocation. A mismatch means the program
                     * and the library were built against different headers,
                     * which is worth refusing rather than truncating. */
                    die("size mismatch in copy relocation", name);
                }
                for (k = 0; k < sym->st_size; k++) {
                    ((u8 *)where)[k] = ((const u8 *)(in->base + def->st_value))[k];
                }
                break;
            }

            case R_X86_64_DTPMOD64:
            case R_X86_64_DTPOFF64:
                /* General-dynamic TLS. REFUSED rather than resolved to
                 * something plausible - a DTPMOD of 1 and a DTPOFF of the
                 * symbol value would link cleanly and then read another
                 * module's thread block. Per-module TLS is its own item; a
                 * program that needs it should fail to load, loudly, until
                 * that item lands. */
                die("general-dynamic TLS is not supported", o->name);
                break;

            case R_X86_64_TPOFF64:
                /* Initial-exec, which the static case already proved works.
                 * Only correct for the main executable and objects loaded at
                 * startup - which is everything here, because there is no
                 * dlopen. */
                if (name == NULL) {
                    die("TPOFF64 with no symbol", o->name);
                }
                {
                    obj_t *in = NULL;
                    const Sym *def = lookup(name, &in);

                    if (def == NULL) {
                        die("undefined TLS symbol", name);
                    }
                    *where = def->st_value + (u64)r[i].r_addend;
                }
                break;

            case R_X86_64_IRELATIVE:
                /* An ifunc: the addend is a RESOLVER to call, and its return
                 * value is the address. Calling it here means it runs before
                 * any library's init - which is the ABI's rule and also the
                 * reason ifunc resolvers are required to touch nothing. */
                *where = ((u64 (*)(void))(o->base + (u64)r[i].r_addend))();
                break;

            default:
                put("rtld: unhandled relocation type ");
                puthex(type);
                put(" in ");
                put(o->name);
                put("\n");
                sc1(SYS_exit_group, 127);
                break;
        }
    }
}

static void relocate(obj_t *o) {
    if (o->relocated) {
        return;
    }
    o->relocated = 1;
    if (o->rela != NULL) {
        apply(o, o->rela, o->relasz / sizeof(Rela));
    }
    /* The PLT, eagerly, right here. There is no lazy path at all - see the
     * note at the top of the file. Every JUMP_SLOT is a real address before
     * the program runs. */
    if (o->jmprel != NULL) {
        apply(o, o->jmprel, o->jmprelsz / sizeof(Rela));
    }
}

static void run_init(obj_t *o) {
    u64 i;

    if (o->initialised) {
        return;
    }
    o->initialised = 1;

    /* Dependencies first. A library's constructor is entitled to call into
     * anything it declared DT_NEEDED on, and running them in load order
     * without this would call libc's constructors after something that
     * depends on them. */
    for (i = 0; i < (u64)o->needed_count; i++) {
        int k;
        for (k = 0; k < object_count; k++) {
            if (seq(objects[k].name, o->needed[i])) {
                run_init(&objects[k]);
            }
        }
    }

    if (o->init != NULL) {
        o->init();
    }
    for (i = 0; i < o->init_arraysz / sizeof(void *); i++) {
        if (o->init_array[i] != NULL) {
            o->init_array[i]();
        }
    }
}

/* --- entry ---------------------------------------------------------------
 *
 * Called from _start in start.S with the stack pointer the kernel built.
 * Returns the address to jump to, which _start does WITHOUT touching the
 * stack - the program expects to find argc where the kernel put it.
 */
u64 rtld_start(u64 *sp) {
    u64  argc = sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    u64 *auxv;
    u64  at_base = 0, at_entry = 0, at_phdr = 0, at_phnum = 0;
    obj_t *main_obj;
    const Phdr *ph;
    u64 i;
    int k;

    {
        u64 n = 0;
        while (envp[n] != NULL) n++;
        auxv = (u64 *)(envp + n + 1);
    }
    for (i = 0; auxv[i] != AT_NULL; i += 2) {
        switch (auxv[i]) {
            case AT_BASE:  at_base  = auxv[i + 1]; break;
            case AT_ENTRY: at_entry = auxv[i + 1]; break;
            case AT_PHDR:  at_phdr  = auxv[i + 1]; break;
            case AT_PHNUM: at_phnum = auxv[i + 1]; break;
            default: break;
        }
    }
    (void)at_base;

    if (at_entry == 0 || at_phdr == 0) {
        die("no AT_ENTRY or AT_PHDR - not started by the kernel", NULL);
    }

    /* The main executable, described by the auxv rather than loaded. It is
     * already mapped; what this needs from it is its dynamic section, so its
     * symbols join the global scope and its relocations get applied.
     *
     * It goes in slot 0, which is what makes the search order in lookup()
     * find the executable's definition of a symbol before any library's. */
    main_obj = &objects[object_count++];
    {
        const char *n = "(main)";
        for (k = 0; n[k]; k++) main_obj->name[k] = n[k];
        main_obj->name[k] = '\0';
    }
    /* Bias zero: the kernel loads the main executable as ET_EXEC at its own
     * vaddrs. A PIE main executable would need the bias the kernel chose,
     * which is exactly what AT_PHDR minus the PT_PHDR vaddr recovers - the
     * arithmetic goes here when the kernel starts loading PIEs. */
    main_obj->base = 0;

    ph = (const Phdr *)at_phdr;
    for (i = 0; i < at_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            main_obj->dynamic = (const Dyn *)(main_obj->base + ph[i].p_vaddr);
        }
    }
    if (main_obj->dynamic == NULL) {
        /* Statically linked, started through the interpreter anyway. Nothing
         * to do but go, and saying so beats faulting on a null dynamic. */
        return at_entry;
    }
    parse_dynamic(main_obj);

    /* Breadth first. objects[] IS the queue: load_object appends, and this
     * loop walks the array as it grows - so everything the executable needs
     * is loaded before anything those libraries need, which is the order that
     * makes a symbol resolve to the shallowest definition.
     *
     * A recursive load would be depth first and would give a transitive
     * dependency's definition priority over a direct one. */
    for (k = 0; k < object_count; k++) {
        int j;
        for (j = 0; j < objects[k].needed_count; j++) {
            load_object(objects[k].needed[j]);
        }
    }

    /* Relocation only after EVERY object is loaded. A relocation resolved
     * against a partial scope finds a weak definition, or nothing, where a
     * library loaded one step later has the real one - and with eager binding
     * that mistake is baked into the GOT rather than retried at first call. */
    for (k = 0; k < object_count; k++) {
        relocate(&objects[k]);
    }

    /* Constructors after all relocation, for the same reason: a constructor
     * runs real code and may call anything. */
    for (k = object_count - 1; k >= 0; k--) {
        run_init(&objects[k]);
    }

    return at_entry;
}
