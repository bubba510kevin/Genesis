#ifndef _MACHINE_ENDIAN_H_
#define _MACHINE_ENDIAN_H_
/* Redirect to the hand-written <sys/endian.h> - see there for why that one
 * is adapted rather than vendored (FreeBSD's is written against the
 * __uint8_t underscore type family). One definition, two spellings, because
 * both are included by real source. */
#include <sys/endian.h>
#endif
