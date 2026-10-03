#!/usr/bin/env python3
"""Run commands in the guest, then press Ctrl-T and capture the task dump.

For a guest that HANGS: guest_run.py waits for a tally that never comes and
then kills QEMU, which tells you that it hung but not where. This types the
commands, waits for the output to go quiet, sends Ctrl-T (proc_dump in
kernel/proc/process.c prints every task's state and every CPU's current
thread), and stops.

    python3 tools/guest_dump.py /bin/verif /bin/systest
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build                                   # noqa: E402
from guest_run import SHELL_READY, SHELL_TIMEOUT, wait_for, wait_quiet     # noqa: E402


def main():
    commands = sys.argv[1:]
    log = os.path.join("build", "guest.log")
    if os.path.exists(log):
        os.remove(log)
    args = build.qemu_args() + ["-display", "none"]
    logf = open(log, "w")
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=logf,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)
    try:
        if wait_for(log, SHELL_READY, SHELL_TIMEOUT) < 0:
            print("guest_dump: the shell never came up", file=sys.stderr)
            return 2
        time.sleep(2)
        for cmd in commands:
            proc.stdin.write(cmd + "\n")
            proc.stdin.flush()
            wait_quiet(log, settle=8.0, timeout=600)
        proc.stdin.write("\x14")
        proc.stdin.flush()
        wait_quiet(log, settle=3.0, timeout=30)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
        logf.close()
    with open(log, "r", errors="replace") as f:
        data = f.read()
    i = data.rfind("--- tasks ---")
    sys.stdout.write(data[i - 2000 if i > 2000 else 0:] if i >= 0 else data[-3000:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
