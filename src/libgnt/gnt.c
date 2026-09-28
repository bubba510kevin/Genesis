/* libgnt - Windows DLLs in a Linux program. See gnt.h. */

#include "gnt.h"

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;

#define SYS_mmap   9
#define SYS_prctl  157

/* Must match kernel/include/ntmix.h. */
#define PR_GENESIS_PE_LOAD  0x47454e10u
#define GNT_PE_LOAD_MAX     16

typedef struct {
    u64 base;
    u32 count;
    u32 reserved;
    struct {
        u64 base;
        u64 size;
        u64 entry;
    } mods[GNT_PE_LOAD_MAX];
} load_out_t;

static int last_errno;

static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;

    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c),
                        "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return r;
}

static u64 slen(const char *s) {
    u64 n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int gnt_pe_errno(void) {
    return last_errno;
}

/* --- loading ------------------------------------------------------------- */

typedef int (GNT_WINAPI *dll_entry_t)(void *instance, u32 reason, void *reserved);

void *gnt_pe_open(const char *path) {
    static const char sysdir[] = "/wsr/System32/";
    char full[256];
    load_out_t out;
    u64 i, n;
    long rc;

    n = 0;
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            n = 1;
        }
    }
    if (n == 0) {                        /* a bare name: the system directory */
        u64 at = 0;

        if (slen(sysdir) + slen(path) + 1 > sizeof(full)) {
            last_errno = -36;            /* -ENAMETOOLONG */
            return 0;
        }
        for (i = 0; sysdir[i] != '\0'; i++) {
            full[at++] = sysdir[i];
        }
        for (i = 0; path[i] != '\0'; i++) {
            full[at++] = path[i];
        }
        full[at] = '\0';
        path = full;
    }

    rc = sc6(SYS_prctl, PR_GENESIS_PE_LOAD, (long)path, (long)&out, 0, 0, 0);
    if (rc != 0) {
        last_errno = (int)rc;
        return 0;
    }

    /* DLL_PROCESS_ATTACH, dependencies first - the order the kernel listed
     * them in. A FALSE return is a DLL refusing to load; it stays mapped
     * (there is no unload) but the caller is told. */
    for (i = 0; i < out.count && i < GNT_PE_LOAD_MAX; i++) {
        if (out.mods[i].entry != 0) {
            dll_entry_t entry = (dll_entry_t)out.mods[i].entry;

            if (!entry((void *)out.mods[i].base, 1 /* DLL_PROCESS_ATTACH */, 0)) {
                last_errno = -8;         /* -ENOEXEC */
                return 0;
            }
        }
    }
    last_errno = 0;
    return (void *)out.base;
}

int gnt_pe_close(void *dll) {
    (void)dll;
    return 0;
}

/* --- exports ------------------------------------------------------------- */

static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd16(const u8 *p) {
    return (u16)(p[0] | (p[1] << 8));
}

static void *lookup(void *dll, const char *name, int depth);

/* "OTHER.Func" or "OTHER.#12": load OTHER.dll (a no-op if it is loaded)
 * and look again there. */
static void *forward(const char *fwd, int depth) {
    char dll[64], sym[128];
    u64 i = 0, j = 0;
    void *base;

    while (fwd[i] != '\0' && fwd[i] != '.' && i + 5 < sizeof(dll)) {
        dll[i] = fwd[i];
        i++;
    }
    if (fwd[i] != '.') {
        return 0;
    }
    dll[i] = '.'; dll[i + 1] = 'd'; dll[i + 2] = 'l'; dll[i + 3] = 'l';
    dll[i + 4] = '\0';
    i++;
    while (fwd[i] != '\0' && j + 1 < sizeof(sym)) {
        sym[j++] = fwd[i++];
    }
    sym[j] = '\0';

    base = gnt_pe_open(dll);
    return base ? lookup(base, sym, depth + 1) : 0;
}

static void *lookup(void *dll, const char *name, int depth) {
    const u8 *b = (const u8 *)dll;
    const u8 *opt, *dir;
    u32 pe, exp_rva, exp_size, nfuncs, nnames, funcs, names, ords, base_ord;
    u32 idx, rva, i;
    int by_ordinal = 0;
    u32 ordinal = 0;

    if (b == 0 || depth > 4 || b[0] != 'M' || b[1] != 'Z') {
        return 0;
    }
    pe = rd32(b + 0x3C);
    if (b[pe] != 'P' || b[pe + 1] != 'E') {
        return 0;
    }
    opt = b + pe + 24;                         /* the optional header       */
    if (rd16(opt) != 0x20B || rd32(opt + 108) == 0) {
        return 0;                              /* not PE32+, or no exports  */
    }
    exp_rva  = rd32(opt + 112);                /* DataDirectory[0]          */
    exp_size = rd32(opt + 116);
    if (exp_rva == 0) {
        return 0;
    }
    dir = b + exp_rva;
    base_ord = rd32(dir + 16);
    nfuncs   = rd32(dir + 20);
    nnames   = rd32(dir + 24);
    funcs    = rd32(dir + 28);
    names    = rd32(dir + 32);
    ords     = rd32(dir + 36);

    if (name[0] == '#') {
        by_ordinal = 1;
        for (i = 1; name[i] >= '0' && name[i] <= '9'; i++) {
            ordinal = ordinal * 10 + (u32)(name[i] - '0');
        }
    }

    if (by_ordinal) {
        if (ordinal < base_ord || ordinal - base_ord >= nfuncs) {
            return 0;
        }
        idx = ordinal - base_ord;
    } else {
        /* Two indirections: the i'th name's ordinal index, then the
         * function. Indexing the function table with i directly is right
         * only for a DLL that happens to export everything by name in
         * order. */
        for (i = 0; i < nnames; i++) {
            if (eq((const char *)(b + rd32(b + names + 4 * i)), name)) {
                break;
            }
        }
        if (i == nnames) {
            return 0;
        }
        idx = rd16(b + ords + 2 * i);
        if (idx >= nfuncs) {
            return 0;
        }
    }

    rva = rd32(b + funcs + 4 * idx);
    if (rva == 0) {
        return 0;
    }
    /* An RVA inside the export directory is a forwarder string, not code -
     * the only thing that says so is where it points. */
    if (rva >= exp_rva && rva < exp_rva + exp_size) {
        return forward((const char *)(b + rva), depth);
    }
    return (void *)(b + rva);
}

void *gnt_pe_sym(void *dll, const char *name) {
    void *p = lookup(dll, name, 0);

    last_errno = p ? 0 : -2;             /* -ENOENT */
    return p;
}

/* --- System V adapters ----------------------------------------------------
 *
 * Each adapter is 22 bytes in an executable page:
 *
 *     mov  r11, <the Win64 function>
 *     mov  rax, <gnt_sysv_to_win64>
 *     jmp  rax
 *
 * R11 is a System V scratch register that carries no argument, so it is
 * free to carry the target; RAX is free too, since the Win64 side ignores
 * the varargs count in AL. gnt_sysv_to_win64 (adapt.S) does the rest.
 *
 * The page is mapped read-write-execute. Genesis's mmap honours PROT_EXEC
 * but not yet the removal of PROT_WRITE, so asking for W^X here would be
 * asking for something that is not applied. */

extern void gnt_sysv_to_win64(void);

#define STUB_BYTES 24
#define PAGE_STUBS (4096 / STUB_BYTES)

static u8 *stub_page;
static int stub_used = PAGE_STUBS;

static void put64(u8 *p, u64 v) {
    int i;

    for (i = 0; i < 8; i++) {
        p[i] = (u8)(v >> (8 * i));
    }
}

void *gnt_pe_sym_sysv(void *dll, const char *name) {
    void *target = gnt_pe_sym(dll, name);
    u8 *s;

    if (target == 0) {
        return 0;
    }
    if (stub_used >= PAGE_STUBS) {
        long p = sc6(SYS_mmap, 0, 4096, 7 /* READ|WRITE|EXEC */,
                     0x22 /* PRIVATE|ANONYMOUS */, -1, 0);

        if (p < 0 && p > -4096) {
            last_errno = (int)p;
            return 0;
        }
        stub_page = (u8 *)p;
        stub_used = 0;
    }
    s = stub_page + STUB_BYTES * stub_used++;
    s[0] = 0x49; s[1] = 0xBB;                       /* mov r11, imm64 */
    put64(s + 2, (u64)target);
    s[10] = 0x48; s[11] = 0xB8;                     /* mov rax, imm64 */
    put64(s + 12, (u64)&gnt_sysv_to_win64);
    s[20] = 0xFF; s[21] = 0xE0;                     /* jmp rax        */
    s[22] = 0xCC; s[23] = 0xCC;
    return s;
}
