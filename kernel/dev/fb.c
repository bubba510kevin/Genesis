#include "bootvbe.h"
#include "device.h"
#include "fb.h"
#include "io.h"
#include "kprintf.h"
#include "paging.h"
#include "pmm.h"
#include "screen.h"
#include "vmalloc.h"

/* The linear framebuffer - see fb.h. ROADMAP item 14(h). */

static fb_info_t fb;
static int       fb_live;

/* Kept here rather than read from low memory every time: the block and the
 * font sit in frames flk.c reserves, but nothing promises they stay
 * reserved, and a console drawing from memory the PMM later hands out is a
 * console that draws garbage the day that changes. */
static boot_vbe_info_t boot_block;
static int             boot_block_valid;
static uint8           font[4096];
static int             font_valid;

const fb_info_t *fb_get(void) {
    return fb_live ? &fb : NULL;
}

const uint8 *fb_font(void) {
    return font_valid ? font : NULL;
}

/* One channel: the top `size` bits of the 8-bit value, at `pos`. */
static uint32 channel(uint8 v, uint8 size, uint8 pos) {
    if (size == 0) {
        return 0;
    }
    if (size > 8) {
        size = 8;
    }
    return ((uint32)v >> (8 - size)) << pos;
}

uint32 fb_rgb(uint8 r, uint8 g, uint8 b) {
    return channel(r, fb.red_size, fb.red_pos) |
           channel(g, fb.green_size, fb.green_pos) |
           channel(b, fb.blue_size, fb.blue_pos);
}

/* The two words build.py wrote into the image head: the largest mode the
 * stub was allowed to pick, width 0 meaning "stay in text mode". Read from
 * the kernel's own mapping of its image - the head is at KERNEL_LMA, and the
 * kernel window maps physical 0 upward at KERNEL_VMA. */
static void requested_mode(uint16 *w, uint16 *h) {
    const volatile uint16 *req = (const volatile uint16 *)
        (KERNEL_VMA + 0x100000ULL + BOOT_VBE_REQ_OFFSET);

    *w = req[0];
    *h = req[1];
}

void fb_init(void) {
    const boot_vbe_info_t *blk =
        (const boot_vbe_info_t *)phys_to_virt(BOOT_VBE_INFO);
    uint64 bytes, off, va;
    uint16 rw, rh;

    requested_mode(&rw, &rh);
    if (blk->magic != BOOT_VBE_MAGIC) {
        kprintf_c(0x0E, "fb: no VBE block from the bootloader - text mode\n");
        return;
    }
    boot_block = *blk;
    boot_block_valid = 1;

    if (boot_block.status & BOOT_VBE_FONT_OK) {
        const uint8 *src = (const uint8 *)phys_to_virt(BOOT_VBE_FONT);
        int i;

        for (i = 0; i < 4096; i++) {
            font[i] = src[i];
        }
        font_valid = 1;
    }

    if (!(boot_block.status & BOOT_VBE_MODE_SET)) {
        if (rw == 0) {
            kprintf_c(0x0F, "fb: text mode requested (GENESIS_VBE=off)\n");
        } else if (!(boot_block.status & BOOT_VBE_PRESENT)) {
            kprintf_c(0x0E, "fb: the BIOS has no VBE - text mode\n");
        } else {
            kprintf_c(0x0E, "fb: no 32bpp linear mode up to %ux%u - "
                            "text mode\n", (uint32)rw, (uint32)rh);
        }
        return;
    }

    /* Anything the console cannot draw into is refused here, once, rather
     * than checked on every pixel. The stub only selects 32bpp direct
     * colour, so reaching this means the block and the stub disagree. */
    if (boot_block.bpp != 32 || boot_block.width == 0 ||
        boot_block.height == 0 ||
        boot_block.pitch < (uint32)boot_block.width * 4 ||
        boot_block.fb_phys == 0 || (boot_block.fb_phys & 0xFFF) != 0) {
        kprintf_c(0x0C, "fb: VBE block is inconsistent (%ux%u bpp %u pitch "
                        "%u at %lx) - not using it\n",
                  (uint32)boot_block.width, (uint32)boot_block.height,
                  (uint32)boot_block.bpp, (uint32)boot_block.pitch,
                  (uint64)boot_block.fb_phys);
        return;
    }

    fb.phys        = boot_block.fb_phys;
    fb.width       = boot_block.width;
    fb.height      = boot_block.height;
    fb.pitch       = boot_block.pitch;
    fb.bpp         = boot_block.bpp;
    fb.red_pos     = boot_block.red_pos;
    fb.red_size    = boot_block.red_size;
    fb.green_pos   = boot_block.green_pos;
    fb.green_size  = boot_block.green_size;
    fb.blue_pos    = boot_block.blue_pos;
    fb.blue_size   = boot_block.blue_size;
    fb.vbe_mode    = boot_block.mode;
    fb.vbe_version = boot_block.vbe_version;

    bytes   = (uint64)fb.pitch * fb.height;
    fb.size = (bytes + PMM_PAGE_SIZE - 1) & ~(uint64)(PMM_PAGE_SIZE - 1);

    /* Above RAM on every machine this runs on (0xFD000000 under QEMU), so
     * it is not in the direct map: it gets its own kernel VA, the way the
     * LAPIC's register page does. Mapped PCD - uncached unless the firmware
     * made the range write-combining (see PAGE_PCD in paging.h). */
    va = kvm_alloc_range(fb.size, PMM_PAGE_SIZE);
    if (va == 0) {
        kprintf_c(0x0C, "fb: no kernel VA for %lx bytes of framebuffer\n",
                  fb.size);
        return;
    }
    for (off = 0; off < fb.size; off += PMM_PAGE_SIZE) {
        if (!vmm_map_page((virt_addr_t)(va + off), fb.phys + off,
                          PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_NX)) {
            kprintf_c(0x0C, "fb: could not map the framebuffer\n");
            return;
        }
    }
    fb.virt = (uint8 *)va;
    fb_live = 1;

    /* The console moves onto the framebuffer now. Before a font there is
     * nothing to draw text with, and the screen stays blank (serial still
     * has everything) - said out loud rather than discovered. */
    if (font_valid) {
        screen_attach_fb(&fb, font);
    } else {
        kprintf_c(0x0E, "fb: no BIOS font - the console stays on serial "
                        "only\n");
    }
    kprintf_c(0x0F, "fb: %ux%ux%u, pitch %u, at %lx (VBE mode %x)\n",
              fb.width, fb.height, fb.bpp, fb.pitch, fb.phys,
              (uint32)fb.vbe_mode);
}

void fb_report(uint8 color) {
    if (!fb_live) {
        kprintf_c(color, "fb: none (VGA text console)\n");
        return;
    }
    kprintf_c(color, "fb: /dev/fb0 %ux%u 32bpp, 0x%lx bytes, VBE %x, "
                     "rgb at %u/%u/%u\n",
              fb.width, fb.height, fb.size, (uint32)fb.vbe_version,
              (uint32)fb.red_pos, (uint32)fb.green_pos, (uint32)fb.blue_pos);
}

/* --- /dev/fb0 ------------------------------------------------------------ */

static int64 fbdev_read(device_t *dev, uint64 offset, void *buf, uint64 n) {
    uint8 *dst = (uint8 *)buf;
    uint64 i;

    (void)dev;
    if (offset >= fb.size) {
        return 0;                               /* end of the device */
    }
    if (n > fb.size - offset) {
        n = fb.size - offset;
    }
    for (i = 0; i < n; i++) {
        dst[i] = fb.virt[offset + i];
    }
    return (int64)n;
}

static int64 fbdev_write(device_t *dev, uint64 offset, const void *buf,
                         uint64 n) {
    const uint8 *src = (const uint8 *)buf;
    uint64 i;

    (void)dev;
    if (offset >= fb.size) {
        return -28;                             /* -ENOSPC, as Linux says */
    }
    if (n > fb.size - offset) {
        n = fb.size - offset;
    }
    for (i = 0; i < n; i++) {
        fb.virt[offset + i] = src[i];
    }
    return (int64)n;
}

static void zero(void *p, uint64 n) {
    uint8 *b = (uint8 *)p;
    uint64 i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

/* struct fb_var_screeninfo: forty u32s. Laid out by index so the offsets
 * are those of <linux/fb.h> without a struct that could pad differently. */
static void fill_var(uint32 *v) {
    zero(v, 160);
    v[0]  = fb.width;             /* xres                  */
    v[1]  = fb.height;            /* yres                  */
    v[2]  = fb.width;             /* xres_virtual          */
    v[3]  = fb.height;            /* yres_virtual          */
    v[6]  = fb.bpp;               /* bits_per_pixel        */
    v[8]  = fb.red_pos;   v[9]  = fb.red_size;     /* red {offset, length}  */
    v[11] = fb.green_pos; v[12] = fb.green_size;
    v[14] = fb.blue_pos;  v[15] = fb.blue_size;
    v[22] = 0xFFFFFFFFu;          /* height (mm): unknown  */
    v[23] = 0xFFFFFFFFu;          /* width (mm): unknown   */
}

static int fbdev_control(device_t *dev, uint32 code, void *arg,
                         uint64 arg_size) {
    uint8 *p = (uint8 *)arg;

    (void)dev;
    (void)arg_size;
    switch (code) {
        case FBIOGET_VSCREENINFO:
            if (arg == NULL) {
                return -14;
            }
            fill_var((uint32 *)p);
            return 0;

        case FBIOPUT_VSCREENINFO: {
            /* The mode was chosen by the BIOS at boot and cannot change now.
             * Asking for exactly what is set succeeds, which is what a
             * program that "sets" the current mode before drawing needs. */
            const uint32 *v = (const uint32 *)p;

            if (arg == NULL) {
                return -14;
            }
            if (v[0] != fb.width || v[1] != fb.height || v[6] != fb.bpp) {
                return -22;
            }
            fill_var((uint32 *)p);
            return 0;
        }

        case FBIOGET_FSCREENINFO: {
            /* struct fb_fix_screeninfo, 80 bytes on x86-64. */
            const char *id = "Genesis VBE";
            int i;

            if (arg == NULL) {
                return -14;
            }
            zero(p, 80);
            for (i = 0; id[i] != '\0' && i < 15; i++) {
                p[i] = (uint8)id[i];
            }
            *(uint64 *)(p + 16) = fb.phys;                 /* smem_start  */
            *(uint32 *)(p + 24) = (uint32)fb.size;         /* smem_len    */
            *(uint32 *)(p + 28) = FB_TYPE_PACKED_PIXELS;   /* type        */
            *(uint32 *)(p + 36) = FB_VISUAL_TRUECOLOR;     /* visual      */
            *(uint32 *)(p + 48) = fb.pitch;                /* line_length */
            return 0;
        }

        default:
            return -25;                                    /* -ENOTTY */
    }
}

static int fbdev_mmap(device_t *dev, uint64 offset, uint64 *phys,
                      uint64 *cache_flags) {
    (void)dev;
    if (offset >= fb.size) {
        return -22;
    }
    *phys        = fb.phys + offset;
    *cache_flags = PAGE_PCD;
    return 0;
}

static const device_ops_t fbdev_ops = {
    .name    = "fb",
    .read    = fbdev_read,
    .write   = fbdev_write,
    .control = fbdev_control,
    .mmap    = fbdev_mmap,
};

static device_t *fbdev;

void fb_register_device(void) {
    int rc;

    if (!fb_live) {
        return;
    }
    fbdev = dev_alloc();
    if (fbdev == NULL) {
        kprintf_c(0x0C, "fb: no device slot for /dev/fb0\n");
        return;
    }
    fbdev->ops        = &fbdev_ops;
    fbdev->kind       = DEVICE_KIND_CHAR;
    fbdev->size       = fb.size;
    fbdev->block_size = 0;
    rc = dev_attach(fbdev, "\\Device\\fb0", NULL);
    if (rc != 0) {
        kprintf_c(0x0C, "fb: attaching \\Device\\fb0 failed (%d)\n", rc);
        dev_free(fbdev);
        fbdev = NULL;
    }
}

/* --- selftest ------------------------------------------------------------ */

static int st_failures;

static void st_check(int ok, const char *what) {
    if (!ok) {
        st_failures++;
        kprintf_c(0x0C, "fb: selftest: %s\n", what);
    }
}

/* The Bochs/QEMU display interface (VBE DISPI, ports 0x1CE/0x1CF). Only
 * present on the emulated adapters, which is exactly where it is useful: it
 * reads the mode back from the ADAPTER, so it proves the BIOS call reached
 * the hardware rather than only that the bootloader wrote a block saying so.
 * Returns 0 and leaves the outputs alone when the interface is absent. */
static int dispi_mode(uint32 *w, uint32 *h, uint32 *bpp, uint32 *enabled) {
    uint16 id;

    outw(0x1CE, 0);                      /* VBE_DISPI_INDEX_ID */
    id = inw(0x1CF);
    if (id < 0xB0C0 || id > 0xB0CF) {
        return 0;
    }
    outw(0x1CE, 1); *w       = inw(0x1CF);
    outw(0x1CE, 2); *h       = inw(0x1CF);
    outw(0x1CE, 3); *bpp     = inw(0x1CF);
    outw(0x1CE, 4); *enabled = inw(0x1CF);
    return 1;
}

int fb_selftest(void) {
    uint16 rw, rh;

    st_failures = 0;
    requested_mode(&rw, &rh);

    /* The block itself, independent of whether a mode was set. */
    st_check(boot_block_valid, "no VBE block from the bootloader");
    if (!boot_block_valid) {
        kprintf_c(0x0C, "fb: selftest FAILED (%d)\n", st_failures);
        return st_failures;
    }
    st_check(font_valid, "the bootloader did not copy the BIOS font");
    if (font_valid) {
        /* Space is blank and 'A' is not: a zero-filled or misplaced copy
         * fails one of the two. */
        int i, blank = 1, inked = 0;

        for (i = 0; i < 16; i++) {
            if (font[' ' * 16 + i] != 0) blank = 0;
            if (font['A' * 16 + i] != 0) inked = 1;
        }
        st_check(blank && inked, "the BIOS font does not look like a font");
    }

    if (rw == 0) {
        kprintf_c(0x0E, "fb: selftest skipped - text mode requested\n");
        return st_failures;
    }
    if (!(boot_block.status & BOOT_VBE_PRESENT)) {
        kprintf_c(0x0E, "fb: selftest skipped - the BIOS has no VBE\n");
        return st_failures;
    }
    st_check((boot_block.status & BOOT_VBE_MODE_SET) != 0,
             "a mode was requested and VBE is present, but none was set");
    st_check(fb_live, "the framebuffer is not live");
    if (!fb_live) {
        kprintf_c(0x0C, "fb: selftest FAILED (%d)\n", st_failures);
        return st_failures;
    }

    /* What the stub was told to pick. */
    st_check(fb.bpp == 32, "not 32 bits per pixel");
    st_check(fb.width <= rw && fb.height <= rh,
             "the mode is larger than the one requested");
    st_check(fb.red_size == 8 && fb.green_size == 8 && fb.blue_size == 8,
             "the colour channels are not 8 bits each");

    /* The adapter agrees, where it can be asked. */
    {
        uint32 w = 0, h = 0, bpp = 0, en = 0;

        if (dispi_mode(&w, &h, &bpp, &en)) {
            st_check(w == fb.width && h == fb.height && bpp == 32,
                     "the adapter's mode differs from the VBE block");
            /* ENABLED (bit 0) and LFB (bit 6): a mode set without bit 14
             * in 4F02h leaves the adapter banked, and the linear address
             * then reaches nothing. */
            st_check((en & 0x41) == 0x41,
                     "the adapter is not in linear-framebuffer mode");
        }
    }

    /* The kernel mapping lands on the framebuffer, page by page. */
    {
        uint64 pages = fb.size / PMM_PAGE_SIZE;
        uint64 probe[3];
        int i;

        probe[0] = 0;
        probe[1] = (pages / 2) * PMM_PAGE_SIZE;
        probe[2] = (pages - 1) * PMM_PAGE_SIZE;
        for (i = 0; i < 3; i++) {
            st_check(vmm_get_phys((virt_addr_t)(fb.virt + probe[i])) ==
                         fb.phys + probe[i],
                     "the kernel mapping does not reach the framebuffer");
        }
    }

    /* Pixels write and read back, at both ends. Saved and restored: the
     * console is already drawing here. */
    {
        volatile uint32 *first = (volatile uint32 *)fb.virt;
        volatile uint32 *last  = (volatile uint32 *)
            (fb.virt + (uint64)(fb.height - 1) * fb.pitch +
             (uint64)(fb.width - 1) * 4);
        uint32 save_first = *first, save_last = *last;

        *first = 0x00A5C3E1u;
        *last  = 0x001E3C5Au;
        st_check(*first == 0x00A5C3E1u && *last == 0x001E3C5Au,
                 "a pixel did not read back");
        *first = save_first;
        *last  = save_last;
    }

    st_check(fb_rgb(0xFF, 0, 0) == (0xFFu << fb.red_pos) &&
             fb_rgb(0, 0, 0xFF) == (0xFFu << fb.blue_pos),
             "fb_rgb packs a channel into the wrong place");

    /* The console's glyph renderer, against the font directly. */
    st_check(screen_fb_selftest() == 0,
             "the console did not draw a glyph as the font has it");

    /* /dev/fb0: registered, its mmap op names the right pages and refuses
     * past the end, and read(2) returns the pixels themselves. */
    st_check(fbdev != NULL, "/dev/fb0 was not registered");
    if (fbdev != NULL) {
        uint64 phys = 0, cache = 0;
        uint32 px = 0;
        volatile uint32 *first = (volatile uint32 *)fb.virt;
        uint32 save = *first;

        st_check(dev_mmap(fbdev, PMM_PAGE_SIZE, &phys, &cache) == 0 &&
                     phys == fb.phys + PMM_PAGE_SIZE &&
                     (cache & PAGE_PCD) != 0,
                 "/dev/fb0 mmap does not map the framebuffer uncached");
        st_check(dev_mmap(fbdev, fb.size, &phys, &cache) == -22,
                 "/dev/fb0 mmap past the end was not refused");
        *first = 0x00123456u;
        st_check(dev_read(fbdev, 0, &px, 4) == 4 && px == 0x00123456u,
                 "/dev/fb0 read does not return the pixels");
        *first = save;
    }

    if (st_failures == 0) {
        kprintf_c(0x0A, "fb: selftest passed\n");
    } else {
        kprintf_c(0x0C, "fb: selftest FAILED (%d)\n", st_failures);
    }
    return st_failures;
}
