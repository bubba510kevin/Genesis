#include "nt.h"
#include "paging.h"
#include "pmm.h"
#include "teb.h"
#include "typesk.h"

/* --- layout assertions ---------------------------------------------------
 *
 * The offsets below are the ABI's, not this kernel's. A program compiled for
 * Windows reads GS:[0x30] for its own TEB and GS:[0x68] for its last error
 * because those constants are baked into the code a compiler emits - so a
 * field declared one byte out is not a bug this kernel can absorb, it is a
 * different structure that happens to have the same field names.
 *
 * Checked at build time rather than tested at run time for the reason
 * UNICODE_STRING taught: a wrong offset produces a plausible value, not a
 * crash, and a test image generated against the same wrong assumption agrees
 * with it. A negative array size fails the build and cannot be agreed with. */
typedef char nt_teb_layout[
    (__builtin_offsetof(nt_teb_t, stack_base)        == 0x008 &&
     __builtin_offsetof(nt_teb_t, stack_limit)       == 0x010 &&
     __builtin_offsetof(nt_teb_t, self)              == 0x030 &&
     __builtin_offsetof(nt_teb_t, client_id_process) == 0x040 &&
     __builtin_offsetof(nt_teb_t, client_id_thread)  == 0x048 &&
     __builtin_offsetof(nt_teb_t, peb)               == 0x060 &&
     __builtin_offsetof(nt_teb_t, last_error)        == 0x068) ? 1 : -1];

typedef char nt_peb_layout[
    (__builtin_offsetof(nt_peb_t, mutant)             == 0x008 &&
     __builtin_offsetof(nt_peb_t, image_base_address) == 0x010 &&
     __builtin_offsetof(nt_peb_t, ldr)                == 0x018 &&
     __builtin_offsetof(nt_peb_t, process_parameters) == 0x020 &&
     __builtin_offsetof(nt_peb_t, process_heap)       == 0x030) ? 1 : -1];

/* The parameters block's offsets, on the same terms. ImagePathName at 0x60
 * and CommandLine at 0x70 are the two that can be checked against Microsoft's
 * own winternl.h - it declares Reserved1[16] and Reserved2[10] ahead of them,
 * which is 0x60 exactly. If those two are right, the padding decisions before
 * them were right too, which is most of what can go wrong here. */
typedef char nt_params_layout[
    (__builtin_offsetof(nt_rtl_user_process_params_t, standard_input)  == 0x020 &&
     __builtin_offsetof(nt_rtl_user_process_params_t, standard_output) == 0x028 &&
     __builtin_offsetof(nt_rtl_user_process_params_t, standard_error)  == 0x030 &&
     __builtin_offsetof(nt_rtl_user_process_params_t,
                        current_directory_path)                        == 0x038 &&
     __builtin_offsetof(nt_rtl_user_process_params_t,
                        current_directory_handle)                      == 0x048 &&
     __builtin_offsetof(nt_rtl_user_process_params_t, dll_path)        == 0x050 &&
     __builtin_offsetof(nt_rtl_user_process_params_t, image_path_name) == 0x060 &&
     __builtin_offsetof(nt_rtl_user_process_params_t, command_line)    == 0x070 &&
     __builtin_offsetof(nt_rtl_user_process_params_t, environment)     == 0x080)
    ? 1 : -1];

/* Map n zeroed, writable user pages at `va` in a space that may not be in
 * CR3. Writable because a program writes its own TEB - SetLastError is a
 * store to GS:[0x68] and nothing else. */
static int map_zeroed(address_space_t *as, uint64 va, uint64 pages) {
    uint64 i;

    for (i = 0; i < pages; i++) {
        uint64 page = va + i * PMM_PAGE_SIZE;
        phys_addr_t phys;
        uint8 *win;
        uint64 b;

        if (vmm_get_phys_in(as, page) != 0) {
            return -12;                  /* -ENOMEM: something is already here */
        }
        phys = vmm_alloc_page_in(as, page,
                                 PAGE_PRESENT | PAGE_RW | PAGE_USER);
        if (phys == 0) {
            return -12;
        }
        win = (uint8 *)phys_to_virt(phys);
        for (b = 0; b < PMM_PAGE_SIZE; b++) {
            win[b] = 0;
        }
    }
    return 0;
}

/* The block is page-aligned and both structures fit inside their first page,
 * so a single window into the first frame reaches every field either one
 * declares. That is checked rather than assumed. */
static void *window(address_space_t *as, uint64 va) {
    phys_addr_t phys = vmm_get_phys_in(as, va & ~0xFFFULL);

    if (phys == 0) {
        return NULL;
    }
    return (void *)(phys_to_virt(phys & ~0xFFFULL) + (va & 0xFFF));
}

int nt_process_init(address_space_t *as, uint64 image_base,
                    uint64 stack_top, uint64 stack_size,
                    int pid, int tid) {
    nt_teb_t *teb;
    nt_peb_t *peb;
    int rc;

    if (as == NULL) {
        return -22;
    }
    /* Both structures have to sit wholly inside one page for the single
     * window above to be enough. They do, by a wide margin - but the day one
     * of them grows past 0x1000 this should fail to build rather than write
     * half a field into the wrong frame. */
    {
        typedef char fits_in_a_page[
            (sizeof(nt_teb_t) <= 0x1000 && sizeof(nt_peb_t) <= 0x1000) ? 1 : -1];
        (void)sizeof(fits_in_a_page);
    }

    rc = map_zeroed(as, NT_TEB_BASE, NT_TEB_PAGES);
    if (rc != 0) {
        return rc;
    }
    rc = map_zeroed(as, NT_PEB_BASE, NT_PEB_PAGES);
    if (rc != 0) {
        return rc;
    }

    teb = (nt_teb_t *)window(as, NT_TEB_BASE);
    peb = (nt_peb_t *)window(as, NT_PEB_BASE);
    if (teb == NULL || peb == NULL) {
        return -12;
    }

    /* StackBase is the HIGHEST address and StackLimit the lowest - the
     * opposite way round from how a POSIX stack is usually described, and
     * the pair a program reads to know whether a pointer is on its own
     * stack. Getting them backwards makes every such test answer no. */
    teb->stack_base  = stack_top;
    teb->stack_limit = stack_top - stack_size;

    teb->self = NT_TEB_BASE;
    teb->peb  = NT_PEB_BASE;

    teb->client_id_process = (uint64)pid;
    teb->client_id_thread  = (uint64)tid;

    /* last_error starts at zero, which is ERROR_SUCCESS, and that is the
     * correct initial value rather than merely a convenient one: a program
     * may call GetLastError before anything has failed. */

    peb->image_base_address = image_base;

    /* ldr, process_parameters and process_heap stay null on purpose. Each is
     * a promise this kernel cannot keep yet - the loader's module list needs
     * dynamic linking, the parameters block needs a command line in NT's own
     * counted-string form, and the heap needs RtlAllocateHeap. A plausible
     * non-null value in any of them would be worse than a null: a program
     * checks for null and copes, and follows a garbage pointer without
     * checking anything. */
    return 0;
}

/* --- a TEB for a thread that is not the first ---------------------------
 *
 * The same five fields nt_process_init writes for the main thread, into a
 * fresh block at `teb_va`. The PEB is NOT touched and NOT duplicated: it is
 * per process, and a second thread's TEB points at the one the main thread's
 * does - which is exactly what makes GetCurrentProcessId agree across
 * threads while GetCurrentThreadId does not. */
int nt_thread_teb_init(address_space_t *as, uint64 teb_va, uint64 stack_top,
                       uint64 stack_size, int pid, int tid) {
    nt_teb_t *teb;
    int rc;

    if (as == NULL || (teb_va & 0xFFF) != 0) {
        return -22;
    }
    rc = map_zeroed(as, teb_va, NT_TEB_PAGES);
    if (rc != 0) {
        return rc;
    }
    teb = (nt_teb_t *)window(as, teb_va);
    if (teb == NULL) {
        return -12;
    }
    teb->stack_base        = stack_top;
    teb->stack_limit       = stack_top - stack_size;
    teb->self              = teb_va;
    teb->peb               = NT_PEB_BASE;
    teb->client_id_process = (uint64)pid;
    teb->client_id_thread  = (uint64)tid;
    return 0;
}

/* Map `pages` zeroed user pages at `va` - a new thread's stack. Exported
 * for NtCreateThreadEx rather than re-implemented there; same reason as the
 * TEB above: one function that knows how to put zeroed user memory into a
 * space through the direct map. */
int nt_map_zeroed(address_space_t *as, uint64 va, uint64 pages) {
    return map_zeroed(as, va, pages);
}

/* --- the parameters block ------------------------------------------------
 *
 * Written through the direct map a page at a time, like everything else that
 * touches a space which is not in CR3. The strings are appended by a bump
 * pointer into the same block, and every field that names one holds a USER
 * virtual address - the program reads them with its own mappings, not with
 * the kernel's window into them, and mixing the two produces a pointer that
 * is valid in exactly the wrong address space. */

typedef struct {
    address_space_t *as;
    uint64           used;            /* offset from NT_PARAMS_BASE        */
    uint64           cap;
    int              overflowed;
} nt_arena_t;

/* Append raw bytes, crossing page boundaries as needed. Sets `overflowed`
 * rather than returning at each of the dozen call sites: the checks that
 * matter are "did it all fit" once at the end and "is the arena still good"
 * before committing, and a builder that stops writing but keeps counting
 * reports the size actually needed. */
static void arena_put(nt_arena_t *a, const void *src, uint64 len) {
    const uint8 *s = (const uint8 *)src;
    uint64 done = 0;

    if (a->overflowed || a->used + len > a->cap) {
        a->overflowed = 1;
        a->used += len;
        return;
    }
    while (done < len) {
        uint64 va     = NT_PARAMS_BASE + a->used + done;
        uint64 offset = va & 0xFFF;
        uint64 chunk  = PMM_PAGE_SIZE - offset;
        uint8 *dst    = (uint8 *)window(a->as, va);
        uint64 i;

        if (dst == NULL) {
            a->overflowed = 1;
            return;
        }
        if (chunk > len - done) {
            chunk = len - done;
        }
        for (i = 0; i < chunk; i++) {
            dst[i] = s[done + i];
        }
        done += chunk;
    }
    a->used += len;
}

/* One ASCII character as UTF-16LE. Everything this kernel has is ASCII: FAT
 * short names, the paths ash builds, the strings execve copied in. A byte
 * above 0x7F would be some code page's idea of a character and there is no
 * code page here, so it becomes U+FFFD rather than a silently wrong letter. */
static void arena_put_wchar(nt_arena_t *a, uint8 c) {
    uint16 w = (c < 0x80) ? (uint16)c : 0xFFFD;

    arena_put(a, &w, 2);
}

static void arena_put_wstr(nt_arena_t *a, const char *s) {
    while (s != NULL && *s != '\0') {
        arena_put_wchar(a, (uint8)*s++);
    }
}

/* A POSIX path as a Windows program expects to see it: /etc/motd becomes
 * C:\etc\motd.
 *
 * The volume letter is a constant because there is one volume. When there are
 * two this becomes a lookup and this comment becomes wrong, which is why the
 * conversion lives in one function rather than at three call sites.
 *
 * This is the exact inverse of what ntdll's RtlDosPathNameToNtPathName_U does
 * on the way back in - C:\etc\motd to \??\C:\etc\motd - so a path handed to a
 * program here and passed straight back to NtOpenFile resolves to the file it
 * came from. That round trip is the thing worth preserving; the letter is
 * not. */
static void arena_put_dos_path(nt_arena_t *a, const char *posix) {
    arena_put_wchar(a, 'C');
    arena_put_wchar(a, ':');
    while (posix != NULL && *posix != '\0') {
        arena_put_wchar(a, (*posix == '/') ? (uint8)'\\' : (uint8)*posix);
        posix++;
    }
}

/* Fill in a UNICODE_STRING for a run of bytes just appended.
 *
 * Length excludes the terminating null and MaximumLength includes it, which
 * is the convention every consumer relies on: a program prints Length bytes
 * and a program that passes Buffer to something expecting a C string relies
 * on the terminator being there. Getting these equal is how a trailing null
 * ends up printed as a stray character. */
static void set_ustr(nt_unicode_string_t *u, uint64 start_va, uint64 bytes) {
    u->length         = (uint16)bytes;
    u->maximum_length = (uint16)(bytes + 2);
    u->buffer         = start_va;
}

/* Does the POSIX path already end in a separator? "/" converts to "C:\\",
 * which is complete; "/usr/bin" converts to "C:\\usr\\bin" and needs one
 * added. Asked of the POSIX form because that is the input - asking after the
 * conversion would mean re-reading bytes already written into another address
 * space. */
static int ends_with_slash(const char *s) {
    char last = '\0';

    while (s != NULL && *s != '\0') {
        last = *s++;
    }
    return last == '/';
}

static int str_has_space(const char *s) {
    while (s != NULL && *s != '\0') {
        if (*s == ' ' || *s == '\t') {
            return 1;
        }
        s++;
    }
    return 0;
}

int nt_process_params_init(address_space_t *as, const nt_params_desc_t *desc) {
    nt_rtl_user_process_params_t *params;
    nt_arena_t a;
    nt_peb_t *peb;
    uint64 struct_size, start, bytes;
    int rc, i;

    if (as == NULL || desc == NULL) {
        return -22;
    }

    /* The structure has to fit in the first page for the single window used
     * to fill it in to reach every field. */
    {
        typedef char params_fit[
            (sizeof(nt_rtl_user_process_params_t) <= 0x1000) ? 1 : -1];
        (void)sizeof(params_fit);
    }

    rc = map_zeroed(as, NT_PARAMS_BASE, NT_PARAMS_PAGES);
    if (rc != 0) {
        return rc;
    }

    /* Strings start after the structure, 8-aligned. Nothing requires that
     * alignment - a UTF-16 string needs 2 - but a block whose parts are
     * aligned is one where a later field added to the structure does not
     * silently shift every string by four bytes. */
    struct_size = (sizeof(nt_rtl_user_process_params_t) + 7) & ~7ULL;

    a.as         = as;
    a.used       = struct_size;
    a.cap        = NT_PARAMS_PAGES * PMM_PAGE_SIZE;
    a.overflowed = 0;

    params = (nt_rtl_user_process_params_t *)window(as, NT_PARAMS_BASE);
    if (params == NULL) {
        return -12;
    }

    params->standard_input  = desc->std_input;
    params->standard_output = desc->std_output;
    params->standard_error  = desc->std_error;

    /* ImagePathName: the full DOS path of the image. */
    start = NT_PARAMS_BASE + a.used;
    arena_put_dos_path(&a, desc->image_path);
    bytes = (NT_PARAMS_BASE + a.used) - start;
    arena_put_wchar(&a, 0);
    set_ustr(&params->image_path_name, start, bytes);

    /* CurrentDirectory, which NT spells with a trailing backslash - C:\ for
     * the root rather than C:. A program that appends a relative path to it
     * produces C:\etcC:\ without one. */
    start = NT_PARAMS_BASE + a.used;
    arena_put_dos_path(&a, desc->cwd);
    if (!ends_with_slash(desc->cwd)) {
        arena_put_wchar(&a, '\\');
    }
    bytes = (NT_PARAMS_BASE + a.used) - start;
    arena_put_wchar(&a, 0);
    set_ustr(&params->current_directory_path, start, bytes);

    /* CommandLine.
     *
     * NT does not have an argument vector: a process is started with ONE
     * string, and splitting it is the runtime's job - CommandLineToArgvW, or
     * the CRT before main. Genesis is arriving from the other side, with the
     * vector ash built, so the string is synthesised by joining it.
     *
     * That join cannot be lossless in general, and it is worth being precise
     * about where it stops being so. An argument containing a space is
     * quoted, which round-trips. An argument containing a quote is not
     * escaped, so it will re-split differently; ash cannot produce one
     * through the paths that reach here today, and the honest fix is to
     * escape when it can. Left as it is rather than half-implemented,
     * because a quoting scheme that is almost right is worse to debug than
     * one that is obviously absent.
     *
     * argv[0] is included, as on Windows: GetCommandLineW returns the
     * program name too, which is why every CRT skips it. */
    start = NT_PARAMS_BASE + a.used;
    for (i = 0; desc->argv != NULL && desc->argv[i] != NULL; i++) {
        int quote = str_has_space(desc->argv[i]);

        if (i > 0) {
            arena_put_wchar(&a, ' ');
        }
        if (quote) {
            arena_put_wchar(&a, '"');
        }
        arena_put_wstr(&a, desc->argv[i]);
        if (quote) {
            arena_put_wchar(&a, '"');
        }
    }
    bytes = (NT_PARAMS_BASE + a.used) - start;
    arena_put_wchar(&a, 0);
    set_ustr(&params->command_line, start, bytes);

    /* Environment: KEY=VALUE, each null-terminated, the whole ended by a
     * second null. The final null is why an empty environment is still two
     * bytes and not zero - a program scans for the double null, and a null
     * pointer or an empty block would be read as unterminated. */
    start = NT_PARAMS_BASE + a.used;
    for (i = 0; desc->envp != NULL && desc->envp[i] != NULL; i++) {
        arena_put_wstr(&a, desc->envp[i]);
        arena_put_wchar(&a, 0);
    }
    arena_put_wchar(&a, 0);
    params->environment = start;

    if (a.overflowed) {
        /* Nothing is committed: PEB->ProcessParameters is still null, so the
         * process sees no block rather than a truncated one. execve turns
         * this into a failed exec, which is the right outcome - a command
         * line silently cut in half runs a different command. */
        return -7;                        /* -E2BIG */
    }

    params->maximum_length = (uint32)a.cap;
    params->length         = (uint32)a.used;

    peb = (nt_peb_t *)window(as, NT_PEB_BASE);
    if (peb == NULL) {
        return -12;
    }
    peb->process_parameters = NT_PARAMS_BASE;
    return 0;
}

/* --- implicit TLS (see teb.h) --------------------------------------------- */

/* Byte copies into and out of a space that may not be the loaded one, a
 * page at a time through the direct map. */
static int as_read(address_space_t *as, uint64 va, void *dst, uint64 n) {
    uint8 *d = (uint8 *)dst;

    while (n > 0) {
        uint8 *w = (uint8 *)window(as, va);
        uint64 chunk = PMM_PAGE_SIZE - (va & 0xFFF);

        if (w == NULL) {
            return -14;
        }
        if (chunk > n) {
            chunk = n;
        }
        {
            uint64 i;
            for (i = 0; i < chunk; i++) {
                d[i] = w[i];
            }
        }
        d += chunk;
        va += chunk;
        n -= chunk;
    }
    return 0;
}

static int as_write(address_space_t *as, uint64 va, const void *src, uint64 n) {
    const uint8 *s = (const uint8 *)src;

    while (n > 0) {
        uint8 *w = (uint8 *)window(as, va);
        uint64 chunk = PMM_PAGE_SIZE - (va & 0xFFF);

        if (w == NULL) {
            return -14;
        }
        if (chunk > n) {
            chunk = n;
        }
        {
            uint64 i;
            for (i = 0; i < chunk; i++) {
                w[i] = s[i];
            }
        }
        s += chunk;
        va += chunk;
        n -= chunk;
    }
    return 0;
}

int nt_tls_publish(address_space_t *as, const nt_tls_table_t *table) {
    typedef char fits[(NT_PEB_TLS_OFFSET + sizeof(nt_tls_table_t) <= 0x1000)
                      ? 1 : -1];
    (void)sizeof(fits);
    return as_write(as, NT_PEB_BASE + NT_PEB_TLS_OFFSET, table, sizeof(*table));
}

int nt_thread_tls_init(address_space_t *as, int slot, uint64 teb_va,
                       uint64 *area_out, uint64 *pages_out) {
    nt_tls_table_t t;
    uint64 area, pages, ptrs[NT_TLS_MAX_MODULES];
    uint32 i;
    int rc;

    *area_out = 0;
    *pages_out = 0;
    if (as_read(as, NT_PEB_BASE + NT_PEB_TLS_OFFSET, &t, sizeof(t)) != 0 ||
        t.magic != NT_TLS_MAGIC || t.count == 0) {
        return 0;                        /* no implicit TLS: nothing to do */
    }
    /* The table lives in user memory, so it is re-validated here: a program
     * that scribbled on its PEB gets a failed thread, not a kernel write
     * somewhere it chose. */
    if (t.count > NT_TLS_MAX_MODULES || t.area_bytes == 0 ||
        t.area_bytes > NT_TLS_AREA_STRIDE || slot < 0 || slot > NT_TLS_MAIN_SLOT) {
        return -22;
    }
    area  = NT_TLS_AREA_BASE + (uint64)slot * NT_TLS_AREA_STRIDE;
    pages = (t.area_bytes + 0xFFF) / 0x1000;
    rc = map_zeroed(as, area, pages);
    if (rc != 0) {
        return rc;
    }
    *area_out = area;
    *pages_out = pages;
    for (i = 0; i < t.count; i++) {
        nt_tls_module_t *m = &t.mod[i];
        uint64 len = m->end - m->start;

        if (m->end < m->start || m->block_offset + len + m->zero_fill > t.area_bytes) {
            return -22;
        }
        ptrs[i] = area + m->block_offset;
        /* The template, copied; the zero fill is already zero - the pages
         * were mapped zeroed. */
        while (len > 0) {
            uint8 buf[256];
            uint64 chunk = len > sizeof(buf) ? sizeof(buf) : len;

            if (as_read(as, m->start + (m->end - m->start - len), buf, chunk) != 0 ||
                as_write(as, ptrs[i] + (m->end - m->start - len), buf, chunk) != 0) {
                return -14;
            }
            len -= chunk;
        }
    }
    if (as_write(as, area, ptrs, (uint64)t.count * 8) != 0) {
        return -14;
    }
    /* ThreadLocalStoragePointer, TEB+0x58: what compiled TLS accesses read
     * through gs:[0x58][_tls_index]. */
    return as_write(as, teb_va + __builtin_offsetof(nt_teb_t, tls_pointer),
                    &area, 8);
}
