#ifndef LINUX_STRING_H
#define LINUX_STRING_H

/* <linux/string.h>.
 *
 * memcpy/memset/memcmp are already global symbols in this kernel - defined in
 * kernel/zfs/zfs_shim.c, where the vendored ZFS reader needed them first -
 * so these are declarations of the existing functions, not new ones. The
 * prototypes must match that definition exactly, which is why the length is
 * `unsigned long` here rather than a size_t this header would have to invent.
 */

void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);
int   memcmp(const void *a, const void *b, unsigned long n);
void *memmove(void *dst, const void *src, unsigned long n);

unsigned long strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, unsigned long n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, unsigned long n);
char  *strchr(const char *s, int c);

/* strlcpy, not strcpy, is what driver source should use and mostly does:
 * it always NUL-terminates and returns the length it WANTED, so truncation
 * is detectable. Returning the source length rather than the copied length
 * is not a quirk - it is the only way the caller can tell. */
unsigned long strlcpy(char *dst, const char *src, unsigned long size);

#endif
