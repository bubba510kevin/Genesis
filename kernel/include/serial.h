#ifndef SERIAL_H
#define SERIAL_H

#include "typesk.h"

/* COM1, as a write-only console mirror.
 *
 * --- Why this exists -----------------------------------------------------
 * Everything the kernel prints goes to the VGA text buffer, which is 80x25
 * and scrolls off. That is fine while the interesting output is the last
 * twenty lines and fatal while it is a test run of two hundred checks, or a
 * boot log you want to diff against yesterday's. QEMU's -serial stdio puts a
 * UART in the machine and pipes it to the terminal, where it is scrollable,
 * greppable, and redirectable to a file - but only if something in the kernel
 * actually writes to the port, which is the part that was missing.
 *
 * --- Why a mirror rather than a console ---
 * print_string keeps writing to VGA exactly as it did; this is an additional
 * sink, not a replacement. The screen stays the interactive surface (it has
 * the cursor, the colours and the line discipline behind it), and the serial
 * port is the transcript. Making serial the console instead would mean moving
 * echo, the cursor and colour handling onto it, and none of that helps you
 * read a log.
 *
 * Colour is discarded: a terminal has its own idea of colour and a log file
 * has none. What matters is the text.
 *
 * --- Now also an INPUT path, which changes the "mirror" story ---
 * Transmit is still a mirror and still polled, for the reasons below. Receive
 * is new, and it exists because of a specific machine: the bare-metal target
 * (a Dell OptiPlex 3040) has a USB keyboard and no PS/2 device at all, while
 * kernel/dev/keyboard.c speaks PS/2 only. Without USB Legacy Support enabled
 * in firmware there is no way to type at that machine - the shell comes up
 * and is unreachable.
 *
 * A serial console costs about fifty lines and needs no USB stack, so it is
 * the cheap way out of that. It also makes the machine automatable the way
 * QEMU already is, which xHCI would not have.
 *
 * Received bytes are pushed into the SAME ring the keyboard fills, through
 * kbd_inject. That is the whole integration: the line discipline, the Ctrl-C
 * handling, the echo and every reader above it are shared, so a serial
 * console behaves identically to the keyboard rather than being a second,
 * subtly different input path.
 *
 * --- Polled TRANSMIT, not interrupt-driven ---
 * Deliberately. This has to work when the kernel is about to halt - inside a
 * fault handler, with interrupts off, possibly on a corrupted stack - which
 * is precisely when an interrupt-driven driver with a queue is least likely
 * to flush. Busy-waiting on the transmit-holding-register bit costs
 * microseconds and works everywhere, including before the IDT exists.
 */

#define SERIAL_COM1 0x3F8

/* Configure COM1 for 115200 8N1 and enable the FIFO. Returns non-zero if a
 * UART actually answered: the loopback test at the end distinguishes a real
 * port from an empty I/O address that reads back 0xFF, so a machine without
 * one is detected rather than being written to forever. */
int serial_init(void);

/* Non-zero once serial_init has found a port. Callers do not need to check -
 * every function here is a no-op without one - but the boot log says so. */
int serial_present(void);

void serial_putc(char c);
void serial_write(const char *s);

/* --- receive --------------------------------------------------------------
 *
 * Enable the UART's receive interrupt and register a handler on IRQ 4. Call
 * after serial_init and after the IRQ layer is up. No-op without a port.
 */
void serial_rx_init(void);

/* The IRQ 4 handler. Drains whatever the UART has and injects it into the
 * keyboard's ring. */
void serial_irq(void);

/* Drain any pending received bytes without an interrupt, returning how many
 * were taken. Cheap - one port read when the FIFO is empty.
 *
 * This exists as INSURANCE, not as the primary path, and the distinction is
 * recorded rather than blurred: on hardware whose IRQ 4 routing this tree has
 * never seen, an interrupt that never arrives would mean a machine with no
 * keyboard AND no serial, which is unreachable. Calling this from the timer
 * tick guarantees input still works at tick granularity. serial_rx_report
 * prints how many bytes came each way, so a silently-broken interrupt is
 * visible instead of being papered over. */
int serial_poll(void);

/* How many received bytes arrived by interrupt and how many by polling. */
void serial_rx_report(uint8 color);

#endif
