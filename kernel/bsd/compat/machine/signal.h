#ifndef _MACHINE_SIGNAL_H_
#define _MACHINE_SIGNAL_H_
/* <machine/signal.h> - the amd64 signal context. Vendored network code
 * includes <sys/signal.h> transitively (route.c, for the socket layer's
 * signalling of a listening process) and never touches the context itself,
 * so this carries the type and no layout. Genesis's own signal delivery is
 * kernel/proc/signal.c and is unrelated to this. */
typedef int sig_atomic_t;
struct sigcontext { int sc_unused; };
typedef struct __mcontext { int mc_unused; } mcontext_t;
#endif
