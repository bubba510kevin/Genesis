#ifndef FB_H
#define FB_H

#include "typesk.h"

/* The linear framebuffer - ROADMAP item 14(h).
 *
 * Not a display driver in any GPU sense: the bootloader asked the VESA BIOS
 * for a 32bpp mode with a linear framebuffer (kernel/arch/vbe_boot.c), and
 * this file maps what it got and hands it out. Three consumers:
 *
 *   the kernel     fb_get() - the text console draws into it (screen.c)
 *   user space     /dev/fb0, the Linux fbdev interface: FBIOGET_VSCREENINFO
 *                  and FBIOGET_FSCREENINFO, read/write at an offset, and
 *                  mmap(MAP_SHARED) of the pixels themselves
 *   later, gdi32   through the same mmap, rendering in software - which is
 *                  the whole plan item 14(h) lays out
 *
 * No mode switching after boot: the BIOS is gone by then. FBIOPUT_VSCREENINFO
 * accepts only the mode already set.
 */

typedef struct fb_info {
    uint64 phys;              /* framebuffer physical address              */
    uint8 *virt;              /* its kernel mapping                        */
    uint64 size;              /* pitch * height, rounded up to a page      */
    uint32 width;
    uint32 height;
    uint32 pitch;             /* bytes per scanline, >= width * 4          */
    uint32 bpp;               /* always 32 - the stub selects nothing else */
    uint8  red_pos, red_size;
    uint8  green_pos, green_size;
    uint8  blue_pos, blue_size;
    uint16 vbe_mode;
    uint16 vbe_version;
} fb_info_t;

/* Read the bootloader's block, keep its font, map the framebuffer and switch
 * the console onto it. After kvm_init (it needs kernel VA) and as early as
 * possible after that, so the boot log is visible on screen. A machine left
 * in text mode is not an error: this reports it and returns. */
void fb_init(void);

/* The framebuffer, or NULL if the machine is in text mode. */
const fb_info_t *fb_get(void);

/* The 8x16 BIOS font the bootloader copied (256 glyphs, 16 bytes each, MSB =
 * leftmost pixel), or NULL if it did not get one. */
const uint8 *fb_font(void);

/* A pixel value for an RGB triple in the current mode's layout. */
uint32 fb_rgb(uint8 r, uint8 g, uint8 b);

/* \Device\fb0, reachable as /dev/fb0. After namespace_init. No-op in text
 * mode. */
void fb_register_device(void);

void fb_report(uint8 color);

/* Boot selftest: the bootloader's block, the mode really being active, the
 * kernel mapping, pixel write/readback, the font, the console's glyph
 * rendering, and the /dev/fb0 read and mmap paths. Prints
 * "fb: selftest passed" or "fb: selftest FAILED (n)"; returns the failures.
 * A machine in text mode prints why the test did not run. */
int fb_selftest(void);

/* --- Linux fbdev ABI ----------------------------------------------------- */
#define FBIOGET_VSCREENINFO  0x4600
#define FBIOPUT_VSCREENINFO  0x4601
#define FBIOGET_FSCREENINFO  0x4602

#define FB_TYPE_PACKED_PIXELS  0
#define FB_VISUAL_TRUECOLOR    2

#endif
