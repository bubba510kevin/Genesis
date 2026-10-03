#ifndef TMPFS_H
#define TMPFS_H

/* tmpfs - a filesystem in memory (ROADMAP 16(e)), kernel/fs/tmpfs.c.
 * Mounted at /tmp at boot. Files and directories with owners and modes,
 * read, write, truncate, create, mkdir, rmdir, unlink, rename, chmod and
 * chown, statfs; an unlinked file lives until its last open goes. Gone at
 * reboot, as a tmpfs is. Its size is capped at a quarter of RAM. */

struct fs_volume;

struct fs_volume *tmpfs_create(void);
void tmpfs_init(void);           /* make one and mount it at /tmp */

#endif
