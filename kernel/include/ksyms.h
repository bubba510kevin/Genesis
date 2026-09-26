#ifndef KSYMS_H
#define KSYMS_H

#include "typesk.h"

/* Kernel self-symbol table (ROADMAP item 11 - "A KERNEL SELF-SYMBOL
 * TABLE"). kernel.elf keeps a full .symtab/.strtab as a build artifact,
 * but nothing in-kernel read it back at runtime and objcopy -O binary
 * strips it from kernel.bin regardless - zero backtrace/ksyms facility
 * existed. This is that facility.
 *
 * The table itself is not a compiled-in C array (that would need a second
 * link to know the addresses to record - see build.py's gen_ksyms() for
 * why it's a post-link binary patch into linker.ld's reserved .ksyms
 * section instead). This header is just the read side. */

/* Address -> the nearest known symbol AT OR BELOW it, and the byte offset
 * from that symbol's start. Returns NULL (leaving *out_offset untouched)
 * if the table is empty or addr is below every known symbol - both mean
 * "not a kernel image address", not an error. */
const char *ksym_lookup(uint64 addr, uint64 *out_offset);

/* The inverse, for the loadable-module linker (kernel/kldload.c): a symbol
 * NAME to its address, or 0 if the kernel does not export it. Zero is
 * unambiguous - nothing lives at address zero. */
uint64 ksym_resolve(const char *name);

#endif
