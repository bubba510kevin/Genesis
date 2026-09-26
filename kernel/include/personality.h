#ifndef PERSONALITY_H
#define PERSONALITY_H

#include "process.h"
#include "typesk.h"

struct syscall_frame;

/* Which syscall ABI a process speaks.
 *
 * --- Why this is a table and not an if ------------------------------------
 * A process tagged PERSONALITY_WINDOWS enters the kernel through the same
 * `syscall` instruction as a Linux one, on the same CPU, with arguments in
 * the same registers. Nothing about the entry path can tell them apart, and
 * nothing about it should have to. What differs is only the meaning of the
 * number in RAX - 0 is read(2) to one and something else entirely to the
 * other - and that is one indirection, taken once per syscall.
 *
 * The alternative is a branch inside every handler, or a second switch bolted
 * onto the first, and both of those are the arrangement that makes a bisect
 * useless later: a bug in the NT surface and a bug in the Linux surface look
 * the same from outside because they live in the same function.
 *
 * This lands as its own change, ahead of any PE parsing, precisely so that
 * "the split broke it" and "the loader broke it" can never be the same
 * commit. The Linux table below IS today's dispatch, moved and not rewritten;
 * the NT table is empty. Nothing tags a process PERSONALITY_WINDOWS yet, so
 * nothing should behave differently - and systest is what says so.
 *
 * --- What belongs in here -------------------------------------------------
 * Only things whose answer depends on the NUMBER SPACE. The dispatch itself
 * obviously does. So does is_sigreturn, which is less obvious and is the
 * reason this struct has two members rather than one: the signal-delivery
 * check in syscall_dispatch compares RAX against SYS_rt_sigreturn (15), and
 * 15 means something different under an ABI whose numbers are ours to define.
 * Left as a bare constant, it would silently suppress signal delivery for
 * whichever NT call happened to land on 15.
 *
 * Everything else - what a handle is, what an object is, which console you
 * get - is deliberately NOT here. Those are shared, and the whole point of
 * one namespace and one object manager is that a PE binary and an ELF binary
 * reach the same console rather than two that drift. */

typedef struct syscall_personality {
    const char *name;
    personality_t id;

    /* Number in RAX, arguments in RDI/RSI/RDX/R10/R8/R9, result in RAX.
     * That much is the x86-64 SYSCALL convention and is not negotiable by a
     * personality; everything above it is. */
    uint64 (*dispatch)(struct syscall_frame *frame);

    /* Is `nr` the call that unwinds a signal frame? Delivering a signal into
     * one nests a second handler on top of the frame it is in the middle of
     * restoring. A personality with no signals answers no to everything. */
    int (*is_sigreturn)(uint64 nr);
} syscall_personality_t;

/* The table for a given tag. Never NULL - an unrecognised tag gets the Linux
 * table, because the alternative is a null call through a function pointer on
 * the syscall path, and a process with a corrupt tag should fail as a wrong
 * answer rather than as a triple fault. */
const syscall_personality_t *personality_for(personality_t id);

/* The table for the running process. Also never NULL: before there is a
 * current process there is nothing but the kernel's own ELF world. */
const syscall_personality_t *personality_current(void);

#endif
