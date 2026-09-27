#include "ioapic.h"
#include "irq.h"
#include "kprintf.h"
#include "lapic.h"
#include "pic.h"
#include "typesk.h"

/* Mask and unmask a legacy line at whichever controller is actually
 * delivering it. Every call site below used to say pic_clear_mask /
 * pic_set_mask directly; routed through here instead so that "which
 * controller" is answered once. See irq_eoi in irq.h for the same argument
 * applied to acknowledgement. */
static void line_unmask(uint8 irq) {
    if (ioapic_active()) {
        ioapic_unmask_irq(irq);
    } else {
        pic_clear_mask(irq);
    }
}

static void line_mask(uint8 irq) {
    if (ioapic_active()) {
        ioapic_mask_irq(irq);
    } else {
        pic_set_mask(irq);
    }
}

void irq_eoi(uint8 irq) {
    if (ioapic_active()) {
        lapic_eoi();
    } else {
        /* pic.c already handles acknowledging the slave before the master
         * for IRQ 8-15, which is the ordering that matters. */
        pic_send_eoi(irq);
    }
}

/* See irq.h. IRQ_COUNT covers the full legacy 0-15 range even though 0
 * and 1 are reserved (interrupt.c never calls irq_dispatch for them) -
 * indexing straight by IRQ number beats a 14-entry table with an offset
 * everyone has to remember to subtract. */
#define IRQ_COUNT 16

/* A shared pool rather than a fixed array per line. 32 registrations across
 * all 14 usable lines, against 14 * N if every line carried its own array -
 * and the distribution is nothing like uniform, since PCI routing crowds
 * several functions onto a couple of lines and leaves the rest empty. The
 * same reasoning bcache and the fatfs volume pool already use. */
#define IRQ_HANDLER_MAX 32

struct irq_slot {
    irq_handler_fn fn;
    void          *ctx;
    int            next;         /* index into slots, or -1 */
};

static struct irq_slot slots[IRQ_HANDLER_MAX];
static int             heads[IRQ_COUNT];
static int             heads_ready;

/* Interrupts nobody claimed, per line. See irq_report for why this is
 * counted and not acted on. */
static uint64 unclaimed[IRQ_COUNT];

/* Every interrupt taken, per line - the load irqbalance plans from. */
static uint64 delivered[IRQ_COUNT];

uint64 irq_delivered(uint8 irq) {
    return irq < IRQ_COUNT ? delivered[irq] : 0;
}

/* -1 is the empty-list marker and 0 is a valid slot index, so the head
 * table cannot start as .bss zeroes. Done lazily rather than in an
 * irq_init() the boot path would have to call at the right moment: there is
 * exactly one thing to initialise and no ordering constraint on it, so a
 * flag here is less to get wrong than a new init hook in flk.c. */
static void ensure_heads(void) {
    int i;

    if (heads_ready) {
        return;
    }
    for (i = 0; i < IRQ_COUNT; i++) {
        heads[i] = -1;
    }
    heads_ready = 1;
}

static int slot_alloc(void) {
    int i;

    for (i = 0; i < IRQ_HANDLER_MAX; i++) {
        if (slots[i].fn == NULL) {
            return i;
        }
    }
    return -1;
}

int irq_register(uint8 irq, irq_handler_fn handler, void *ctx) {
    int s, tail;

    ensure_heads();

    if (irq < 2 || irq >= IRQ_COUNT || handler == NULL) {
        return -1;
    }
    s = slot_alloc();
    if (s < 0) {
        /* Pool exhausted. Reported rather than silently dropped: a driver
         * that thinks it registered an interrupt handler and did not will
         * present as a device that never responds, a long way from here. */
        kprintf_c(0x0C, "irq: handler pool full (%d), IRQ %d not registered\n",
                  IRQ_HANDLER_MAX, irq);
        return -1;
    }

    slots[s].fn   = handler;
    slots[s].ctx  = ctx;
    slots[s].next = -1;

    /* Appended, not pushed. Registration order is the dispatch order, and a
     * driver that attaches first should be asked first - pushing at the head
     * would silently reverse that and make dispatch order depend on attach
     * order backwards, which is the kind of thing that only shows up as a
     * timing difference between two machines. */
    if (heads[irq] < 0) {
        heads[irq] = s;
    } else {
        tail = heads[irq];
        while (slots[tail].next >= 0) {
            tail = slots[tail].next;
        }
        slots[tail].next = s;
    }

    line_unmask(irq);
    return 0;
}

void irq_unregister_handler(uint8 irq, irq_handler_fn handler, void *ctx) {
    int cur, prev = -1;

    ensure_heads();

    if (irq < 2 || irq >= IRQ_COUNT) {
        return;
    }
    for (cur = heads[irq]; cur >= 0; prev = cur, cur = slots[cur].next) {
        if (slots[cur].fn != handler || slots[cur].ctx != ctx) {
            continue;
        }
        if (prev < 0) {
            heads[irq] = slots[cur].next;
        } else {
            slots[prev].next = slots[cur].next;
        }
        slots[cur].fn   = NULL;
        slots[cur].ctx  = NULL;
        slots[cur].next = -1;
        break;
    }

    /* Only once the line is genuinely empty. Masking on any unregistration
     * would silence the line for every device still on it - the specific
     * bug that makes shared IRQs hard, and the reason this is not just
     * "call pic_set_mask in unregister". */
    if (heads[irq] < 0) {
        line_mask(irq);
    }
}

void irq_unregister(uint8 irq) {
    int cur, next;

    ensure_heads();

    if (irq < 2 || irq >= IRQ_COUNT) {
        return;
    }
    line_mask(irq);
    for (cur = heads[irq]; cur >= 0; cur = next) {
        next = slots[cur].next;
        slots[cur].fn   = NULL;
        slots[cur].ctx  = NULL;
        slots[cur].next = -1;
    }
    heads[irq] = -1;
}

void irq_dispatch(uint8 irq) {
    int cur;
    int claimed = 0;

    ensure_heads();

    if (irq >= IRQ_COUNT) {
        return;
    }
    delivered[irq]++;
    for (cur = heads[irq]; cur >= 0; cur = slots[cur].next) {
        if (slots[cur].fn(slots[cur].ctx)) {
            claimed++;
        }
    }
    if (claimed == 0) {
        unclaimed[irq]++;
    }
}

int irq_handler_count(uint8 irq) {
    int cur, n = 0;

    ensure_heads();

    if (irq >= IRQ_COUNT) {
        return 0;
    }
    for (cur = heads[irq]; cur >= 0; cur = slots[cur].next) {
        n++;
    }
    return n;
}

void irq_report(uint8 color) {
    int i;
    int any = 0;

    ensure_heads();

    for (i = 0; i < IRQ_COUNT; i++) {
        int n = irq_handler_count((uint8)i);

        if (n == 0 && unclaimed[i] == 0) {
            continue;
        }
        if (!any) {
            kprintf_c(color, "legacy IRQ lines:\n");
            any = 1;
        }
        kprintf_c(color, "  irq %d  %d handler%s  unclaimed %lx%s\n",
                  i, n, n == 1 ? "" : "s", unclaimed[i],
                  n > 1 ? "  (shared)" : "");
    }
    if (!any) {
        kprintf_c(color, "legacy IRQ lines: none registered\n");
    }
}

/* --- module unload: does any handler live in these bytes? -----------------
 *
 * Asked by kldload.c before it unmaps a module image. An interrupt handler
 * is the worst of the stale pointers a departing module can leave: it is
 * called from interrupt context, at an arbitrary moment, with no caller to
 * report the fault to and often nothing on the screen afterwards.
 *
 * BOTH the function and the ctx are checked, and the ctx is not the paranoid
 * half. A handler is typically a shared trampoline living in the KERNEL with
 * a softc from the MODULE as its ctx - if only the function were checked,
 * that registration would be declared clean and the first interrupt after
 * the unload would read a freed softc rather than fault, which is quieter and
 * worse. irq_unregister_handler already matches on the pair for the mirror
 * image of this reason. */
int irq_count_handlers_in_range(uint64 base, uint64 size) {
    int line;
    int n = 0;

    if (size == 0 || !heads_ready) {
        return 0;
    }
    for (line = 0; line < IRQ_COUNT; line++) {
        int idx;

        for (idx = heads[line]; idx >= 0; idx = slots[idx].next) {
            uint64 fn  = (uint64)slots[idx].fn;
            uint64 ctx = (uint64)slots[idx].ctx;

            if ((fn >= base && fn < base + size) ||
                (ctx != 0 && ctx >= base && ctx < base + size)) {
                n++;
            }
        }
    }
    return n;
}
