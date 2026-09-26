/* Host tests for the TEB, the PEB and RTL_USER_PROCESS_PARAMETERS.
 *
 * Everything here is a structure a program reads without asking, which is
 * exactly the class of thing that fails quietly on target. A wrong offset, a
 * Length that includes its terminator, a Buffer holding a kernel address
 * instead of a user one - none of those crash. They produce a program that
 * prints half a string, or sees an empty command line, or writes to a handle
 * that is not the one the shell gave it, and the only evidence is the wrong
 * output.
 *
 * The offsets themselves are asserted at build time in ntproc.c and are not
 * re-checked here; a test can only agree with the same mistake. What this
 * checks is the part a build-time assertion cannot see: the bytes actually
 * written, read back the way the program will read them.
 *
 * Note what every check below does with the pointers in the block. They are
 * USER virtual addresses in the space being built, so the test resolves each
 * one through the page tables rather than dereferencing it - which is also
 * the difference the code under test has to get right, and would be invisible
 * if the harness simply followed the pointer. */

#include <stdio.h>
#include <string.h>

#include "nt.h"
#include "paging.h"
#include "pmm.h"
#include "teb.h"

static int nt_failures;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        nt_failures++;
    }
}

/* Read `bytes` of UTF-16 from a user address in `as`, as ASCII, the way the
 * program would see it. Page-by-page, because a string is free to cross a
 * page and the arena that wrote it had to handle that too. */
static void read_wstr(address_space_t *as, uint64 va, uint64 bytes,
                      char *out, size_t out_cap) {
    size_t n = 0;
    uint64 i;

    for (i = 0; i + 1 < bytes && n + 1 < out_cap; i += 2) {
        phys_addr_t phys = vmm_get_phys_in(as, (va + i) & ~0xFFFULL);
        const uint8 *p;
        uint16 w;

        if (phys == 0) {
            break;
        }
        p = (const uint8 *)phys_to_virt(phys) + ((va + i) & 0xFFF);
        /* The low byte only: everything here is ASCII, and reading the pair
         * as a 16-bit value would depend on the string not straddling the
         * page boundary between them. */
        w = (uint16)p[0];
        if (((va + i + 1) & 0xFFF) == 0) {
            phys = vmm_get_phys_in(as, (va + i + 1) & ~0xFFFULL);
            if (phys == 0) {
                break;
            }
            p = (const uint8 *)phys_to_virt(phys);
            w |= (uint16)p[0] << 8;
        } else {
            w |= (uint16)p[1] << 8;
        }
        out[n++] = (char)(w & 0xFF);
    }
    out[n] = '\0';
}

static uint16 read_u16(address_space_t *as, uint64 va) {
    phys_addr_t phys = vmm_get_phys_in(as, va & ~0xFFFULL);
    const uint8 *p;

    if (phys == 0) {
        return 0xFFFF;
    }
    p = (const uint8 *)phys_to_virt(phys) + (va & 0xFFF);
    return (uint16)(p[0] | ((uint16)p[1] << 8));
}

static void *user_ptr(address_space_t *as, uint64 va) {
    phys_addr_t phys = vmm_get_phys_in(as, va & ~0xFFFULL);

    if (phys == 0) {
        return NULL;
    }
    return (void *)(phys_to_virt(phys) + (va & 0xFFF));
}

/* --- the tests ---------------------------------------------------------- */

static const char *argv_simple[] = { "hello.exe", "one", "two", NULL };
static const char *argv_spaces[] = { "hello.exe", "a b", "c", NULL };
static const char *envp_two[]    = { "PATH=/bin", "HOME=/", NULL };

static void fill(nt_params_desc_t *d) {
    d->image_path = "/bin/hello.exe";
    d->cwd        = "/";
    d->argv       = argv_simple;
    d->envp       = envp_two;
    d->std_input  = NT_HANDLE_FROM_INDEX(0);
    d->std_output = NT_HANDLE_FROM_INDEX(1);
    d->std_error  = NT_HANDLE_FROM_INDEX(2);
}

/* Every build gets a FRESH address space, because nt_process_init maps the
 * TEB, the PEB and the block, and mapping over something already there is
 * -ENOMEM by design - the check that catches a second exec writing over a
 * live process's own structures. Reusing a space here would test that
 * refusal over and over instead of the thing under test. */
static nt_rtl_user_process_params_t *build(address_space_t **as_out,
                                           nt_params_desc_t *d, int *rc_out) {
    address_space_t *as = vmm_space_create();
    int rc;

    *as_out = as;
    if (as == NULL) {
        *rc_out = -12;
        return NULL;
    }
    rc = nt_process_init(as, 0x140000000ULL, 0x40000000ULL, 0x100000ULL, 7, 7);
    if (rc != 0) {
        *rc_out = rc;
        return NULL;
    }
    rc = nt_process_params_init(as, d);
    *rc_out = rc;
    if (rc != 0) {
        return NULL;
    }
    return (nt_rtl_user_process_params_t *)user_ptr(as, NT_PARAMS_BASE);
}

static void test_block_is_reachable(void) {
    nt_params_desc_t d;
    nt_rtl_user_process_params_t *pp;
    nt_peb_t *peb;
    address_space_t *as;
    int rc;

    printf("\nntproc: the PEB points at a block the program can reach\n");

    fill(&d);
    pp = build(&as, &d, &rc);
    check(rc == 0, "the parameters block was built");
    check(pp != NULL, "and is mapped in the process's own space");
    if (pp == NULL) {
        return;
    }

    peb = (nt_peb_t *)user_ptr(as, NT_PEB_BASE);
    check(peb != NULL && peb->process_parameters == NT_PARAMS_BASE,
          "PEB->ProcessParameters points at it - the one link that matters");
    check(pp->maximum_length == NT_PARAMS_PAGES * 0x1000,
          "MaximumLength is what was actually allocated");
    check(pp->length > 0 && pp->length <= pp->maximum_length,
          "and Length is inside it");
}

static void test_standard_handles(void) {
    nt_params_desc_t d;
    nt_rtl_user_process_params_t *pp;
    address_space_t *as;
    int rc;

    printf("\nntproc: the three GetStdHandle answers\n");

    fill(&d);
    pp = build(&as, &d, &rc);
    if (pp == NULL) {
        check(0, "the block built");
        return;
    }
    check(pp->standard_input  == NT_HANDLE_FROM_INDEX(0) &&
          pp->standard_output == NT_HANDLE_FROM_INDEX(1) &&
          pp->standard_error  == NT_HANDLE_FROM_INDEX(2),
          "are the inherited descriptors 0, 1 and 2, NT-encoded");
    check(pp->standard_input != 0 && pp->standard_output != 0,
          "and none of them is the never-valid zero");

    /* A descriptor closed before execve must produce zero rather than an
     * encoding of a table slot that is not open. */
    fill(&d);
    d.std_input = 0;
    pp = build(&as, &d, &rc);
    if (pp == NULL) {
        check(0, "the block built with stdin closed");
        return;
    }
    check(pp->standard_input == 0,
          "a closed descriptor leaves zero, not an encoding of index 0");
    check(pp->standard_output == NT_HANDLE_FROM_INDEX(1),
          "without disturbing the other two");
}

static void test_paths_and_command_line(void) {
    nt_params_desc_t d;
    nt_rtl_user_process_params_t *pp;
    address_space_t *as;
    char buf[512];
    int rc;

    printf("\nntproc: DOS paths, the command line, and the environment\n");

    fill(&d);
    pp = build(&as, &d, &rc);
    if (pp == NULL) {
        check(0, "the block built");
        return;
    }

    read_wstr(as, pp->image_path_name.buffer, pp->image_path_name.length,
              buf, sizeof(buf));
    check(strcmp(buf, "C:\\bin\\hello.exe") == 0,
          "ImagePathName is the POSIX path in DOS spelling");
    check(pp->image_path_name.length == 16 * 2,
          "with Length in BYTES, not characters");
    check(pp->image_path_name.maximum_length ==
          pp->image_path_name.length + 2,
          "and MaximumLength counting the terminator Length excludes");
    check(read_u16(as, pp->image_path_name.buffer +
                       pp->image_path_name.length) == 0,
          "which is really there - Buffer is null-terminated");

    read_wstr(as, pp->current_directory_path.buffer,
              pp->current_directory_path.length, buf, sizeof(buf));
    check(strcmp(buf, "C:\\") == 0,
          "CurrentDirectory of \"/\" is C:\\ - already ends in a separator");

    read_wstr(as, pp->command_line.buffer, pp->command_line.length,
              buf, sizeof(buf));
    check(strcmp(buf, "hello.exe one two") == 0,
          "CommandLine joins argv INCLUDING argv[0], as Windows does");

    /* The environment is not a UNICODE_STRING - it is a bare pointer to a
     * run of null-terminated strings ended by a second null. Read the first
     * entry, then check the double null is where it should be. */
    read_wstr(as, pp->environment, 9 * 2, buf, sizeof(buf));
    check(strcmp(buf, "PATH=/bin") == 0, "Environment starts with the first entry");
    check(read_u16(as, pp->environment + 9 * 2) == 0,
          "each entry is null-terminated");
    check(read_u16(as, pp->environment + (9 + 1 + 7) * 2) == 0 &&
          read_u16(as, pp->environment + (9 + 1 + 7 + 1) * 2) == 0,
          "and the block ends with the double null a program scans for");
}

static void test_cwd_and_quoting(void) {
    nt_params_desc_t d;
    nt_rtl_user_process_params_t *pp;
    address_space_t *as;
    char buf[512];
    int rc;

    printf("\nntproc: a cwd that is not the root, and an argument with a space\n");

    fill(&d);
    d.cwd  = "/usr/bin";
    d.argv = argv_spaces;
    pp = build(&as, &d, &rc);
    if (pp == NULL) {
        check(0, "the block built");
        return;
    }

    read_wstr(as, pp->current_directory_path.buffer,
              pp->current_directory_path.length, buf, sizeof(buf));
    check(strcmp(buf, "C:\\usr\\bin\\") == 0,
          "CurrentDirectory gains the trailing separator NT puts there");

    read_wstr(as, pp->command_line.buffer, pp->command_line.length,
              buf, sizeof(buf));
    check(strcmp(buf, "hello.exe \"a b\" c") == 0,
          "an argument containing a space is quoted, so it re-splits as one");
}

static void test_empty_and_overflow(void) {
    nt_params_desc_t d;
    nt_rtl_user_process_params_t *pp;
    address_space_t *as;
    static const char *big[64];
    static char        blob[4096];
    char buf[64];
    int rc, i;

    printf("\nntproc: nothing to say, and too much to say\n");

    fill(&d);
    d.argv = NULL;
    d.envp = NULL;
    pp = build(&as, &d, &rc);
    if (pp == NULL) {
        check(0, "the block built with no argv and no envp");
        return;
    }
    check(pp->command_line.length == 0, "an absent argv is an empty command line");
    read_wstr(as, pp->command_line.buffer, 2, buf, sizeof(buf));
    check(buf[0] == '\0', "still null-terminated, so a program can read it");
    check(pp->environment != 0 &&
          read_u16(as, pp->environment) == 0 &&
          read_u16(as, pp->environment + 2) == 0,
          "and an absent envp is the double null, not a null pointer");

    /* More than two pages of strings. The failure has to be reported, not
     * truncated: a command line cut in half runs a different command. */
    for (i = 0; i < 4095; i++) {
        blob[i] = 'x';
    }
    blob[4095] = '\0';
    for (i = 0; i < 8; i++) {
        big[i] = blob;
    }
    big[8] = NULL;

    vmm_space_destroy(as);

    fill(&d);
    d.argv = big;
    pp = build(&as, &d, &rc);
    check(rc == -7, "a command line too big for the block is -E2BIG");
    check(pp == NULL, "and no block is handed back");

    {
        nt_peb_t *peb = (nt_peb_t *)user_ptr(as, NT_PEB_BASE);
        check(peb != NULL && peb->process_parameters == 0,
              "the PEB is left null - no half-built block to follow");
    }
}

int ntproc_run_tests(void) {
    nt_failures = 0;

    test_block_is_reachable();
    test_standard_handles();
    test_paths_and_command_line();
    test_cwd_and_quoting();
    test_empty_and_overflow();

    return nt_failures;
}
