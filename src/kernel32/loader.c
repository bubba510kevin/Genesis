/* LoadLibrary and GetProcAddress - for DLLs and for Linux shared objects.
 * ROADMAP item 14(e) (loading at run time) and item 19, stage 3 (a Windows
 * program using a Linux .so).
 *
 * The loading is the kernel's: NtGenesisLoadImage maps an image and
 * everything it needs that is not loaded yet - the same PE linker execve
 * uses, or, for an ELF shared object, kernel/exec/elfso.c - and reports the
 * new modules, dependencies first. What stays here is what has to run in
 * ring 3: each new module's initialisers (DllMain for a DLL; DT_INIT and
 * .init_array for a .so, called System V), and symbol lookup.
 *
 * --- calling a .so from Windows code ---------------------------------------
 * A program written for Windows calls whatever GetProcAddress returns as a
 * Win64 function. A .so's functions are System V. So for a .so,
 * GetProcAddress returns an ADAPTER (winelf.S) that moves the Win64
 * arguments to where System V expects them and saves the registers Win64
 * promises its caller but System V does not (RSI, RDI, XMM6-XMM15). It is
 * exact for up to 14 integer/pointer arguments, and for leading
 * floating-point arguments (XMM0-3 pass through). For anything else - mixed
 * int and float arguments, structs by value - GenesisGetElfProcAddress
 * returns the raw address, to call through a pointer declared
 * __attribute__((sysv_abi)), and the compiler does the translation.
 *
 * A .so run this way has no libc of its own set up in this process: no
 * thread pointer in %fs, no musl startup. Freestanding code and code that
 * only makes system calls works; code that touches errno or __thread does
 * not yet.
 *
 * Paths: "C:\x\y.so" and "C:/x/y" are the root volume's /x/y; any other
 * name with a separator is a path as it stands; a bare name is looked for
 * in /lib if it is a .so and in /wsr/System32 otherwise (".dll" added when
 * there is no extension). */

#include "kernel32.h"

#define GNT_KIND_PE   1
#define GNT_KIND_ELF  2

/* --- path ---------------------------------------------------------------- */

static int ends_with_ci(const char *s, SIZE_T n, const char *suffix) {
    SIZE_T m = 0, i;

    while (suffix[m] != '\0') {
        m++;
    }
    if (n < m) {
        return 0;
    }
    for (i = 0; i < m; i++) {
        char a = s[n - m + i], b = suffix[i];

        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + 32);
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static int is_shared_object(const char *s, SIZE_T n) {
    SIZE_T i;

    if (ends_with_ci(s, n, ".so")) {
        return 1;
    }
    for (i = 0; i + 4 <= n; i++) {       /* libfoo.so.1 */
        if (s[i] == '.' && s[i + 1] == 's' && s[i + 2] == 'o' && s[i + 3] == '.') {
            return 1;
        }
    }
    return 0;
}

/* `in` (narrow) to the POSIX path the kernel reads. 0 if it will not fit or
 * names a drive other than C:. */
static int to_posix(const char *in, char *out, SIZE_T cap) {
    SIZE_T n = 0, at = 0, i;
    int has_sep = 0, has_dot = 0;
    const char *prefix = "";

    while (in[n] != '\0') {
        if (in[n] == '\\' || in[n] == '/') {
            has_sep = 1;
        }
        n++;
    }
    if (n == 0) {
        return 0;
    }
    if (n >= 2 && in[1] == ':') {
        if (in[0] != 'C' && in[0] != 'c') {
            return 0;                    /* only the root volume, for now */
        }
        in += 2;
        n -= 2;
        if (n == 0) {
            prefix = "/";
        }
    } else if (!has_sep) {
        prefix = is_shared_object(in, n) ? "/lib/" : "/wsr/System32/";
        for (i = 0; i < n; i++) {
            if (in[i] == '.') {
                has_dot = 1;
            }
        }
    }
    for (i = 0; prefix[i] != '\0'; i++) {
        if (at + 1 >= cap) {
            return 0;
        }
        out[at++] = prefix[i];
    }
    for (i = 0; i < n; i++) {
        if (at + 1 >= cap) {
            return 0;
        }
        out[at++] = (in[i] == '\\') ? '/' : in[i];
    }
    if (!has_sep && !has_dot && prefix[0] != '\0' && prefix[1] == 'w') {
        const char *ext = ".dll";

        for (i = 0; ext[i] != '\0'; i++) {
            if (at + 1 >= cap) {
                return 0;
            }
            out[at++] = ext[i];
        }
    }
    out[at] = '\0';
    return 1;
}

/* --- initialisers -------------------------------------------------------- */

typedef BOOL (WINAPI *dll_main_t)(HMODULE, DWORD, LPVOID);
typedef void (__attribute__((sysv_abi)) *elf_init_t)(int, char **, char **);

static int run_initialisers(const GNT_LOAD_OUT *out) {
    DWORD i, k;

    for (i = 0; i < out->count && i < GNT_LOAD_MAX; i++) {
        const GNT_LOAD_MODULE *m = &out->mods[i];

        if (m->kind == GNT_KIND_PE) {
            if (m->entry != 0 &&
                !((dll_main_t)m->entry)((HMODULE)m->base, 1 /* PROCESS_ATTACH */,
                                        NULL_PTR)) {
                return 0;
            }
            continue;
        }
        /* ELF: DT_INIT, then .init_array in order - the order the ELF
         * spec gives. (argc, argv, envp) are what glibc passes; musl passes
         * nothing and an initialiser taking no arguments ignores them. */
        if (m->entry != 0) {
            ((elf_init_t)m->entry)(0, NULL_PTR, NULL_PTR);
        }
        for (k = 0; k < m->init_count; k++) {
            ULONGLONG fn = ((const ULONGLONG *)m->init_array)[k];

            if (fn != 0 && fn != (ULONGLONG)-1) {
                ((elf_init_t)fn)(0, NULL_PTR, NULL_PTR);
            }
        }
    }
    return 1;
}

HMODULE WINAPI LoadLibraryA(LPCSTR name) {
    char path[260];
    GNT_LOAD_OUT out;
    NTSTATUS st;

    if (name == NULL_PTR || !to_posix(name, path, sizeof(path))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL_PTR;
    }
    st = NtGenesisLoadImage(path, &out);
    if (!NT_SUCCESS(st)) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return NULL_PTR;
    }
    if (!run_initialisers(&out)) {
        SetLastError(ERROR_DLL_INIT_FAILED);
        return NULL_PTR;
    }
    return (HMODULE)out.base;
}

HMODULE WINAPI LoadLibraryW(LPCWSTR name) {
    char narrow[260];
    SIZE_T i;

    if (name == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL_PTR;
    }
    for (i = 0; name[i] != 0; i++) {
        if (i + 1 >= sizeof(narrow) || name[i] >= 0x80) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL_PTR;
        }
        narrow[i] = (char)name[i];
    }
    narrow[i] = '\0';
    return LoadLibraryA(narrow);
}

HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    (void)file;
    (void)flags;
    return LoadLibraryW(name);
}

HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    (void)file;
    (void)flags;
    return LoadLibraryA(name);
}

/* Nothing is unloaded yet; FreeLibrary says it worked and leaves the image
 * where it is, which every caller that frees and never touches the module
 * again cannot tell from the real thing. */
BOOL WINAPI FreeLibrary(HMODULE module) {
    return module != NULL_PTR;
}

/* --- PE exports ------------------------------------------------------------ */

static DWORD rd32(const BYTE *p) {
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) |
           ((DWORD)p[3] << 24);
}

static WORD rd16(const BYTE *p) {
    return (WORD)(p[0] | (p[1] << 8));
}

static ULONGLONG rd64(const BYTE *p) {
    return (ULONGLONG)rd32(p) | ((ULONGLONG)rd32(p + 4) << 32);
}

static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static FARPROC pe_export(HMODULE module, LPCSTR name, int depth);

static FARPROC pe_forward(const char *fwd, int depth) {
    char dll[64], sym[128];
    SIZE_T i = 0, j = 0;
    HMODULE target;

    while (fwd[i] != '\0' && fwd[i] != '.' && i + 5 < sizeof(dll)) {
        dll[i] = fwd[i];
        i++;
    }
    if (fwd[i] != '.') {
        return NULL_PTR;
    }
    dll[i] = '.'; dll[i + 1] = 'd'; dll[i + 2] = 'l'; dll[i + 3] = 'l';
    dll[i + 4] = '\0';
    i++;
    while (fwd[i] != '\0' && j + 1 < sizeof(sym)) {
        sym[j++] = fwd[i++];
    }
    sym[j] = '\0';

    target = LoadLibraryA(dll);          /* already loaded: just its base */
    if (target == NULL_PTR) {
        return NULL_PTR;
    }
    if (sym[0] == '#') {
        ULONG_PTR ord = 0;

        for (i = 1; sym[i] >= '0' && sym[i] <= '9'; i++) {
            ord = ord * 10 + (ULONG_PTR)(sym[i] - '0');
        }
        return pe_export(target, (LPCSTR)ord, depth + 1);
    }
    return pe_export(target, sym, depth + 1);
}

static FARPROC pe_export(HMODULE module, LPCSTR name, int depth) {
    const BYTE *b = (const BYTE *)module;
    const BYTE *opt, *dir;
    DWORD pe, exp_rva, exp_size, nfuncs, nnames, funcs, names, ords;
    DWORD base_ord, idx, rva, i;
    ULONG_PTR ordinal = (ULONG_PTR)name;

    if (depth > 4) {
        return NULL_PTR;
    }
    pe  = rd32(b + 0x3C);
    opt = b + pe + 24;
    if (b[pe] != 'P' || b[pe + 1] != 'E' || rd16(opt) != 0x20B ||
        rd32(opt + 108) == 0) {
        return NULL_PTR;
    }
    exp_rva  = rd32(opt + 112);
    exp_size = rd32(opt + 116);
    if (exp_rva == 0) {
        return NULL_PTR;
    }
    dir      = b + exp_rva;
    base_ord = rd32(dir + 16);
    nfuncs   = rd32(dir + 20);
    nnames   = rd32(dir + 24);
    funcs    = rd32(dir + 28);
    names    = rd32(dir + 32);
    ords     = rd32(dir + 36);

    if ((ordinal >> 16) == 0) {          /* MAKEINTRESOURCE-style ordinal */
        if (ordinal < base_ord || ordinal - base_ord >= nfuncs) {
            return NULL_PTR;
        }
        idx = (DWORD)(ordinal - base_ord);
    } else {
        for (i = 0; i < nnames; i++) {
            if (str_eq((const char *)(b + rd32(b + names + 4 * i)), name)) {
                break;
            }
        }
        if (i == nnames) {
            return NULL_PTR;
        }
        idx = rd16(b + ords + 2 * i);
        if (idx >= nfuncs) {
            return NULL_PTR;
        }
    }
    rva = rd32(b + funcs + 4 * idx);
    if (rva == 0) {
        return NULL_PTR;
    }
    if (rva >= exp_rva && rva < exp_rva + exp_size) {
        return pe_forward((const char *)(b + rva), depth);
    }
    return (FARPROC)(b + rva);
}

/* --- ELF symbols ------------------------------------------------------------
 *
 * The ELF header and program headers sit at the base (the first PT_LOAD
 * starts at file offset 0), so the dynamic section is found from them. Its
 * pointers are unrelocated addresses, relative to the base for a shared
 * object. */

#define DT_NULL      0
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_GNU_HASH  0x6ffffef5
#define PT_DYNAMIC   2

static ULONGLONG elf_symbol_count(const BYTE *b, ULONGLONG hash, ULONGLONG gnu_hash) {
    if (hash != 0) {
        return rd32(b + hash + 4);       /* nchain */
    }
    if (gnu_hash != 0) {
        const BYTE *g = b + gnu_hash;
        DWORD nbuckets = rd32(g), symoffset = rd32(g + 4), bloom = rd32(g + 8);
        const BYTE *buckets = g + 16 + 8 * (ULONGLONG)bloom;
        const BYTE *chains = buckets + 4 * (ULONGLONG)nbuckets;
        DWORD i, last = 0;

        for (i = 0; i < nbuckets; i++) {
            if (rd32(buckets + 4 * i) > last) {
                last = rd32(buckets + 4 * i);
            }
        }
        if (last < symoffset) {
            return symoffset;
        }
        while ((rd32(chains + 4 * (ULONGLONG)(last - symoffset)) & 1) == 0) {
            last++;
        }
        return (ULONGLONG)last + 1;
    }
    return 0;
}

static void *elf_symbol(HMODULE module, LPCSTR name) {
    const BYTE *b = (const BYTE *)module;
    ULONGLONG phoff = rd64(b + 32), dyn = 0, strtab = 0, symtab = 0;
    ULONGLONG hash = 0, gnu_hash = 0, count, i;
    WORD phentsize = rd16(b + 54), phnum = rd16(b + 56);

    if ((ULONG_PTR)name >> 16 == 0) {
        return NULL_PTR;                 /* ELF has no ordinals */
    }
    for (i = 0; i < phnum; i++) {
        const BYTE *ph = b + phoff + i * phentsize;

        if (rd32(ph) == PT_DYNAMIC) {
            dyn = rd64(ph + 16);         /* p_vaddr */
        }
    }
    if (dyn == 0) {
        return NULL_PTR;
    }
    for (i = 0; rd64(b + dyn + 16 * i) != DT_NULL; i++) {
        ULONGLONG tag = rd64(b + dyn + 16 * i), val = rd64(b + dyn + 16 * i + 8);

        if (tag == DT_STRTAB)   strtab = val;
        if (tag == DT_SYMTAB)   symtab = val;
        if (tag == DT_HASH)     hash = val;
        if (tag == DT_GNU_HASH) gnu_hash = val;
    }
    if (strtab == 0 || symtab == 0) {
        return NULL_PTR;
    }
    count = elf_symbol_count(b, hash, gnu_hash);
    for (i = 1; i < count; i++) {
        const BYTE *sym = b + symtab + 24 * i;
        BYTE info = sym[4], bind = (BYTE)(info >> 4);
        WORD shndx = rd16(sym + 6);

        if (shndx == 0 || (bind != 1 && bind != 2)) {  /* undefined, local */
            continue;
        }
        if (str_eq((const char *)(b + strtab + rd32(sym)), name)) {
            return (void *)(b + rd64(sym + 8));
        }
    }
    return NULL_PTR;
}

static int is_elf(HMODULE module) {
    const BYTE *b = (const BYTE *)module;

    return b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
}

/* --- adapters (winelf.S) ----------------------------------------------------- */

#define GNT_ADAPTERS 256
extern ULONGLONG gnt_elf_targets[GNT_ADAPTERS];
extern BYTE gnt_elf_adapters[];          /* GNT_ADAPTERS stubs, 16 bytes each */
static LONG adapters_used;

static FARPROC adapter_for(void *target) {
    LONG i, n = adapters_used;

    for (i = 0; i < n && i < GNT_ADAPTERS; i++) {
        if (gnt_elf_targets[i] == (ULONGLONG)target) {
            return (FARPROC)(gnt_elf_adapters + 16 * i);
        }
    }
    i = __sync_fetch_and_add(&adapters_used, 1);
    if (i >= GNT_ADAPTERS) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL_PTR;
    }
    gnt_elf_targets[i] = (ULONGLONG)target;
    return (FARPROC)(gnt_elf_adapters + 16 * i);
}

FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name) {
    void *p;

    if (module == NULL_PTR || name == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL_PTR;
    }
    if (is_elf(module)) {
        p = elf_symbol(module, name);
        if (p == NULL_PTR) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return NULL_PTR;
        }
        return adapter_for(p);
    }
    p = (void *)pe_export(module, name, 0);
    if (p == NULL_PTR) {
        SetLastError(ERROR_PROC_NOT_FOUND);
    }
    return (FARPROC)p;
}

/* The raw System V address of a .so's symbol, for a caller that declares
 * the pointer __attribute__((sysv_abi)) - every argument type then works.
 * Genesis-only. */
FARPROC WINAPI GenesisGetElfProcAddress(HMODULE module, LPCSTR name) {
    void *p = NULL_PTR;

    if (module != NULL_PTR && name != NULL_PTR && is_elf(module)) {
        p = elf_symbol(module, name);
    }
    if (p == NULL_PTR) {
        SetLastError(ERROR_PROC_NOT_FOUND);
    }
    return (FARPROC)p;
}
