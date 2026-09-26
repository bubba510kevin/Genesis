#ifndef LINUX_KERNEL_H
#define LINUX_KERNEL_H

#include "linux/errno.h"
#include "linux/types.h"

/* <linux/kernel.h> - the header essentially every Linux driver includes,
 * directly or through another. printk, the pr_* wrappers, and the handful of
 * macros driver code uses without thinking about where they come from.
 *
 * Backed by kprintf_c, which is Genesis's own. The format-string dialects
 * agree closely enough for driver code: %d %u %x %s %p %lx %ld all mean the
 * same thing in both. They do NOT agree on Linux's %pM / %pI4 / %pS extended
 * pointer formats, which is worth knowing before a network driver prints a
 * MAC address and gets a hex pointer - see linux_printk in kernel/driver/
 * lkpi.c, which says so at the implementation too.
 */

/* Log levels. Linux encodes them as a "<N>" prefix ON the format string, and
 * driver source writes pr_info("...") far more often than printk(KERN_INFO
 * "..."), but both spellings occur and both have to work. The prefix is
 * parsed off by linux_printk and turned into a Genesis console colour, so
 * KERN_ERR really does come out red rather than being stripped and lost. */
#define KERN_EMERG   "<0>"
#define KERN_ALERT   "<1>"
#define KERN_CRIT    "<2>"
#define KERN_ERR     "<3>"
#define KERN_WARNING "<4>"
#define KERN_NOTICE  "<5>"
#define KERN_INFO    "<6>"
#define KERN_DEBUG   "<7>"
#define KERN_CONT    ""

int printk(const char *fmt, ...);

#define pr_emerg(fmt, ...)   printk(KERN_EMERG   fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)   printk(KERN_ALERT   fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)    printk(KERN_CRIT    fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)     printk(KERN_ERR     fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)    printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_warning(fmt, ...) printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)  printk(KERN_NOTICE  fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)    printk(KERN_INFO    fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)   printk(KERN_DEBUG   fmt, ##__VA_ARGS__)
#define pr_cont(fmt, ...)    printk(KERN_CONT    fmt, ##__VA_ARGS__)

/* The one macro that would be genuinely surprising to hand-roll wrong.
 * `ptr` is a pointer to `member` inside some `type`; this recovers the
 * enclosing struct. The (char *) cast is not decoration - pointer arithmetic
 * on the struct type would scale by sizeof(type). */
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Statement-expression min/max, so the arguments are evaluated once. Linux
 * does the same and for the same reason: max(i++, j) with a naive macro
 * increments twice. */
#ifndef min
#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); \
                     _a < _b ? _a : _b; })
#endif
#ifndef max
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); \
                     _a > _b ? _a : _b; })
#endif
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define clamp(v, lo, hi) min(max(v, lo), hi)

#define DIV_ROUND_UP(n, d)  (((n) + (d) - 1) / (d))
#define ALIGN(x, a)         (((x) + (a) - 1) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a)    (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define swap(a, b) \
    do { __typeof__(a) _t = (a); (a) = (b); (b) = _t; } while (0)

/* Branch hints. Real ones - gcc supports __builtin_expect under exactly the
 * flags this kernel builds with - rather than the no-op definitions a compat
 * layer often gets away with. Costs nothing and means driver source that
 * marks its error paths unlikely() actually gets what it asked for. */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* BUG_ON / WARN_ON.
 *
 * BUG_ON really does stop, because the alternative is worse: a driver that
 * asserts an invariant and then continues past it having been violated
 * corrupts something further away, and this kernel has a backtrace (Part 2 of
 * the item 11 pass) precisely so that stopping here is informative. WARN_ON
 * prints and returns the condition, which is what lets `if (WARN_ON(x))
 * return -EINVAL;` work as driver source expects. */
void linux_bug(const char *file, int line, const char *cond);
int  linux_warn(const char *file, int line, const char *cond);

#define BUG() linux_bug(__FILE__, __LINE__, "BUG()")
#define BUG_ON(c) \
    do { if (unlikely(c)) linux_bug(__FILE__, __LINE__, #c); } while (0)
#define WARN_ON(c)      (unlikely(c) ? linux_warn(__FILE__, __LINE__, #c) : 0)
#define WARN_ON_ONCE(c) WARN_ON(c)
#define WARN(c, fmt, ...) \
    (unlikely(c) ? (printk(KERN_WARNING fmt, ##__VA_ARGS__), 1) : 0)

#endif
