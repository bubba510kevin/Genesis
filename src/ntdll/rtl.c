#include "ntdll.h"
#include "ntsyscalls.h"

/* The runtime library: the parts of ntdll that are not system calls.
 *
 * Original implementations written against the documented interfaces - the
 * WDK documentation, the published PE/COFF spec and Windows Internals.
 * Nothing here is derived from ReactOS or Wine; both are copyleft and copying
 * a line of either would settle the licensing question for the whole tree. */

PTEB NtCurrentTeb(void) {
    /* GS:[0x30] is the TEB's own address. It has to be read out of the
     * segment rather than taken as &something, because GS-relative addressing
     * has no address to take - which is exactly why the field exists. */
    PTEB teb;

    __asm__ volatile ("movq %%gs:0x30, %0" : "=r"(teb));
    return teb;
}

static SIZE_T wcslen_local(PCWSTR s) {
    SIZE_T n = 0;

    while (s[n] != 0) {
        n++;
    }
    return n;
}

void RtlInitUnicodeString(PUNICODE_STRING d, PCWSTR s) {
    if (d == 0) {
        return;
    }
    if (s == 0) {
        d->Length = 0;
        d->MaximumLength = 0;
        d->Buffer = 0;
        return;
    }
    /* Bytes, not characters. Both fields. The terminator is counted in
     * MaximumLength and not in Length, which is what lets a caller append. */
    d->Length        = (WORD)(wcslen_local(s) * 2);
    d->MaximumLength = (WORD)(d->Length + 2);
    d->Buffer        = (PWSTR)s;
}

/* --- DOS paths to NT paths ----------------------------------------------
 *
 * C:\dir\file  ->  \??\C:\dir\file
 *
 * This is the point where the object namespace stops being theoretical for
 * anything above ntdll. Every Win32 call that takes a filename comes through
 * here, and what comes out the other side is a native path the kernel
 * resolves through \??\ - which is the same tree /dev and ash resolve
 * through. The unparsed remainder the namespace has carried since it was
 * written is what makes \??\C:\dir\file work: \??\C: names the volume and
 * \dir\file is handed to it.
 *
 * Only the drive-letter form is handled. UNC paths, device paths and the
 * \\?\ prefix each have their own rule and each deserves to fail visibly
 * rather than be guessed at. */

#define NT_PREFIX_LEN 4                    /* \??\ */

BOOL RtlDosPathNameToNtPathName_U(PCWSTR dos, PUNICODE_STRING nt,
                                  PCWSTR *file_part, PVOID reserved) {
    static const WCHAR prefix[NT_PREFIX_LEN] = { '\\', '?', '?', '\\' };
    SIZE_T len, i;
    PWSTR  out;
    SIZE_T last_sep = 0;

    (void)reserved;
    if (dos == 0 || nt == 0) {
        return 0;
    }
    /* A drive letter, a colon and a separator. Anything else is a form this
     * does not implement, and saying so is better than producing a plausible
     * native path that names nothing. */
    if (dos[0] == 0 || dos[1] != ':' || (dos[2] != '\\' && dos[2] != '/')) {
        return 0;
    }

    len = wcslen_local(dos);
    if ((len + NT_PREFIX_LEN + 1) * 2 > 0xFFF0u) {
        return 0;                          /* Length is a WORD, in bytes */
    }

    out = (PWSTR)RtlAllocateHeap(0, 0, (len + NT_PREFIX_LEN + 1) * 2);
    if (out == 0) {
        return 0;
    }

    for (i = 0; i < NT_PREFIX_LEN; i++) {
        out[i] = prefix[i];
    }
    for (i = 0; i < len; i++) {
        /* Forward slashes are legal in a Win32 path and are not legal in a
         * native one - the namespace separator is a backslash and nothing
         * downstream translates. */
        WCHAR c = (dos[i] == '/') ? (WCHAR)'\\' : dos[i];

        out[NT_PREFIX_LEN + i] = c;
        if (c == '\\') {
            last_sep = NT_PREFIX_LEN + i;
        }
    }
    out[NT_PREFIX_LEN + len] = 0;

    nt->Buffer        = out;
    nt->Length        = (WORD)((NT_PREFIX_LEN + len) * 2);
    nt->MaximumLength = (WORD)(nt->Length + 2);

    if (file_part != 0) {
        /* Points into the CONVERTED string, not the original: a caller uses
         * it to name the leaf of the path it is about to open, and the two
         * strings no longer have the same length. */
        *file_part = (last_sep != 0) ? &out[last_sep + 1] : 0;
    }
    return 1;
}

/* --- the heap ------------------------------------------------------------
 *
 * A bump allocator with a single free list, over NtAllocateVirtualMemory.
 * Deliberately simple, and worth saying why rather than apologising for it:
 * what RtlAllocateHeap has to be at this stage is CORRECT and reachable from
 * kernel32's HeapAlloc. Coalescing, per-size bins and the low-fragmentation
 * heap are all replaceable behind this interface later without any caller
 * knowing, and none of them can be debugged before a PE with imports runs at
 * all.
 *
 * Nothing is ever returned to the kernel. That is not a leak in the sense
 * that matters - the address space goes away with the process - and a heap
 * that never unmaps is what most allocators do anyway. */

#define HEAP_CHUNK  (64u * 1024u)
#define HEAP_ALIGN  16u

typedef struct block {
    SIZE_T        size;                    /* payload bytes, aligned      */
    struct block *next_free;
} block_t;

static BYTE  *heap_cursor;
static SIZE_T heap_left;
static block_t *heap_free_list;

static SIZE_T align_up(SIZE_T n) {
    return (n + (HEAP_ALIGN - 1)) & ~(SIZE_T)(HEAP_ALIGN - 1);
}

static int heap_grow(SIZE_T need) {
    PVOID  base = 0;
    SIZE_T size = need > HEAP_CHUNK ? align_up(need) : HEAP_CHUNK;
    NTSTATUS st;

    st = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!NT_SUCCESS(st) || base == 0) {
        return 0;
    }
    heap_cursor = (BYTE *)base;
    heap_left   = size;
    return 1;
}

PVOID RtlAllocateHeap(PVOID heap, DWORD flags, SIZE_T size) {
    SIZE_T   want = align_up(size ? size : 1);
    SIZE_T   total = want + align_up(sizeof(block_t));
    block_t **link;
    block_t  *b;

    (void)heap;

    /* First fit. A first-fit scan over one list is the wrong long-term
     * answer and the right first one: it is short enough to be obviously
     * correct, which is what matters while everything above it is new. */
    for (link = &heap_free_list; *link != 0; link = &(*link)->next_free) {
        if ((*link)->size >= want) {
            b = *link;
            *link = b->next_free;
            b->next_free = 0;
            goto found;
        }
    }

    if (heap_left < total && !heap_grow(total)) {
        return 0;
    }
    b = (block_t *)heap_cursor;
    b->size = want;
    b->next_free = 0;
    heap_cursor += total;
    heap_left   -= total;

found:
    {
        BYTE *payload = (BYTE *)b + align_up(sizeof(block_t));

        if (flags & HEAP_ZERO_MEMORY) {
            SIZE_T i;

            for (i = 0; i < want; i++) {
                payload[i] = 0;
            }
        }
        return payload;
    }
}

BOOL RtlFreeHeap(PVOID heap, DWORD flags, PVOID address) {
    block_t *b;

    (void)heap;
    (void)flags;
    if (address == 0) {
        return 1;                          /* freeing nothing succeeds */
    }
    b = (block_t *)((BYTE *)address - align_up(sizeof(block_t)));
    b->next_free = heap_free_list;
    heap_free_list = b;
    return 1;
}

/* --- process startup -----------------------------------------------------
 *
 * On Windows the kernel enters LdrInitializeThunk rather than the image's
 * entry point, and this runs before a single instruction of the program.
 * Genesis does not do that yet - execve jumps straight to the image - so this
 * is currently called by the image itself, and the ordering is the same
 * either way: the TEB and PEB already exist, built by the kernel, because
 * this reads its arguments out of them and cannot be what creates them.
 *
 * What it does NOT do is as important as what it does. It does not walk an
 * import table, resolve a DLL or run a TLS callback, because none of those
 * exist yet - the module list needs dynamic linking. Writing a plausible
 * stand-in for any of them would produce a PEB that lies. */
void LdrInitializeThunk(void) {
    PTEB teb = NtCurrentTeb();

    if (teb == 0 || teb->ProcessEnvironmentBlock == 0) {
        return;
    }
    /* The process heap. The PEB field is what HeapAlloc(GetProcessHeap())
     * resolves to, and it is null until something claims it - so claim it,
     * with a value RtlAllocateHeap recognises. There is one heap, so the
     * handle only has to be non-null and stable. */
    if (teb->ProcessEnvironmentBlock->ProcessHeap == 0) {
        teb->ProcessEnvironmentBlock->ProcessHeap =
            (PVOID)teb->ProcessEnvironmentBlock;
    }
    teb->LastErrorValue = 0;
}

/* The DLL's entry point. Named for what MinGW's linker expects by default so
 * that -nostdlib does not also mean --entry on the command line. Does
 * nothing: there is no per-DLL initialisation to run, and the loader that
 * would call this on attach is item 7. */
BOOL DllMainCRTStartup(PVOID instance, DWORD reason, PVOID reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return 1;
}
