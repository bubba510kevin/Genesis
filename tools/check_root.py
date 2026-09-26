#!/usr/bin/env python3
"""Check a staged root against what the FAT16 driver can actually read.

Two failures this catches, both of which have now happened for real.

1. A NAME THAT DOES NOT FIT 8.3.
   The rtld was first staged as ld-genesis-x86_64.so.1, following the
   musl/glibc convention. fatfs.py refused it at image-build time - correctly,
   but late: the file had already been built and staged, and the error came
   from a tool three steps removed from the one that chose the name.

   What makes this worth a separate check rather than leaving it to fatfs.py
   is that the two halves of the system DISAGREE about long names. fatfs.py
   raises. The kernel's fat_name_to_entry silently truncates the stem at eight
   characters and the extension at three, so it would look up a different name
   than the one asked for and report -ENOENT. Refusing at staging time is the
   only point where the failure names the file and the fix.

2. A CASE COLLISION.
   8.3 names fold to upper case, so System32 and system32 are the SAME
   directory entry. That has already happened once in /wsr, and the symptom
   was two directories merging into one with no error from anything. The
   symlinked-root layout makes it likely again: /wsr/X and /X are two views
   with two chances to differ in case.

Exit status is the number of problems, so a build script can just check it.
Reports everything rather than stopping at the first: a staging tree usually
has all its collisions from one cause, and fixing them one build at a time is
slow.
"""

import os
import sys

# Characters FAT rejects in a short name. The set matters less than having
# one: a name that reaches the driver with a '+' in it does not fail, it
# resolves to something else.
INVALID = set('"*+,/:;<=>?[\\]|')


def fits_83(name):
    """Return None if the name fits 8.3, or a string saying why it does not."""
    if name in (".", ".."):
        return None

    # partition() splits on the FIRST dot, which is what fatfs.py does - so
    # "a.b.c" gives an extension of "b.c" and a dot inside an extension is not
    # a valid short name. Counting dots directly says so more clearly than
    # letting the length check catch it by accident.
    if name.count(".") > 1:
        return "more than one dot"

    stem, _, ext = name.partition(".")
    if len(stem) > 8:
        return f"stem is {len(stem)} characters, 8.3 allows 8"
    if len(ext) > 3:
        return f"extension is {len(ext)} characters, 8.3 allows 3"
    if not stem:
        return "no stem"

    bad = sorted(set(name) & INVALID)
    if bad:
        return "invalid characters: " + " ".join(bad)
    if any(ord(c) > 126 or ord(c) < 32 for c in name):
        return "non-ASCII or control characters"
    return None


def check(root):
    problems = 0

    for dirpath, dirnames, filenames in os.walk(root):
        seen = {}
        rel = os.path.relpath(dirpath, root)
        shown = "/" if rel == "." else "/" + rel.replace(os.sep, "/")

        for name in sorted(dirnames + filenames):
            why = fits_83(name)
            if why is not None:
                print(f"{shown.rstrip(chr(47))}/{name}: {why}", file=sys.stderr)
                problems += 1
                continue

            # The folded form is the actual directory key. Two names that fold
            # together are one entry on the volume, and whichever is written
            # second overwrites or duplicates the first depending on the
            # writer - neither of which is what the tree says.
            key = name.upper()
            if key in seen and seen[key] != name:
                print(f"{shown}: '{name}' and '{seen[key]}' both fold to "
                      f"'{key}' - one FAT entry, two names",
                      file=sys.stderr)
                problems += 1
            else:
                seen[key] = name

    return problems


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "root"
    if not os.path.isdir(root):
        print(f"check_root: {root} is not a directory", file=sys.stderr)
        return 1

    problems = check(root)
    if problems:
        print(f"\ncheck_root: {problems} name(s) the FAT16 driver cannot read "
              f"as written", file=sys.stderr)
    return problems


if __name__ == "__main__":
    sys.exit(main())
