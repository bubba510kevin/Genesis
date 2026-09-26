#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "typesk.h"

/* PS/2 keyboard plus a canonical-mode line discipline.
 *
 * --- Why the line discipline lives here ---------------------------------
 * Someone has to echo typed characters, apply backspace, and decide when a
 * line is finished. On a real system that is the tty layer, and a shell with
 * line editing turns it off and does the job itself in raw mode. Doing it in
 * the kernel is the smaller of the two: it needs no termios state machine and
 * no escape-sequence parsing, and it means read() returns whole lines, which
 * is exactly the shape ash expects when its own editing is compiled out.
 *
 * The consequence is a hard requirement on the userspace side: busybox must
 * be built with FEATURE_EDITING off. If ash does its own editing it will put
 * the terminal in raw mode - which this driver accepts and ignores - and then
 * both ends will echo, producing every character twice.
 *
 * --- Structure ---
 * Two layers, split so the interesting half can be tested off-target:
 *
 *   kbd_scancode()  hardware byte -> character, pushed into the ring. Called
 *                   from the IRQ, and directly by tests.
 *   kbd_read_line() ring -> a line, with echo and editing. Called from read().
 *
 * The ring is a single-producer single-consumer queue: the IRQ writes, the
 * syscall reads, and nothing else touches either index. That is what makes it
 * safe without a lock. */

/* Reset driver state. Call before interrupts are enabled. */
void kbd_init(void);

/* IRQ1 handler body: reads port 0x60 and feeds kbd_scancode. */
void kbd_irq(void);

/* Translate one scancode and queue whatever it produces. Modifier keys update
 * internal state and queue nothing. Exposed for tests. */
void kbd_scancode(uint8 code);

/* Push an already-decoded character into the same ring the keyboard fills,
 * and wake anyone waiting. For input that did not come from the PS/2
 * controller - the serial console is the caller. Takes a character, NOT a
 * scancode: kbd_scancode is the PS/2 translation layer and has nothing to say
 * about a byte that arrived already decoded. */
void kbd_inject(char c);

/* Read one line into buf, blocking until Enter.
 *
 * Returns the byte count including the trailing newline; 0 for end of input
 * (Ctrl-D on an empty line), which is what tells a shell to exit; or -EINTR
 * if a signal arrived first, Ctrl-C included. Never returns more than max
 * bytes, and reserves room for the newline, so a line longer than the buffer
 * is delivered in pieces rather than truncated.
 *
 * The distinction between 0 and -EINTR is the whole contract: exactly one
 * keystroke may end a shell's input, and it is Ctrl-D. */
int64 kbd_read_line(char *buf, uint64 max);

/* Non-blocking: 1 if a character is queued. */
int kbd_has_input(void);

/* Non-blocking: 1 if a call to kbd_read_line would RETURN rather than block.
 *
 * Not the same question as kbd_has_input, and the difference is the whole of
 * what poll(2) needs from this file. The ring holding three characters means
 * a read will consume them and then block for the rest of the line; it does
 * not mean a read will return. So this scans for the bytes that END a line -
 * a newline, a carriage return, Ctrl-D, or Ctrl-C, that last one because it
 * ends the read with -EINTR, which is a return.
 *
 * Answering with kbd_has_input instead is the bug where a program polls, is
 * told the terminal is readable, calls read, and blocks anyway - the exact
 * failure poll exists to prevent, made harder to find by the fact that it
 * only shows up when the user types without pressing Enter.
 *
 * A line longer than the reader's buffer also returns early, and this cannot
 * see that: the buffer size belongs to the caller. Under-reporting there is
 * harmless - the character that eventually ends the line wakes the poller. */
int kbd_has_line(void);

#endif
