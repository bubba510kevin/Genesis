#ifndef LINUX_ERRNO_H
#define LINUX_ERRNO_H

/* Linux's errno values, as driver source spells them.
 *
 * Driver code returns these NEGATED (`return -ENODEV;`) - that is the kernel
 * convention, not the userspace one, and the negation happens at the return
 * statement rather than being baked into the constant. So these are the
 * positive values, exactly as Linux defines them.
 *
 * The numbers matter and are not free to choose: a driver may compare
 * against them, and more to the point, kernel/proc/syscall.c already answers
 * userspace with these same numbers. Two different meanings for 19 in one
 * kernel would be a bug nobody could see.
 */
#define EPERM            1
#define ENOENT           2
#define ESRCH            3
#define EINTR            4
#define EIO              5
#define ENXIO            6
#define E2BIG            7
#define ENOEXEC          8
#define EBADF            9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EMLINK          31
#define EPIPE           32
#define ERANGE          34
#define ENAMETOOLONG    36
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40
#define EOVERFLOW       75
#define ETIMEDOUT      110
#define EOPNOTSUPP      95
#define ENOBUFS        105
#define EPROBE_DEFER   517   /* Linux-internal, above the POSIX range */

#endif
