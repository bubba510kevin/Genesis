#ifndef GENESIS_BSD_COMPAT_SYS_VMMEter_H
#define GENESIS_BSD_COMPAT_SYS_VMMEter_H
/* VM statistics. UMA reads free-page counts to decide whether to wait.
 * Genesis's PMM answers the only question that matters. */
#include <sys/types.h>
unsigned long genesis_vm_free_count(void);
#define vm_free_count()      genesis_vm_free_count()
#define vm_page_count_min()  (0)
#define vm_page_count_severe() (0)
#define vm_wait_domain(d)    genesis_vm_wait_domain(d)
void genesis_vm_wait_domain(int domain);
#endif
