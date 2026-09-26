#include "acl.h"
#include "kprintf.h"
#include "process.h"
#include "typesk.h"

/* The check for the "supreme" privilege (root + NT AUTHORITY\SYSTEM + NT
 * SERVICE\TrustedInstaller fused into one exemption from every ACL check -
 * see cred_is_supreme in acl.h).
 *
 * --- why this is a boot-time selftest and not a systest section -----------
 * Half of it - cred_is_supreme and acl_access's bypass - is pure logic with
 * no I/O and no process, and belongs with the rest of acl.c's own testing
 * philosophy (see the header comment there: "nothing here does I/O,
 * allocates, or touches a filesystem"). The other half -
 * genesis_supreme_uid_get/set - IS reachable from ring 3 through
 * PR_GENESIS_GRANT_SUPREME, unlike a kernel thread, so the -EPERM-for-non-
 * root gate on that path is exercised from systest.c instead; this file
 * checks only that the global the syscall sets is the global acl_access
 * reads.
 *
 * --- what makes this a test rather than a smoke check ---------------------
 * The failure this is guarding against is not "supreme does nothing" - that
 * fails loudly the first time anyone uses it. It is "supreme does too much
 * or too little silently": a cred_is_supreme that also returns true for an
 * ordinary uid, or an acl_access bypass that only fires when the ACL would
 * have allowed the access anyway (which is not a bypass, it is a no-op that
 * happens to pass a test that never denies anything). So every assertion
 * below pairs a case that must stay denied with the one change that must
 * flip it, rather than checking allow-cases alone. */

static int g_fail;

static void check(int cond, const char *what) {
    if (!cond) {
        kprintf("acl: FAIL - %s\n", what);
        g_fail = 1;
    }
}

void acl_selftest(void) {
    cred_t c;
    acl_t a;
    int saved;

    g_fail = 0;

    /* --- cred_is_supreme, in isolation from acl_access ------------------- */

    cred_init_nobody(&c);
    check(!cred_is_supreme(&c), "a nobody credential is not supreme");

    cred_init_nobody(&c);
    c.euid = 0;
    check(cred_is_supreme(&c), "root alone must still count as supreme");

    cred_init_nobody(&c);
    c.euid = 4242;
    c.supreme = 1;
    check(cred_is_supreme(&c),
          "the supreme flag must grant it to a non-root uid");

    cred_init_nobody(&c);
    c.euid = 4242;
    c.supreme = 0;
    check(!cred_is_supreme(&c),
          "an ordinary uid with the flag clear must NOT be supreme - "
          "the control for the case above");

    /* --- acl_access actually bypassing, not merely a permissive ACL ------- */

    /* owner uid 1, group 1, mode 0000: grants NOTHING to anyone, which is
     * the point - a bypass that only shows up against a permissive ACL is
     * not a bypass. */
    acl_from_mode(0, 1, 1, &a);

    cred_init_nobody(&c);
    c.euid = 4242;
    check(acl_access(&a, &c, ACE_READ_DATA) != 0,
          "an ordinary uid must be denied by a 0000 ACL - the control for "
          "the bypass check below");

    c.supreme = 1;
    check(acl_access(&a, &c, ACE_READ_DATA) == 0,
          "supreme must bypass a 0000 ACL that denies everyone, the same "
          "way root already does");

    /* And root itself is unaffected by any of the above - this is not a new
     * code path root goes through, it is the same line. */
    cred_init_nobody(&c);
    c.euid = 0;
    check(acl_access(&a, &c, ACE_READ_DATA) == 0,
          "root must still bypass a 0000 ACL");

    /* --- chown: an owner holds WRITE_ACL and still may not give away ---- */

    acl_from_mode(0644, 4242, 4242, &a);
    cred_init_nobody(&c);
    c.euid = 4242;
    check(acl_chown_permitted(&a, &c, 555, ACL_CHOWN_KEEP) != 0,
          "an ordinary owner must not be able to chown its file away");

    c.supreme = 1;
    check(acl_chown_permitted(&a, &c, 555, ACL_CHOWN_KEEP) == 0,
          "the supreme uid must be able to - the control for the above");

    /* --- the global genesis_supreme_uid_get/set round-trips -------------- */

    saved = genesis_supreme_uid_get();
    check(saved == -1, "nothing should hold supreme this early in boot");

    genesis_supreme_uid_set(7);
    check(genesis_supreme_uid_get() == 7,
          "the getter must report what the setter just set");

    genesis_supreme_uid_set(-1);
    check(genesis_supreme_uid_get() == -1,
          "revoke (-1) must clear it back to nobody");

    if (g_fail) {
        kprintf("acl: selftest FAILED\n");
    } else {
        kprintf("acl: supreme selftest passed\n");
    }
}
