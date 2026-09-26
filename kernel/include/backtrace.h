#ifndef BACKTRACE_H
#define BACKTRACE_H

#include "typesk.h"

/* Frame-pointer-chain stack walk, printed through ksyms.c's lookup
 * (ROADMAP item 11 - the fatal-exception path used to print only a bare
 * RIP; this is what turns that into an actual call stack).
 *
 * Relies on every kernel function keeping a standard `push rbp; mov rbp,
 * rsp` prologue, which this build already gets for free: build.py's
 * CFLAGS pass no -O flag at all, so GCC defaults to -O0, and frame-pointer
 * omission never happens at -O0 regardless of target. If that ever
 * changes, this file needs -fno-omit-frame-pointer added explicitly, not
 * a rewrite.
 *
 * `rip` is frame 0 (wherever execution actually was); `rbp` is the frame
 * pointer to start walking FROM - the caller's, i.e. the value that was
 * live at `rip`. interrupt_frame (interrupt.h) already captures both for
 * every trap. */
void backtrace_print(uint64 rip, uint64 rbp, uint8 color);

#endif
