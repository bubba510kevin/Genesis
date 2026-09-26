/* Genesis's implementation of the libsa surface the vendored ZFS reader is
 * written against. NOT vendored code.
 *
 * --- Why this file is the whole port -------------------------------------
 * kernel/zfs/vendor/ is FreeBSD's standalone ZFS reader, unmodified. It was
 * written for a boot loader, which means it already assumes what Genesis can
 * offer and nothing more: no threads, no locks, no interrupts, no sleeping,
 * one caller at a time, and I/O through a function pointer. Everything it
 * needs from its environment is malloc, free, printf and a handful of string
 * and memory routines - which is this file.
 *
 * That is the reason this vendoring is tractable at all when vendoring
 * module/zfs is not. The kernel-side OpenZFS wants taskqs, condition
 * variables, kmem caches, an ARC and a zio pipeline; the loader-side reader
 * wants malloc.
 *
 * printf is the interesting one. The vendored code prints diagnostics with
 * %s, %d, %llu and %llx, so this is a real (small) formatter rather than a
 * stub - a reader that refuses a pool and cannot say which feature it
 * refused would be useless in exactly the case it exists for. */

/* This file is on the VENDORED side of the wall: it is compiled against
 * kernel/zfs/compat/, not against typesk.h. So it does not include kheap.h or
 * screen.h - typesk.h typedefs size_t to `unsigned long long` and the compat
 * world says `unsigned long`, and one translation unit cannot hold both.
 *
 * The four Genesis functions it needs are declared here instead, in types
 * that mean the same thing on both sides. Same arrangement as zfs_genesis.h
 * going the other way, for the same reason. */
void *kmalloc(unsigned long long size);
void *kcalloc(unsigned long long count, unsigned long long size);
void *krealloc(void *ptr, unsigned long long size);
void  kfree(void *ptr);
void  print_string(const char *str, unsigned char color);
void  print_char(char c, unsigned char color);

/* This file does NOT include zfs_libsa.h: it defines the functions that
 * header renames things to, and including it would rename the definitions
 * too. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* The formatter measures into a buffer whose size is a byte count. Spelled
 * out rather than size_t so that this file, which sits on the vendored side
 * of the wall, does not depend on which stddef.h won. */
typedef unsigned long long uint64_sz;

void *zfs_libsa_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    size_t i;

    /* Overlap-safe, because the header maps memmove here too: the vendored
     * nvlist code moves overlapping ranges when it packs a list down. */
    if (d < s || d >= s + n) {
        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

void *zfs_libsa_memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

int zfs_libsa_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    size_t i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

size_t zfs_libsa_strlen(const char *s) {
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int zfs_libsa_strcmp(const char *a, const char *b) {
    size_t i;

    for (i = 0; a[i] != '\0' && a[i] == b[i]; i++) {
    }
    if (a[i] == b[i]) {
        return 0;
    }
    return (unsigned char)a[i] < (unsigned char)b[i] ? -1 : 1;
}

int zfs_libsa_strncmp(const char *a, const char *b, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (unsigned char)a[i] < (unsigned char)b[i] ? -1 : 1;
        }
        if (a[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

char *zfs_libsa_strcpy(char *dst, const char *src) {
    size_t i;

    for (i = 0; src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return dst;
}

size_t zfs_libsa_strlcpy(char *dst, const char *src, size_t cap) {
    size_t len = zfs_libsa_strlen(src);
    size_t i;

    /* Returns the length it WANTED to write, which is what the callers in
     * nvlist.c test against the buffer size. Returning the length written
     * would make a truncation look like a success. */
    if (cap != 0) {
        for (i = 0; i + 1 < cap && src[i] != '\0'; i++) {
            dst[i] = src[i];
        }
        dst[i] = '\0';
    }
    return len;
}

char *zfs_libsa_strchr(const char *s, int c) {
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == (char)c) {
            return (char *)(unsigned long)&s[i];
        }
    }
    return (c == '\0') ? (char *)(unsigned long)&s[i] : NULL;
}

/* --- allocation ---------------------------------------------------------- */

void *zfs_libsa_malloc(size_t n) {
    return kmalloc((unsigned long long)n);
}

void *zfs_libsa_calloc(size_t n, size_t size) {
    return kcalloc((unsigned long long)n, (unsigned long long)size);
}

/* nvlist.c grows its buffer with realloc. Worth naming what its absence did:
 * with no declaration, C89 rules made the call implicitly int-returning, so
 * the returned pointer was TRUNCATED TO 32 BITS and assigned to a uint8_t *.
 * On a kernel whose heap lives at 0xFFFFFFFF90000000 that is a pointer to
 * nothing, and the build said so only as a warning. Compiling the vendored
 * code with -Wall is what caught it. */
void *zfs_libsa_realloc(void *p, size_t n) {
    return krealloc(p, (unsigned long long)n);
}

void zfs_libsa_free(void *p) {
    kfree(p);
}

char *zfs_libsa_strdup(const char *s) {
    size_t n = zfs_libsa_strlen(s) + 1;
    char *out = (char *)zfs_libsa_malloc(n);

    if (out != NULL) {
        zfs_libsa_memcpy(out, s, n);
    }
    return out;
}

/* --- printf --------------------------------------------------------------
 *
 * %s %c %d %i %u %x %p, with ll/l/z length modifiers, a field width and the
 * zero flag. That is what the vendored files use; anything else prints the
 * conversion verbatim rather than guessing, so an unhandled format shows up
 * as a strange message instead of as a walk off the end of the argument
 * list. */

#define ZFS_PRINT_COLOR 0x0E

/* One formatter, two front ends.
 *
 * printf goes to the screen; vsnprintf goes to a buffer, and asprintf
 * allocates one. All three exist because the vendored code uses all three -
 * and the way that was discovered is worth recording: the HOST test build
 * links against a real libc, so every one of these resolved silently there
 * and only the freestanding kernel build revealed them, as implicit
 * declarations. An implicit declaration of a function returning a POINTER is
 * not a style warning on a 64-bit kernel: the value is truncated to int and
 * the pointer is destroyed. Building both targets is what catches it, which
 * is the same lesson as SYSCALL_TRACE=1 having to compile. */

typedef struct fmt_sink {
    char  *buf;          /* NULL to write to the screen instead */
    uint64_sz cap;
    uint64_sz used;
} fmt_sink_t;

static void sink_put(fmt_sink_t *sink, char c) {
    if (sink->buf == NULL) {
        print_char(c, ZFS_PRINT_COLOR);
        sink->used++;
        return;
    }
    if (sink->used + 1 < sink->cap) {
        sink->buf[sink->used] = c;
    }
    sink->used++;
}

static void sink_str(fmt_sink_t *sink, const char *s) {
    while (*s != '\0') {
        sink_put(sink, *s++);
    }
}

static void sink_num(fmt_sink_t *sink, unsigned long long value, unsigned base,
                     int is_neg, int width, int zero_pad) {
    char buf[24];
    int  n = 0;
    int  pad;

    if (value == 0) {
        buf[n++] = '0';
    }
    while (value != 0) {
        unsigned digit = (unsigned)(value % base);

        buf[n++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        value /= base;
    }
    if (is_neg) {
        buf[n++] = '-';
    }
    for (pad = n; pad < width; pad++) {
        sink_put(sink, zero_pad ? '0' : ' ');
    }
    while (n > 0) {
        sink_put(sink, buf[--n]);
    }
}

static void fmt_core(fmt_sink_t *sink, const char *fmt, va_list ap) {
    int i = 0;

    while (fmt[i] != '\0') {
        int width = 0;
        int zero_pad = 0;
        int longs = 0;
        int precision = -1;

        if (fmt[i] != '%') {
            sink_put(sink, fmt[i++]);
            continue;
        }
        i++;
        if (fmt[i] == '-') {
            i++;                 /* left justify: accepted, not honoured */
        }
        if (fmt[i] == '0') {
            zero_pad = 1;
            i++;
        }
        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i++] - '0');
        }
        /* %.*s, which the vendored code uses to print a vdev type out of an
         * nvlist string that is NOT terminated - the length comes from the
         * nvlist. Without this the type prints as the literal "%.*s" and the
         * refusal message names nothing, which is the one thing a refusal has
         * to do. */
        if (fmt[i] == '.') {
            i++;
            if (fmt[i] == '*') {
                precision = va_arg(ap, int);
                i++;
            } else {
                precision = 0;
                while (fmt[i] >= '0' && fmt[i] <= '9') {
                    precision = precision * 10 + (fmt[i++] - '0');
                }
            }
        }
        while (fmt[i] == 'l' || fmt[i] == 'z' || fmt[i] == 'j') {
            if (fmt[i] == 'l') {
                longs++;
            } else {
                longs = 2;
            }
            i++;
        }

        switch (fmt[i]) {
            case 's': {
                const char *s = va_arg(ap, const char *);

                if (s == NULL) {
                    s = "(null)";
                }
                if (precision >= 0) {
                    int k;

                    for (k = 0; k < precision && s[k] != '\0'; k++) {
                        sink_put(sink, s[k]);
                    }
                } else {
                    sink_str(sink, s);
                }
                break;
            }
            case 'c':
                sink_put(sink, (char)va_arg(ap, int));
                break;
            case 'd':
            case 'i': {
                long long v = (longs >= 2) ? va_arg(ap, long long)
                                           : (long long)va_arg(ap, int);

                sink_num(sink, v < 0 ? (unsigned long long)(-v)
                                     : (unsigned long long)v,
                         10, v < 0, width, zero_pad);
                break;
            }
            case 'u': {
                unsigned long long v = (longs >= 2)
                    ? va_arg(ap, unsigned long long)
                    : (unsigned long long)va_arg(ap, unsigned int);

                sink_num(sink, v, 10, 0, width, zero_pad);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long long v = (longs >= 2)
                    ? va_arg(ap, unsigned long long)
                    : (unsigned long long)va_arg(ap, unsigned int);

                sink_num(sink, v, 16, 0, width, zero_pad);
                break;
            }
            case 'p':
                sink_str(sink, "0x");
                sink_num(sink, (unsigned long long)(unsigned long)
                         va_arg(ap, void *), 16, 0, 0, 0);
                break;
            case '%':
                sink_put(sink, '%');
                break;
            default:
                /* Printed verbatim rather than guessed at. An unhandled
                 * conversion that consumed an argument would walk the
                 * argument list out of step for everything after it. */
                sink_put(sink, '%');
                sink_put(sink, fmt[i]);
                break;
        }
        if (fmt[i] != '\0') {
            i++;
        }
    }
}

int zfs_libsa_printf(const char *fmt, ...) {
    fmt_sink_t sink;
    va_list ap;

    sink.buf = NULL;
    sink.cap = 0;
    sink.used = 0;
    va_start(ap, fmt);
    fmt_core(&sink, fmt, ap);
    va_end(ap);
    return (int)sink.used;
}

int zfs_libsa_vsnprintf(char *buf, uint64_sz cap, const char *fmt, va_list ap) {
    fmt_sink_t sink;

    sink.buf = buf;
    sink.cap = cap;
    sink.used = 0;
    fmt_core(&sink, fmt, ap);
    if (cap > 0) {
        buf[sink.used < cap - 1 ? sink.used : cap - 1] = '\0';
    }
    /* The length it WANTED to write, as snprintf promises - a caller that
     * checks for truncation checks this against the buffer size. */
    return (int)sink.used;
}

int zfs_libsa_snprintf(char *buf, uint64_sz cap, const char *fmt, ...) {
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = zfs_libsa_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int zfs_libsa_asprintf(char **out, const char *fmt, ...) {
    va_list ap;
    int n;
    char *buf;

    /* Measured first, then allocated. The alternative - guess a size and
     * hope - is how a vdev with a long path gets a truncated name that
     * nothing ever compares against the real one. */
    va_start(ap, fmt);
    n = zfs_libsa_vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return -1;
    }
    buf = (char *)zfs_libsa_malloc((size_t)n + 1);
    if (buf == NULL) {
        *out = NULL;
        return -1;
    }
    va_start(ap, fmt);
    (void)zfs_libsa_vsnprintf(buf, (uint64_sz)n + 1, fmt, ap);
    va_end(ap);
    *out = buf;
    return n;
}

char *zfs_libsa_strcat(char *dst, const char *src) {
    size_t n = zfs_libsa_strlen(dst);
    size_t i;

    for (i = 0; src[i] != '\0'; i++) {
        dst[n + i] = src[i];
    }
    dst[n + i] = '\0';
    return dst;
}

int zfs_libsa_iscntrl(int c) {
    return (c >= 0 && c < 0x20) || c == 0x7F;
}

/* The loader's pager, which Genesis does not have: spa_all_status prints the
 * pool listing through it. Zero means "keep going" - a non-zero return is the
 * loader's "the user pressed q". */
int zfs_libsa_pager_output(const char *s) {
    print_string(s, 0x07);
    return 0;
}

/* zfssubr.c calls panic() on a raidz reconstruction it considers impossible.
 * It must not take the machine down: the pool is data, not a kernel
 * invariant, and a corrupt disk is not a reason to stop the world. It prints
 * and returns, and the read that called it fails on the checksum it was going
 * to fail on anyway. */
void zfs_libsa_panic(const char *fmt, ...) {
    (void)fmt;
    print_string("zfs: the vendored reader hit an internal assertion "
                 "(the pool is probably damaged)\n", 0x0C);
}

/* --- the symbols a freestanding link needs -------------------------------
 *
 * memcpy, memset, memmove and memcmp are here because GCC EMITS CALLS TO
 * THEM. Not because the vendored code names them - the compat header renames
 * those - but because a struct assignment or a large initialisation becomes a
 * call to memcpy no matter how freestanding the build claims to be, and the
 * vendored reader assigns 200-byte uberblocks and 128-byte block pointers
 * around. Genesis has never needed them before: every other file in this
 * kernel hand-rolls its copies and its structs are small.
 *
 * They are the first libc-shaped symbols in this kernel, and they are
 * deliberately confined to a file the boundary check covers. Nothing else
 * calls them by name - if something starts to, it should be because the
 * kernel grew a string.c on purpose, not because ZFS left these lying about.
 *
 * The other direction is worth noting too: realloc, asprintf, vsnprintf,
 * strcat, bcmp and iscntrl are NOT defined here. The compat header renames
 * them to the zfs_libsa_ implementations above, which are real - the port
 * needed them the moment the kernel build ran, because the host build had
 * been resolving them against libc without anybody noticing. */

void *memcpy(void *dst, const void *src, unsigned long n) {
    return zfs_libsa_memcpy(dst, src, n);
}

void *memmove(void *dst, const void *src, unsigned long n) {
    return zfs_libsa_memcpy(dst, src, n);
}

void *memset(void *dst, int c, unsigned long n) {
    return zfs_libsa_memset(dst, c, n);
}

int memcmp(const void *a, const void *b, unsigned long n) {
    return zfs_libsa_memcmp(a, b, n);
}
