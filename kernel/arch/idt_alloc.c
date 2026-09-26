#include "idt.h"
#include "typesk.h"

/* The dynamic interrupt-vector allocator described in idt.h.
 *
 * A flat table indexed by vector rather than a bitmap plus a side array: 208
 * entries of {handler, ctx} is 3.3KB of .bss, the lookup on the interrupt
 * path is one index with no search, and there is no way for the "is it
 * allocated" bit and the "what runs" pointer to disagree. A bitmap would
 * save the pointers and buy a second source of truth for the same question,
 * which is the trade this file declines.
 */

#define VECTOR_COUNT (IDT_DYNAMIC_LAST - IDT_DYNAMIC_FIRST + 1)

static idt_vector_fn handlers[VECTOR_COUNT];
static void         *contexts[VECTOR_COUNT];

static int in_range(int vector) {
    return vector >= IDT_DYNAMIC_FIRST && vector <= IDT_DYNAMIC_LAST;
}

int idt_alloc_vector_at(int vector, idt_vector_fn handler, void *ctx) {
    int i;

    if (!in_range(vector) || handler == NULL) {
        return -1;
    }
    i = vector - IDT_DYNAMIC_FIRST;
    if (handlers[i] != NULL) {
        return -1;
    }
    /* Context first, then the handler. The order matters even on one CPU:
     * an interrupt can arrive between the two stores, and idt_dispatch_vector
     * tests the handler. Writing the handler last means a dispatch either
     * sees no handler at all, or sees one whose context is already valid -
     * never a handler with a stale context from whoever held this vector
     * before. */
    contexts[i] = ctx;
    handlers[i] = handler;
    return 0;
}

int idt_alloc_vector(idt_vector_fn handler, void *ctx) {
    int vector;

    /* Top down - see idt.h for why. */
    for (vector = IDT_DYNAMIC_LAST; vector >= IDT_DYNAMIC_FIRST; vector--) {
        if (handlers[vector - IDT_DYNAMIC_FIRST] == NULL) {
            if (idt_alloc_vector_at(vector, handler, ctx) == 0) {
                return vector;
            }
        }
    }
    return -1;
}

void idt_free_vector(int vector) {
    int i;

    if (!in_range(vector)) {
        return;
    }
    i = vector - IDT_DYNAMIC_FIRST;
    /* Handler first, mirroring the allocation order: after this store a
     * late interrupt on this vector finds nothing bound and is treated as
     * spurious, rather than calling a handler whose context has just been
     * cleared out from under it. */
    handlers[i] = NULL;
    contexts[i] = NULL;
}

int idt_dispatch_vector(uint8 vector) {
    int i;

    if (!in_range((int)vector)) {
        return 0;
    }
    i = (int)vector - IDT_DYNAMIC_FIRST;
    if (handlers[i] == NULL) {
        return 0;
    }
    handlers[i](contexts[i]);
    return 1;
}

int idt_vector_count(void) {
    int i;
    int n = 0;

    for (i = 0; i < VECTOR_COUNT; i++) {
        if (handlers[i] != NULL) {
            n++;
        }
    }
    return n;
}
