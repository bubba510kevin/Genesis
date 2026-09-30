#include "screen.h"
#include "fb.h"
#include "io.h"
#include "paging.h"
#include "process.h"
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

/* --- two back ends, one grid ---------------------------------------------
 * ROADMAP item 14(h) put the screen into a graphics mode, and a graphics
 * mode has no text buffer: the console now DRAWS its characters, 8x16 pixels
 * each in the BIOS font, into the linear framebuffer.
 *
 * So the grid of cells is kept here, in `cells`, whatever the back end - one
 * 16-bit VGA-format cell (character | attribute << 8) per position. In text
 * mode each change is also written to the VGA buffer, exactly as before. In
 * framebuffer mode it is drawn, and `drawn` records what each cell on the
 * screen currently shows, so that a scroll repaints only the cells whose
 * content actually changed. That matters on real hardware, where video memory
 * is uncached and a full-screen copy per newline is the slow part of a boot.
 *
 * The grid grows to fit the mode - 128x48 at 1024x768 - and the text-mode
 * contents printed before the framebuffer came up are carried across. */
#define MAX_COLS 240
#define MAX_ROWS 100
#define CELL_INVALID 0xFFFFu

static void move_cursor(void);

static uint16 *video = (uint16 *)VGA_BASE;
static uint16  cells[MAX_COLS * MAX_ROWS];
static uint16  drawn[MAX_COLS * MAX_ROWS];
static int screen_cols = VGA_WIDTH, screen_rows = VGA_HEIGHT;
static int screen_col = 0, screen_row = 0;

static const fb_info_t *fbi;         /* non-NULL once the console draws    */
static const uint8     *fbfont;
static uint32           palette[16];
static int              cursor_col = -1, cursor_row = -1;

/* A program that owns the display (KDSETMODE KD_GRAPHICS) - see
 * screen_set_graphics. The grid keeps updating; nothing is drawn. */
static int gfx_owner;

/* The sixteen VGA colours, as the attribute nibble indexes them. */
static const uint8 vga_rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00},
    {0x00, 0xAA, 0xAA}, {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA},
    {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA}, {0x55, 0x55, 0x55},
    {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
    {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55},
    {0xFF, 0xFF, 0xFF},
};

int screen_width_chars(void)  { return screen_cols; }
int screen_height_chars(void) { return screen_rows; }

static int drawing(void) {
    return fbi != NULL && gfx_owner == 0;
}

/* One cell, into the framebuffer. `force` repaints even when `drawn` says
 * the screen already shows this - used to erase the cursor. */
static void render_cell(int col, int row, int force) {
    uint32 idx = (uint32)(row * screen_cols + col);
    uint16 v = cells[idx];
    const uint8 *glyph;
    uint32 fg, bg;
    int y, x;

    if (!force && drawn[idx] == v) {
        return;
    }
    glyph = fbfont + (uint32)(v & 0xFF) * 16;
    fg = palette[(v >> 8) & 0x0F];
    bg = palette[(v >> 12) & 0x0F];
    for (y = 0; y < 16; y++) {
        volatile uint32 *px = (volatile uint32 *)
            (fbi->virt + (uint64)(row * 16 + y) * fbi->pitch +
             (uint64)col * 8 * 4);
        uint8 bits = glyph[y];

        for (x = 0; x < 8; x++) {
            px[x] = (bits & (0x80 >> x)) ? fg : bg;
        }
    }
    drawn[idx] = v;
}

/* Everything whose content differs from what is on the screen. */
static void flush_cells(void) {
    int r, c;

    for (r = 0; r < screen_rows; r++) {
        for (c = 0; c < screen_cols; c++) {
            render_cell(c, r, 0);
        }
    }
}

static void invalidate_all(void) {
    int i;

    for (i = 0; i < screen_cols * screen_rows; i++) {
        drawn[i] = CELL_INVALID;
    }
    cursor_col = -1;
    cursor_row = -1;
}

/* One cell's raw value (character | attribute << 8), wherever the grid is
 * shown. putc_at and the terminal's shifting operations both come here. */
static int t_quiet;   /* the terminal selftest: update the grid, show nothing */

static void set_cell(int col, int row, uint16 v) {
    if (col < 0 || col >= screen_cols || row < 0 || row >= screen_rows) {
        return;  /* never write outside the visible page - the VGA aperture
                  * runs to 0xBFFFF, so an overrun scribbles silently into
                  * text pages 1-7 instead of faulting */
    }
    cells[row * screen_cols + col] = v;
    if (t_quiet) {
        return;
    }
    if (fbi == NULL) {
        video[row * VGA_WIDTH + col] = v;
    } else if (drawing()) {
        render_cell(col, row, 0);
    }
}

static void putc_at(char c, int col, int row, uint8 color) {
    /* (uint8) before (uint16) matters: char is signed on x86, so a byte with
     * the high bit set sign-extends to 0xFFxx and the OR leaves the attribute
     * as 0xFF - white on white - discarding the caller's colour. Only shows
     * up on non-ASCII bytes, which is why it hides for a long time. */
    set_cell(col, row, (uint16)(uint8)c | ((uint16)color << 8));
}

/* Shift everything up one row and blank the bottom. Called before each
 * character is written rather than after each row advance: that covers all
 * three ways screen_row can move on (newline, wrap at the last column, and a
 * row left dangling at the end of a previous call) with one check. */
static void scroll_if_needed(uint8 color) {
    int i, n = screen_cols * screen_rows;

    if (screen_row < screen_rows) {
        return;
    }

    for (i = 0; i < n - screen_cols; i++) {
        cells[i] = cells[i + screen_cols];
    }
    for (i = n - screen_cols; i < n; i++) {
        cells[i] = (uint16)' ' | ((uint16)color << 8);
    }
    if (fbi == NULL) {
        for (i = 0; i < n; i++) {
            video[i] = cells[i];
        }
    } else if (drawing()) {
        flush_cells();
    }

    screen_row = screen_rows - 1;
    screen_col = 0;
}

/* A display owner that exited without handing the screen back gets it taken
 * back here, on the next line of console output - otherwise a program that
 * crashes in graphics mode leaves the console invisible for good. */
static void reclaim_display(void) {
    process_t *p;

    if (gfx_owner == 0) {
        return;
    }
    p = proc_find(gfx_owner);
    if (p == NULL || p->state == PROC_ZOMBIE || p->state == PROC_UNUSED) {
        screen_set_graphics(0, 0);
    }
}

/* --- the terminal -----------------------------------------------------------
 *
 * The screen is a TERMINAL, not a printer: what programs write to the console
 * includes control characters and escape sequences, and until this existed
 * every one of them was drawn as a glyph. readline erases a character with
 * "\b \b" and moves along a line with \r, \b and ESC [ C/K; bash's Backspace
 * put the font's glyph for 0x08 on the screen instead of taking a character
 * back. (The serial side never had the problem - it passes bytes through to
 * a real terminal emulator, which interprets them.)
 *
 * A subset of the Linux console (TERM=linux), which is what programs are told
 * this is: \r \b \t, BEL ignored, other C0 controls dropped; ESC 7 / ESC 8
 * (save/restore cursor), ESC c (reset); and CSI sequences - cursor movement
 * A B C D G d H f, erase K J X, delete/insert characters P @, save/restore
 * s u, and SGR colours (m). Private-mode sequences (ESC [ ? ...) are parsed
 * and ignored: the cursor is always shown.
 *
 * The parser's state is static because a sequence can be split across
 * writes - console_write hands this 64 bytes at a time. */
enum { T_NORMAL, T_ESC, T_CSI };
#define T_MAXPARAM 8

static int    t_state = T_NORMAL;
static int    t_param[T_MAXPARAM];
static int    t_nparam;
static int    t_private;
static int    t_sgr = -1;          /* attribute set by SGR, -1 = the caller's */
static int    t_saved_col, t_saved_row;

static uint8 t_color(uint8 color) {
    return t_sgr >= 0 ? (uint8)t_sgr : color;
}

/* The cursor's row with a pending scroll resolved to the last row: after a
 * line fills, screen_row can be one past the end until the next character. */
static void t_clamp(void) {
    if (screen_row >= screen_rows) {
        screen_row = screen_rows - 1;
    }
    if (screen_row < 0) {
        screen_row = 0;
    }
    if (screen_col >= screen_cols) {
        screen_col = screen_cols - 1;
    }
    if (screen_col < 0) {
        screen_col = 0;
    }
}

static void t_erase(int from, int to, uint8 color) {    /* [from, to) cells */
    int i;

    for (i = from; i < to; i++) {
        putc_at(' ', i % screen_cols, i / screen_cols, color);
    }
}

/* SGR: 0 reset, 1 bright, 7 reverse, 30-37/90-97 foreground, 40-47
 * background, 39/49 default. Anything else is ignored rather than guessed. */
static void t_sgr_apply(uint8 color) {
    int i, a = t_sgr >= 0 ? t_sgr : color;
    static const uint8 ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

    if (t_nparam == 0) {
        t_sgr = -1;
        return;
    }
    for (i = 0; i < t_nparam; i++) {
        int p = t_param[i];

        if (p == 0) {
            t_sgr = -1;
            a = color;
            continue;
        }
        if (p == 1) {
            a |= 0x08;
        } else if (p == 7) {
            a = ((a & 0x0F) << 4) | ((a >> 4) & 0x0F);
        } else if (p >= 30 && p <= 37) {
            a = (a & 0xF8) | ansi_to_vga[p - 30];
        } else if (p >= 90 && p <= 97) {
            a = (a & 0xF0) | 0x08 | ansi_to_vga[p - 90];
        } else if (p == 39) {
            a = (a & 0xF0) | (color & 0x0F);
        } else if (p >= 40 && p <= 47) {
            a = (a & 0x0F) | (ansi_to_vga[p - 40] << 4);
        } else if (p == 49) {
            a = (a & 0x0F) | (color & 0xF0);
        } else {
            continue;
        }
        t_sgr = a;
    }
}

static void t_csi(char final, uint8 color) {
    int p0 = t_nparam > 0 ? t_param[0] : 0;
    int n = p0 > 0 ? p0 : 1;
    int i, row_base;
    uint8 c = t_color(color);

    if (t_private) {
        return;                     /* ESC [ ? ... : modes, ignored */
    }
    if (final == 'm') {
        t_sgr_apply(color);
        return;
    }
    t_clamp();
    row_base = screen_row * screen_cols;
    switch (final) {
        case 'A': screen_row -= n;             break;
        case 'B': screen_row += n;             break;
        case 'C': screen_col += n;             break;
        case 'D': screen_col -= n;             break;
        case 'G': screen_col = n - 1;          break;
        case 'd': screen_row = n - 1;          break;
        case 'H':
        case 'f':
            screen_row = n - 1;
            screen_col = (t_nparam > 1 && t_param[1] > 0) ? t_param[1] - 1 : 0;
            break;
        case 'K':
            if (p0 == 0) {
                t_erase(row_base + screen_col, row_base + screen_cols, c);
            } else if (p0 == 1) {
                t_erase(row_base, row_base + screen_col + 1, c);
            } else {
                t_erase(row_base, row_base + screen_cols, c);
            }
            break;
        case 'J':
            if (p0 == 0) {
                t_erase(row_base + screen_col, screen_rows * screen_cols, c);
            } else if (p0 == 1) {
                t_erase(0, row_base + screen_col + 1, c);
            } else {
                t_erase(0, screen_rows * screen_cols, c);
            }
            break;
        case 'X':
            t_erase(row_base + screen_col,
                    row_base + (screen_col + n < screen_cols ? screen_col + n
                                                             : screen_cols), c);
            break;
        case 'P':                   /* delete n characters, pull the rest left */
            for (i = screen_col; i < screen_cols; i++) {
                set_cell(i, screen_row, i + n < screen_cols
                         ? cells[row_base + i + n]
                         : (uint16)' ' | ((uint16)c << 8));
            }
            break;
        case '@':                   /* insert n blanks, push the rest right */
            for (i = screen_cols - 1; i >= screen_col; i--) {
                set_cell(i, screen_row, i - n >= screen_col
                         ? cells[row_base + i - n]
                         : (uint16)' ' | ((uint16)c << 8));
            }
            break;
        case 's':
            t_saved_col = screen_col;
            t_saved_row = screen_row;
            break;
        case 'u':
            screen_col = t_saved_col;
            screen_row = t_saved_row;
            break;
        default:
            break;                  /* unknown: consumed, not drawn */
    }
    t_clamp();
}

/* One byte of output onto the grid. */
static void t_putc(char ch, uint8 color) {
    uint8 b = (uint8)ch;

    if (t_state == T_ESC) {
        t_state = T_NORMAL;
        if (b == '[') {
            t_state = T_CSI;
            t_nparam = 0;
            t_private = 0;
            t_param[0] = 0;
        } else if (b == '7') {
            t_saved_col = screen_col;
            t_saved_row = screen_row;
        } else if (b == '8') {
            screen_col = t_saved_col;
            screen_row = t_saved_row;
            t_clamp();
        } else if (b == 'c') {
            t_sgr = -1;
            t_erase(0, screen_rows * screen_cols, color);
            screen_col = 0;
            screen_row = 0;
        }
        return;
    }
    if (t_state == T_CSI) {
        if (b >= '0' && b <= '9') {
            if (t_nparam == 0) {
                t_nparam = 1;
            }
            t_param[t_nparam - 1] = t_param[t_nparam - 1] * 10 + (b - '0');
            return;
        }
        if (b == ';') {
            if (t_nparam == 0) {
                t_nparam = 1;
            }
            if (t_nparam < T_MAXPARAM) {
                t_param[t_nparam++] = 0;
            }
            return;
        }
        if (b == '?' || b == '>' || b == '=') {
            t_private = 1;
            return;
        }
        if (b >= 0x40 && b <= 0x7E) {
            t_state = T_NORMAL;
            t_csi((char)b, color);
            return;
        }
        if (b < 0x20 || b > 0x7E) {
            t_state = T_NORMAL;     /* malformed: drop the sequence */
        }
        return;
    }

    switch (b) {
        case 0x1B:
            t_state = T_ESC;
            return;
        case '\n':
            scroll_if_needed(t_color(color));
            screen_row++;
            screen_col = 0;
            return;
        case '\r':
            if (screen_row >= screen_rows) {
                scroll_if_needed(t_color(color));
            }
            screen_col = 0;
            return;
        case '\b':
            if (screen_col > 0) {
                screen_col--;
            }
            return;
        case '\t':
            scroll_if_needed(t_color(color));
            screen_col = (screen_col + 8) & ~7;
            if (screen_col >= screen_cols) {
                screen_col = screen_cols - 1;
            }
            return;
        default:
            break;
    }
    if (b < 0x20 || b == 0x7F) {
        return;                     /* BEL and the other controls: not drawn */
    }
    scroll_if_needed(t_color(color));
    putc_at(ch, screen_col, screen_row, t_color(color));
    screen_col++;
    if (screen_col >= screen_cols) {
        screen_col = 0;
        screen_row++;
    }
}

static void con_write(const char *str, uint8 color) {
    int i;

    for (i = 0; str[i] != '\0'; i++) {
        t_putc(str[i], color);
    }
    move_cursor();
}

void print_string(const char *str, uint8 color) {
    /* Mirrored to COM1 before anything else happens, so the transcript is
     * complete even when the screen half of this scrolls the line away or the
     * machine halts mid-function. Colour is dropped: a terminal has its own,
     * and a log file has none. */
    serial_write(str);
    reclaim_display();
    con_write(str, color);
}

/* --- cursor ---------------------------------------------------------------
 * Purely cosmetic until there is a prompt, and then suddenly not: a shell
 * with no visible cursor is unusable, because you cannot see where the next
 * character will land or what backspace just removed.
 *
 * Text mode: the VGA cursor position is a 16-bit cell index written through
 * the CRTC index/data port pair. Framebuffer mode: an underline in the cell's
 * bottom two scanlines, erased by repainting the cell when it moves. */
static void move_cursor(void) {
    int col = screen_col, row = screen_row;

    if (fbi == NULL) {
        uint16 pos = (uint16)(row * VGA_WIDTH + col);

        outb(0x3D4, 0x0F);
        outb(0x3D5, (uint8)(pos & 0xFF));
        outb(0x3D4, 0x0E);
        outb(0x3D5, (uint8)((pos >> 8) & 0xFF));
        return;
    }
    if (!drawing()) {
        return;
    }
    if (row >= screen_rows) {
        row = screen_rows - 1;          /* the next character scrolls first */
    }
    if (col >= screen_cols) {
        col = screen_cols - 1;
    }
    if (col == cursor_col && row == cursor_row) {
        return;
    }
    if (cursor_col >= 0) {
        render_cell(cursor_col, cursor_row, 1);
    }
    {
        uint32 idx = (uint32)(row * screen_cols + col);
        uint32 fg = palette[(cells[idx] >> 8) & 0x0F];
        int y, x;

        if (fg == palette[(cells[idx] >> 12) & 0x0F]) {
            fg = palette[7];            /* a blank cell's own colours match */
        }
        for (y = 14; y < 16; y++) {
            volatile uint32 *px = (volatile uint32 *)
                (fbi->virt + (uint64)(row * 16 + y) * fbi->pitch +
                 (uint64)col * 8 * 4);
            for (x = 0; x < 8; x++) {
                px[x] = fg;
            }
        }
        drawn[idx] = CELL_INVALID;      /* the screen no longer shows cells[] */
    }
    cursor_col = col;
    cursor_row = row;
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
        screen_col = screen_cols - 1;
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
    for (int i = 0; i < screen_cols * screen_rows; i++) {
        cells[i] = (uint16)' ' | ((uint16)color << 8);
        if (fbi == NULL) {
            video[i] = cells[i];
        }
    }
    screen_col = 0;
    screen_row = 0;
    if (drawing()) {
        flush_cells();
    }
    move_cursor();
}

/* --- the framebuffer back end ------------------------------------------- */

void screen_attach_fb(const fb_info_t *info, const uint8 *font) {
    int new_cols = (int)(info->width / 8);
    int new_rows = (int)(info->height / 16);
    int r, c, i;

    if (new_cols > MAX_COLS) new_cols = MAX_COLS;
    if (new_rows > MAX_ROWS) new_rows = MAX_ROWS;
    if (new_cols < VGA_WIDTH || new_rows < VGA_HEIGHT) {
        return;             /* smaller than 80x25: stay a (blind) text grid */
    }

    /* Re-lay the 80x25 grid into the wider one, top-left, in place: walking
     * backwards, because every cell moves to an index at or above its old
     * one. The rest is blank. */
    for (r = new_rows - 1; r >= 0; r--) {
        for (c = new_cols - 1; c >= 0; c--) {
            uint16 v = 0x0720;

            if (r < screen_rows && c < screen_cols) {
                v = cells[r * screen_cols + c];
            }
            cells[r * new_cols + c] = v;
        }
    }
    screen_cols = new_cols;
    screen_rows = new_rows;

    fbfont = font;
    fbi    = info;
    for (i = 0; i < 16; i++) {
        palette[i] = fb_rgb(vga_rgb[i][0], vga_rgb[i][1], vga_rgb[i][2]);
    }
    invalidate_all();
    flush_cells();
    move_cursor();
}

void screen_set_graphics(int on, int owner_pid) {
    if (on) {
        gfx_owner = owner_pid > 0 ? owner_pid : -1;
        return;
    }
    if (gfx_owner == 0) {
        return;
    }
    gfx_owner = 0;
    if (fbi != NULL) {
        invalidate_all();
        flush_cells();
        move_cursor();
    }
}

int screen_graphics_owner(void) {
    return gfx_owner;
}

/* The terminal interpreter, checked against the Linux console's behaviour on
 * the bottom row, with t_quiet set so the screen never shows it and the row
 * put back afterwards. Returns the number of failed checks.
 *
 * Each check is something a program actually sends: readline's erase is
 * "\b \b" and its redisplay uses \r, CSI K, CSI D/C, CSI P and CSI @. */
static void t_feed(const char *s) {
    while (*s != '\0') {
        t_putc(*s++, 0x07);
    }
}

static int t_expect(int row, const char *want, int col) {
    int i, bad = 0;

    for (i = 0; want[i] != '\0'; i++) {
        if ((char)(cells[row * screen_cols + i] & 0xFF) != want[i]) {
            bad++;
        }
    }
    if (screen_col != col || screen_row != row) {
        bad++;
    }
    return bad;
}

int screen_term_selftest(void) {
    static uint16 saved[MAX_COLS];
    int row = screen_rows - 1, col = screen_col, srow = screen_row;
    int sgr = t_sgr, scol = t_saved_col, ssrow = t_saved_row;
    int i, bad = 0;
    char pos[16];
    int n = row + 1, k = 0;

    for (i = 0; i < screen_cols; i++) {
        saved[i] = cells[row * screen_cols + i];
    }
    /* ESC [ <last row> ; 1 H */
    pos[k++] = 0x1B;
    pos[k++] = '[';
    if (n >= 100) pos[k++] = (char)('0' + n / 100);
    if (n >= 10)  pos[k++] = (char)('0' + (n / 10) % 10);
    pos[k++] = (char)('0' + n % 10);
    pos[k++] = ';';
    pos[k++] = '1';
    pos[k++] = 'H';
    pos[k] = '\0';

    t_quiet = 1;
    t_state = T_NORMAL;
    t_sgr = -1;

    t_feed(pos);
    t_feed("\x1b[2K");
    bad += t_expect(row, "        ", 0);           /* CUP, then EL 2 */
    t_feed("abc\b\bX");
    bad += t_expect(row, "aXc", 2);                /* \b moves, does not draw */
    t_feed("\r\x1b[K");
    bad += t_expect(row, "   ", 0);                /* CR, EL 0 */
    t_feed("hello\x1b[3D\x1b[P");
    bad += t_expect(row, "helo ", 2);              /* CUB, DCH */
    t_feed("\x1b[2@");
    bad += t_expect(row, "he  lo", 2);             /* ICH */
    t_feed("\x1b[C\x1b" "[1C");
    bad += t_expect(row, "he  lo", 4);             /* CUF, default and 1 */
    t_feed("\r\x1b[K\tZ");
    bad += t_expect(row, "        Z", 9);          /* HT to column 8 */
    t_feed("\x1b[31mR\x1b[0m\x07");
    bad += t_expect(row, "        ZR", 10);        /* SGR, BEL not drawn */
    if (((cells[row * screen_cols + 9] >> 8) & 0x0F) != 4 || t_sgr != -1) {
        bad++;                                     /* red, then reset */
    }
    t_feed("\x1b[?25l");
    bad += t_expect(row, "        ZR ", 10);       /* private mode: ignored */

    for (i = 0; i < screen_cols; i++) {
        cells[row * screen_cols + i] = saved[i];
    }
    t_quiet = 0;
    t_state = T_NORMAL;
    t_sgr = sgr;
    t_saved_col = scol;
    t_saved_row = ssrow;
    screen_col = col;
    screen_row = srow;
    return bad;
}

/* Draw one known cell and compare every pixel with the font - the console's
 * renderer checked against its own source, rather than trusted because the
 * boot log looks right on a screen nobody is watching. Uses the bottom-right
 * cell and puts it back afterwards. */
int screen_fb_selftest(void) {
    int col, row, y, x, bad = 0;
    uint32 idx;
    uint16 save;
    const uint8 *glyph;
    uint32 fg, bg;

    if (fbi == NULL) {
        return 1;
    }
    col = screen_cols - 1;
    row = screen_rows - 1;
    idx = (uint32)(row * screen_cols + col);
    save = cells[idx];

    cells[idx] = (uint16)'G' | (0x1E << 8);     /* yellow on blue */
    render_cell(col, row, 1);
    glyph = fbfont + 'G' * 16;
    fg = palette[0x0E];
    bg = palette[0x01];
    for (y = 0; y < 16; y++) {
        const volatile uint32 *px = (const volatile uint32 *)
            (fbi->virt + (uint64)(row * 16 + y) * fbi->pitch +
             (uint64)col * 8 * 4);
        for (x = 0; x < 8; x++) {
            uint32 want = (glyph[y] & (0x80 >> x)) ? fg : bg;

            if (px[x] != want) {
                bad++;
            }
        }
    }
    cells[idx] = save;
    if (drawing()) {
        render_cell(col, row, 1);
        if (col == cursor_col && row == cursor_row) {
            cursor_col = -1;            /* redraw the cursor over it */
            move_cursor();
        }
    }
    return bad;
}
