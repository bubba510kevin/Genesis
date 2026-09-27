#ifndef BOOTVBE_H
#define BOOTVBE_H

/* What the real-mode VBE stub (kernel/arch/vbe_boot.c) leaves behind for the
 * kernel, and where. ROADMAP item 14(h).
 *
 * Shared by the stub - which is assembly inside a C file, so everything here
 * is a plain #define usable in both - and by kernel/dev/fb.c, which reads the
 * block back through the direct map.
 *
 * --- Low-memory placement ------------------------------------------------
 * Beside the E820 buffer boot.asm already leaves at 0x5000 (32 entries end at
 * 0x5308), in the conventional memory below the boot sector's stack at
 * 0x7C00. flk.c reserves physical 0 through the end of the kernel image
 * before the PMM hands anything out, so all of it survives until fb_init
 * copies what it needs.
 *
 *   0x1000 - 0x1FFF   BOOT_VBE_FONT     the BIOS 8x16 font, 256 glyphs
 *   0x5400 - 0x543F   BOOT_VBE_INFO     the block described below
 *   0x5440 - 0x544F   BOOT_VBE_SCRATCH  the stub's own loop state
 *   0x5600 - 0x57FF   BOOT_VBE_CTRL     VBE controller info (4F00h output)
 *   0x5800 - 0x58FF   BOOT_VBE_MODEINFO VBE mode info (4F01h output)
 *
 * The font is kept well away from 0x7C00: the BIOS runs INT 10h on the boot
 * sector's stack, which grows down from there.
 */

#define BOOT_VBE_FONT      0x1000
#define BOOT_VBE_INFO      0x5400
#define BOOT_VBE_SCRATCH   0x5440
#define BOOT_VBE_CTRL      0x5600
#define BOOT_VBE_MODEINFO  0x5800

/* "GFB1". Written first, with every status bit clear, so a block that holds
 * the magic always holds a truthful status - and a block that does not was
 * never written by this boot. */
#define BOOT_VBE_MAGIC     0x31424647

/* Status bits (offset 4). */
#define BOOT_VBE_FONT_OK   0x0001   /* 4096 bytes at BOOT_VBE_FONT are a font */
#define BOOT_VBE_MODE_SET  0x0002   /* a linear-framebuffer mode is active    */
#define BOOT_VBE_PRESENT   0x0004   /* the BIOS answered 4F00h at all         */

/* The image head (vbe_boot.c): where the request words and the stub sit
 * relative to KERNEL_LMA. boot.asm's far call targets 0xFFFF:(0x10 + STUB). */
#define BOOT_VBE_REQ_OFFSET   8
#define BOOT_VBE_STUB_OFFSET  0x10

/* The largest mode the stub accepts unless build.py is told otherwise
 * (GENESIS_VBE=WxH, or GENESIS_VBE=off for text mode). */
#define BOOT_VBE_DEFAULT_W    1024
#define BOOT_VBE_DEFAULT_H    768

#define BOOT_STR_(x) #x
#define BOOT_STR(x)  BOOT_STR_(x)

#ifndef __ASSEMBLER__
#include "typesk.h"

/* The block at BOOT_VBE_INFO. Offsets are fixed by the stub, which writes
 * them by number; the _Static_assert below keeps this struct honest. */
typedef struct __attribute__((packed)) boot_vbe_info {
    uint32 magic;           /*  0 BOOT_VBE_MAGIC                          */
    uint16 status;          /*  4 BOOT_VBE_* bits                         */
    uint16 mode;            /*  6 the VBE mode number that was set        */
    uint16 width;           /*  8                                          */
    uint16 height;          /* 10                                          */
    uint16 pitch;           /* 12 bytes per scanline                       */
    uint8  bpp;             /* 14                                          */
    uint8  memory_model;    /* 15 6 = direct colour                        */
    uint32 fb_phys;         /* 16 linear framebuffer physical address      */
    uint8  red_size;        /* 20                                          */
    uint8  red_pos;         /* 21                                          */
    uint8  green_size;      /* 22                                          */
    uint8  green_pos;       /* 23                                          */
    uint8  blue_size;       /* 24                                          */
    uint8  blue_pos;        /* 25                                          */
    uint16 vbe_version;     /* 26 BCD, 0x0300 for VBE 3.0                  */
    uint16 vram_64k;        /* 28 total video memory, 64KB units           */
    uint8  reserved[34];    /* 30 .. 63                                    */
} boot_vbe_info_t;

_Static_assert(sizeof(boot_vbe_info_t) == 64, "boot_vbe_info_t layout");
#endif

#endif
