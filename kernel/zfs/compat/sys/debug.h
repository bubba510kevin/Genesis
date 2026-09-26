/* Genesis compat shim: the assertion macros the vendored list.c uses. They
 * compile out. A list invariant failing means the reader has already gone
 * wrong somewhere the checksum should have caught, and a kernel that halts on
 * it is worse than one that returns an error. */
#ifndef ZFSCOMPAT_SYS_DEBUG_H
#define ZFSCOMPAT_SYS_DEBUG_H
#define ASSERT(x)               ((void)0)
#define ASSERT3P(a, op, b)      ((void)0)
#define ASSERT3U(a, op, b)      ((void)0)
#define ASSERT3S(a, op, b)      ((void)0)
#define VERIFY(x)               ((void)(x))
#define EQUIV(a, b)             ((void)0)
#define list_d2l(a, obj)        ((list_node_t *)(((char *)obj) + (a)->list_offset))
#endif
