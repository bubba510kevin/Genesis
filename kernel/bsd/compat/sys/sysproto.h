#ifndef _SYS_SYSPROTO_H_
#define _SYS_SYSPROTO_H_

/* <sys/sysproto.h> - the generated argument structures for every FreeBSD
 * system call, and DELIBERATELY EMPTY here.
 *
 * The real file was vendored first. It is machine-generated from syscalls.master
 * and declares a struct per syscall, which means it declares the argument types
 * of every syscall FreeBSD has: acl_type_t, cpuwhich_t, cap_rights_t, fd_set,
 * mcontext_t, aio_cb, kevent... None of those types exist in this tree, and
 * building them would mean porting FreeBSD's whole syscall surface to get
 * three network files to compile.
 *
 * Three files include it - net/route.c, net/route/route_tables.c and
 * net/route/route_helpers.c - and each of them for the SAME single reason:
 * sys_setfib(), the system call that changes a process's routing table. That
 * syscall is not reachable here (Genesis's syscall table is in
 * kernel/proc/syscall.c and has no setfib), so the struct its declaration
 * needs does not have to exist.
 *
 * The one thing this file must still provide is that struct, because
 * sys_setfib()'s DEFINITION mentions it and is compiled whether or not it can
 * be called.
 */

struct setfib_args {
	int	fibnum;
};

struct thread;

#endif /* _SYS_SYSPROTO_H_ */
