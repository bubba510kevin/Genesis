#ifndef LINUX_COMPLETION_H
#define LINUX_COMPLETION_H

#include "linux/types.h"

/* <linux/completion.h> - "wait until something has happened", counted.
 *
 * complete() releases one waiter (or banks one release if nobody waits yet),
 * complete_all() releases every waiter now and later until reinit. The wait
 * really blocks: the waiting thread is off every CPU until complete() - on
 * this CPU or another - wakes it. Storage for the wait queue is opaque here
 * and checked against the kernel's in lkpi_smp.c. */
struct completion {
    volatile unsigned int done;
    unsigned long         wq_opaque[8];    /* a Genesis wait_queue_t: a bitmap of up to 512 slots */
};

#define COMPLETION_ALL_DONE 0x7FFFFFFFu

void init_completion(struct completion *x);
void reinit_completion(struct completion *x);
void complete(struct completion *x);
void complete_all(struct completion *x);
void wait_for_completion(struct completion *x);
/* Returns 0 on timeout, else the jiffies left (at least 1). */
unsigned long wait_for_completion_timeout(struct completion *x,
                                          unsigned long timeout);
/* -ERESTARTSYS if a signal ended the wait, else 0. */
int  wait_for_completion_interruptible(struct completion *x);
int  try_wait_for_completion(struct completion *x);
int  completion_done(struct completion *x);

#define DECLARE_COMPLETION(name) \
    struct completion name = { 0, { 0 } }
#define DECLARE_COMPLETION_ONSTACK(name) \
    struct completion name; init_completion(&name)

#endif
