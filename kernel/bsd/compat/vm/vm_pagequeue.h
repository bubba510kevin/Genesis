#ifndef GENESIS_BSD_COMPAT_VM_vm_pagequeue_H
#define GENESIS_BSD_COMPAT_VM_vm_pagequeue_H
/* See kernel/bsd/README.md. Genesis has no vm_pagequeue.h equivalent; what uma_core.c
 * needs from this header is declared in vm/vm_extern.h or vm/vm_page.h,
 * which is where the real backing lives. */
#include <vm/vm.h>
#include <vm/vm_param.h>
#endif

/* Per-domain page queues. One domain, no page daemon, so the only thing
 * uma_core.c asks - "is this domain short of memory" - is answered by the
 * PMM directly. */
#define VM_DOMAIN(d)              (&genesis_vm_domain)
#define vm_domain_freecnt_inc(d, n)  do { } while (0)
#define vm_domain_allocate(d, r, n)  (1)
/* No domain is empty - there is one and it has the machine's memory. */
#define VM_DOMAIN_EMPTY(d)           (0)
struct genesis_vm_domain_s {
    int vmd_dummy;
    /* The domain's name, as UMA's per-keg sysctl tree labels it. It needed a
     * value once <sys/sysctl.h> stopped being compiled out: SYSCTL_ADD_NODE
     * builds a real node named VM_DOMAIN(i)->vmd_name. One NUMA domain here,
     * so there is one name and it is the one upstream uses for domain 0. */
    const char *vmd_name;
};
extern struct genesis_vm_domain_s genesis_vm_domain;
