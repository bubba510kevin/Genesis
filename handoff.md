# Handoff — Genesis

Last updated 2026-09-26. Read this first if you're picking the project back
up cold. It points at the detailed prose in `ROADMAP.md` rather than
repeating it — this file is orientation, `ROADMAP.md` is the record.

## What Genesis is

A from-scratch x86-64 OS kernel (boots on real bare metal and QEMU) that
deliberately supports three ecosystems at once: native ELF/musl userspace, a
Windows subsystem (`/wsr`, clean-room `ntdll.dll`/`kernel32.dll`, real PE
binaries run on it), and three driver models side by side (FreeBSD Newbus,
Linux via a LinuxKPI shim, Windows WDM/KMDF) — all three run *unmodified*
vendored driver source against real hardware. Large parts of the kernel are
real vendored FreeBSD source (`kernel/bsd/`) and ZFS (`kernel/zfs/`, kept
CDDL-separate on purpose), not reimplementations.

Engineering culture: self-tests everywhere, bug postmortems live in
`src/verif.c`, and `ROADMAP.md` is written as dense narrative prose
explaining *why* each decision was made — item numbers are never renumbered
because source comments reference them by number.

## THE GOAL

**A daily-drivable desktop that runs the Windows 7 DE, and can run real Win32
binaries.** That's the actual destination — not "an interesting kernel
experiment." Everything else in this file is either progress toward it or a
side-quest that happened to be useful on the way.

**The full dependency inventory for that goal is `ROADMAP.md` item 14** —
read it before assuming what's needed. Short version: the single decision
that determines everything else is that Genesis will write its **own**
clean-room `user32.dll`/`gdi32.dll`/`shell32.dll` (the same strategy already
proven at small scale for `ntdll`/`kernel32`) rather than try to run the real
Microsoft ones, because those talk directly to `win32k.sys` — an entire
undocumented kernel subsystem Windows Vista+ moved GDI/USER into, and
reverse-engineering it is the single hardest, buggiest thing ReactOS has
spent decades on. A clean-room DLL implementing the *documented* export
surface is indistinguishable to any well-behaved third-party `.exe`.

**Honest scale check**: item 14 alone (real multithreading, structured
exception handling, the registry, USER32/GDI32/COM/RPC, a display driver, a
mouse driver, USB, a session/service architecture, and either a real shell
namespace or a native fallback shell) is collectively bigger than everything
built in this tree so far, combined. ReactOS has been working on almost
exactly this problem since 1996 and still doesn't have a daily-drivable
modern desktop. That's the honest comparison, not a discouraging one — it
means this is a real, hard, well-precedented problem, not one this project is
failing to solve quickly.

**Where this session's work fits on that path**: not directly on it, but not
a detour either. The NT ACL/security-descriptor work (Unit 3 below) is
exactly the foundation item 14(g) — a real `RegGetKeySecurity` or
`SetNamedSecurityInfo` — would sit on top of. The "supreme" privilege (Unit
1) is the same kind of primitive real Windows' SYSTEM/TrustedInstaller
accounts are. None of it is multithreading, SEH, or USER32/GDI32 though —
those are still fully unstarted, and per item 14's own ordering, real
multithreading is the biggest single missing prerequisite before any of the
graphical stack can even begin.

## Project state — most recent session, three connected units

All three fully verified (host tests + real kernel boot in QEMU with real
device I/O) — see "How to verify" below.

### Unit 1 — the "supreme" privilege
`kernel/fs/acl.c`'s root bypass in `acl_access` used to carry a comment
predicting this exact feature ("when this kernel grows privileges as a real
concept, this is the line that becomes a privilege test instead of a uid
test"). `cred_is_supreme()` is that: root OR the one uid a root process has
designated (`genesis_supreme_uid_get/set`, `kernel/proc/process.c` — a single
kernel-global, never stored per-process, so it can't go stale). Granted via
`prctl()` (`PR_GENESIS_GRANT_SUPREME`/`REVOKE`/`QUERY` in
`kernel/proc/syscall.c`) rather than a new syscall number, root-gated,
kprintf-logged. Boot selftest: `kernel/fs/acl_selftest.c`.

**Not done**: no ring-3/systest.c check that a *non-root* caller's grant
attempt is actually refused with `-EPERM` (only reasoned about in code, not
tested from userland yet). `kernel/fs/ntsec.c` also doesn't render the
supreme uid's SID as `S-1-5-18` (SYSTEM) — cosmetic, doesn't affect the
actual privilege bypass.

### Unit 2 — gnfs, the native COW filesystem
New subsystem, `kernel/gnfs/` (BSD-2-Clause, no CDDL boundary, not under
`kernel/zfs/`): `gnfs_format.c` (pure: checksum, root record, bitmap
allocator, layout helpers), `gnfs_object.c` (pure: onode table, directory
entries), `gnfs_vfs.c` (device-facing: real `fs_ops_t`, prober, commit ring).
Public header `kernel/include/gnfs.h` mirrors `zfs.h`'s one-call opacity.
Format tool: `tools/mkgnfs.c`.

Shape: a fixed ring of root records (`txg % N`, reusing the uberblock ring's
own crash-consistency argument) each naming a bitmap region and an object
table region, both COW'd as whole-structure ping-pongs between two fixed
locations each commit. The object table is FLAT (an object number is a
direct array index, never an indirect chain of blocks pointing at blocks) —
that flatness is the actual fix for the depth that broke the old
OpenZFS-compatible write path. Files/dirs capped at `GNFS_OBJ_DIRECT * 4096`
= 48KB (no indirect blocks yet, refused past that with `-EFBIG` rather than
truncated). `fs_ops_t` is real: lookup, read, write (gaps read as zero),
iterate, statfs, create, truncate, mkdir, rmdir, unlink. `rename` is NULL
(deferred).

**Not done, disclosed in ROADMAP.md**: the dataset directory and snapshot
retention. Retaining an old root record already IS a real, untouched
snapshot (COW throughout means nothing was overwritten in place) — but the
allocator doesn't yet know to *refuse* handing out a block a retained
snapshot still references, since there's only one live bitmap. That's the
one piece of "keep snapshots" from the original scoping that's still open.

### Unit 3 — the ACL write path, on gnfs
`fs_ops_t` gained `setacl`; `kernel/fs/vfs.c`'s `fs_setacl()` gates it on
`ACE_WRITE_ACL` via the same `fs_access` check everything else uses (so
root/supreme bypass it automatically, same as read). `gnfs_onode_t` gained
`acl_block` (0 = no stored ACL, falls back to the existing mode-projection
convention — same convention `ZFS_ACL_TRIVIAL` already established one layer
down). Two new pure functions in `kernel/fs/acl.c`: `acl_inherit` (real
NT/NFSv4 inheritance semantics: `FILE_INHERIT`/`DIRECTORY_INHERIT`,
`NO_PROPAGATE`, floored on a plain mode-derived ACL) and `acl_apply_chmod`
(rewrites only owner@/group@/everyone@, leaves named grants/denies/inherited
entries untouched). `gnfs_op_create`/`gnfs_op_mkdir` call `acl_inherit`
against the parent's effective ACL. New syscalls `chmod(2)`/`fchmod(2)`
(real Linux numbers 90/91) built entirely on
`fs_getacl` → `acl_apply_chmod` → `fs_setacl`.

**Not done, disclosed in ROADMAP.md**: `chown`/`fchown`. It needs
`ACE_WRITE_OWNER` as its *own* gate, not a reuse of `ACE_WRITE_ACL` — an
ordinary owner holds `WRITE_ACL` unconditionally (so they can always chmod
their own file) but should NOT thereby be able to give it away to an
arbitrary other uid, which is exactly the distinction POSIX draws between
chmod and chown. Getting that right is a small, self-contained next step,
not a copy-paste of chmod's wiring.

## What to work on next

**Immediate loose ends from this session** (small, well-scoped, sitting
right where the work stopped):
1. ~~`chown`/`fchown`~~ — **DONE 2026-09-26**: syscalls 92/93 →
   `fs_setowner` (vfs.c) → pure `acl_chown_permitted` (acl.c) →
   `fs_ops_t::setowner` (gnfs). WRITE_OWNER = take-for-self only; owner may
   chgrp into its own groups; supreme may do anything. See ROADMAP.md
   ("CHOWN, ADDED THE NEXT SESSION"). Left open: suid/sgid clearing on
   chown (harmless until exec honours those bits), and a ring-3 systest.
1b. ~~Creator ownership~~ — **DONE 2026-09-26**: `fs_ops_t::create`/`mkdir`
   take the creator's `cred`; gnfs records euid/egid. Found while doing it,
   all still open (ROADMAP.md, "THE CREATOR NOW OWNS WHAT IT CREATES"):
   - ~~No parent-directory write check on create/mkdir~~ — **DONE
     2026-09-26** (`fs_may_add_entry`, vfs.c): w+x on the parent, -EEXIST
     checked first. Side effect: non-root can't create on FAT (root-owned
     0755 projection); nothing does today.
   - ~~Deletion ungated~~ — **DONE 2026-09-26**: unlink/rmdir/rename take
     the cred (`fs_may_remove_entry`, vfs.c): ACE_DELETE on the object OR
     DELETE_CHILD+EXECUTE on the parent; rename = remove + add + remove
     replaced. Also fixed: directory w never granted ACE_DELETE_CHILD, so
     `access(dir, W_OK)` failed for non-root owners.
   - ~~chmod special bits~~ — **DONE**: `fs_chmod` (vfs.c) sets/clears
     suid/sgid/sticky in the same commit as the ACL (`setacl` gained a
     `special` arg); sgid silently dropped for non-members, as Linux.
   - ~~setgid directories~~ / ~~sticky directories~~ — **DONE**: new objects
     take a setgid parent's group (subdirs inherit sgid); in a sticky dir
     only the entry's/dir's owner or supreme may remove (-EPERM).
   - ~~create mode ignored~~ — **DONE**: open(O_CREAT)/mkdir honour
     `mode & ~umask` (perm bits; +sticky for mkdir). Default umask now 022.
   - **Still open:** chown should clear suid/sgid (POSIX) — harmless until
     exec honours those bits, must land with that; moving a directory to a
     new parent should need write on the directory itself (".." changes);
     ~~none of this exercised from ring 3~~ — **DONE**: systest's
     `test_gnfs_perms` (57 checks, as real uids, on `/mnt/f`) covers every
     item above end to end through the real syscalls, plus the supreme
     grant/refusal; two planted plumbing bugs (unlink passing NULL, umask
     not applied) each fail it.
2. Snapshot-aware allocation (Unit 2) — the allocator needs to know about
   every *retained* root record's bitmap, not just the live one.
3. The dataset directory (Unit 2) — multiple named filesystems in one gnfs
   volume, what makes "snapshot"/"dataset" real user-facing concepts.
4. Indirect blocks (Unit 2) — lift the 48KB file cap; one bounded indirect
   block, matching classic Unix inode design.
5. `rename(2)` on gnfs — currently NULL.
6. ~~The ring-3 test for `PR_GENESIS_GRANT_SUPREME`'s `-EPERM` gate~~ —
   **DONE 2026-09-26**, in systest's gnfs section (below).
7. SACL/auditing — bigger, lower priority.

**The actual next milestone toward THE GOAL**, once the above is cleared or
if you want to jump straight at it: **real multithreading** — item 14(a),
and per its own ordering the biggest single missing prerequisite before any
of the USER32/GDI32/display work can start. `process.h`'s own
`sig_handlers`/`sig_blocked` split already has a comment naming `clone()` as
the thing that will eventually need this. Everything graphical assumes
threads exist.

## How to verify anything above still works

This machine has no native gcc/qemu toolchain (Windows). Build and test
through WSL Debian, which has everything, pointed at this same working
copy via `/mnt/c/...` — no separate Gentoo box or SSH needed:

```
wsl.exe -d Debian -- bash -lc "cd '/mnt/c/Users/kevin/code/Genesis/Genesis' && bash tests/host/run.sh"
wsl.exe -d Debian -- bash -lc "cd '/mnt/c/Users/kevin/code/Genesis/Genesis' && python3 build.py image"
```

**The full check - boots the real machine and runs the ring-3 suites.**
`tools/guest_run.py` boots with `build.py run`'s drives (FAT root, both ZFS
pools, and - since 2026-09-26 - a fresh gnfs volume at `/mnt/f`), types
`/bin/verif` and `/bin/systest` into the shell over serial, and waits for
each tally. About five minutes:
```
wsl.exe -d Debian -- bash -lc "cd '/mnt/c/Users/kevin/code/Genesis/Genesis' && python3 tools/guest_run.py > build/guest.txt 2>&1"
grep -E "^(verification|systest): [0-9]" build/guest.txt   # expect 0 failed on both
```
As of 2026-09-26: `verification: 160 passed, 0 failed`,
`systest: 439 passed, 0 failed`.

After changing `src/systest.c` (or `src/verif.c`), rebuild it and restage
the FAT disk - `build_user.sh` stages via `sudo mount`, which needs a
password, so do the systest part by hand and let `build.py disk` restage
from `root/` with its own FAT writer (no sudo):
```
wsl.exe -d Debian -- bash -lc "cd '/mnt/c/Users/kevin/code/Genesis/Genesis' && gcc -std=c99 -Wall -Wextra -O1 -static -no-pie -nostdlib -nostartfiles -ffreestanding -fno-stack-protector -fno-builtin -mno-red-zone -o root/bin/systest src/systest.c && rm -f build/disk.img && python3 build.py disk"
```
(`build/disk.img` is regenerated from `root/` that way; the one from before
2026-09-26 is kept as `build/disk.img.bak-2026-09-26` in case it held
hand-dropped files.)

**Do NOT boot-test gnfs by putting it on IDE index 1** (an older version of
this file said to): the first volume to mount becomes `/`, so the empty
gnfs disk becomes the root, there is no `/bin/busybox` on it, and no
userland ever runs - the boot looks clean only because nothing ran. Use
`guest_run.py`, or `build.py run`, which attach it correctly.

Version control: this path is a git repo since 2026-09-26 (branch `main`).
