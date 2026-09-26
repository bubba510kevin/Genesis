/* SHA-1, VENDORED from crypto/sha1.c.
 *
 * Reached from exactly one place: if_ethersubr.c's ether_gen_addr, which
 * hashes the host UUID and interface name into a locally-administered MAC
 * address for a NIC whose EEPROM is blank.
 *
 * Vendored rather than hand-written for the obvious reason - a hash function
 * with a subtle bug produces plausible-looking wrong output forever - and
 * because it stands entirely alone, which is the easiest vendoring decision
 * in this tree.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>

#include "vendor/sha1.inc"
