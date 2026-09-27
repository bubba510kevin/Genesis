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

static void putc_at(char c, int col, int row, uint8 color) {
    uint16 v;

    if (col < 0 || col >= screen_cols || row < 0 || row >= screen_rows) {
        return;  /* never write outside the visible page - the VGA aperture
                  * runs to 0xBFFFF, so an overrun scribbles silently into
                  * text pages 1-7 instead of faulting */
    }
    /* (uint8) before (uint16) matters: char is signed on x86, so a byte with
     * the high bit set sign-extends to 0xFFxx and the OR leaves the attribute
     * as 0xFF - white on white - discarding the caller's colour. Only shows
     * up on non-ASCII bytes, which is why it hides for a long time. */
    v = (uint16)(uint8)c | ((uint16)color << 8);
    cells[row * screen_cols + col] = v;
    if (fbi == NULL) {
        video[row * VGA_WIDTH + col] = v;
    } else if (drawing()) {
        render_cell(col, row, 0);
    }
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

void print_string(const char *str, uint8 color) {
    /* Mirrored to COM1 before anything else happens, so the transcript is
     * complete even when the screen half of this scrolls the line away or the
     * machine halts mid-function. Colour is dropped: a terminal has its own,
     * and a log file has none. */
    serial_write(str);
    reclaim_display();

    for (int i = 0; str[i] != '\0'; i++) {
        scroll_if_needed(color);

        if (str[i] == '\n') {
            screen_row++;
            screen_col = 0;
            continue;
        }

        putc_at(str[i], screen_col, screen_row, color);
        screen_col++;
        if (screen_col >= screen_cols) {
            screen_col = 0;
            screen_row++;
        }
    }
    move_cursor();
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
