#include "screen.h"
#include "io.h"
#include "paging.h"
#include "serial.h"

/* The VGA text buffer is at physical 0xB8000. The kernel linear-maps
 * physical 0-4MB at KERNEL_VMA, so it appears here. Deriving it from
 * KERNEL_VMA rather than hardcoding 0xC00B8000 means this follows
 * automatically if the kernel half ever moves.
 *
 * This is also step 1 of retiring the identity map: it works right now
 * because paging.c maps the low 4MB at BOTH directory entry 0 and entry
 * 768, so both 0xB8000 and 0xC00B8000 resolve. Once screen.c no longer
 * uses the low alias and the GDT has been re-loaded with a high base,
 * entry 0 can be deleted. */
#define VGA_BASE    (KERNEL_VMA + 0xB8000u)
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

static void move_cursor(void);

static uint16 *video = (uint16 *)VGA_BASE;
static int screen_col = 0, screen_row = 0;

static void putc_at(char c, int col, int row, uint8 color) {
    if (col < 0 || col >= VGA_WIDTH || row < 0 || row >= VGA_HEIGHT) {
        return;  /* never write outside the visible page - the VGA aperture
                  * runs to 0xBFFFF, so an overrun scribbles silently into
                  * text pages 1-7 instead of faulting */
    }
    /* (uint8) before (uint16) matters: char is signed on x86, so a byte with
     * the high bit set sign-extends to 0xFFxx and the OR leaves the attribute
     * as 0xFF - white on white - discarding the caller's colour. Only shows
     * up on non-ASCII bytes, which is why it hides for a long time. */
    video[row * VGA_WIDTH + col] = (uint16)(uint8)c | ((uint16)color << 8);
}

/* Shift everything up one row and blank the bottom. Called before each
 * character is written rather than after each row advance: that covers all
 * three ways screen_row can move on (newline, wrap at column 80, and a row
 * left dangling at the end of a previous call) with one check. */
static void scroll_if_needed(uint8 color) {
    if (screen_row < VGA_HEIGHT) {
        return;
    }

    for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++) {
        video[i] = video[i + VGA_WIDTH];
    }
    for (int i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++) {
        video[i] = (uint16)' ' | ((uint16)color << 8);
    }

    screen_row = VGA_HEIGHT - 1;
    screen_col = 0;
}

void print_string(const char *str, uint8 color) {
    /* Mirrored to COM1 before anything else happens, so the transcript is
     * complete even when the screen half of this scrolls the line away or the
     * machine halts mid-function. Colour is dropped: a terminal has its own,
     * and a log file has none. */
    serial_write(str);

    for (int i = 0; str[i] != '\0'; i++) {
        scroll_if_needed(color);

        if (str[i] == '\n') {
            screen_row++;
            screen_col = 0;
            continue;
        }

        putc_at(str[i], screen_col, screen_row, color);
        screen_col++;
        if (screen_col >= VGA_WIDTH) {
            screen_col = 0;
            screen_row++;
        }
    }
    move_cursor();
}

/* --- hardware cursor ----------------------------------------------------
 * Purely cosmetic until there is a prompt, and then suddenly not: a shell
 * with no visible cursor is unusable, because you cannot see where the next
 * character will land or what backspace just removed. The VGA cursor position
 * is a 16-bit cell index written through the CRTC index/data port pair. */
static void move_cursor(void) {
    uint16 pos = (uint16)(screen_row * VGA_WIDTH + screen_col);

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8)((pos >> 8) & 0xFF));
}

void print_char(char c, uint8 color) {
    char buf[2];
    buf[0] = c;
    buf[1] = '\0';
    print_string(buf, color);
}

/* Backspace on a terminal is three characters, not one: step left, overwrite
 * with a space, step left again. The screen version below can simply blank a
 * cell; a serial line has no cells. */
static void serial_backspace(void) {
    serial_write("\b \b");
}

/* Destructive backspace: step back, blank the cell, stay there.
 *
 * Terminals treat plain \b as "move left" and leave the character visible, so
 * the echo path needs this rather than printing a backspace byte. Wrapping to
 * the end of the previous row matters more than it looks - without it, erasing
 * across a line boundary silently stops at column 0 and the line the kernel
 * thinks it holds diverges from the one on screen. */
void print_backspace(uint8 color) {
    if (screen_col == 0) {
        if (screen_row == 0) {
            return;
        }
        screen_row--;
        screen_col = VGA_WIDTH - 1;
    } else {
        screen_col--;
    }
    putc_at(' ', screen_col, screen_row, color);
    move_cursor();
    serial_backspace();
}

void print_hex(uint32 value, uint8 color) {
    const char *digits = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = digits[(value >> (28 - i * 4)) & 0xF];
    }
    buf[10] = '\0';
    print_string(buf, color);
}

void print_hex64(uint64 value, uint8 color) {
    const char *digits = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = digits[(value >> (60 - i * 4)) & 0xF];
    }
    buf[18] = '\0';
    print_string(buf, color);
}

void clear_screen(uint8 color) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        video[i] = (uint16)' ' | ((uint16)color << 8);
    }
    screen_col = 0;
    screen_row = 0;
    move_cursor();
}
