/* Host tests for path.c.
 *
 * Pure string manipulation with no disk behind it, which is exactly why it is
 * worth pinning down here: the edge cases ("/..", "a/../..", trailing slashes,
 * a buffer one byte too small) are the kind you find interactively by typing
 * cd until something looks wrong, and then cannot reproduce.
 *
 * The rule being tested throughout: the output is always absolute, never has a
 * trailing slash except for the root, never has an empty component, and never
 * contains "." or ".." at all. */

#include <stdio.h>
#include <string.h>

#include "path.h"
#include "typesk.h"

static int path_failures;

static void expect(const char *cwd, const char *in, const char *want) {
    char out[PATH_MAX_LEN];
    int rc = path_normalize(cwd, in, out, sizeof(out));

    if (rc != PATH_OK) {
        printf("  FAIL  (%s, %s) returned %d, wanted \"%s\"\n", cwd, in, rc, want);
        path_failures++;
        return;
    }
    if (strcmp(out, want) != 0) {
        printf("  FAIL  (%s, %s) -> \"%s\", wanted \"%s\"\n", cwd, in, out, want);
        path_failures++;
        return;
    }
    printf("  ok    (%s, %s) -> %s\n", cwd, in, want);
}

static void expect_err(const char *cwd, const char *in, uint64 bufsz,
                       int want_rc, const char *what) {
    char out[PATH_MAX_LEN];
    int rc = path_normalize(cwd, in, out, bufsz);

    if (rc != want_rc) {
        printf("  FAIL  %s: returned %d, wanted %d\n", what, rc, want_rc);
        path_failures++;
    } else {
        printf("  ok    %s\n", what);
    }
}

int path_run_tests(void) {
    printf("\npath: absolute inputs ignore the cwd\n");
    expect("/usr/bin", "/bin/busybox", "/bin/busybox");
    expect("/usr/bin", "/", "/");
    /* Getting this backwards is the classic bug: "cd /bin" from /usr landing
     * in /usr/bin, which then works for a while because both exist. */
    expect("/usr", "/bin", "/bin");

    printf("\npath: relative inputs join the cwd\n");
    expect("/", "bin", "/bin");
    expect("/usr", "bin", "/usr/bin");
    expect("/usr/local", "bin/thing", "/usr/local/bin/thing");

    printf("\npath: dot and dot-dot\n");
    expect("/usr/bin", ".", "/usr/bin");
    expect("/usr/bin", "./.", "/usr/bin");
    expect("/usr/bin", "..", "/usr");
    expect("/usr/local/bin", "../..", "/usr");
    expect("/usr/bin", "../lib", "/usr/lib");
    expect("/", "..", "/");
    /* Climbing past the root is not an error anywhere else, and a shell that
     * fails `cd ../../..` from two levels down would be surprising. */
    expect("/usr", "../../../..", "/");
    expect("/usr/bin", "/a/b/../../c", "/c");
    expect("/", ".", "/");

    printf("\npath: slashes\n");
    expect("/", "//bin//busybox", "/bin/busybox");
    expect("/usr", "bin/", "/usr/bin");
    /* "///" is absolute - it begins with a slash - so it discards the cwd and
     * means the root, exactly as `cd ///` does anywhere else. The relative
     * all-slashes case cannot be written, because there isn't one. */
    expect("/usr", "///", "/");
    expect("/usr/", "bin", "/usr/bin");     /* a cwd with a stray slash */
    expect("/", "/", "/");
    expect("/bin", "", "/bin");             /* nothing to apply */

    printf("\npath: the result never keeps a trailing slash\n");
    expect("/", "bin/", "/bin");
    expect("/", "bin/./", "/bin");
    expect("/", "bin/subdir/..", "/bin");

    printf("\npath: refusals\n");
    expect_err("/", "/averyveryverylongpath/that/keeps/going/and/going/and/going"
                    "/until/it/cannot/possibly/fit/in/the/buffer/provided/here",
               32, PATH_ERR_TOOLONG, "an oversized result is refused, not truncated");
    expect_err("/", "/bin", 1, PATH_ERR_INVAL, "a buffer too small for \"/\" is invalid");
    expect_err(NULL, "/bin", PATH_MAX_LEN, PATH_ERR_INVAL, "a null cwd is invalid");
    expect_err("/", NULL, PATH_MAX_LEN, PATH_ERR_INVAL, "a null input is invalid");

    printf("\npath: component length is left to the filesystem\n");
    /* 8.3 is a FAT rule, not a path-syntax rule. Rejecting it here as well
     * would mean editing two files the day anything else is mounted. */
    expect("/", "areallylongname.text", "/areallylongname.text");

    return path_failures;
}
