#ifndef _SYS_MSAN_H_
#define _SYS_MSAN_H_
/* KMSAN - the kernel memory sanitizer, which tracks whether every byte is
 * initialised. A debugging build option (options KMSAN), off in GENERIC and
 * off here. Its check calls compile away. */
#define kmsan_check(p, l, d)        do { } while (0)
#define kmsan_check_mbuf(m, d)      do { } while (0)
#define kmsan_mark(p, l, c)         do { } while (0)
#define kmsan_mark_mbuf(m, c)       do { } while (0)
#define kmsan_orig(p, l, c, pc)     do { } while (0)
#define kmsan_init(void)            do { } while (0)
#define kmsan_thread_alloc(td)      do { } while (0)
#endif
