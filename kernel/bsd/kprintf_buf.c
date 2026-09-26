/* snprintf and vsnprintf - formatting into a BUFFER.
 *
 * Genesis's kprintf family writes to the console and has no buffer-producing
 * form, so this is a small real implementation. Vendored source uses it to
 * build interface names, sysctl strings and the string ether_gen_addr
 * hashes.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include "typesk.h"

/* --- snprintf ------------------------------------------------------------
 *
 * Genesis's kprintf family writes to the console and has no buffer-producing
 * form, so this is a small real implementation rather than a redirect. It
 * supports what driver source actually formats into a string: %s, %d, %u,
 * %x and %c. An unrecognised specifier is copied through literally rather
 * than skipped, so a format this does not handle is visible in the output
 * instead of silently producing a short string.
 */
static void snput(char *buf, unsigned long size, unsigned long *pos, char c) {
    if (*pos + 1 < size) {
        buf[*pos] = c;
    }
    (*pos)++;
}

static void snputs(char *buf, unsigned long size, unsigned long *pos,
                   const char *s) {
    while (*s != '\0') {
        snput(buf, size, pos, *s++);
    }
}

static void snputnum(char *buf, unsigned long size, unsigned long *pos,
                     uint64 v, int base, int negative) {
    char tmp[24];
    int n = 0;

    if (negative) {
        snput(buf, size, pos, '-');
    }
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v != 0) {
        int d = (int)(v % (uint64)base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (uint64)base;
    }
    while (n-- > 0) {
        snput(buf, size, pos, tmp[n]);
    }
}

int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap) {
    unsigned long pos = 0;

    while (*fmt != '\0') {
        if (*fmt != '%') {
            snput(buf, size, &pos, *fmt++);
            continue;
        }
        fmt++;
        switch (*fmt) {
            case 's': snputs(buf, size, &pos, va_arg(ap, const char *)); break;
            case 'c': snput(buf, size, &pos, (char)va_arg(ap, int)); break;
            case 'x': snputnum(buf, size, &pos, va_arg(ap, unsigned int), 16, 0);
                      break;
            case 'u': snputnum(buf, size, &pos, va_arg(ap, unsigned int), 10, 0);
                      break;
            case 'd': {
                int v = va_arg(ap, int);
                snputnum(buf, size, &pos, v < 0 ? (uint64)(-(int64)v) : (uint64)v,
                         10, v < 0);
                break;
            }
            case '%': snput(buf, size, &pos, '%'); break;
            default:
                /* Copied through, not dropped - an unhandled specifier is
                 * then visible rather than producing a mysteriously short
                 * string. */
                snput(buf, size, &pos, '%');
                snput(buf, size, &pos, *fmt);
                break;
        }
        if (*fmt != '\0') {
            fmt++;
        }
    }
    if (size > 0) {
        buf[pos < size ? pos : size - 1] = '\0';
    }
    return (int)pos;
}

int snprintf(char *buf, unsigned long size, const char *fmt, ...) {
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* --- the rest of the if_t surface a bigger driver reaches for ------------ */

/* if_hwassist is a SEPARATE word from if_capenable and they carry DIFFERENT
 * bit namespaces - CSUM_* here, IFCAP_* there. Setting one with the other's
 * bits is silent: the numbers are all small and none of them is invalid. */
