#ifndef GENESIS_BSD_COMPAT_VM_MEMGUARD_H
#define GENESIS_BSD_COMPAT_VM_MEMGUARD_H
/* memguard, compiled out. is_memguard_addr must answer FALSE rather than
 * being undefined: uma_core.c branches on it on the free path. */
#define is_memguard_addr(a)      (0)
#define memguard_cmp_zone(z)     (0)
#define memguard_alloc(a, b)     (NULL)
#define memguard_free(a)         do { } while (0)
#endif
