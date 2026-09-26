#ifndef PIC_H
#define PIC_H

#include "typesk.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define PIC1_OFFSET 0x20   /* IRQ0-7  -> vectors 32-39 */
#define PIC2_OFFSET 0x28   /* IRQ8-15 -> vectors 40-47 */

void pic_remap(uint8 offset1, uint8 offset2);
void pic_send_eoi(uint8 irq);
void pic_set_mask(uint8 irq);
void pic_clear_mask(uint8 irq);

#endif
