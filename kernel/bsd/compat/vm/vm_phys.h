#ifndef GENESIS_BSD_COMPAT_VM_vm_phys_H
#define GENESIS_BSD_COMPAT_VM_vm_phys_H
/* See kernel/bsd/README.md. Genesis has no vm_phys.h equivalent; what uma_core.c
 * needs from this header is declared in vm/vm_extern.h or vm/vm_page.h,
 * which is where the real backing lives. */
#include <vm/vm.h>
#include <vm/vm_param.h>
#endif
