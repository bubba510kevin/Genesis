#!/usr/bin/env python3
"""The staged trees, checked against what FAT16 can actually represent.

TWO TREES since the userland split: the root/ of a Genesis-userland checkout
(GENESIS_USERLAND, default ../Genesis-userland) and this repository's
modules/root overlay, which build.py stages on top of it. They are checked as
the ONE directory tree they become on the volume, so a name in one that
collides with a name in the other is caught here too - and a file present in
both, which fatfs would refuse at image-build time, is named here first.
When no userland checkout is found the check says so and passes: there is
nothing to stage, and build.py disk refuses on its own.

Why this exists, and why it is a BUILD check rather than a boot one
-------------------------------------------------------------------
The /wsr mirror collided with itself once already: FAT 8.3 names are
UPPERCASED on write, so `System32` and `system32` in one directory are the
same eleven bytes, and the second one staged silently replaced the first.
Nothing reported it. The failure surfaced much later as a missing DLL.

The roadmap suggested a systest or verification.c check that walks the mirror
at run time. This is the same check moved earlier, and earlier is strictly
better here: the collision is a property of the SOURCE TREE and of the naming
rules, both of which are known before the image is built. A run-time check
tells you the image you already booted is wrong; this refuses to build it.

It is also the only place the check can be complete. By the time the guest
sees the volume the collision has already happened - one of the two names is
simply gone - so a walk of the mounted filesystem cannot tell a collision
from a file nobody staged.

What is checked
---------------
  1. Every name fits 8.3, which is what tools/fatfs.py enforces by raising -
     duplicated here so the failure names every offender at once rather than
     the first one the stager trips over.
  2. No two entries in a directory fold to the same 8.3 name. This is the
     System32/system32 case and any other spelling of it.
  3. The names are checked against the SAME encoder the stager uses
     (fatfs.Fat16.name_to_11) rather than a reimplementation of it, so the
     check cannot drift away from the thing it is checking.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import fatfs  # noqa: E402

USERLAND_ROOT = os.path.join(
    os.environ.get("GENESIS_USERLAND") or
    os.path.join(os.path.dirname(ROOT), "Genesis-userland"), "root")
MODULES_ROOT = os.path.join(ROOT, "modules", "root")

passes = 0
failures = 0


def ok(msg):
    global passes
    passes += 1
    print(f"  ok    {msg}")


def fail(msg):
    global failures
    failures += 1
    print(f"  FAIL  {msg}")


def short_of(name):
    """The 8.3 bytes the stager would write, or None if it would refuse."""
    try:
        return fatfs.Fat16.name_to_11(name)
    except ValueError:
        return None


def merged_tree(roots):
    """{volume dir: {name: (is_dir, [(root, rel path)...])}} over all roots."""
    tree = {}
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            rel_dir = os.path.relpath(dirpath, root)
            vdir = "/" if rel_dir == "." else "/" + rel_dir.replace(os.sep, "/")
            entries = tree.setdefault(vdir, {})
            for name, is_dir in ([(d, True) for d in dirnames] +
                                 [(f, False) for f in filenames]):
                rel = os.path.relpath(os.path.join(dirpath, name), root)
                slot = entries.setdefault(name, (is_dir, []))
                slot[1].append((root, rel))
    return tree


def main():
    print("\nstaged tree: what FAT16 can represent")

    if not os.path.isdir(USERLAND_ROOT):
        print(f"  skip  no userland checkout at {USERLAND_ROOT} "
              "(set GENESIS_USERLAND) - nothing to check")
        print("\nstaged tree: 0 passed, 0 failed")
        return 0

    roots = [USERLAND_ROOT]
    if os.path.isdir(MODULES_ROOT):
        roots.append(MODULES_ROOT)
    tree = merged_tree(roots)

    too_long = []
    collisions = []
    duplicates = []
    dirs_walked = len(tree)
    entries = 0

    for vdir in sorted(tree):
        seen = {}
        for name in sorted(tree[vdir]):
            is_dir, where = tree[vdir][name]
            entries += 1
            rel = where[0][1]
            if not is_dir and len(where) > 1:
                duplicates.append((vdir, name, [r for r, _ in where]))
            s = short_of(name)
            if s is None:
                too_long.append(rel)
                continue
            if s in seen and seen[s] != name:
                collisions.append((rel, seen[s], s.decode("ascii").strip()))
            else:
                seen[s] = name

    # A control on the walk itself. A check that silently walked nothing would
    # report no collisions and no over-long names, which is exactly what a
    # clean tree looks like - so the count has to be asserted too.
    if entries > 0 and dirs_walked > 1:
        ok(f"walked {dirs_walked} directories, {entries} entries "
           f"across {len(roots)} tree(s)")
    else:
        fail(f"the walk found almost nothing ({dirs_walked} dirs, "
             f"{entries} entries) - the check is not looking at the tree")

    if too_long:
        for rel in too_long:
            fail(f"{rel} does not fit 8.3 and cannot be staged")
    else:
        ok("every staged name fits 8.3")

    if collisions:
        for rel, other, short in collisions:
            fail(f"{rel} and {other} both fold to '{short}' - "
                 f"one will silently replace the other")
    else:
        ok("no two names in a directory fold to the same 8.3 name")

    if duplicates:
        for vdir, name, where in duplicates:
            fail(f"{vdir}/{name} is in more than one tree: {where}")
    else:
        ok("no file is staged by both the userland and the modules tree")

    # The specific case that already happened, asserted by name so a
    # regression is reported as the thing it is rather than as a generic
    # collision somewhere in the tree.
    spellings = set()
    count = 0
    for vdir, names in tree.items():
        if vdir == "/wsr" or vdir.startswith("/wsr/"):
            for name, (is_dir, _) in names.items():
                if is_dir and name.lower() == "system32":
                    spellings.add(name)
                    count += 1
    if len(spellings) > 1:
        fail(f"/wsr has System32 under more than one spelling: "
             f"{sorted(spellings)}")
    else:
        ok(f"/wsr spells System32 one way everywhere ({count} of them)")

    print(f"\nstaged tree: {passes} passed, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
