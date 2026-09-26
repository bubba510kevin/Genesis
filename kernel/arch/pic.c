#include "io.h"
#include "pic.h"




void pic_remap(uint8 offset1, uint8 offset2) {
    uint8 mask1 = inb(PIC1_DATA);
    uint8 mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11); io_wait();  /* ICW1: start init, expect ICW4 */
    outb(PIC2_CMD, 0x11); io_wait();

    outb(PIC1_DATA, offset1); io_wait(); /* ICW2: vector offset */
    outb(PIC2_DATA, offset2); io_wait();

    outb(PIC1_DATA, 4); io_wait();  /* ICW3: tell master PIC2 is at IRQ2 */
    outb(PIC2_DATA, 2); io_wait();  /* ICW3: tell slave its cascade identity */

    outb(PIC1_DATA, 0x01); io_wait(); /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, mask1); /* restore saved masks */
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint8 irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}

void pic_set_mask(uint8 irq) {
    uint16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8 which = irq < 8 ? irq : irq - 8;
    uint8 value = inb(port) | (1 << which);
    outb(port, value);
}

void pic_clear_mask(uint8 irq) {
    uint16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8 which = irq < 8 ? irq : irq - 8;
    uint8 value = inb(port) & ~(1 << which);
    outb(port, value);
}
