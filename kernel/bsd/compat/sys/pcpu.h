#ifndef GENESIS_BSD_COMPAT_SYS_PCPU_H
#define GENESIS_BSD_COMPAT_SYS_PCPU_H

/* <sys/pcpu.h> - per-CPU data for FreeBSD code.
 *
 * DPCPU ("dynamic per-CPU"): upstream places each DPCPU_DEFINE in a linker
 * set that is copied once per CPU and reached through a per-CPU offset. Here
 * a DPCPU variable is an array with one slot per possible CPU, which gives
 * the same interface - DPCPU_PTR, DPCPU_GET/SET, the _ID_ forms, DPCPU_SUM -
 * with no replication step, because every macro names the variable rather
 * than taking its address. (LinuxKPI's per-CPU variables do use the offset
 * scheme - see <linux/percpu.h> - because Linux code takes their addresses.) */

#include <sys/smp.h>

#define DPCPU_NAME(n)                dpcpu_entry_##n
#define DPCPU_DEFINE(t, n)           __typeof__(t) DPCPU_NAME(n)[SMP_MAX_CPUS]
#define DPCPU_DEFINE_STATIC(t, n)    static DPCPU_DEFINE(t, n)
#define DPCPU_DECLARE(t, n)          extern __typeof__(t) DPCPU_NAME(n)[SMP_MAX_CPUS]

#define DPCPU_ID_PTR(i, n)           (&DPCPU_NAME(n)[(i)])
#define DPCPU_ID_GET(i, n)           (DPCPU_NAME(n)[(i)])
#define DPCPU_ID_SET(i, n, v)        (DPCPU_NAME(n)[(i)] = (v))
#define DPCPU_PTR(n)                 DPCPU_ID_PTR(curcpu, n)
#define DPCPU_GET(n)                 DPCPU_ID_GET(curcpu, n)
#define DPCPU_SET(n, v)              DPCPU_ID_SET(curcpu, n, v)

#define DPCPU_SUM(n) ({                                          \
    __typeof__(DPCPU_NAME(n)[0]) _sum = 0;                       \
    int _i;                                                      \
    CPU_FOREACH(_i) {                                            \
        _sum += DPCPU_NAME(n)[_i];                               \
    }                                                            \
    _sum;                                                        \
})
#define DPCPU_VARSUM(n, var) ({                                  \
    __typeof__(DPCPU_NAME(n)[0].var) _sum = 0;                   \
    int _i;                                                      \
    CPU_FOREACH(_i) {                                            \
        _sum += DPCPU_NAME(n)[_i].var;                           \
    }                                                            \
    _sum;                                                        \
})
#define DPCPU_ZERO(n) do {                                       \
    int _i;                                                      \
    for (_i = 0; _i < SMP_MAX_CPUS; _i++) {                      \
        __builtin_memset(&DPCPU_NAME(n)[_i], 0, sizeof(DPCPU_NAME(n)[0])); \
    }                                                            \
} while (0)

#endif
