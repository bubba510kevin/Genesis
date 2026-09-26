/* Format a gnfs volume image, entirely in userland.
 *
 * Calls kernel/gnfs/gnfs_format.c's own gnfs_format() - not a second,
 * from-scratch writer of the same layout - through an fwrite-based callback,
 * exactly the way tests/host/gnfs_test.c exercises the same function through
 * a dev_stub_t. One formatting implementation, called by three things
 * (this tool, the host test, and - once something in the kernel needs to
 * format a volume rather than mount an already-formatted one - the kernel
 * itself) rather than a python or shell reimplementation that could disagree
 * with the reader about what a fresh volume looks like the way
 * tests/host/fat_test.c's own README warns tools/fatfs.py's output can.
 *
 * --- why this does not #include "gnfs_layout.h" ----------------------------
 * That header pulls in kernel/include/typesk.h, which typedefs its own
 * `size_t` (over `unsigned long long`) - freestanding code has no host
 * <stddef.h> to agree with. <stdio.h>'s `size_t` and typesk.h's disagree on
 * the underlying type name even though both are 64 bits wide on this target,
 * and a translation unit cannot hold both typedefs. tests/host/run.sh solves
 * this for the whole host test harness by compiling against a scratch copy
 * of kernel/include with `size_t` renamed; a single small tool does not need
 * that scaffolding; it is simpler to declare exactly the one function this
 * file calls, with plain built-in types that are the SAME types typesk.h's
 * uint64/int64 alias (unsigned long long / signed long long, verbatim) -
 * so the declaration here and gnfs_format.c's own definition describe
 * identical types to the compiler, just spelled two different ways in two
 * translation units that never both see typesk.h.
 *
 * Usage: mkgnfs <image-path> <size-in-bytes>
 */
#include <stdio.h>
#include <stdlib.h>

/* Return type is `long long`, not `int` - it has to match gnfs_write_fn's
 * real definition (kernel/include/gnfs_layout.h: `int64`, i.e. `signed long
 * long`) bit for bit. The x86-64 SysV ABI only guarantees the low 32 bits of
 * RAX for a function returning `int`; gnfs_format.c reads all 64 on the far
 * side of this pointer, expecting the width its own header promised. */
typedef long long (*gnfs_write_fn_host)(void *ctx, unsigned long long offset,
                                        const void *buf,
                                        unsigned long long len);
extern int gnfs_format(void *ctx, gnfs_write_fn_host write,
                       unsigned long long total_bytes);

static long long file_write(void *ctx, unsigned long long offset,
                            const void *buf, unsigned long long len) {
    FILE *f = (FILE *)ctx;

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        return -5;                /* -EIO */
    }
    if (fwrite(buf, 1, (size_t)len, f) != (size_t)len) {
        return -5;
    }
    return (long long)len;
}

int main(int argc, char **argv) {
    FILE *f;
    unsigned long long size;
    int rc;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <image-path> <size-in-bytes>\n", argv[0]);
        return 2;
    }
    size = strtoull(argv[2], NULL, 0);

    f = fopen(argv[1], "w+b");
    if (f == NULL) {
        perror("fopen");
        return 1;
    }
    /* Extend the file to its full size first (a sparse hole reads as zero on
     * every filesystem worth using for this), so gnfs_format's writes into
     * the ring and bitmap regions land inside the file rather than past its
     * current end. */
    if (fseek(f, (long)(size - 1), SEEK_SET) != 0 ||
        fputc(0, f) == EOF) {
        perror("sizing image");
        fclose(f);
        return 1;
    }

    rc = gnfs_format(f, file_write, size);
    fclose(f);
    if (rc != 0) {
        fprintf(stderr, "gnfs_format failed: %d\n", rc);
        return 1;
    }

    printf("gnfs: formatted %s, %llu bytes\n", argv[1], size);
    return 0;
}
