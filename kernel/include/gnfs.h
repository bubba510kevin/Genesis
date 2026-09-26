#ifndef GNFS_H
#define GNFS_H

/* Genesis Native FS: the ONE header the rest of the kernel sees.
 *
 * Mirrors kernel/include/zfs.h's own shape deliberately - a single opaque
 * entry point, everything else reachable only through it - even though
 * kernel/gnfs/ carries none of kernel/zfs/'s CDDL boundary concerns (this is
 * original code, not a vendored port). The pattern is worth reusing anyway:
 * it is what let ZFS be added without touching a file that already worked,
 * and it is what will let a THIRD filesystem be added the same way. */

/* Register the gnfs prober with volume.c. Call once at boot, before
 * volume_init - see zfs_init's own comment on why order matters here. */
void gnfs_init(void);

#endif /* GNFS_H */
