#ifndef TTY_H
#define TTY_H

#include "object.h"
#include "typesk.h"

/* The console terminal: its object, its termios line discipline and its
 * foreground process group. See kernel/dev/tty.c. */

/* Reset the terminal to the defaults a Linux console starts with (canonical,
 * echoing, signals on). Called once at boot, before any input arrives. */
void tty_init(void);

object_t *tty_console(void);

/* One byte of input from a keyboard or the serial line, already decoded to a
 * character. Runs the line discipline: input mapping, signal characters,
 * canonical editing and echo. Interrupt context; the big kernel lock is held. */
void tty_input(char c);

/* The terminal ioctls (TCGETS/TCSETS*, TIOC*PGRP, TIOCSCTTY, TIOC*WINSZ,
 * FIONREAD...). -ENOTTY for any request it does not handle. `arg` is the
 * caller's pointer (or value), already checked to be a user address when it
 * is non-zero. */
int64 tty_ioctl(uint32 request, uint64 arg);

int  tty_foreground_pgid(void);
void tty_set_foreground_pgid(int pgid);

#endif
