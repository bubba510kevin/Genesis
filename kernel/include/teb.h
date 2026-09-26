#ifndef TEB_H
#define TEB_H

#include "paging.h"
#include "typesk.h"

/* The Thread Environment Block and Process Environment Block.
 *
 * --- What these are ------------------------------------------------------
 * Everything a Windows program knows about itself without asking the kernel.
 * GS:[0x30] is its own TEB, GS:[0x60] is the PEB, GS:[0x68] is the value
 * GetLastError returns. None of those are system calls; they are loads from
 * a page the kernel prepared before the image ever ran, which is why gs_base
 * had to become per-thread state before any of this could work.
 *
 * The kernel builds them, not ntdll. That is how NT does it too, and it has
 * to be: LdrInitializeThunk runs with a stack, a TEB and a PEB already in
 * place - it fills in the loader's own fields, it does not bootstrap the
 * block it is reading its arguments out of.
 *
 * --- Provenance ----------------------------------------------------------
 * The offsets are public. They are in Windows Internals, they are what
 * `dt _TEB` prints in a debugger, and they are relied on by name in
 * Microsoft's own published headers via NtCurrentTeb(). Nothing here is
 * copied from ReactOS or Wine.
 *
 * --- Why every field is spelled out --------------------------------------
 * These are ABI structures, so their offsets are fixed by what a compiler
 * building a Windows program produces - not by anything this kernel chooses.
 * A field at the wrong offset does not fail to build and does not crash; it
 * reads the wrong eight bytes, and the symptom is a program that behaves
 * oddly for reasons nothing points at. That already happened once here, with
 * UNICODE_STRING, so: no packed attribute, padding written out where the ABI
 * has padding, and a build-time assertion on every offset that matters.
 *
 * Only the prefix that is actually populated is declared. A partial structure
 * that is honest about being partial beats a complete one transcribed from
 * memory, and the fields below are the ones something reads today. */

typedef struct {
    /* NT_TIB, the first 0x38 bytes. exception_list is the 32-bit SEH chain
     * and is unused on x86-64, where exception handling is table-driven out
     * of .pdata - it stays zero and is not a placeholder for something to
     * fill in later. */
    uint64 exception_list;            /* 0x000 */
    uint64 stack_base;                /* 0x008 highest stack address     */
    uint64 stack_limit;               /* 0x010 lowest committed address  */
    uint64 sub_system_tib;            /* 0x018 */
    uint64 fiber_data;                /* 0x020 */
    uint64 arbitrary_user_pointer;    /* 0x028 */

    /* The TEB's own address. GS:[0x30] is how a program gets a pointer to
     * its TEB it can pass around, since GS-relative addressing cannot be
     * taken the address of. */
    uint64 self;                      /* 0x030 */

    uint64 environment_pointer;       /* 0x038 */
    uint64 client_id_process;         /* 0x040 pid                       */
    uint64 client_id_thread;          /* 0x048 tid                       */
    uint64 active_rpc_handle;         /* 0x050 */
    uint64 tls_pointer;               /* 0x058 */
    uint64 peb;                       /* 0x060 */

    /* GetLastError and SetLastError are loads and stores here and nothing
     * else - no system call, no lock. That is why they are cheap on Windows
     * and why they cannot work at all until a TEB exists. */
    uint32 last_error;                /* 0x068 */
    uint32 owned_critical_sections;   /* 0x06C */

    uint64 csr_client_thread;         /* 0x070 */
    uint64 win32_thread_info;         /* 0x078 */
} nt_teb_t;

typedef struct {
    uint8  inherited_address_space;   /* 0x000 */
    uint8  read_image_file_exec_options;
    uint8  being_debugged;
    uint8  bit_field;
    uint32 reserved0;                 /* 0x004 ABI padding before a pointer */
    uint64 mutant;                    /* 0x008 */

    /* Where the image was loaded. A PE that has to find its own headers -
     * to walk its exports, or its .pdata - starts here rather than from a
     * relocated constant. */
    uint64 image_base_address;        /* 0x010 */

    uint64 ldr;                       /* 0x018 PEB_LDR_DATA; needs item 7 */
    uint64 process_parameters;        /* 0x020 command line lives here    */
    uint64 sub_system_data;           /* 0x028 */
    uint64 process_heap;              /* 0x030 RtlAllocateHeap's default  */
    uint64 fast_peb_lock;             /* 0x038 */
} nt_peb_t;

/* Where they go.
 *
 * Below USER_MMAP_BASE (0x30000000) so an mmap can never land on them, and
 * far below the image at 0x140000000. Windows puts the TEB wherever the
 * kernel felt like and a program is expected to find it through GS rather
 * than to know the address - so this being a constant is a convenience for
 * testing, not a contract, and nothing outside the kernel should depend on
 * the value.
 *
 * Two pages for the TEB because the real one is about 0x1800 bytes and a
 * program that pokes a field past what is declared above should find mapped
 * zeroes rather than a fault. */
#define NT_TEB_BASE   0x0000000020000000ULL
#define NT_TEB_PAGES  2
#define NT_PEB_BASE   (NT_TEB_BASE + (NT_TEB_PAGES * 0x1000ULL))
#define NT_PEB_PAGES  1

/* RTL_USER_PROCESS_PARAMETERS, straight after the PEB. Two pages: the
 * structure is 0x88 bytes and everything after it is strings, whose total is
 * bounded by what execve was able to copy in from user space in the first
 * place - EXEC_STR_BYTES for the arguments and the environment together, at
 * most doubled by the conversion to UTF-16, plus two paths. That is under 6KB
 * against 8, and the builder checks rather than trusting the arithmetic. */
#define NT_PARAMS_BASE  (NT_PEB_BASE + (NT_PEB_PAGES * 0x1000ULL))
#define NT_PARAMS_PAGES 2

/* Build the TEB and PEB for a process about to start at `image_base`, in an
 * address space that is not necessarily the one in CR3 - execve prepares the
 * new image before committing to it, so this has to work through the direct
 * map like the loaders do.
 *
 * Returns 0, or a negative errno. On success the caller sets the thread's
 * gs_base to NT_TEB_BASE; this does not touch any MSR itself, because which
 * of the two GS MSRs is the user's depends on where the CPU is standing and
 * that decision belongs in one place. */
int nt_process_init(address_space_t *as, uint64 image_base,
                    uint64 stack_top, uint64 stack_size,
                    int pid, int tid);

/* What the process was started with, in the caller's own terms.
 *
 * Grouped into a structure rather than passed as seven arguments because
 * three of them are handles and three are paths, and a caller that swaps two
 * of either produces a process that works until something looks closely.
 *
 * Paths are POSIX and absolute, as the rest of the kernel has them; the DOS
 * spelling a Windows program expects is this builder's job, not the caller's.
 * The handles are NT-encoded values, already checked to be open - the handle
 * table belongs to process.c and object.c, and nothing here needs to learn
 * about it to copy three integers.
 *
 * A null argv or envp is legal and produces an empty command line or a single
 * empty environment block, which is what a process started with neither
 * should see. */
typedef struct {
    const char        *image_path;    /* POSIX, absolute: /bin/hello.exe   */
    const char        *cwd;           /* POSIX, absolute, may be "/"       */
    const char *const *argv;          /* NULL-terminated, may be NULL      */
    const char *const *envp;          /* NULL-terminated, may be NULL      */
    uint64             std_input;     /* NT HANDLE, or 0 if not open       */
    uint64             std_output;
    uint64             std_error;
} nt_params_desc_t;

/* Build the parameters block and point the PEB at it.
 *
 * Runs after nt_process_init, and needs it to have run: the last thing this
 * does is store into PEB->ProcessParameters, which means the PEB has to be
 * mapped already. Returns 0, or a negative errno - -E2BIG if the strings do
 * not fit in NT_PARAMS_PAGES, which is a bound worth failing on rather than
 * truncating a command line into something that parses differently. */
int nt_process_params_init(address_space_t *as, const nt_params_desc_t *desc);

#endif
