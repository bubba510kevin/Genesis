#ifndef GENESIS_BSD_COMPAT_SYS_STACK_H
#define GENESIS_BSD_COMPAT_SYS_STACK_H
/* Stack capture, for UMA's debug allocator trace. Compiled out. */
struct stack { int st_dummy; };
#define stack_save(s)     do { } while (0)
#define stack_print(s)    do { } while (0)
#endif
