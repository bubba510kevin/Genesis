#ifndef GENESIS_BSD_COMPAT_SYS_DOMAINSET_H
#define GENESIS_BSD_COMPAT_SYS_DOMAINSET_H

/* NUMA domain sets, collapsed to one domain.
 *
 * Every one of these is a real upstream concept that means something on a
 * multi-socket machine and means exactly one thing here: domain 0. Kept as
 * named types rather than #defined away, so uma_core.c's declarations still
 * parse unmodified and the day a second domain exists there is somewhere to
 * put it. */

#include <sys/types.h>

struct domainset {
    int ds_dummy;
};
struct domainset_ref {
    struct domainset *dr_policy;
    int               dr_iter;
};

extern struct domainset  genesis_domainset;
#define DOMAINSET_RR()      (&genesis_domainset)
#define DOMAINSET_PREF(d)   (&genesis_domainset)
#define DOMAINSET_FIXED(d)  (&genesis_domainset)

#define vm_ndomains 1

#define domainset_empty_vm() do { } while (0)

#endif
