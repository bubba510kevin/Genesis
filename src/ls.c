/* ls - list directory contents.
 *
 * --- Why this is an ordinary C program now --------------------------------
 * It used to be freestanding: its own syscall stubs, its own string
 * functions, its own qsort, 418 lines of which maybe 120 were about listing a
 * directory. That was the right call when there was no libc on the volume.
 * There is one now - musl builds and runs - so the freestanding version was
 * carrying a libc that already existed, badly.
 *
 * What went with it is more interesting than the line count. Every one of
 * those hand-written pieces was an assumption about the kernel that nothing
 * checked: that getdents64 records are 8-aligned, that d_reclen includes the
 * terminator, that stat's st_mode bits are in the Linux positions. Written by
 * hand they agreed with the kernel because the same person wrote both. Going
 * through musl means they agree with the kernel or the program breaks, which
 * is the only version of that check worth having.
 *
 * This is also what makes ls the natural first dynamic binary. It is small,
 * its output is verifiable by eye, and it touches opendir/readdir/stat/printf
 * - four different corners of libc, so a broken relocation shows up as wrong
 * output rather than as nothing happening.
 *
 * --- What did NOT move ----------------------------------------------------
 * systest stays freestanding, deliberately. It tests the syscall interface,
 * and a test that reaches the kernel through libc cannot tell a kernel bug
 * from a libc workaround - musl retries, translates errnos and emulates
 * missing calls, all of which is exactly what a syscall test must not do.
 * Two programs, two reasons, and they do not converge.
 *
 * Build: tools/build_user.sh
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int opt_long;      /* -l */
static int opt_all;       /* -a */
static int opt_one;       /* -1 */

#define MAX_ENTRIES 512

typedef struct {
    char name[256];
} entry_t;

/* Sorting is not decoration.
 *
 * FAT hands entries back in on-disk order, which is creation order with
 * deleted slots reused - so an unsorted listing of one directory changes
 * between runs for no visible reason. That makes ls useless for the thing it
 * is mostly used for here, which is checking whether a file you just staged
 * is where you think it is. */
static int by_name(const void *a, const void *b) {
    return strcmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

static void mode_string(mode_t m, char *out) {
    /* S_ISDIR and friends rather than a hand-written mask on st_mode.
     *
     * The freestanding version tested (mode & 0xF000) == 0x4000 with the
     * constants written out, which is correct on Linux and is a claim about
     * the kernel's ABI that nothing verified. Going through the macros means
     * the header and the kernel agree, or nothing works - which is the check
     * that was missing. */
    out[0] = S_ISDIR(m)  ? 'd'
           : S_ISLNK(m)  ? 'l'
           : S_ISBLK(m)  ? 'b'
           : S_ISCHR(m)  ? 'c'
           : S_ISFIFO(m) ? 'p'
           :               '-';
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    out[3] = (m & S_IXUSR) ? 'x' : '-';
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_IXGRP) ? 'x' : '-';
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    out[9] = (m & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void print_long(const char *dir, const char *name) {
    char path[1024];
    struct stat st;
    char perms[11];

    if (strcmp(dir, ".") == 0) {
        snprintf(path, sizeof(path), "%s", name);
    } else {
        snprintf(path, sizeof(path), "%s/%s", dir, name);
    }

    if (stat(path, &st) != 0) {
        /* A name that a directory listing produced and stat cannot resolve is
         * worth reporting rather than skipping. On this kernel it usually
         * means the merged /dev view listed something whose object has gone -
         * which is exactly the surprise-removal case, and silently dropping
         * the row would hide it. */
        printf("?????????  %10s %s\n", "?", name);
        return;
    }

    mode_string(st.st_mode, perms);
    printf("%s %10lld %s\n", perms, (long long)st.st_size, name);
}

static int list_dir(const char *path, int show_header, int multiple) {
    DIR *d;
    struct dirent *de;
    entry_t *entries;
    size_t count = 0;
    size_t i;

    d = opendir(path);
    if (d == NULL) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    entries = calloc(MAX_ENTRIES, sizeof(entry_t));
    if (entries == NULL) {
        fprintf(stderr, "ls: out of memory\n");
        closedir(d);
        return 1;
    }

    while ((de = readdir(d)) != NULL) {
        if (!opt_all && de->d_name[0] == '.') {
            continue;
        }
        if (count >= MAX_ENTRIES) {
            /* Said out loud rather than truncated silently. A listing that is
             * quietly short is worse than no listing: it answers "is my file
             * there" with a confident no. */
            fprintf(stderr, "ls: %s: more than %d entries, listing truncated\n",
                    path, MAX_ENTRIES);
            break;
        }
        snprintf(entries[count].name, sizeof(entries[count].name),
                 "%s", de->d_name);
        count++;
    }
    closedir(d);

    qsort(entries, count, sizeof(entry_t), by_name);

    if (show_header && multiple) {
        printf("%s:\n", path);
    }
    for (i = 0; i < count; i++) {
        if (opt_long) {
            print_long(path, entries[i].name);
        } else if (opt_one) {
            printf("%s\n", entries[i].name);
        } else {
            printf("%s%s", entries[i].name, (i + 1 < count) ? "  " : "");
        }
    }
    if (!opt_long && !opt_one && count > 0) {
        printf("\n");
    }

    free(entries);
    return 0;
}

int main(int argc, char **argv) {
    const char *paths[64];
    int npaths = 0;
    int status = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            const char *f;
            for (f = argv[i] + 1; *f; f++) {
                switch (*f) {
                    case 'l': opt_long = 1; break;
                    case 'a': opt_all  = 1; break;
                    case '1': opt_one  = 1; break;
                    default:
                        fprintf(stderr, "ls: unknown option -%c\n", *f);
                        return 2;
                }
            }
        } else if (npaths < (int)(sizeof(paths) / sizeof(paths[0]))) {
            paths[npaths++] = argv[i];
        }
    }

    if (npaths == 0) {
        paths[npaths++] = ".";
    }

    for (i = 0; i < npaths; i++) {
        struct stat st;

        /* A plain FILE argument prints as one row rather than being opened as
         * a directory. `ls /bin/busybox` should say something about that file,
         * and opendir on it returns -ENOTDIR, which the old version reported
         * as an error. */
        if (stat(paths[i], &st) == 0 && !S_ISDIR(st.st_mode)) {
            if (opt_long) {
                print_long(".", paths[i]);
            } else {
                printf("%s\n", paths[i]);
            }
            continue;
        }
        if (i > 0 && npaths > 1) {
            printf("\n");
        }
        status |= list_dir(paths[i], 1, npaths > 1);
    }
    return status;
}
