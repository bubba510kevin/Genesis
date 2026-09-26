#include "io.h"
#include "irq.h"
#include "keyboard.h"
#include "kprintf.h"
#include "serial.h"
#include "typesk.h"

/* A 16550 at the conventional COM1 address. Register offsets from the base,
 * and two of them change meaning depending on a bit in the line control
 * register, which is the single most common way to get this wrong:
 *
 *   +0  data (read: RX, write: TX)   -- or divisor low  when DLAB is set
 *   +1  interrupt enable             -- or divisor high when DLAB is set
 *   +2  FIFO control (write)
 *   +3  line control                 -- bit 7 is DLAB
 *   +4  modem control
 *   +5  line status                  -- bit 5: transmit holding reg empty
 */
#define REG_DATA        0
#define REG_IER         1
#define REG_FIFO        2
#define REG_LCR         3
#define REG_MCR         4
#define REG_LSR         5

#define LCR_DLAB        0x80
#define LCR_8N1         0x03      /* 8 data bits, no parity, one stop bit */

#define LSR_TX_EMPTY    0x20

#define MCR_DTR_RTS     0x03
#define MCR_OUT2        0x08      /* gates the UART's interrupt line       */
#define MCR_LOOPBACK    0x10

static int present;

int serial_present(void) {
    return present;
}

int serial_init(void) {
    uint8 probe;

    present = 0;

    outb(SERIAL_COM1 + REG_IER, 0x00);        /* no interrupts: polled       */

    /* 115200 baud: the UART's 115200Hz clock divided by 1. The divisor is
     * two registers that only exist while DLAB is set, which is why it goes
     * back down immediately afterwards - leaving it set turns every
     * subsequent write to +0 into a baud change instead of a character. */
    outb(SERIAL_COM1 + REG_LCR, LCR_DLAB);
    outb(SERIAL_COM1 + REG_DATA, 0x01);       /* divisor low  */
    outb(SERIAL_COM1 + REG_IER,  0x00);       /* divisor high */
    outb(SERIAL_COM1 + REG_LCR, LCR_8N1);

    /* Enable and clear both FIFOs, 14-byte trigger. */
    outb(SERIAL_COM1 + REG_FIFO, 0xC7);

    /* Loopback, and send a byte to see whether anything is listening. An
     * absent port floats high and reads back 0xFF; a real one hands back
     * exactly what was written. Without this check a machine with no UART
     * spins forever in serial_putc waiting for a transmit-empty bit that will
     * never be set - a hang at boot, on hardware, with nothing printed. */
    outb(SERIAL_COM1 + REG_MCR, MCR_DTR_RTS | MCR_OUT2 | MCR_LOOPBACK);
    outb(SERIAL_COM1 + REG_DATA, 0xAE);
    probe = inb(SERIAL_COM1 + REG_DATA);
    if (probe != 0xAE) {
        return 0;
    }

    outb(SERIAL_COM1 + REG_MCR, MCR_DTR_RTS | MCR_OUT2);
    present = 1;
    return 1;
}

void serial_putc(char c) {
    /* Bounded rather than a bare while loop. A UART that stops asserting
     * transmit-empty is a broken UART, and hanging the kernel because of one
     * is worse than dropping the character - especially since the caller is
     * often a fault handler trying to say what went wrong. */
    int spins = 100000;

    if (!present) {
        return;
    }
    while (spins-- > 0 && !(inb(SERIAL_COM1 + REG_LSR) & LSR_TX_EMPTY)) {
    }
    outb(SERIAL_COM1 + REG_DATA, (uint8)c);
}

void serial_write(const char *s) {
    if (!present || s == NULL) {
        return;
    }
    while (*s != '\0') {
        /* The screen treats \n as "next line, column zero" because that is
         * what a text-mode cursor needs. A terminal needs both characters, so
         * the carriage return is added here rather than being carried in
         * every string in the kernel. */
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s);
        s++;
    }
}

/* --- receive ---------------------------------------------------------------
 *
 * See serial.h for why this exists at all. The short version: the bare-metal
 * target has a USB keyboard and keyboard.c is PS/2-only, so without this the
 * shell comes up on a machine nobody can type at.
 */

#define LSR_DATA_READY  0x01
#define IER_RX_AVAIL    0x01

static uint32 rx_by_irq;
static uint32 rx_by_poll;

/* Take everything the UART has and hand it to the keyboard's ring.
 *
 * Bytes go in almost untranslated, because the consumer already copes: the
 * line reader in keyboard.c treats '\n' and '\r' alike as end-of-line, and
 * '\b' and 0x7F alike as backspace. A terminal sends CR for Enter and DEL for
 * backspace, so both of those already land correctly. Translating them here
 * would mean two places deciding what a line terminator is.
 *
 * Ctrl-C (0x03) and Ctrl-D (0x04) are likewise raw bytes on the wire and are
 * exactly what kbd_read_line already looks for, so a serial Ctrl-C raises
 * SIGINT through the same path a PS/2 Ctrl-C does.
 */
static int serial_drain(uint32 *counter) {
    int taken = 0;

    if (!present) {
        return 0;
    }
    /* Bounded. A UART whose data-ready bit is stuck would otherwise spin
     * here forever with interrupts off, which on a non-preemptive kernel is
     * the whole machine. Sixty-four is well past the 16-byte FIFO. */
    while (taken < 64 && (inb(SERIAL_COM1 + REG_LSR) & LSR_DATA_READY)) {
        kbd_inject((char)inb(SERIAL_COM1 + REG_DATA));
        taken++;
    }
    if (counter != NULL) {
        *counter += (uint32)taken;
    }
    return taken;
}

void serial_irq(void) {
    (void)serial_drain(&rx_by_irq);
}

int serial_poll(void) {
    return serial_drain(&rx_by_poll);
}

/* Returns whether this handler CLAIMED the interrupt, which on IRQ 4 is not a
 * formality: the line is conventionally shared between COM1 and COM3, and
 * irq.h is explicit that a handler which cannot tell must return zero so a
 * device that can speak up. Claiming unconditionally would defeat the
 * unclaimed-interrupt detection for everything else on the line. Bytes taken
 * is exactly the right test - if the FIFO was empty, this UART did not raise
 * it. */
static int serial_irq_handler(void *ctx) {
    (void)ctx;
    return serial_drain(&rx_by_irq) > 0 ? 1 : 0;
}

void serial_rx_init(void) {
    if (!present) {
        return;
    }

    /* Drain anything the firmware left in the FIFO before enabling the
     * interrupt. Otherwise the first thing the shell sees is a byte that
     * arrived before the kernel started - the same reason kbd_init drains the
     * 8042 controller. */
    while (inb(SERIAL_COM1 + REG_LSR) & LSR_DATA_READY) {
        (void)inb(SERIAL_COM1 + REG_DATA);
    }

    /* MCR_OUT2 is already set by serial_init and it is load-bearing here in a
     * way it was not for transmit: on a real 16550 that bit gates the UART's
     * interrupt line onto the bus, so without it the RX interrupt is enabled
     * in the UART and never reaches the interrupt controller. */
    outb(SERIAL_COM1 + REG_IER, IER_RX_AVAIL);

    irq_register(4, serial_irq_handler, NULL);
}

void serial_rx_report(uint8 color) {
    if (!present) {
        return;
    }
    /* Both numbers, always. If the interrupt path is dead the poll count
     * climbs alone, which is the signal that IRQ 4 is not routed on this
     * machine - and that is worth seeing rather than being quietly carried
     * by the fallback. */
    kprintf_c(color, "serial: rx %u by irq, %u by poll\n",
              rx_by_irq, rx_by_poll);
}
