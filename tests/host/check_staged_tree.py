#!/usr/bin/env python3
"""The staged root/ tree, checked against what FAT16 can actually represent.

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

ROOT_DIR = os.path.join(ROOT, "root")

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


def main():
    print("\nstaged tree: what FAT16 can represent")

    if not os.path.isdir(ROOT_DIR):
        fail(f"{ROOT_DIR} does not exist - nothing is staged")
        return 1

    too_long = []
    collisions = []
    dirs_walked = 0
    entries = 0

    for dirpath, dirnames, filenames in os.walk(ROOT_DIR):
        dirs_walked += 1
        seen = {}
        for name in sorted(dirnames) + sorted(filenames):
            entries += 1
            rel = os.path.relpath(os.path.join(dirpath, name), ROOT_DIR)
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
        ok(f"walked {dirs_walked} directories, {entries} entries")
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

    # The specific case that already happened, asserted by name so a
    # regression is reported as the thing it is rather than as a generic
    # collision somewhere in the tree.
    wsr = os.path.join(ROOT_DIR, "wsr")
    if os.path.isdir(wsr):
        found = []
        for dirpath, dirnames, _ in os.walk(wsr):
            for d in dirnames:
                if d.lower() == "system32":
                    found.append(os.path.relpath(os.path.join(dirpath, d),
                                                 ROOT_DIR))
        spellings = {os.path.basename(p) for p in found}
        if len(spellings) > 1:
            fail(f"/wsr has System32 under more than one spelling: "
                 f"{sorted(spellings)}")
        else:
            ok(f"/wsr spells System32 one way everywhere ({len(found)} of them)")

    print(f"\nstaged tree: {passes} passed, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
