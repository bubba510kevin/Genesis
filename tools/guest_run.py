#!/usr/bin/env python3
"""Boot Genesis under QEMU, run commands at its shell, and collect the output.

Why this exists
---------------
Two of the three test suites in this tree - src/systest.c and src/verif.c -
are USERLAND programs. They run from the guest's shell, which means "did the
tests pass" used to be a question answered by a human sitting in front of the
QEMU window. A suite that only runs when someone remembers to run it is a
suite that stops being run.

How it types, and why that changed
----------------------------------
This drove the guest with QEMU monitor `sendkey` commands - one per character,
with a delay between them, because the kernel read input from the emulated PS/2
controller and nothing read the serial port back. build.py said so next to its
-serial flag.

kernel/dev/serial.c has a receive path now (added for the bare-metal target,
which has a USB keyboard and no PS/2 device at all), so the serial port is a
real console: characters written to QEMU's stdin land in the same input ring
the keyboard fills. That makes this a pipe instead of a scancode injector -
faster, and with nothing to get wrong in a key-name table.

The old approach needed a KEYMAP entry per symbol and a missing one silently
mistyped the command. That whole class of failure is gone.
"""

import os
import re
import subprocess
import sys
import time


def wait_for(path, pattern, timeout, since=0):
    """Wait until `pattern` appears in `path` beyond offset `since`.

    Returns the new end offset, or -1 on timeout. Polling a file rather than
    reading the pipe directly because the serial output is redirected to one -
    stdin is the write side of the same console and cannot also be the read
    side.
    """
    rx = re.compile(pattern, re.M)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                f.seek(since)
                chunk = f.read()
        except FileNotFoundError:
            chunk = ""
        if rx.search(chunk):
            return since + len(chunk)
        time.sleep(0.25)
    return -1


def wait_quiet(path, settle=2.0, timeout=60):
    """Wait until `path` stops growing for `settle` seconds.

    Matching a suite's tally line is NOT the same as the suite having
    finished: verif prints "verification: N passed, M failed" and then keeps
    going, dumping print_manual_queue() behind it. Sending the next command on
    the tally alone typed it into the still-running program's stdin instead of
    the shell, and the second suite never ran at all - the driver reported one
    result and exited cleanly, which is the failure mode that looks like
    success.

    Waiting for the prompt would be more precise, but the prompt is a bare
    "# " with no newline and this is a line-oriented poll. Quiet is a weaker
    signal and a sufficient one.
    """
    deadline = time.time() + timeout
    last = -1
    stable_since = time.time()
    while time.time() < deadline:
        try:
            size = os.path.getsize(path)
        except OSError:
            size = 0
        if size != last:
            last = size
            stable_since = time.time()
        elif time.time() - stable_since >= settle:
            return True
        time.sleep(0.25)
    return False


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(here)
    sys.path.insert(0, here)

    import importlib.util
    spec = importlib.util.spec_from_file_location("genesis_build", "build.py")
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)

    commands = sys.argv[1:] or ["/bin/verif", "/bin/systest"]
    log = os.path.join("build", "guest.log")
    if os.path.exists(log):
        os.remove(log)

    # -serial stdio, with stdin a pipe and stdout the log. build.py's own
    # qemu_args() already asks for -serial stdio for interactive use, so the
    # drives, the NIC and the AHCI controller stay whatever it decided.
    args = build.qemu_args()
    args += ["-display", "none"]

    logf = open(log, "w")
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=logf,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)

    rc = 0
    try:
        # The shell's banner. Waiting for the PROMPT would be nicer but it is
        # a bare "# " with no newline after it, which a line-oriented poll
        # cannot see arrive.
        pos = wait_for(log, r"built-in shell \(ash\)", 180)
        if pos < 0:
            print("guest_run: the shell never came up", file=sys.stderr)
            return 2
        time.sleep(2)

        for cmd in commands:
            proc.stdin.write(cmd + "\n")
            proc.stdin.flush()
            # Anchored on the suites' own final line. A looser pattern matched
            # the word "failed" inside the explanatory prose verif prints as it
            # goes, so the driver decided the run was over a third of the way
            # through and quit QEMU under it - reporting fewer passes and no
            # failures, which is the worst thing a test runner can do.
            pos = wait_for(log,
                           r"^(verification|systest): \d+ passed, \d+ failed",
                           600, since=pos)
            if pos < 0:
                print(f"guest_run: {cmd} produced no tally", file=sys.stderr)
                rc = 1
                break
            # The tally is not the end of the output - see wait_quiet.
            wait_quiet(log)
            pos = os.path.getsize(log)
    finally:
        try:
            proc.stdin.write("\n")
            proc.stdin.flush()
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
        logf.close()

    with open(log, "r", errors="replace") as f:
        sys.stdout.write(f.read())
    return rc


if __name__ == "__main__":
    sys.exit(main())
