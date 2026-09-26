#ifndef _SYS_JAIL_H_
#define _SYS_JAIL_H_

/* <sys/jail.h> - FreeBSD's OS-level virtualization, stubbed.
 *
 * A jail restricts a process to a subset of the filesystem, the network
 * addresses and the privilege set. Network code consults it constantly:
 * "may this credential bind that address", "is this interface visible in
 * this jail". Genesis has one flat system with no jails and no credentials
 * to attach them to, so every such question has the same answer - yes, this
 * is the unrestricted case - which is exactly what prison0 means upstream.
 *
 * Stubbed rather than vendored: the real header is the whole jail parameter
 * system (cpusets, hostnames, per-jail sysctls) and it answers questions
 * that cannot be asked here.
 *
 * The direction these stubs err in is PERMISSIVE, and that is worth stating.
 * On a system with no privilege separation at all, "everything is allowed"
 * is not a security regression - there is nothing to separate. It would
 * become one the moment credentials mean something.
 */

struct ucred;
struct vnet;

/* struct prison is DEFINED here rather than left opaque, because
 * net/route/route_tables.c registers a jail-attach method that reads
 * pr_vnet off one. One field, because one field is what is read.
 *
 * prison0 is upstream's name for the unrestricted, always-present root
 * jail - every process is in it, and being in it means no restriction. */
struct prison {
    struct vnet *pr_vnet;
    /* The jail's name, which kern/uipc_socket.c prints in a diagnostic. One
     * jail, and it is the root one, so this is prison0's name upstream. */
    char         pr_name[8];
};

extern struct prison prison0;

/* The OSD (object-specific data) method slots a subsystem can register on a
 * jail. Upstream's enumeration; the routing table registers PR_METHOD_ATTACH
 * to refuse a process being moved into a jail whose vnet has fewer fibs than
 * its current one. There are no jails here, so no method is ever called -
 * but PR_MAXMETHOD sizes an array in vendored code, so the values matter. */
enum prison_method {
    PR_METHOD_CREATE = 0,
    PR_METHOD_GET,
    PR_METHOD_SET,
    PR_METHOD_CHECK,
    PR_METHOD_ATTACH,
    PR_METHOD_REMOVE,
    PR_MAXMETHOD
};

#define PRISON_IP4  1
#define PRISON_IP6  2

/* Is this credential jailed at all? Never. */
#define jailed(cred)                    (0)
#define jailed_without_vnet(cred)       (0)
/* May this credential use this address? Always - see above. */
#define prison_check_ip4(cred, ia)      (0)
#define prison_check_ip6(cred, ia)      (0)
#define prison_if(cred, sa)             (0)
#define prison_flag(cred, flag)         (0)
/* May this credential use this address family? All of them - see above. */
#define prison_check_af(cred, af)       (0)
#define prison_check_ip4_locked(pr, ia) (0)
/* "Give me the address this jail must use instead" - there is no jail, so the
 * caller's own choice stands. Upstream returns EAFNOSUPPORT when the jail has
 * no address of that family, and 0 with *ia rewritten when it does; 0 with
 * *ia untouched is the unrestricted case. */
#define prison_get_ip4(cred, ia)        (0)
#define prison_get_ip6(cred, ia)        (0)
/* Which address families a jail restricts. PR_IP4 means "this jail has an
 * IPv4 address list"; prison0 does not. */
#define PR_IP4                          0x00000010
#define PR_IP6                          0x00000020
#define prison_equal_ip4(p1, p2)        (1)
#define prison_local_ip4(cred, ia)      (0)
#define prison_remote_ip4(cred, ia)     (0)
#define prison_saddrsel_ip4(cred, ia)   (1)

/* --- the host identity ether_gen_addr derives a MAC from ------------------
 *
 * When a NIC has no address in its EEPROM, if_ethersubr.c invents a stable
 * locally-administered one by hashing the host UUID and the jail name. That
 * gives an address which survives a reboot and differs between machines.
 *
 * Genesis has neither. getcredhostuuid returns the all-zero DEFAULT_HOSTUUID
 * and getjailname returns the empty prison0 name - which is exactly what an
 * un-configured FreeBSD machine reports, and upstream handles that case: it
 * falls back to hashing the interface name instead.
 *
 * So the generated address is stable within a boot and NOT stable across
 * boots or between two machines running this image. That is a real
 * limitation for anything that remembers a MAC, and it is upstream's own
 * documented degraded path rather than something invented here. */
#define HOSTUUIDLEN      64
#define DEFAULT_HOSTUUID "00000000-0000-0000-0000-000000000000"

void getcredhostuuid(struct ucred *cred, char *buf, size_t size);
void getjailname(struct ucred *cred, char *name, size_t len);

#endif
