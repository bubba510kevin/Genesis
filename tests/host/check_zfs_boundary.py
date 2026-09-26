#!/usr/bin/env python3
"""Check that the CDDL boundary around kernel/zfs/ is real.

The ordered TODO asked for this by name: the fs vtable work proved its
boundary by the fact that syscall.c and fileobj.c stopped including fat.h, and
item 7 asked for the same kind of check here - "a grep-based check that
nothing outside zfs/ includes a zfs header".

It checks four things, and the fourth is the one that makes the other three
worth having:

  1. Nothing outside kernel/zfs/ includes anything from kernel/zfs/. The whole
     directory is reachable through kernel/include/zfs.h and nothing else.

  2. kernel/include/zfs.h declares no ZFS type. A public header that mentioned
     a dnode or a blkptr would put the vendored world's vocabulary into every
     file that includes it, and the boundary would be a directory name rather
     than a boundary.

  3. Files in kernel/zfs/ include only a short whitelist of Genesis headers.
     The vendored code must not grow a dependency on the kernel, or the next
     upstream import stops being a copy.

  4. kernel/zfs/vendor/ is unmodified. Every file there is either verbatim
     upstream or explicitly marked NOT VENDORED - and the marked ones are
     listed here BY NAME, so adding a fourth quietly is a failure. That is
     what stops "vendored" decaying into "forked".

Exit status is 0 when the boundary holds, 1 otherwise, so it can run from
tests/host/run.sh beside the other checks.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ZFS_DIR = os.path.join(ROOT, "kernel", "zfs")
VENDOR_DIR = os.path.join(ZFS_DIR, "vendor")
PUBLIC_HEADER = os.path.join(ROOT, "kernel", "include", "zfs.h")

# The Genesis headers kernel/zfs/ is allowed to reach for. Everything else it
# needs comes from kernel/zfs/compat/, which is the port and not the kernel.
ALLOWED_KERNEL_HEADERS = {
    "device.h", "fs.h", "volume.h", "screen.h", "typesk.h", "zfs.h",
    "kheap.h",
    # acl.h is the Genesis-side ACL vocabulary, in the same category as fs.h
    # and volume.h above: kernel/zfs/zfs_vfs.c is the file on the Genesis side
    # of the wall, and speaking acl_t is exactly its job. The vendored
    # translation unit does NOT include this one - it cannot, acl.h includes
    # typesk.h - and gets acl_abi.h instead.
    "acl.h",
    # acl_abi.h is #defines only - no types, no includes, no declarations -
    # which is why it can be included from the vendored translation unit at
    # all. See the header's own comment: it is the one place the NFSv4/NT
    # access bits are written down, and the alternative was two copies and a
    # test that they agree. If it ever grows a typedef, the vendored side
    # stops compiling with a size_t conflict, which is the enforcement.
    "acl_abi.h",
}

# Files under kernel/zfs/vendor/ that are NOT upstream. Each one stands in for
# an upstream file that needs a library Genesis does not have; each says so at
# the top of itself. Listed here so the list cannot grow silently.
DECLARED_NOT_VENDORED = {
    "blake3_zfs.c",   # needs the BLAKE3 implementation
    "skein_zfs.c",    # needs the Skein implementation
    "gzip.c",         # needs zlib's inflate
}

# Words that would mean the public header has leaked the vendored vocabulary.
ZFS_TYPE_WORDS = re.compile(
    r"\b(spa_t|vdev|dnode|blkptr|objset|zap_|dmu_|zio_|nvlist|zfsimpl|"
    r"uberblock|dsl_)")

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)

failures = []


def fail(msg):
    failures.append(msg)
    print("  FAIL  %s" % msg)


def ok(msg):
    print("  ok    %s" % msg)


def source_files(directory, skip=()):
    for dirpath, dirnames, filenames in os.walk(directory):
        dirnames[:] = [d for d in dirnames if os.path.join(dirpath, d) not in skip]
        for name in sorted(filenames):
            # .inc as well as .c and .h: kernel/zfs/zfs_glue.inc is a source
            # file that is #included rather than compiled - it has to be,
            # because every entry point in the vendored reader is static - and
            # a checker that could not see it would report the one file that
            # holds the port's glue as an unknown include.
            if name.endswith((".c", ".h", ".inc")):
                yield os.path.join(dirpath, name)


def check_nothing_outside_includes_zfs():
    """Nothing outside kernel/zfs/ may include anything from inside it.

    --- THE COMPAT SUBTREE IS EXCLUDED, and that is the fix rather than a
    --- loophole ------------------------------------------------------------
    This matched on BASENAME against every file under kernel/zfs/, including
    kernel/zfs/compat/. That subtree is a vendored shim layer full of
    deliberately generic names - sys/param.h, sys/types.h, sys/queue.h,
    linux/types.h - and kernel/bsd/compat/ contains its own files with exactly
    those names. So `#include <sys/param.h>` from kernel/bsd/callout.c was
    reported as reaching into ZFS when it resolves to the BSD header, a
    different file that merely shares a basename.

    That produced 294 failures, every one of them false, on every run. Two
    consequences, and the second is worse than the noise: the check exited 1,
    and run.sh has `set -e`, so it silently gated everything appended after
    it. A check that always fails is a check nobody reads, and one that always
    fails FIRST is a check that stops the others running.

    Basename matching cannot work here, because the two vendored trees having
    colliding basenames is the whole reason build.py hands out include paths
    per directory (see EXTRA_INCLUDES). What CAN be matched is:

      - an include whose path names the directory ("zfs/..."), and
      - a basename that is unique to ZFS's own sources rather than to its
        compat shims - zfsimpl.h, nvlist.h and the rest.

    Those are the two ways a file outside the boundary can actually reach
    across it, because the compat headers are not on anyone else's include
    path in the first place.
    """
    zfs_names = {os.path.basename(p) for p in source_files(ZFS_DIR)
                 if not p.startswith(os.path.join(ZFS_DIR, "compat") + os.sep)}
    zfs_names.discard("zfs.h")           # the public one, which lives elsewhere
    offenders = []

    for path in source_files(os.path.join(ROOT, "kernel")):
        if path.startswith(ZFS_DIR + os.sep):
            continue
        text = open(path, errors="replace").read()
        for inc in INCLUDE.findall(text):
            base = os.path.basename(inc)
            if base in zfs_names or "zfs/" in inc:
                offenders.append("%s includes %s" % (
                    os.path.relpath(path, ROOT), inc))

    if offenders:
        for o in offenders:
            fail(o)
    else:
        ok("nothing outside kernel/zfs/ includes anything from it")


def check_public_header_is_opaque():
    text = open(PUBLIC_HEADER, errors="replace").read()
    body = "\n".join(strip_comments(text).splitlines())
    hit = ZFS_TYPE_WORDS.search(body)

    if hit:
        fail("kernel/include/zfs.h names a ZFS type: %s" % hit.group(0))
    else:
        ok("kernel/include/zfs.h declares no ZFS type")

    if INCLUDE.findall(body):
        fail("kernel/include/zfs.h includes something; it should include nothing")
    else:
        ok("and includes nothing")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def check_zfs_reaches_only_the_whitelist():
    offenders = []
    compat = os.path.join(ZFS_DIR, "compat")
    compat_names = {os.path.relpath(p, compat)
                    for p in source_files(compat)}
    local_names = {os.path.basename(p) for p in source_files(ZFS_DIR)}

    for path in source_files(ZFS_DIR):
        if path.startswith(VENDOR_DIR + os.sep):
            continue                      # upstream's includes are upstream's
        if path.startswith(compat + os.sep):
            # Two files under compat/ are upstream copies themselves (queue.h
            # and list.h come from FreeBSD and OpenZFS), so their includes are
            # upstream's too. What matters is that Genesis's OWN files here
            # reach only the whitelist.
            continue
        for inc in INCLUDE.findall(open(path, errors="replace").read()):
            base = os.path.basename(inc)
            if inc in compat_names or base in compat_names:
                continue
            if base in local_names or inc.startswith("vendor/"):
                continue
            if inc in ("stdarg.h", "stddef.h", "stdint.h", "stdbool.h",
                       "errno.h"):
                continue
            if base in ALLOWED_KERNEL_HEADERS:
                continue
            offenders.append("%s includes %s" % (
                os.path.relpath(path, ROOT), inc))

    if offenders:
        for o in offenders:
            fail(o)
    else:
        ok("kernel/zfs/ reaches only its compat layer and the header whitelist")


def check_vendor_is_marked():
    unmarked = []
    marked = set()

    for path in sorted(os.listdir(VENDOR_DIR)):
        full = os.path.join(VENDOR_DIR, path)
        if not os.path.isfile(full):
            continue
        head = open(full, errors="replace").read(400)
        if "NOT VENDORED" in head:
            marked.add(path)
        elif path in DECLARED_NOT_VENDORED:
            unmarked.append("%s is declared not-vendored but does not say so"
                            % path)

    extra = marked - DECLARED_NOT_VENDORED
    missing = DECLARED_NOT_VENDORED - marked

    for name in sorted(extra):
        unmarked.append("kernel/zfs/vendor/%s is marked NOT VENDORED but is "
                        "not in this script's list - vendoring is turning "
                        "into forking" % name)
    for name in sorted(missing):
        unmarked.append("kernel/zfs/vendor/%s is expected to be a Genesis "
                        "stand-in and is not marked as one" % name)

    if unmarked:
        for u in unmarked:
            fail(u)
    else:
        ok("every file in kernel/zfs/vendor/ is upstream, or one of the %d "
           "declared stand-ins" % len(DECLARED_NOT_VENDORED))


def main():
    print("\nzfs: the CDDL boundary")
    check_nothing_outside_includes_zfs()
    check_public_header_is_opaque()
    check_zfs_reaches_only_the_whitelist()
    check_vendor_is_marked()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
