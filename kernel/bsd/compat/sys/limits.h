#ifndef GENESIS_BSD_COMPAT_SYS_LIMITS_H
#define GENESIS_BSD_COMPAT_SYS_LIMITS_H
#ifndef CHAR_BIT
#define CHAR_BIT   8
#endif
#ifndef INT_MAX
#define INT_MAX    0x7FFFFFFF
#endif
#ifndef UINT_MAX
#define UINT_MAX   0xFFFFFFFFU
#endif
#ifndef LONG_MAX
#define LONG_MAX   0x7FFFFFFFFFFFFFFFL
#endif
#ifndef ULONG_MAX
#define ULONG_MAX  0xFFFFFFFFFFFFFFFFUL
#endif
#ifndef SIZE_T_MAX
#define SIZE_T_MAX ULONG_MAX
#endif
#ifndef OFF_MAX
#define OFF_MAX    LONG_MAX
#endif
#endif
