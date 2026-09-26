#include "kprintf.h"
#include "screen.h"

#include <stdarg.h>

/* See kprintf.h for scope: %d %u %x %lx %s %% only, no width/precision/
 * padding/pointer/float. Each conversion builds its text into a small stack
 * buffer and hands it to print_string once, the same shape print_hex/
 * print_hex64 (screen.c) already use - one write to the (possibly serial-
 * mirrored) output per field, not one per digit. */

static void print_udec(uint64 value, uint8 color) {
    char buf[21]; /* max 2^64-1 is 20 digits, +NUL */
    int i = 20;
    buf[i] = '\0';
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value != 0) {
            buf[--i] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    print_string(&buf[i], color);
}

static void print_sdec(int32 value, uint8 color) {
    if (value < 0) {
        print_char('-', color);
        /* -(int64) sidesteps INT32_MIN overflowing back to itself. */
        print_udec((uint64)(-(int64)value), color);
    } else {
        print_udec((uint64)value, color);
    }
}

/* Lowercase, no leading zeros, no 0x prefix - "%x:%x" is meant to read like
 * Linux's own dev_err("%04x:%04x", vendor, device) idiom, and this kernel's
 * only existing hex printers (print_hex/print_hex64) already own the fixed-
 * width "0x"-prefixed form for anyone who wants that instead. */
static void print_hex_trimmed(uint64 value, uint8 color) {
    const char *digits = "0123456789abcdef";
    char buf[17];
    int i = 16;
    buf[i] = '\0';
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value != 0) {
            buf[--i] = digits[value & 0xF];
            value >>= 4;
        }
    }
    print_string(&buf[i], color);
}

/* Zero-pad `value` to `width` hex digits. Separate from print_hex_trimmed
 * rather than a parameter on it, because the trimmed form is what every
 * existing caller wants and this is what real driver source wants.
 *
 * Width support exists because VENDORED DRIVER SOURCE uses it constantly -
 * "%08x" for a register dump is universal - and a format specifier this
 * printed literally came out as the text "0x%08x" in the boot log, which is
 * both useless and looks like a driver bug rather than a printf gap. */
static void print_hex_padded(uint64 value, int width, uint8 color) {
    char buf[17];
    int i = 16;

    buf[16] = '\0';
    if (width > 16) {
        width = 16;
    }
    while (i > 16 - width || value != 0) {
        buf[--i] = "0123456789ABCDEF"[value & 0xF];
        value >>= 4;
        if (i == 0) {
            break;
        }
    }
    print_string(&buf[i], color);
}

void kvprintf(uint8 color, const char *fmt, va_list ap) {
    for (int i = 0; fmt[i] != '\0'; i++) {
        int width = 0;

        if (fmt[i] != '%') {
            print_char(fmt[i], color);
            continue;
        }

        i++;
        /* An optional zero-padded field width: the '0' flag then digits.
         * Only the zero-padded form is accepted - space padding is not
         * something this console needs and half-supporting it would be
         * worse than not offering it. */
        if (fmt[i] == '0') {
            i++;
        }
        /* Digits with or without the leading zero: "%08x" pads and "%6D"
         * counts bytes, and both spell the number the same way. */
        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i] - '0');
            i++;
        }
        if (width > 0 && (fmt[i] == 'x' || fmt[i] == 'X')) {
            print_hex_padded(va_arg(ap, uint32), width, color);
            continue;
        }
        if (width > 0 && fmt[i] == 'l' && (fmt[i + 1] == 'x' ||
                                           fmt[i + 1] == 'X')) {
            i++;
            print_hex_padded(va_arg(ap, uint64), width, color);
            continue;
        }

        /* %*D and %6D - FreeBSD's HEX DUMP: take a pointer and a separator
         * string, and print `width` bytes as hex joined by it. A MAC address
         * is printed as %6D with ":", which is where this comes from -
         * net/if_ethersubr.c's ether_ifattach does exactly that, and without
         * this the boot log said "Ethernet address: %6D".
         *
         * The separator is a SECOND vararg after the pointer, which is what
         * makes this unlike every other specifier here. */
        if (fmt[i] == 'D' || (width > 0 && fmt[i] == 'D')) {
            const unsigned char *p = va_arg(ap, const unsigned char *);
            const char *sep = va_arg(ap, const char *);
            int n = width > 0 ? width : 6;
            int k;

            for (k = 0; k < n; k++) {
                if (k > 0 && sep != 0) {
                    print_string(sep, color);
                }
                print_hex_padded(p[k], 2, color);
            }
            continue;
        }

        switch (fmt[i]) {
            case 'd':
                print_sdec(va_arg(ap, int32), color);
                break;
            case 'u':
                print_udec(va_arg(ap, uint32), color);
                break;
            case 'x':
                print_hex_trimmed(va_arg(ap, uint32), color);
                break;
            case 'l':
                /* Only %lx is recognised - see kprintf.h. Anything else
                 * after 'l' falls through to the default case below and
                 * prints literally, rather than guessing. */
                if (fmt[i + 1] == 'x') {
                    i++;
                    print_hex_trimmed(va_arg(ap, uint64), color);
                } else {
                    print_char('%', color);
                    print_char('l', color);
                }
                break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                print_string(s ? s : "(null)", color);
                break;
            }
            case '%':
                print_char('%', color);
                break;
            case '\0':
                /* Trailing lone '%' - print it and stop, rather than
                 * reading past the end of fmt. */
                print_char('%', color);
                return;
            default:
                /* Unknown conversion: print it back verbatim. A silently
                 * dropped conversion is a harder bug to notice than a
                 * literal "%q" in the output. */
                print_char('%', color);
                print_char(fmt[i], color);
                break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(0x0F, fmt, ap);
    va_end(ap);
}

void kprintf_c(uint8 color, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(color, fmt, ap);
    va_end(ap);
}
