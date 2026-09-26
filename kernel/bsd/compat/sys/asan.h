#ifndef GENESIS_BSD_COMPAT_SYS_ASAN_H
#define GENESIS_BSD_COMPAT_SYS_ASAN_H
/* Address sanitizer, compiled out. */
#define kasan_mark(a, b, c, d) do { } while (0)
#endif
