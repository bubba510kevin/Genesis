#ifndef SCREEN_H
#define SCREEN_H

#include "typesk.h"

void print_string(const char *str, uint8 color);

void print_hex(uint32 value, uint8 color);

void clear_screen(uint8 color);

void print_hex64(uint64 value, uint8 color);

/* One character, and a destructive backspace. Both exist for the terminal
 * echo path in keyboard.c; print_string cannot express either. */
void print_char(char c, uint8 color);
void print_backspace(uint8 color);

/* The console grid, in characters: 80x25 in VGA text mode, and whatever
 * the framebuffer fits at 8x16 once it is drawing there (128x48 at
 * 1024x768). What TIOCGWINSZ reports. */
int screen_width_chars(void);
int screen_height_chars(void);

/* Move the console onto the linear framebuffer (fb.c calls this once the
 * framebuffer is mapped). What was on the text screen is carried across. */
struct fb_info;
void screen_attach_fb(const struct fb_info *info, const uint8 *font);

/* Hand the display to a program, or take it back - Linux's KDSETMODE
 * KD_GRAPHICS / KD_TEXT. While a program owns it the console keeps its grid
 * (and serial keeps everything) but draws nothing; taking it back repaints
 * the whole screen. An owner that exits without handing it back loses it at
 * the next console output. `owner_pid` is recorded for exactly that. */
void screen_set_graphics(int on, int owner_pid);

/* The pid that owns the display, or 0 when the console does. */
int screen_graphics_owner(void);

/* The framebuffer console's glyph renderer, checked pixel by pixel against
 * the font. Returns the number of wrong pixels (1 if there is no
 * framebuffer console at all). Called by fb_selftest. */
int screen_fb_selftest(void);

/* The console's terminal interpreter (control characters, CSI sequences),
 * checked on the bottom row without showing anything. 0 if it behaved like
 * the Linux console. */
int screen_term_selftest(void);

#endif