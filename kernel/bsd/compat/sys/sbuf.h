#ifndef GENESIS_BSD_COMPAT_SYS_SBUF_H
#define GENESIS_BSD_COMPAT_SYS_SBUF_H
/* String buffers, used only by the sysctl handlers this build compiles
 * out. */
/* Enough of a string buffer to serve a sysctl handler that formats its
 * answer. Upstream's has auto-extension, a drain callback and section
 * support; this one is a fixed caller-supplied buffer with an overflow flag,
 * which is exactly the shape sbuf_new_for_sysctl() is used in. */
struct sbuf {
    char  *s_buf;
    int    s_size;
    int    s_len;
    int    s_error;
    int    s_flags;
    struct sysctl_req *s_req;
};

/* Real declarations, not macros. <sys/sysctl.h> is vendored now and declares
 * sbuf_new_for_sysctl() itself; a function-like macro of the same name turns
 * that declaration into a syntax error, which is how these stopped being
 * macros. Implemented in kernel/bsd/kern_sysctl.c. */
struct sysctl_req;

struct sbuf *sbuf_new_for_sysctl(struct sbuf *s, char *buf, int length,
                                 struct sysctl_req *req);
void  sbuf_delete(struct sbuf *s);
int   sbuf_finish(struct sbuf *s);
int   sbuf_drain(struct sbuf *s);
int   sbuf_bcat(struct sbuf *s, const void *buf, size_t len);
int   sbuf_cat(struct sbuf *s, const char *str);
int   sbuf_printf(struct sbuf *s, const char *fmt, ...);
void  sbuf_clear_flags(struct sbuf *s, int flags);
char *sbuf_data(struct sbuf *s);
int   sbuf_len(struct sbuf *s);

#define SBUF_FIXEDLEN                    0x00000000  /* the only mode here */
#define SBUF_AUTOEXTEND                  0x00000001
#define SBUF_INCLUDENUL                  0x00000002

/* sbuf_new() with a caller-supplied buffer, which is the only form this
 * implementation supports - SBUF_AUTOEXTEND would mean owning and growing an
 * allocation, and no caller in the vendored tree asks for it. Passing a NULL
 * buffer (upstream's "allocate one for me") returns NULL rather than
 * pretending. */
struct sbuf *sbuf_new(struct sbuf *s, char *buf, int length, int flags);
int   sbuf_error(const struct sbuf *s);
/* Reset a buffer to empty without releasing it, so a caller can format a
 * second answer into the same storage. */
void  sbuf_clear(struct sbuf *s);
#endif
