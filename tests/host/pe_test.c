/* Host tests for pe.c's header parsing.
 *
 * The mapping stage needs an address space and is exercised on target; the
 * PARSE does not, and the parse is where a PE loader goes wrong in ways that
 * are miserable to debug from a QEMU window. Specifically:
 *
 *   - PointerToRawData and VirtualAddress are different numbers. A loader
 *     that conflates them works while FileAlignment equals SectionAlignment
 *     and silently loads garbage otherwise, which is the single most common
 *     way to get this wrong. The fixture below deliberately sets them 0x200
 *     and 0x1000 apart.
 *   - every offset in the file is a number read out of the file, so every one
 *     of them is a bounds check that has to happen before a pointer is
 *     computed rather than after.
 *   - PE32 and PE32+ differ by four bytes at ImageBase, and every field after
 *     it shifts. Read one as the other and you get plausible nonsense.
 *
 * The fixture is built here rather than checked in as a binary so a failure
 * points at a line you can read, and so each test can corrupt exactly one
 * field and assert on the specific error that produces.
 */

#include <stdio.h>
#include <string.h>

#include "pe.h"
#include "typesk.h"

static int pe_failures;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        pe_failures++;
    }
}

static void check_eq(long got, long want, const char *what) {
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s  (got %ld, wanted %ld)\n", what, got, want);
        pe_failures++;
    }
}

/* The same layout src/mkpe.py emits: headers at 0x80, .text raw 0x200 /
 * RVA 0x1000, .rdata raw 0x400 / RVA 0x2000. */
#define IMG_SIZE      0x800
#define LFANEW        0x80
#define OPT_OFF       (LFANEW + 4 + 20)
#define SEC_OFF       (OPT_OFF + 240)
#define TEXT_RVA      0x1000
#define RDATA_RVA     0x2000
#define IMAGE_BASE    0x140000000ULL

static uint8 img[IMG_SIZE];

static void put16(uint64 off, uint16 v) { img[off] = (uint8)v; img[off+1] = (uint8)(v>>8); }
static void put32(uint64 off, uint32 v) { int i; for (i=0;i<4;i++) img[off+i] = (uint8)(v>>(8*i)); }
static void put64(uint64 off, uint64 v) { int i; for (i=0;i<8;i++) img[off+i] = (uint8)(v>>(8*i)); }

static void build_fixture(void) {
    uint64 s;

    memset(img, 0, sizeof(img));

    put16(0, 0x5A4D);                       /* MZ                          */
    put32(0x3C, LFANEW);
    put32(LFANEW, 0x00004550);              /* PE\0\0                      */

    /* COFF */
    put16(LFANEW + 4 + 0,  0x8664);         /* Machine: AMD64              */
    put16(LFANEW + 4 + 2,  2);              /* NumberOfSections            */
    put16(LFANEW + 4 + 16, 240);            /* SizeOfOptionalHeader        */
    put16(LFANEW + 4 + 18, 0x0022);         /* EXECUTABLE_IMAGE            */

    /* PE32+ optional header */
    put16(OPT_OFF + 0,  0x020B);            /* Magic: PE32+                */
    put32(OPT_OFF + 4,  0x20);              /* SizeOfCode                  */
    put32(OPT_OFF + 16, TEXT_RVA);          /* AddressOfEntryPoint         */
    put32(OPT_OFF + 20, TEXT_RVA);          /* BaseOfCode                  */
    put64(OPT_OFF + 24, IMAGE_BASE);        /* ImageBase                   */
    put32(OPT_OFF + 32, 0x1000);            /* SectionAlignment            */
    put32(OPT_OFF + 36, 0x200);             /* FileAlignment               */
    put32(OPT_OFF + 56, 0x3000);            /* SizeOfImage                 */
    put32(OPT_OFF + 60, 0x200);             /* SizeOfHeaders               */
    put16(OPT_OFF + 68, 3);                 /* Subsystem: CUI              */
    put32(OPT_OFF + 108, 16);               /* NumberOfRvaAndSizes         */

    /* Section table: name, vsize, rva, rawsize, rawptr, ..., chars */
    s = SEC_OFF;
    memcpy(&img[s], ".text", 5);
    put32(s + 8,  0x20);                    /* VirtualSize                 */
    put32(s + 12, TEXT_RVA);                /* VirtualAddress              */
    put32(s + 16, 0x200);                   /* SizeOfRawData               */
    put32(s + 20, 0x200);                   /* PointerToRawData            */
    put32(s + 36, 0x60000020u);             /* CODE | EXECUTE | READ       */

    s = SEC_OFF + 40;
    memcpy(&img[s], ".rdata", 6);
    put32(s + 8,  0x56);
    put32(s + 12, RDATA_RVA);
    put32(s + 16, 0x200);
    put32(s + 20, 0x400);
    put32(s + 36, 0x40000040u);             /* INITIALIZED | READ          */
}

static void test_valid(void) {
    pe_info_t info;

    printf("\npe: a well-formed PE32+ image\n");
    build_fixture();

    check(pe_is_pe(img, sizeof(img)), "pe_is_pe recognises it");
    check_eq(pe_validate(img, sizeof(img), &info), PE_OK, "and it validates");
    check_eq((long)info.preferred_base, (long)IMAGE_BASE, "ImageBase is read whole");
    check_eq((long)info.entry, (long)(IMAGE_BASE + TEXT_RVA),
             "entry is ImageBase + AddressOfEntryPoint, not the RVA alone");
    check_eq((long)info.image_size, 0x3000, "SizeOfImage is read");
    check_eq(info.section_count, 2, "both sections are counted");
    check_eq(info.has_imports, 0, "and it reports no imports");

    /* The number that matters most. The image's preferred base has to fit
     * below the user-space limit, or relocation stops being optional. */
    check(info.preferred_base + info.image_size < PE_USER_LIMIT,
          "the usual 0x140000000 fits in user space - no .reloc needed to run");
}

static void test_not_a_pe(void) {
    pe_info_t info;

    printf("\npe: things that are not a PE\n");

    build_fixture();
    put16(0, 0x457F);                       /* ELF magic instead of MZ     */
    check(!pe_is_pe(img, sizeof(img)), "an ELF is not mistaken for a PE");
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_MAGIC, "and is -MAGIC");

    build_fixture();
    put32(LFANEW, 0);                       /* MZ, but no PE signature     */
    check(!pe_is_pe(img, sizeof(img)),
          "a DOS stub with no PE header is not a PE");

    /* e_lfanew is a file offset read out of the file. Pointing it past the
     * end must be caught before it is added to a pointer. */
    build_fixture();
    put32(0x3C, 0x7FFFFFFF);
    check(!pe_is_pe(img, sizeof(img)), "an e_lfanew past the end is rejected");
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_MAGIC,
             "rather than dereferenced");

    check(!pe_is_pe(img, 4), "and a file too short to hold a DOS header");
    check(!pe_is_pe(NULL, 100), "and a null image");
}

static void test_wrong_shape(void) {
    pe_info_t info;

    printf("\npe: PE images this loader will not run\n");

    build_fixture();
    put16(LFANEW + 4, 0x014C);              /* i386                        */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_MACHINE,
             "a 32-bit x86 image is -MACHINE");

    build_fixture();
    put16(OPT_OFF, 0x010B);                 /* PE32                        */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_OPTMAGIC,
             "PE32 is refused, not read as PE32+ with every field shifted");

    build_fixture();
    put16(LFANEW + 4 + 18, 0x2022);         /* DLL bit set                 */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_NOTEXE,
             "a DLL is not something to execve");

    build_fixture();
    put16(LFANEW + 4 + 18, 0x0000);         /* EXECUTABLE_IMAGE cleared    */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_NOTEXE,
             "and neither is an object file");

    build_fixture();
    put32(OPT_OFF + 32, 0x200);             /* SectionAlignment < a page   */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_ALIGN,
             "sub-page SectionAlignment is refused - permissions are per page");

    build_fixture();
    put32(OPT_OFF + 16, 0);                 /* AddressOfEntryPoint = 0     */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_NOTEXE,
             "an entry point of zero would mean jumping at the DOS header");

    build_fixture();
    put32(OPT_OFF + 16, 0x9000);            /* entry outside the image     */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_NOTEXE,
             "and an entry point past SizeOfImage is refused");
}

static void test_bounds(void) {
    pe_info_t info;

    printf("\npe: every offset in the file is a bounds check\n");

    build_fixture();
    put32(SEC_OFF + 20, 0x7FFF0000u);       /* PointerToRawData past EOF   */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_SECTION,
             "a section whose raw data lies outside the file is -SECTION");

    build_fixture();
    put32(SEC_OFF + 16, 0x7FFF0000u);       /* SizeOfRawData past EOF      */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_SECTION,
             "and so is one whose raw data runs off the end");

    build_fixture();
    put32(SEC_OFF + 12, 0x7FFF0000u);       /* VirtualAddress past image   */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_SECTION,
             "and one mapped outside SizeOfImage");

    build_fixture();
    put16(LFANEW + 4 + 2, 1000);            /* more sections than fit      */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_TRUNCATED,
             "a section count the file cannot hold is -TRUNCATED");

    build_fixture();
    put32(OPT_OFF + 60, 0x7FFF0000u);       /* SizeOfHeaders past EOF      */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_TRUNCATED,
             "and so is a SizeOfHeaders larger than the file");

    /* SizeOfOptionalHeader is what the section table's position is computed
     * from. Trusting sizeof() instead is how a section table gets read as
     * whatever followed a longer optional header. */
    build_fixture();
    put16(LFANEW + 4 + 16, 20);             /* absurdly short              */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_TRUNCATED,
             "an optional header shorter than PE32+ requires is -TRUNCATED");
}

/* A section whose raw size exceeds its virtual size is normal - it is
 * FileAlignment padding - and copying all of it would spill into whatever is
 * mapped next. The parse has to survive it; the load caps it. */
static void test_raw_larger_than_virtual(void) {
    pe_info_t info;

    printf("\npe: FileAlignment padding is not section content\n");
    build_fixture();
    put32(SEC_OFF + 8,  0x20);              /* VirtualSize  0x20           */
    put32(SEC_OFF + 16, 0x200);             /* SizeOfRawData 0x200         */
    check_eq(pe_validate(img, sizeof(img), &info), PE_OK,
             "a raw size larger than the virtual size still validates");
    check_eq((long)info.entry, (long)(IMAGE_BASE + TEXT_RVA),
             "and the entry point is unaffected");
}

/* The failure src/mkpe.py actually produced.
 *
 * Its raw file offsets were hardcoded with a 0x200 gap for .text. When the
 * Win64 convention gave every call site a stack frame, .text grew past that
 * gap - and the generator's layout loop padded with `b"\x00" * (offset -
 * len(out))`, which for a negative count is an empty string rather than an
 * error. The header then described a .rdata beginning inside .text, this
 * loader mapped precisely that, and the image read its own string constants
 * as machine code. The visible result was STATUS_ACCESS_VIOLATION from a
 * pointer check in nt.c, several layers from the cause.
 *
 * Both ends now speak: the generator asserts, and the loader refuses. */
static void test_overlapping_raw_data(void) {
    pe_info_t info;

    printf("\npe: a section table that describes overlapping raw data\n");

    build_fixture();
    check_eq(pe_validate(img, sizeof(img), &info), PE_OK,
             "the fixture's two sections do not overlap");

    /* .text is 0x200 long at 0x200, so it ends exactly where .rdata begins.
     * Adjacent is not overlapping - the check must not reject this. */
    build_fixture();
    put32(SEC_OFF + 16, 0x200);
    check_eq(pe_validate(img, sizeof(img), &info), PE_OK,
             "and sections that merely abut are fine");

    /* Now .text runs one FileAlignment unit past .rdata's start, which is
     * exactly the shape mkpe.py emitted. */
    build_fixture();
    put32(SEC_OFF + 16, 0x400);             /* .text SizeOfRawData 0x400   */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_SECTION,
             "a .text overrunning .rdata's raw offset is -SECTION");

    /* And the same in the other direction: a later section that starts
     * inside an earlier one. Order in the table is not guaranteed, so the
     * check has to be pairwise rather than a high-water mark. */
    build_fixture();
    put32(SEC_OFF + 40 + 20, 0x300);        /* .rdata raw ptr inside .text */
    check_eq(pe_validate(img, sizeof(img), &info), PE_ERR_SECTION,
             "and so is a section starting inside an earlier one");

    /* A section with no raw data has no raw range to overlap, and .bss-like
     * sections legitimately leave PointerToRawData meaningless. */
    build_fixture();
    put32(SEC_OFF + 40 + 16, 0);            /* .rdata SizeOfRawData 0      */
    put32(SEC_OFF + 40 + 20, 0x200);        /* pointer inside .text        */
    check_eq(pe_validate(img, sizeof(img), &info), PE_OK,
             "a section with no bytes in the file is exempt");
}

int pe_run_tests(void) {
    pe_failures = 0;
    test_valid();
    test_not_a_pe();
    test_wrong_shape();
    test_bounds();
    test_raw_larger_than_virtual();
    test_overlapping_raw_data();
    return pe_failures;
}
