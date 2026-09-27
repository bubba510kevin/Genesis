/* crypto/siphash/siphash.c, VENDORED WHOLE (kernel/bsd/vendor/siphash.inc).
 *
 * SipHash-2-4, the keyed hash TCP uses for SYN cookies and initial sequence
 * numbers (tcp_subr's tcp_new_isn, tcp_syncache's cookie MAC) - so that an
 * off-path attacker cannot predict either.
 *
 * From FreeBSD main at 8b668bc7e7c8 (2026-08-10), the same snapshot every
 * other file in kernel/bsd/ was taken from. Not edited.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>

#include "vendor/siphash.inc"
