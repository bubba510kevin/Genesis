#ifndef ATKBDC_H
#define ATKBDC_H

#include "typesk.h"

/* The i8042 keyboard controller, as a bus. ROADMAP item 14(j).
 *
 * FreeBSD's name and FreeBSD's shape: atkbdc0 is the controller, and the
 * devices behind its two ports are its children - atkbd0 on the keyboard
 * port, psm0 on the auxiliary (mouse) port. Here the keyboard stays what it
 * was (kernel/dev/keyboard.c reads port 0x60 from IRQ 1 directly and nothing
 * about that changes), and psm0 is a real Newbus child: atkbdc_init adds it
 * to the "atkbdc" devclass and the psm driver (kernel/dev/psm.c, written in
 * the FreeBSD DEVMETHOD/DRIVER_MODULE idiom) probes and attaches to it.
 *
 * This file owns the controller's protocol - the status port, the command
 * byte, the two-step write to the aux port - so a driver never touches 0x64
 * itself. Every wait is bounded: a controller that stops answering costs a
 * timeout and an error, never a hung boot.
 *
 * Polled I/O here runs with the mouse interrupt not yet delivering (before
 * sti, or with psm's handler not yet registered). A keyboard byte that turns
 * up while a response is being waited for is handed to the keyboard rather
 * than eaten.
 */

/* What a child of atkbdc carries as ivars. */
typedef struct atkbdc_ivars {
    int port;               /* ATKBDC_PORT_KBD or ATKBDC_PORT_AUX */
    int irq;                /* 1 or 12 */
} atkbdc_ivars_t;

#define ATKBDC_PORT_KBD  0
#define ATKBDC_PORT_AUX  1

/* Probe the controller, enable its aux port, and add + attach psm0 (unless
 * hint.psm.0.disabled). Before sti, after irq/ioapic init and after the psm
 * driver has registered (psm_atkbdc_newbus_module_init). Prints a line
 * either way. */
void atkbdc_init(void);

/* Send one byte to the aux device and collect its ACK (0xFA). Retries a
 * RESEND (0xFE) twice. 0 on ACK, -1 otherwise. */
int atkbdc_aux_command(uint8 byte);

/* Read one byte from the aux device, waiting up to about `ms`
 * milliseconds. 0 and *out on success, -1 on timeout. Keyboard bytes that
 * arrive meanwhile go to the keyboard. */
int atkbdc_aux_read(uint8 *out, uint32 ms);

/* Send one byte to the aux device and return WITHOUT reading its reply -
 * for a driver whose interrupt handler collects the ACK. Once psm's handler
 * is live this is the only safe way to command the mouse: a polled read of
 * the reply would race the handler for the same byte. 0, or -1 if the
 * controller would not take it. */
int atkbdc_aux_write(uint8 byte);

/* Non-blocking: if the controller holds an aux byte, read it into *out and
 * return 1; otherwise 0, leaving a keyboard byte where it is for IRQ 1. What
 * psm's interrupt handler drains the controller with. */
int atkbdc_aux_poll(uint8 *out);

/* Controller loopback (command 0xD3): the controller places `byte` in its
 * output buffer AS IF the aux device had sent it, raising IRQ 12. How the
 * mouse's boot selftest injects packets with no mouse moving. 0 on success,
 * -1 if the controller would not take the command. */
int atkbdc_aux_loopback(uint8 byte);

/* Enable or disable the aux port's interrupt in the command byte. */
int atkbdc_aux_irq(int on);

#endif
