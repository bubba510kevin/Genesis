#include "bootvbe.h"

/* ===========================================================================
 * The graphics mode-set, in real mode, at the very front of the image.
 * ---------------------------------------------------------------------------
 * ROADMAP item 14(h). Choosing and setting a VESA mode needs INT 10h, and the
 * BIOS is only callable in real mode - so this has to happen before
 * boot.asm's switch to protected mode. The boot sector itself cannot hold it:
 * it has eight bytes left below the partition table. So the sector loads the
 * kernel exactly as before and then makes ONE far call, into this code,
 * which the image carries at a fixed offset from its start.
 *
 * --- How real mode reaches an image loaded at 1MB ------------------------
 * The High Memory Area. With the A20 gate open (boot.asm opens it before the
 * load, for its own reasons), segment 0xFFFF reaches linear 0xFFFF0 through
 * 0x10FFEF - the first 64KB above 1MB, minus 16 bytes. The image starts at
 * 0x100000 = 0xFFFF:0x0010, so the stub below, at image offset 0x10, is
 * 0xFFFF:0x0020. The call is five bytes in the boot sector; nothing else in
 * the load path changed.
 *
 * --- The layout of the first 0x10 bytes ---------------------------------
 *   +0   jmp _kernel_entry   boot.asm still `call`s 0x100000 in 32-bit mode;
 *                            this is what it lands on now
 *   +8   u16 width, u16 height   the largest mode to accept; width 0 means
 *                            "stay in text mode". build.py patches these
 *                            from GENESIS_VBE, so the choice is a build
 *                            setting, not an edit here.
 *   +12  "GVBE"             so build.py can refuse to patch an image whose
 *                            head is not this one
 *   +16  the real-mode stub
 *
 * The section is placed FIRST by linker.ld (KEEP(*(.lowtext.head))), which is
 * what pins every offset above. Sorting kernel/ alphabetically would
 * otherwise put whichever .lowtext object came first at 0x100000.
 *
 * --- What the stub does ---------------------------------------------------
 *   1. Writes the BOOT_VBE_MAGIC header at BOOT_VBE_INFO, status 0, so the
 *      kernel never reads a stale block as a real one.
 *   2. Copies the BIOS 8x16 font (INT 10h AX=1130h BH=06) to BOOT_VBE_FONT.
 *      The kernel has no font of its own; this one is in every VGA BIOS, and
 *      the text console needs it once the screen stops being a text screen.
 *      Done even when no mode is requested - the font is cheap and the
 *      kernel may want it later.
 *   3. VBE 4F00h (controller info) and a walk of the mode list with 4F01h,
 *      keeping the LARGEST mode that is: supported, graphics, has a linear
 *      framebuffer (attributes 0x91), 32 bits per pixel, direct colour
 *      (memory model 6), and no larger than the requested width/height.
 *      A walk rather than a fixed mode number, because VBE mode numbers
 *      above 0x11B are vendor-assigned - 0x118 is 1024x768 at 24bpp on one
 *      BIOS and something else on the next.
 *   4. 4F02h with bit 14 set (use the linear framebuffer), and on success the
 *      mode's geometry, pitch, framebuffer address and colour layout into the
 *      info block with BOOT_VBE_MODE_SET.
 *
 * Any failure leaves the screen in text mode with the matching status bits
 * clear. That is a supported outcome, not an error: the kernel's console
 * keeps using the VGA text buffer and /dev/fb0 simply does not appear.
 *
 * Addressing: DS and ES are 0 and every data reference is an absolute
 * low-memory address (bootvbe.h), because this code is LINKED at 0x100010
 * but EXECUTES with CS = 0xFFFF - a reference to one of its own labels would
 * need that difference applied by hand. Its only reads through CS are the two
 * request words, at fixed offsets. Jumps and calls are IP-relative and do not
 * care.
 *
 * Scratch: BOOT_VBE_SCRATCH holds the loop state (the mode-list pointer, the
 * best mode so far and its pixel count) in memory rather than in registers,
 * because a VBE BIOS is allowed to clobber more than it documents and some do.
 * ======================================================================== */

__asm__(
".section .lowtext.head,\"ax\"\n"
".code32\n"
".global _image_head\n"
"_image_head:\n"
"    jmp  _kernel_entry\n"
"    .org _image_head + " BOOT_STR(BOOT_VBE_REQ_OFFSET) "\n"
"    .word " BOOT_STR(BOOT_VBE_DEFAULT_W) "\n"
"    .word " BOOT_STR(BOOT_VBE_DEFAULT_H) "\n"
"    .ascii \"GVBE\"\n"
"    .org _image_head + " BOOT_STR(BOOT_VBE_STUB_OFFSET) "\n"

".code16\n"
".intel_syntax noprefix\n"
"vbe_stub:\n"
"    pushad\n"
"    push ds\n"
"    push es\n"
"    push fs\n"
"    xor  ax, ax\n"
"    mov  ds, ax\n"
"    mov  es, ax\n"
"    cld\n"

/* 1. a fresh, zeroed block with the magic in it */
"    mov  di, " BOOT_STR(BOOT_VBE_INFO) "\n"
"    mov  cx, 32\n"
"    rep  stosw\n"
"    mov  dword ptr ds:" BOOT_STR(BOOT_VBE_INFO) ", " BOOT_STR(BOOT_VBE_MAGIC) "\n"

/* 2. the BIOS font: ES:BP points at it, CX is bytes per glyph */
"    mov  ax, 0x1130\n"
"    mov  bh, 6\n"
"    int  0x10\n"
"    cmp  cx, 16\n"
"    jne  vbe_no_font\n"
"    push ds\n"
"    push es\n"
"    pop  ds\n"
"    mov  si, bp\n"
"    xor  ax, ax\n"
"    mov  es, ax\n"
"    mov  di, " BOOT_STR(BOOT_VBE_FONT) "\n"
"    mov  cx, 2048\n"
"    rep  movsw\n"
"    pop  ds\n"
"    or   word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 4) ", " BOOT_STR(BOOT_VBE_FONT_OK) "\n"
"vbe_no_font:\n"
"    xor  ax, ax\n"
"    mov  es, ax\n"

/* 3. was a mode asked for at all? */
"    mov  ax, word ptr cs:" BOOT_STR(BOOT_VBE_REQ_OFFSET + 0x10) "\n"
"    test ax, ax\n"
"    jz   vbe_done\n"

"    mov  di, " BOOT_STR(BOOT_VBE_CTRL) "\n"
"    mov  dword ptr ds:" BOOT_STR(BOOT_VBE_CTRL) ", 0x32454256\n"   /* 'VBE2' */
"    mov  ax, 0x4F00\n"
"    int  0x10\n"
"    cmp  ax, 0x004F\n"
"    jne  vbe_done\n"
"    or   word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 4) ", " BOOT_STR(BOOT_VBE_PRESENT) "\n"
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_CTRL + 0x04) "\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 26) ", ax\n"
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_CTRL + 0x12) "\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 28) ", ax\n"

"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_CTRL + 0x0E) "\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 0) ", ax\n"
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_CTRL + 0x10) "\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 2) ", ax\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 4) ", 0xFFFF\n"
"    mov  dword ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 8) ", 0\n"
/* Bounded as well as terminated: a mode list is only as trustworthy as the
 * BIOS that built it, and one missing its 0xFFFF would walk this through
 * memory until something happened to match. */
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 6) ", 512\n"

"vbe_next_mode:\n"
"    dec  word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 6) "\n"
"    jz   vbe_pick\n"
"    mov  si, word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 0) "\n"
"    mov  fs, word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 2) "\n"
"    mov  cx, word ptr fs:[si]\n"
"    add  si, 2\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 0) ", si\n"
"    cmp  cx, 0xFFFF\n"
"    je   vbe_pick\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 12) ", cx\n"
"    xor  ax, ax\n"
"    mov  es, ax\n"
"    mov  di, " BOOT_STR(BOOT_VBE_MODEINFO) "\n"
"    mov  ax, 0x4F01\n"
"    int  0x10\n"
"    cmp  ax, 0x004F\n"
"    jne  vbe_next_mode\n"
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x00) "\n"
"    and  ax, 0x0091\n"
"    cmp  ax, 0x0091\n"
"    jne  vbe_next_mode\n"
"    cmp  byte ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x19) ", 32\n"
"    jne  vbe_next_mode\n"
"    cmp  byte ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x1B) ", 6\n"
"    jne  vbe_next_mode\n"
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x12) "\n"
"    cmp  ax, word ptr cs:" BOOT_STR(BOOT_VBE_REQ_OFFSET + 0x10) "\n"
"    ja   vbe_next_mode\n"
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x14) "\n"
"    cmp  ax, word ptr cs:" BOOT_STR(BOOT_VBE_REQ_OFFSET + 0x12) "\n"
"    ja   vbe_next_mode\n"
"    movzx eax, word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x12) "\n"
"    movzx ebx, word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x14) "\n"
"    imul eax, ebx\n"
"    cmp  eax, dword ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 8) "\n"
"    jbe  vbe_next_mode\n"
"    mov  dword ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 8) ", eax\n"
"    mov  cx, word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 12) "\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 4) ", cx\n"
"    jmp  vbe_next_mode\n"

/* 4. set the winner, and describe it */
"vbe_pick:\n"
"    mov  cx, word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 4) "\n"
"    cmp  cx, 0xFFFF\n"
"    je   vbe_done\n"
"    xor  ax, ax\n"
"    mov  es, ax\n"
"    mov  di, " BOOT_STR(BOOT_VBE_MODEINFO) "\n"
"    mov  ax, 0x4F01\n"
"    int  0x10\n"
"    cmp  ax, 0x004F\n"
"    jne  vbe_done\n"
"    mov  bx, word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 4) "\n"
"    or   bx, 0x4000\n"
"    mov  ax, 0x4F02\n"
"    int  0x10\n"
"    cmp  ax, 0x004F\n"
"    jne  vbe_done\n"

"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_SCRATCH + 4) "\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 6) ", ax\n"
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x12) "\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 8) ", ax\n"
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x14) "\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 10) ", ax\n"
/* The pitch. VBE 3.0 gives the LINEAR pitch separately (offset 0x32), and on
 * some adapters it differs from the banked one at 0x10 - so the 3.0 field is
 * preferred whenever the BIOS is new enough to have filled it in. */
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x10) "\n"
"    cmp  word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 26) ", 0x0300\n"
"    jb   vbe_pitch_done\n"
"    cmp  word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x32) ", 0\n"
"    je   vbe_pitch_done\n"
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x32) "\n"
"vbe_pitch_done:\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 12) ", ax\n"
"    mov  al, byte ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x19) "\n"
"    mov  byte ptr ds:" BOOT_STR(BOOT_VBE_INFO + 14) ", al\n"
"    mov  al, byte ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x1B) "\n"
"    mov  byte ptr ds:" BOOT_STR(BOOT_VBE_INFO + 15) ", al\n"
"    mov  eax, dword ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x28) "\n"
"    mov  dword ptr ds:" BOOT_STR(BOOT_VBE_INFO + 16) ", eax\n"
"    mov  eax, dword ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x1F) "\n"
"    mov  dword ptr ds:" BOOT_STR(BOOT_VBE_INFO + 20) ", eax\n"
"    mov  ax, word ptr ds:" BOOT_STR(BOOT_VBE_MODEINFO + 0x23) "\n"
"    mov  word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 24) ", ax\n"
"    or   word ptr ds:" BOOT_STR(BOOT_VBE_INFO + 4) ", " BOOT_STR(BOOT_VBE_MODE_SET) "\n"

"vbe_done:\n"
"    pop  fs\n"
"    pop  es\n"
"    pop  ds\n"
"    popad\n"
"    retf\n"
".att_syntax prefix\n"
".code64\n"
".previous\n"
);
