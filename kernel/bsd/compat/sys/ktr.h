#ifndef GENESIS_BSD_COMPAT_SYS_KTR_H
#define GENESIS_BSD_COMPAT_SYS_KTR_H
/* Kernel tracing, compiled out - the same way a GENERIC-minus-debugging
 * config does it. The classes are defined so the CTR* call sites still
 * name something real. */
#define KTR_UMA 0
#define CTR0(m, f)                     do { } while (0)
#define CTR1(m, f, a)                  do { } while (0)
#define CTR2(m, f, a, b)               do { } while (0)
#define CTR3(m, f, a, b, c)            do { } while (0)
#define CTR4(m, f, a, b, c, d)         do { } while (0)
#define CTR5(m, f, a, b, c, d, e)      do { } while (0)
#define CTR6(m, f, a, b, c, d, e, g)   do { } while (0)
#endif
