#ifndef GENESIS_BSD_COMPAT_VM_vm_domainset_H
#define GENESIS_BSD_COMPAT_VM_vm_domainset_H
/* See kernel/bsd/README.md. Genesis has no vm_domainset.h equivalent; what uma_core.c
 * needs from this header is declared in vm/vm_extern.h or vm/vm_page.h,
 * which is where the real backing lives. */
#include <vm/vm.h>
#include <vm/vm_param.h>
#endif

/* ONE domain, so the iterator visits it once and stops. Written as a real
 * two-call loop rather than #defined away, because uma_core.c's call sites
 * are `while (vm_domainset_iter_policy(&di, &domain) == 0)` shapes that have
 * to terminate. */
struct vm_domainset_iter { int done; };
#define vm_domainset_iter_page_init(di, obj, pi, dom, req)  ((di)->done = 0, *(dom) = 0)
#define vm_domainset_iter_policy_init(di, ds, dom, req)     ((di)->done = 0, *(dom) = 0)
#define vm_domainset_iter_policy_ref_init(di, dr, dom, req) ((di)->done = 0, *(dom) = 0)
#define vm_domainset_iter_policy(di, dom)                   ((di)->done ? -1 : ((di)->done = 1, *(dom) = 0, 0))
#define vm_domainset_iter_page(di, obj, dom)                ((di)->done ? -1 : ((di)->done = 1, *(dom) = 0, 0))
#define vm_phys_domain(pa)   (0)
#define _vm_phys_domain(pa)  (0)
