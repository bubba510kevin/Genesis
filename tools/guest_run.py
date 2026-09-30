#!/usr/bin/env python3
"""Boot Genesis under QEMU, run commands at its shell, and collect the output.

Why this exists
---------------
Two of the three test suites - src/systest/systest.c and src/verif/verif.c in Genesis-userland -
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
import socket
import subprocess
import sys
import time


# --- mouse input, through the QEMU monitor ---------------------------------
#
# /bin/fbtest (ROADMAP items 14(h) and (j)) reads /dev/mouse0 and needs
# somebody to move the mouse. The monitor's mouse_move / mouse_button commands
# are exactly that: QEMU's emulated PS/2 mouse turns them into real packets on
# IRQ 12, so the whole path - controller, driver, decoder, queue, read(2) - is
# exercised with nothing faked in the guest. fbtest prints a marker line when
# it is ready for each step; each marker maps to the input it asks for.
#
# mouse_move dx dy [dz]: dy positive is DOWN, and dz positive is the wheel
# turned UP (away from the user). mouse_button takes a state bitmask: 1 left,
# 2 right, 4 middle, 0 all released.
MOUSE_SCRIPTS = {
    "MOUSE-WAIT-1": (0.2, ["mouse_move 4 2", "mouse_move 3 3",
                           "mouse_button 1", "mouse_button 0",
                           "mouse_move 0 0 1"]),
    # The delay is the point here: fbtest is about to BLOCK in read(2), and
    # input that arrives before it gets there would test the queue rather
    # than the wakeup.
    "MOUSE-WAIT-2": (1.5, ["mouse_button 2", "mouse_button 0"]),
}


class Monitor:
    """A human-monitor (HMP) connection over TCP. Lazily connected: most runs
    never type a monitor command."""

    def __init__(self, port):
        self.port = port
        self.sock = None

    def _read_prompt(self, timeout=10):
        buf = b""
        self.sock.settimeout(timeout)
        while not buf.endswith(b"(qemu) "):
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf

    def command(self, line):
        if self.sock is None:
            self.sock = socket.create_connection(("127.0.0.1", self.port),
                                                 timeout=10)
            self._read_prompt()
        self.sock.sendall(line.encode() + b"\n")
        return self._read_prompt()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# The guest's shell is ready for input. Process 1 is /sbin/init, which starts
# bash as the login shell (its prompt is "bash-5.3# "); a root with no init
# boots BusyBox's ash instead, which announces itself with its banner. Either
# means "type now". 300s: loading bash's 1.4MB over polled ATA takes ~25s
# under TCG, on top of the boot.
SHELL_READY = r"built-in shell \(ash\)|bash-[0-9.]+# "
SHELL_TIMEOUT = 300


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
        except OSError:
            # FileNotFoundError before QEMU creates it, and on WSL an
            # occasional ENODATA when something on the Windows side reads the
            # log at the same moment. Both mean "try again".
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


def wait_for_tally(log, since, monitor, timeout=600):
    """wait_for on a suite's tally line, answering fbtest's MOUSE-WAIT
    markers on the way. Returns the new offset, or -1 on timeout."""
    tally = re.compile(r"^[A-Za-z0-9_.-]+: \d+ passed, \d+ failed", re.M)
    marker = re.compile(r"(MOUSE-WAIT-\d+)")
    done = set()
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(log, "r", errors="replace") as f:
                f.seek(since)
                chunk = f.read()
        except OSError:
            chunk = ""
        for m in marker.finditer(chunk):
            name = m.group(1)
            if name in done or name not in MOUSE_SCRIPTS:
                continue
            done.add(name)
            delay, cmds = MOUSE_SCRIPTS[name]
            time.sleep(delay)
            for c in cmds:
                monitor.command(c)
                time.sleep(0.1)
        if tally.search(chunk):
            return since + len(chunk)
        time.sleep(0.25)
    return -1


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(here)
    sys.path.insert(0, here)

    import importlib.util
    spec = importlib.util.spec_from_file_location("genesis_build", "build.py")
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)

    # bashtest is GNU bash running a script (ROADMAP item 15); it is last
    # because exec'ing bash takes ~25s under TCG (polled ATA PIO).
    commands = sys.argv[1:] or ["/bin/verif", "/bin/systest", "/bin/thr.exe",
                                "/bin/smp.exe", "/bin/tls.exe",
                                "/bin/wait.exe", "/bin/seh.exe", "/bin/mix.exe",
                                "/bin/elfmix",
                                "/bin/fbtest",
                                "/bin/bash /usr/tests/bashtest.sh"]
    log = os.path.join("build", "guest.log")
    if os.path.exists(log):
        os.remove(log)

    # -serial stdio, with stdin a pipe and stdout the log. build.py's own
    # qemu_args() already asks for -serial stdio for interactive use, so the
    # drives, the NIC and the AHCI controller stay whatever it decided.
    args = build.qemu_args()
    args += ["-display", "none"]
    mon_port = free_port()
    args += ["-monitor", f"tcp:127.0.0.1:{mon_port},server,nowait"]
    monitor = Monitor(mon_port)

    logf = open(log, "w")
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=logf,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)

    rc = 0
    try:
        # ash's banner, or bash's prompt - see SHELL_READY.
        pos = wait_for(log, SHELL_READY, SHELL_TIMEOUT)
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
            pos = wait_for_tally(log, pos, monitor)
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
