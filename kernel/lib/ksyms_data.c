#include "typesk.h"

/* Real reserved space for the kernel symbol table (ROADMAP item 11) -
 * build.py's gen_ksyms() overwrites these bytes in kernel.bin after the
 * normal link. Must be defined here, as a real C object with at least one
 * non-zero byte, rather than as a bare ". += N;" in linker.ld: an all-
 * zero array (or a linker-script location-counter reservation with no
 * input section at all) gets classified NOBITS by ld - same as .bss - and
 * objcopy -O binary then emits nothing for it, leaving nowhere in
 * kernel.bin for gen_ksyms() to write into. The leading 1 is what forces
 * this into a real PROGBITS section; see kernel/ksyms.c, which never
 * looks at this content's own value, only its address and size (must
 * match KSYMS_RESERVED_SIZE in build.py exactly). */
__attribute__((section(".ksyms")))
const uint8 __ksyms_reserved[262144] = { 1 };
