#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "typesk.h"

/* The PS/2 keyboard: scancodes in, characters out to the terminal.
 *
 * --- Where the line discipline went ---------------------------------------
 * This file used to hold it too: a ring of raw characters and kbd_read_line,
 * which echoed, erased and assembled lines at READ time, always canonical.
 * That forced BusyBox to be built without line editing (ash in raw mode would
 * have echoed everything twice) and could not run readline at all. The
 * discipline is kernel/dev/tty.c now, with a real termios, and it runs at
 * INPUT time. What is left here is the translation layer:
 *
 *   kbd_scancode()  hardware byte -> character(s) -> tty_input(). Called from
 *                   the IRQ, and directly by tests.
 *   kbd_inject()    an already-decoded character from something that is not
 *                   the PS/2 controller (the serial console) -> tty_input().
 *
 * The navigation keys produce the escape sequences a Linux console sends
 * (ESC [ A for Up, and so on), so readline's history and cursor movement
 * work from the PS/2 keyboard as they do over the serial line. */

/* Reset driver state. Call before interrupts are enabled. */
void kbd_init(void);

/* IRQ1 handler body: reads port 0x60 and feeds kbd_scancode. */
void kbd_irq(void);

/* Translate one scancode and hand whatever it produces to the terminal.
 * Modifier keys update internal state and produce nothing. Exposed for
 * tests. */
void kbd_scancode(uint8 code);

/* One already-decoded character to the terminal. Takes a character, NOT a
 * scancode. Ctrl-T (0x14) is consumed here as the task-dump key (see
 * proc_dump) and never reaches the terminal. */
void kbd_inject(char c);

#endif
