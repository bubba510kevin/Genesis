/* Genesis compat shim for the vendored ZFS reader. Not vendored code.
 *
 * The FreeBSD standalone reader is written against libsa - the boot loader's
 * miniature libc. This header is that surface, and zfs_shim.c is the
 * implementation over Genesis's own facilities: kmalloc for malloc,
 * print_string for printf.
 *
 * It is deliberately the SMALLEST surface that compiles the vendored files:
 * every name here appears in zfsimpl.c, zfssubr.c or nvlist.c. Growing it to
 * "a libc" would be inviting the rest of the kernel to use it. */
#ifndef ZFS_LIBSA_H
#define ZFS_LIBSA_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

void *zfs_libsa_malloc(size_t n);
void *zfs_libsa_calloc(size_t n, size_t size);
void *zfs_libsa_realloc(void *p, size_t n);
void  zfs_libsa_free(void *p);
int   zfs_libsa_printf(const char *fmt, ...);
int   zfs_libsa_vsnprintf(char *buf, unsigned long long cap, const char *fmt,
                          va_list ap);
int   zfs_libsa_snprintf(char *buf, unsigned long long cap, const char *fmt,
                         ...);
int   zfs_libsa_asprintf(char **out, const char *fmt, ...);
char *zfs_libsa_strcat(char *dst, const char *src);
int   zfs_libsa_iscntrl(int c);

void  *zfs_libsa_memcpy(void *dst, const void *src, size_t n);
void  *zfs_libsa_memset(void *dst, int c, size_t n);
int    zfs_libsa_memcmp(const void *a, const void *b, size_t n);
size_t zfs_libsa_strlen(const char *s);
int    zfs_libsa_strcmp(const char *a, const char *b);
int    zfs_libsa_strncmp(const char *a, const char *b, size_t n);
char  *zfs_libsa_strcpy(char *dst, const char *src);
size_t zfs_libsa_strlcpy(char *dst, const char *src, size_t cap);
char  *zfs_libsa_strdup(const char *s);
char  *zfs_libsa_strchr(const char *s, int c);

#define malloc(n)        zfs_libsa_malloc(n)
#define calloc(n, s)     zfs_libsa_calloc((n), (s))
#define realloc(p, n)    zfs_libsa_realloc((p), (n))
#define free(p)          zfs_libsa_free(p)
#define printf           zfs_libsa_printf
#define vsnprintf        zfs_libsa_vsnprintf
#define snprintf         zfs_libsa_snprintf
#define asprintf         zfs_libsa_asprintf
#define strcat           zfs_libsa_strcat
#define iscntrl          zfs_libsa_iscntrl
#define bcmp(a, b, n)    zfs_libsa_memcmp((a), (b), (n))
#define memcpy(d, s, n)  zfs_libsa_memcpy((d), (s), (n))
#define memmove(d, s, n) zfs_libsa_memcpy((d), (s), (n))
#define memset(d, c, n)  zfs_libsa_memset((d), (c), (n))
#define memcmp(a, b, n)  zfs_libsa_memcmp((a), (b), (n))
#define strlen(s)        zfs_libsa_strlen(s)
#define strcmp(a, b)     zfs_libsa_strcmp((a), (b))
#define strncmp(a, b, n) zfs_libsa_strncmp((a), (b), (n))
#define strcpy(d, s)     zfs_libsa_strcpy((d), (s))
#define strlcpy(d, s, n) zfs_libsa_strlcpy((d), (s), (n))
#define strdup(s)        zfs_libsa_strdup(s)
#define strchr(s, c)     zfs_libsa_strchr((s), (c))
#define bcopy(s, d, n)   ((void)zfs_libsa_memcpy((d), (s), (n)))
#define bzero(d, n)      ((void)zfs_libsa_memset((d), 0, (n)))
#define assert(x)        ((void)0)

/* The loader pages its status output through this. Genesis has no pager; the
 * status listing goes to the same place every other boot report goes. */
int zfs_libsa_pager_output(const char *s);
#define pager_output(s)  zfs_libsa_pager_output(s)

#endif
