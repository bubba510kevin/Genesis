#include "atkbdc.h"
#include "bus.h"
#include "hints.h"
#include "io.h"
#include "keyboard.h"
#include "kprintf.h"

/* The i8042 controller and its bus - see atkbdc.h. ROADMAP item 14(j). */

#define KBC_DATA     0x60
#define KBC_STATUS   0x64       /* read  */
#define KBC_COMMAND  0x64       /* write */

#define STS_OBF      0x01       /* output buffer full: a byte to read      */
#define STS_IBF      0x02       /* input buffer full: do not write yet     */
#define STS_AUXB     0x20       /* the byte in the output buffer is aux's  */

#define CMD_READ_CFG     0x20
#define CMD_WRITE_CFG    0x60
#define CMD_AUX_DISABLE  0xA7
#define CMD_AUX_ENABLE   0xA8
#define CMD_AUX_TEST     0xA9
#define CMD_WRITE_AUX_OB 0xD3   /* loopback: next data byte "from" aux     */
#define CMD_WRITE_AUX    0xD4   /* next data byte goes TO the aux device   */

#define CFG_KBD_IRQ      0x01
#define CFG_AUX_IRQ      0x02
#define CFG_AUX_CLOCK_OFF 0x20

#define AUX_ACK          0xFA
#define AUX_RESEND       0xFE

/* A spin is one status-port read. On real hardware an I/O port read takes
 * about a microsecond, so this is on the order of a hundred milliseconds -
 * far past anything a working controller needs, and short enough that a
 * dead one costs a boot very little. */
#define SPINS_PER_MS  1000u

static int wait_writable(void) {
    uint32 i;

    for (i = 0; i < 100 * SPINS_PER_MS; i++) {
        if (!(inb(KBC_STATUS) & STS_IBF)) {
            return 0;
        }
    }
    return -1;
}

static int command(uint8 cmd) {
    if (wait_writable() != 0) {
        return -1;
    }
    outb(KBC_COMMAND, cmd);
    return 0;
}

static int write_data(uint8 byte) {
    if (wait_writable() != 0) {
        return -1;
    }
    outb(KBC_DATA, byte);
    return 0;
}

/* One byte from the output buffer, of the wanted kind. The other kind is
 * not discarded: a keystroke typed while the mouse is being configured goes
 * to the keyboard, and an aux byte met while waiting for a controller reply
 * is dropped - nothing is listening to the mouse yet. */
static int read_byte(int want_aux, uint8 *out, uint32 ms) {
    uint32 i;

    for (i = 0; i < ms * SPINS_PER_MS; i++) {
        uint8 st = inb(KBC_STATUS);

        if (!(st & STS_OBF)) {
            continue;
        }
        if (((st & STS_AUXB) != 0) == (want_aux != 0)) {
            *out = inb(KBC_DATA);
            return 0;
        }
        if (st & STS_AUXB) {
            (void)inb(KBC_DATA);
        } else {
            kbd_scancode(inb(KBC_DATA));
        }
    }
    return -1;
}

static int read_config(uint8 *cfg) {
    if (command(CMD_READ_CFG) != 0) {
        return -1;
    }
    return read_byte(0, cfg, 100);
}

static int write_config(uint8 cfg) {
    if (command(CMD_WRITE_CFG) != 0) {
        return -1;
    }
    return write_data(cfg);
}

int atkbdc_aux_read(uint8 *out, uint32 ms) {
    return read_byte(1, out, ms);
}

int atkbdc_aux_command(uint8 byte) {
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        uint8 reply;

        if (command(CMD_WRITE_AUX) != 0 || write_data(byte) != 0) {
            return -1;
        }
        if (atkbdc_aux_read(&reply, 100) != 0) {
            return -1;
        }
        if (reply == AUX_ACK) {
            return 0;
        }
        if (reply != AUX_RESEND) {
            return -1;
        }
    }
    return -1;
}

int atkbdc_aux_write(uint8 byte) {
    if (command(CMD_WRITE_AUX) != 0) {
        return -1;
    }
    return write_data(byte);
}

int atkbdc_aux_poll(uint8 *out) {
    uint8 st = inb(KBC_STATUS);

    if ((st & (STS_OBF | STS_AUXB)) != (STS_OBF | STS_AUXB)) {
        return 0;
    }
    *out = inb(KBC_DATA);
    return 1;
}

int atkbdc_aux_loopback(uint8 byte) {
    if (command(CMD_WRITE_AUX_OB) != 0) {
        return -1;
    }
    return write_data(byte);
}

int atkbdc_aux_irq(int on) {
    uint8 cfg;

    if (read_config(&cfg) != 0) {
        return -1;
    }
    cfg = on ? (uint8)(cfg | CFG_AUX_IRQ) : (uint8)(cfg & ~CFG_AUX_IRQ);
    return write_config(cfg);
}

/* --- the bus -------------------------------------------------------------- */

static bus_dev_t      *atkbdc_root;
static atkbdc_ivars_t  aux_ivars = { ATKBDC_PORT_AUX, 12 };

void atkbdc_init(void) {
    uint8 cfg, test;
    bus_dev_t *child;

    /* A controller at all: the status port of an absent one floats to 0xFF,
     * which reads as "input buffer full" forever and fails the first
     * command below. */
    if (read_config(&cfg) != 0) {
        kprintf_c(0x0E, "atkbdc: no i8042 controller answers - no PS/2 "
                        "mouse\n");
        return;
    }

    /* The aux port. Its interrupt stays OFF until psm has configured the
     * mouse: the ACKs to its commands are read by polling, and an IRQ 12
     * for each would arrive later with nothing in the buffer. Its clock is
     * switched on by CMD_AUX_ENABLE. */
    cfg = (uint8)(cfg & ~CFG_AUX_IRQ);
    if (write_config(cfg) != 0 || command(CMD_AUX_ENABLE) != 0) {
        kprintf_c(0x0C, "atkbdc: could not enable the aux port\n");
        return;
    }
    if (command(CMD_AUX_TEST) != 0 || read_byte(0, &test, 100) != 0 ||
        test != 0x00) {
        kprintf_c(0x0E, "atkbdc: aux port test failed - no PS/2 mouse "
                        "port\n");
        return;
    }
    if (read_config(&cfg) != 0 || (cfg & CFG_AUX_CLOCK_OFF)) {
        kprintf_c(0x0E, "atkbdc: aux clock stays off - no PS/2 mouse "
                        "port\n");
        return;
    }

    atkbdc_root = bus_add_child(NULL, NULL, NULL);
    if (atkbdc_root == NULL) {
        kprintf_c(0x0C, "atkbdc: no bus node\n");
        return;
    }
    kprintf_c(0x0F, "atkbdc0: i8042, aux port present\n");

    if (hint_disabled("psm", 0)) {
        kprintf_c(0x0E, "atkbdc0: psm0 disabled by hint\n");
        return;
    }
    child = bus_add_child(atkbdc_root, devclass_find("atkbdc"), &aux_ivars);
    if (child == NULL) {
        kprintf_c(0x0C, "atkbdc: no bus node for psm0\n");
        return;
    }
    device_set_devclass(child, "psm");
    bus_attach_children(atkbdc_root);
}
