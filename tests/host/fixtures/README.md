# tests/host/fixtures

## `gnfsfix.img.gz` — the gnfs ACL fixture

A 4MB gnfs volume, gzipped. `build.py` unpacks a fresh copy on every run and
attaches it to the guest as the second volume, where it mounts at `/mnt/d`.

| Path | Owner | Mode | Why it is there |
|---|---|---|---|
| `/etc/motd` | root | 0644 | systest's "every volume behaves like the root" checks read it (open, read, seek, stat, getdents, `-ENOTDIR`, `-ENOENT`) |
| `/secret.txt` | 1000:1000 | 0600 + ACL | the ACL fixture, below |
| `/readable.txt` | 1000:1000 | 0644 | a file with **no** stored ACL, projected from its mode |
| `/open.txt` | 0:0 | 0666 | a control: modes are per file, not one constant |

### `secret.txt`'s ACL — the reason this fixture exists

Four entries, in this order:

1. ALLOW `owner@` read + write (+ metadata, write-ACL)
2. ALLOW user **1001** read (+ metadata)
3. DENY `everyone@` read + write + append + execute
4. ALLOW `everyone@` metadata only

Mode 0600 says uid 1001 may not read this file, and the ACL says it may. A
kernel that only looks at mode bits and one that honours the ACL **must
disagree about uid 1001**, so a test can tell which one is running. A
fixture whose ACL is only `owner@`/`group@`/`everyone@` can't do that,
because it's the mode word in another form.

The order matters too: NFSv4 and NT both let the **first** entry that mentions
a permission decide it, so the deny in third place must come after the two
allows it would otherwise override.

### How it is made, and kept honest

The image is built by **the kernel's own gnfs code**, run on the host through
the test harness's fake device, never by a second implementation of the
format: `tests/host/gnfs_fixture.c`, `gnfs_fixture_build()`.

Every `tests/host/run.sh` regenerates it (`vmm_test --gnfs-fixture PATH`) and
compares it **byte for byte** with the committed copy. So the committed image
can't drift from what the current gnfs writes. After a deliberate change to
the gnfs format or the fixture's contents:

    UPDATE_FIXTURES=1 bash tests/host/run.sh

The same file checks the fixture on the host (`gnfs_fixture_run_tests`): the
ACL decoded entry by entry, the access decisions it has to produce for the
owner, uid 1001, a stranger and root, and its Windows `SECURITY_DESCRIPTOR`
view (SIDs, entry order, the unchanged access mask, and the NFSv4→NT ACE flag
translation).

## History

This directory used to hold ZFS pools written by real OpenZFS, with a v1 and
a v5 ZPL pool both carrying the same `secret.txt` ACL. ZFS was removed from
the tree on 2026-09-26 and gnfs took over its fixtures; the pools and their
build scripts are in git history if they are ever wanted again.
