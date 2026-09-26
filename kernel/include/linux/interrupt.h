#ifndef LINUX_INTERRUPT_H
#define LINUX_INTERRUPT_H

#include "linux/types.h"

typedef int irqreturn_t;
#define IRQ_NONE    0
#define IRQ_HANDLED 1

typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);

#define IRQF_SHARED      0x0004  /* honoured - irq.c chains handlers per
                                   * line, and lkpi.c allocates one
                                   * trampoline record per registration
                                   * rather than one per IRQ number */
#define IRQF_NOBALANCING 0

/* Implemented in kernel/lkpi.c. Bridges Linux's (irq, dev_id) ->
 * irqreturn_t handler shape onto irq.h's (ctx) -> void shape via a small
 * per-registration trampoline record - the same role lkpi_request_irq/
 * lkpi_free_irq play in FreeBSD's own LinuxKPI, just against Genesis's
 * irq_register/irq_unregister (irq.h) instead of FreeBSD's bus_setup_intr. */
int  linux_request_irq(unsigned int irq, irq_handler_t handler,
                        unsigned long flags, const char *name, void *dev_id);
void linux_free_irq(unsigned int irq, void *dev_id);

#define request_irq(irq, handler, flags, name, dev_id) \
    linux_request_irq((irq), (handler), (flags), (name), (dev_id))
#define free_irq(irq, dev_id) linux_free_irq((irq), (dev_id))

#endif
