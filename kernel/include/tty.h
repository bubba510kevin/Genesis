#ifndef TTY_H
#define TTY_H

#include "object.h"

/* The console, as an object.
 *
 * This exists to prove the object layer carries its weight: read() and
 * write() no longer test whether fd is 0, 1 or 2 - they look up a handle and
 * call through its type. Redirecting a descriptor at a file later changes
 * nothing in the syscall, because the syscall never knew what it was talking
 * to.
 *
 * One shared instance. A tty is a device, not a file: two opens are two views
 * of the same hardware, so there is nothing per-open to allocate. */
object_t *tty_console(void);

/* The process group signals from the keyboard are delivered to. Zero until a
 * shell claims it with TIOCSPGRP. */
int  tty_foreground_pgid(void);
void tty_set_foreground_pgid(int pgid);

#endif
