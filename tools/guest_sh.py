#!/usr/bin/env python3
"""Boot the guest, type lines into its shell, and print what came back.

For poking at a program interactively when there is no tally to wait for -
bringing up a new shell, say. Each argument is one line typed at the
console; the output is allowed to go quiet between lines. Escapes are
expanded, so a control character can be sent: "\\x1a" is ^Z, "\\x03" is ^C,
"\\t" is a tab.

    python3 tools/guest_sh.py /bin/bash 'echo $BASH_VERSION' exit

The whole serial transcript is in build/guest.log; the part after the shell
prompt appeared is printed.
"""

import codecs
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build                                   # noqa: E402
from guest_run import wait_for, wait_quiet     # noqa: E402


def main():
    argv = sys.argv[1:]
    if len(argv) == 2 and argv[0] == "-f":
        # One line per line of FILE, verbatim - no host shell in between to
        # expand $VARS or eat quotes. Escapes are still expanded.
        with open(argv[1], "r") as f:
            argv = f.read().splitlines()
    lines = [codecs.decode(a, "unicode_escape") for a in argv]
    settle = float(os.environ.get("GENESIS_SETTLE", "3"))
    log = os.path.join("build", "guest.log")
    if os.path.exists(log):
        os.remove(log)
    args = build.qemu_args() + ["-display", "none"]
    logf = open(log, "w")
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=logf,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)
    start = 0
    try:
        start = wait_for(log, r"built-in shell \(ash\)", 180)
        if start < 0:
            print("guest_sh: the shell never came up", file=sys.stderr)
            return 2
        time.sleep(2)
        for line in lines:
            # "@wait:REGEX" types nothing: it waits (up to 300s) for REGEX to
            # appear in the output after this point. For a program that is
            # slow to start - bash takes ~25s to load under TCG with polled
            # ATA - where going quiet is not the same as being ready.
            if line.startswith("@wait:"):
                here = os.path.getsize(log)     # a byte offset, for seek()
                if wait_for(log, line[6:], 300, since=here) < 0:
                    print("guest_sh: timed out waiting for %r" % line[6:],
                          file=sys.stderr)
                continue
            # A line ending in a control character is sent as-is (no newline):
            # ^Z typed at a running job must not be followed by a stray Enter.
            if line and ord(line[-1]) < 0x20 and line[-1] != "\n":
                proc.stdin.write(line)
            else:
                proc.stdin.write(line + "\n")
            proc.stdin.flush()
            wait_quiet(log, settle=settle, timeout=120)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
        logf.close()
    with open(log, "r", errors="replace") as f:
        data = f.read()
    i = data.find("built-in shell (ash)")
    sys.stdout.write(data[i:] if i >= 0 else data[-4000:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
