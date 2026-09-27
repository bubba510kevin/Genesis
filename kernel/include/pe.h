#ifndef PE_H
#define PE_H

#include "paging.h"
#include "typesk.h"

/* PE32+ parsing and loading.
 *
 * --- Provenance ----------------------------------------------------------
 * Every structure below is transcribed from the published PE/COFF
 * specification, which Microsoft documents openly. Nothing here is derived
 * from ReactOS or Wine - both are copyleft and would settle the licensing
 * question for the whole kernel the moment a line of either was copied. The
 * field names are the spec's own, in this kernel's spelling.
 *
 * --- How this differs from the ELF loader --------------------------------
 * Three things, and they are the whole of the work:
 *
 * 1. There are no program headers. A PE's sections ARE its load map, and the
 *    file offset and the virtual offset of a section are two independent
 *    numbers (PointerToRawData and VirtualAddress) rather than one field
 *    read twice. Conflating them works for exactly as long as FileAlignment
 *    equals SectionAlignment and then silently loads garbage.
 *
 * 2. Addresses in the file are RVAs - offsets from ImageBase - not absolute.
 *    Everything is relative until a base is chosen.
 *
 * 3. The image says where it WANTS to live, and may or may not care. If the
 *    preferred base is unavailable the image moves, and every absolute
 *    address baked into it has to be adjusted by the difference. That is what
 *    .reloc is for, and it is the one part of this with no ELF analogue at
 *    all for a static executable.
 *
 * Genesis has room for the usual preferred base: MinGW and link.exe both
 * default to 0x140000000 for a 64-bit executable, which is 5GB - above the
 * user stack at 1GB, far below PE_USER_LIMIT. So the common case needs no
 * relocation, and the relocation path below exists for the case where the
 * base is taken or out of range rather than as the default route.
 */

#define PE_OK               0
#define PE_ERR_TRUNCATED   -1   /* file smaller than its own headers claim  */
#define PE_ERR_MAGIC       -2   /* no MZ, or no PE\0\0 where e_lfanew says  */
#define PE_ERR_MACHINE     -3   /* not x86-64                               */
#define PE_ERR_OPTMAGIC    -4   /* PE32 rather than PE32+                   */
#define PE_ERR_NOTEXE      -5   /* a DLL, or not an executable image        */
#define PE_ERR_ALIGN       -6   /* SectionAlignment finer than a page       */
#define PE_ERR_ADDRESS     -7   /* the image will not fit in user space     */
#define PE_ERR_SECTION     -8   /* a section's extent is outside the file   */
#define PE_ERR_RELOC       -9   /* needs to move and cannot, or bad .reloc  */
#define PE_ERR_NOMEM      -10   /* out of physical frames                   */
#define PE_ERR_SUBSYSTEM  -12   /* kernel-mode load, not IMAGE_SUBSYSTEM_NATIVE */
#define PE_ERR_TLS        -13   /* a TLS directory this loader will not take   */

/* Same limit and same reason as ELF_USER_LIMIT: at or above this is the
 * non-canonical hole or kernel space, and an image claiming an address up
 * there would map over the running kernel. Deliberately a separate constant
 * from the ELF one even though the value matches - they answer to different
 * formats, and tying them together would make a change to one silently
 * change the other. */
#define PE_USER_LIMIT     0x0000800000000000ULL

/* Where an image goes when it cannot have the base it asked for. Above the
 * usual 0x140000000 so that a relocated image and a preferred-base one can
 * coexist, which is what a process loading a DLL will need. */
#define PE_FALLBACK_BASE  0x0000000200000000ULL

/* Kernel-mode driver images (pe_load_driver, see below) never share
 * PE_USER_LIMIT/PE_FALLBACK_BASE - those are userspace canonical-hole
 * constants and would map a driver into a process's own address range.
 * PE_DRIVER_BASE is the fallback value now (ROADMAP item 11's kernel VA
 * allocator, vmalloc.h, reserves the real window at boot via
 * pe_driver_window_init() below) - kept as a literal so this still means
 * something if that reservation was ever skipped. A real .sys's preferred
 * ImageBase is never a kernel-half address, so a kernel-mode load takes
 * the fallback branch in load_image almost every time - that is the
 * routine case here, not an edge case. */
#define PE_DRIVER_BASE        0xFFFFFFFFA1000000ULL
#define DRIVER_IMAGE_MAX_SIZE 0x0000000002000000ULL   /* 32MB */

/* Reserve DRIVER_IMAGE_MAX_SIZE from the kernel VA allocator (vmalloc.h)
 * for kernel-mode driver loads, instead of trusting PE_DRIVER_BASE not to
 * collide with KHEAP_START/KSTACK_BASE by comment alone. Call once, after
 * kvm_init(), before the first pe_load_driver(). Safe to skip -
 * PE_DRIVER_BASE remains correct, just no longer collision-checked. */
void pe_driver_window_init(void);

/* IMAGE_SUBSYSTEM_NATIVE - the one subsystem value a kernel-mode driver
 * image is allowed to have (checked only when pe_load_driver's kernel_mode
 * is set; pe_validate/pe_load_into/pe_load_executable never read this
 * field, matching their existing behaviour exactly). */
#define PE_SUBSYSTEM_NATIVE 1

#define PE_DOS_MAGIC      0x5A4Du         /* "MZ"                           */
#define PE_NT_SIGNATURE   0x00004550u     /* "PE\0\0"                       */
#define PE_MACHINE_AMD64  0x8664u
#define PE_OPT_MAGIC_64   0x020Bu         /* PE32+                          */

/* IMAGE_FILE_* characteristics, only the two that decide whether this is
 * something to run. */
#define PE_FILE_EXECUTABLE_IMAGE  0x0002u
#define PE_FILE_DLL               0x2000u

/* IMAGE_SCN_* section characteristics. The three memory bits and the one
 * content bit that means "this section has no bytes in the file". */
#define PE_SCN_CNT_UNINITIALIZED  0x00000080u
#define PE_SCN_MEM_EXECUTE        0x20000000u
#define PE_SCN_MEM_READ           0x40000000u
#define PE_SCN_MEM_WRITE          0x80000000u

/* Data directory indices. Only the two that matter this phase. */
#define PE_DIR_EXPORT      0
#define PE_DIR_IMPORT      1
#define PE_DIR_BASERELOC   5
#define PE_DIR_TLS         9
#define PE_DIR_COUNT      16

/* Base relocation types, from the spec's table. ABSOLUTE is the padding
 * entry a block uses to reach a 4-byte boundary and means "do nothing"; DIR64
 * is the only one an x86-64 image actually needs. */
#define PE_REL_ABSOLUTE    0
#define PE_REL_DIR64      10

typedef struct {
    uint16 e_magic;
    uint8  reserved[58];
    uint32 e_lfanew;            /* file offset of the PE signature */
} pe_dos_header_t;

typedef struct __attribute__((packed)) {
    uint16 machine;
    uint16 number_of_sections;
    uint32 time_date_stamp;
    uint32 pointer_to_symbol_table;
    uint32 number_of_symbols;
    uint16 size_of_optional_header;
    uint16 characteristics;
} pe_coff_header_t;

typedef struct __attribute__((packed)) {
    uint32 virtual_address;
    uint32 size;
} pe_data_directory_t;

/* Packed, and not for tidiness: the optional header begins at e_lfanew + 24,
 * and e_lfanew is whatever the linker felt like. Nothing guarantees ImageBase
 * lands 8-aligned, so the compiler must not assume it. */
typedef struct __attribute__((packed)) {
    uint16 magic;
    uint8  major_linker_version;
    uint8  minor_linker_version;
    uint32 size_of_code;
    uint32 size_of_initialized_data;
    uint32 size_of_uninitialized_data;
    uint32 address_of_entry_point;
    uint32 base_of_code;
    uint64 image_base;
    uint32 section_alignment;
    uint32 file_alignment;
    uint16 major_os_version;
    uint16 minor_os_version;
    uint16 major_image_version;
    uint16 minor_image_version;
    uint16 major_subsystem_version;
    uint16 minor_subsystem_version;
    uint32 win32_version_value;
    uint32 size_of_image;
    uint32 size_of_headers;
    uint32 checksum;
    uint16 subsystem;
    uint16 dll_characteristics;
    uint64 size_of_stack_reserve;
    uint64 size_of_stack_commit;
    uint64 size_of_heap_reserve;
    uint64 size_of_heap_commit;
    uint32 loader_flags;
    uint32 number_of_rva_and_sizes;
    pe_data_directory_t directory[PE_DIR_COUNT];
} pe_opt_header64_t;

typedef struct __attribute__((packed)) {
    char   name[8];
    uint32 virtual_size;
    uint32 virtual_address;      /* RVA                                     */
    uint32 size_of_raw_data;
    uint32 pointer_to_raw_data;  /* FILE offset - not the same number       */
    uint32 pointer_to_relocations;
    uint32 pointer_to_linenumbers;
    uint16 number_of_relocations;
    uint16 number_of_linenumbers;
    uint32 characteristics;
} pe_section_header_t;

/* IMAGE_EXPORT_DIRECTORY. Three parallel arrays and an ordinal base, which
 * is the shape that makes name lookup two indirections rather than one:
 * AddressOfNames[i] gives a name, AddressOfNameOrdinals[i] gives the index
 * into AddressOfFunctions for THAT name. Using i directly on
 * AddressOfFunctions works for most DLLs and silently returns the wrong
 * function for any that exports by ordinal as well as by name. */
typedef struct __attribute__((packed)) {
    uint32 characteristics;
    uint32 time_date_stamp;
    uint16 major_version;
    uint16 minor_version;
    uint32 name_rva;
    uint32 ordinal_base;
    uint32 number_of_functions;
    uint32 number_of_names;
    uint32 address_of_functions;
    uint32 address_of_names;
    uint32 address_of_name_ordinals;
} pe_export_dir_t;

/* IMAGE_IMPORT_DESCRIPTOR, one per imported DLL, terminated by an all-zero
 * entry rather than by a count. */
typedef struct __attribute__((packed)) {
    uint32 original_first_thunk;   /* the hint/name table, may be 0 */
    uint32 time_date_stamp;
    uint32 forwarder_chain;
    uint32 name;                   /* RVA of the DLL's name        */
    uint32 first_thunk;            /* the IAT: what gets written   */
} pe_import_desc_t;

/* Where the loader looks for an imported DLL. One directory, deliberately:
 * a search PATH for libraries is a security decision and a compatibility
 * decision, and neither should be made by accident this early. */
#define PE_SYSTEM_DIR "/wsr/System32/"

#define PE_MAX_MODULES 8

/* Reads a whole file for the loader. Returns 0 and a buffer the loader will
 * release through the matching free, or a negative errno. Supplied by the
 * caller so pe.c does not have to know what a filesystem is. */
typedef int (*pe_file_reader_t)(const char *path, uint8 **out, uint32 *size);
typedef void (*pe_file_release_t)(uint8 *buf);

typedef struct {
    uint64 entry;            /* absolute, after any relocation              */
    uint64 image_base;       /* where it actually landed                    */
    uint64 preferred_base;   /* what the header asked for                   */
    uint64 image_size;       /* SizeOfImage, rounded to SectionAlignment    */
    uint64 highest_vaddr;    /* image_base + image_size; where brk starts   */
    uint32 section_count;
    int    relocated;        /* non-zero if .reloc had to be applied        */
    int    has_imports;      /* non-zero if the import directory is present */

    /* ntdll!RtlUserThreadStart in the linked image, or 0 if the executable
     * did not pull in an ntdll that exports it. Where NtCreateThreadEx starts
     * every new thread - the same job PspUserThreadStartup's lookup of it
     * does on NT - found here because this is the one moment the module
     * list exists. */
    uint64 thread_start;
    /* ntdll's KiUserApcDispatcher and KiUserExceptionDispatcher, found the
     * same way: where the kernel sends a thread to run a user APC, and to
     * handle an exception. 0 when absent. */
    uint64 apc_dispatcher;
    uint64 exception_dispatcher;

    /* Implicit TLS: the executable's and every loaded DLL's TLS directory,
     * with indices assigned and written back (see teb.h). count 0: none.
     * has_tls_callbacks: some module has callbacks, so the main thread must
     * enter through ntdll (RtlUserThreadStart) to have them run. */
    int    tls_count;
    int    has_tls_callbacks;
    struct {
        uint64 module_base, start, end, zero_fill, index_addr, callbacks;
    } tls[8];
    /* Every image in the process - the executable first, then each DLL in
     * load order - for the module table published in the PEB (teb.h,
     * NT_PEB_MODULES_OFFSET), which is how ntdll finds the unwind tables
     * of the code an exception passes through. */
    int    mod_count;
    struct {
        uint64 base, size;
    } mods[PE_MAX_MODULES + 1];
} pe_info_t;

/* Cheap enough to call on every execve: reads two magic numbers and nothing
 * else. Non-zero if this looks like a PE image, which is what tells execve
 * which loader and which personality to use. */
int pe_is_pe(const void *image, uint64 size);

/* Validate the headers and fill in `info` as though the image were loaded at
 * its preferred base. Touches nothing outside the supplied buffer. */
int pe_validate(const void *image, uint64 size, pe_info_t *info);

/* Print the header summary and one line per section. Intended to be diffed
 * against `objdump -p` on the host, the same way elf_report is diffed against
 * readelf. A parse that agrees with a reference implementation is correct in
 * a way that one which merely does not crash is not. */
void pe_report(const void *image, uint64 size, uint8 color);

/* Validate, map every section into `as`, copy the raw data, zero the rest,
 * and apply base relocations if the image had to move. Writes through the
 * direct map rather than through the target addresses, so `as` need not be
 * the one in CR3 and a malformed image is still just an errno. */
int pe_load_into(address_space_t *as, const void *image, uint64 size,
                 pe_info_t *info);

/* Load an executable AND everything it imports, resolving its IAT.
 *
 * The difference from pe_load_into is the import table. An image with no
 * imports is finished when its sections are mapped; one with imports has a
 * table of addresses that are still zero, and it will jump through them.
 *
 * Dependencies are read with `reader` from PE_SYSTEM_DIR and loaded into the
 * same address space, once each. Resolution is by name against the
 * dependency's export directory.
 *
 * Returns PE_OK, or PE_ERR_IMPORT if something it needs is missing - which
 * is a load failure and not something to paper over, because a null IAT entry
 * is a jump to address zero the first time the program calls the function. */
#define PE_ERR_IMPORT   -11
int pe_load_executable(address_space_t *as, const void *image, uint64 size,
                       pe_info_t *info, pe_file_reader_t reader,
                       pe_file_release_t release);

/* Load a kernel-mode driver image (.sys): validates as a DLL-
 * characteristics, IMAGE_SUBSYSTEM_NATIVE image (PE_ERR_SUBSYSTEM if not),
 * maps its sections into `as` (normally vmm_kernel_space()) WITHOUT
 * PAGE_USER, inside the PE_DRIVER_BASE/DRIVER_IMAGE_MAX_SIZE window rather
 * than user address space, and resolves imports exactly like
 * pe_load_executable - except an import from "ntoskrnl.exe"/"hal.dll" (case-
 * insensitive) resolves against the synthetic export table in
 * kernel/ntoskrnl_exports.c instead of reading a mapped image's own export
 * directory. `info->entry` on success is the driver's DriverEntry address -
 * the caller's job (see kernel/sysload.c) is handing that to
 * IoCreateDriver (wdm.h), not this function's. */
int pe_load_driver(address_space_t *as, const void *image, uint64 size,
                   pe_info_t *info, pe_file_reader_t reader,
                   pe_file_release_t release);

const char *pe_strerror(int rc);

#endif
