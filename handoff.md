# Handoff — Genesis

*Where to pick the work up. For what already works, subsystem by subsystem,
see `FEATURES.md`; for why each decision was made, `ROADMAP.md` (item numbers
are never renumbered — source comments cite them).*

Last updated 2026-09-27. Repository: https://github.com/bubba510kevin/Genesis
(branch `main`).

## What Genesis is, in one paragraph

A from-scratch x86-64 kernel that runs three ecosystems at once: Linux ELF
binaries (musl, BusyBox), Windows PE binaries on clean-room `ntdll.dll` /
`kernel32.dll`, and unmodified drivers from three driver models (FreeBSD
Newbus, Linux via LinuxKPI, Windows WDM). Large parts are real vendored
FreeBSD (`kernel/bsd/`: the whole IPv4/TCP stack, sockets, `if_re`). It runs
on every CPU (SMP under a big kernel lock), has its own COW filesystem (gnfs)
with NT-style ACLs, and gets its address by DHCP.

**THE GOAL:** a daily-drivable desktop running the Windows 7 DE with real
Win32 binaries. `ROADMAP.md` item 14 is the dependency-ordered inventory for
it, (a) through (s). **Genesis does not write GUI DLLs or drivers from
scratch.** It runs the precompiled ones (Microsoft's user32/gdi32/comctl32/
shell32 and `.sys` drivers, win32k.sys included, from the user's own Windows
install) and writes the support they need: the loader, NT syscalls, and the
ntoskrnl/hal/port-library export surface drivers import. Drivers with source
(FreeBSD, Linux) are ported and modified, not rewritten. ntdll and kernel32
stay Genesis's own. Be honest about scale: item 14 is bigger than everything
built so far.

## Where things stand (2026-09-27)

Done and verified in the latest sessions — details in `ROADMAP.md`:
- **SMP** (item 13): every CPU runs processes; per-CPU timers, idle threads,
  affinity, IPIs, targeted TLB shootdown; SMP APIs for all three driver models;
  **interrupt balancing** (`kernel/arch/irqbalance.c`).
- **Windows threads + TLS** (item 14(a)): `CreateThread` family, per-thread
  TEB/stack, `TlsAlloc`/`FlsAlloc`/implicit `.tls` with TLS callbacks.
- **Win32 synchronisation**: critical sections, SRW locks, condition
  variables, `WaitOnAddress` — all block in the kernel
  (`NtWaitForAlertByThreadId`, NT syscalls 0x20/0x21).
- **Win32 thread control** (item 14(a), all but the table size): mutexes
  (`CreateMutexW`, `ReleaseMutex`) with **abandonment** (`WAIT_ABANDONED` when
  the owner dies), `CREATE_SUSPENDED` / `SuspendThread` / `ResumeThread`
  (NT syscalls 0x22/0x23), and `TerminateThread` on another thread.
- **Dispatcher objects, completed** (item 14(b)): `WaitForMultipleObjects`
  (wait-any; all-or-nothing wait-all), events and semaphores in kernel32,
  **APCs** (`QueueUserAPC`, alertable `SleepEx`/`WaitFor*Ex`). Underneath:
  `kernel/exec/nt_context.c` captures a thread's registers into a real Win64
  `CONTEXT` on its stack, and `NtContinue` restores every register by
  leaving through `iretq`.
- **Structured exception handling** (item 14(c)): a fault in a Windows
  process goes to `ntdll!KiUserExceptionDispatcher`; ntdll has the x64
  unwinder (`RtlVirtualUnwind`, `RtlUnwindEx`, `__C_specific_handler`),
  vectored handlers and the unhandled filter. Tested with real clang-built
  `__try`/`__except`/`__finally` (`src/winseh`).
- **Networking** (item 6): TCP + lo0, Linux-ABI sockets, **DHCP** with renewal.
- **gnfs v2**: big files, rename, snapshots; ZFS removed.
- **Display** (item 14(h), branch `claude/vbe-fb-ps2-mouse`): the bootloader
  sets a VESA/VBE 32bpp linear-framebuffer mode (1024x768 by default) through a
  real-mode stub at the head of the kernel image (`kernel/arch/vbe_boot.c`);
  the console draws into it; `/dev/fb0` has fbdev ioctls and `mmap`.
- **Mouse** (mouse half of item 14(j), same branch): PS/2 `psm` driver
  (FreeBSD Newbus idiom) on an `atkbdc` bus, wheel included; `/dev/mouse0`
  serves evdev `input_event` records. `/bin/fbtest` covers both from ring 3.

## What needs to be done now, in order

`ROADMAP.md` is organized into **four phases** (THE PLAN, at its top). Work
them in order; a later phase pulls forward only the pieces of an earlier one
it needs. **The GUI rule:** nothing that deals with a GUI is written from
scratch — it is taken (precompiled Windows binaries on the real win32k.sys,
or ported open source); Genesis writes the support underneath.

### Phase 1 — run bash (ROADMAP item 15)
GNU bash from upstream, built static against musl like BusyBox, as the login
shell with readline and job control, then its own test suite in the guest.
Missing underneath (item 15 (a)-(l)): `select`/`pselect6`, real rlimits
(`prlimit64` is ENOSYS), `getrusage`, a writable `/tmp` (tmpfs), `/proc`
(`/proc/self/fd` at least), FIFOs and `/dev/fd`, staged `/etc/passwd` etc.,
symlinks, long filenames on the root (FAT is 8.3 — `.bashrc` can't exist),
BusyBox applets or coreutils, terminal completeness (TIOCSCTTY, SIGTTOU/
SIGTTIN, VMIN/VTIME, SIGWINCH), `#!` scripts and setuid exec. Find the rest
with the musl ptrace-trace method (item 9).

### Phase 2 — a modern kernel, Linux and NT 10.0 parity (item 16)
A gap inventory, worked in the order phases 3-4 and real programs need it.
The pieces already queued from earlier work belong here:

**Finish item 14(a) — threads**
- **Named mutexes**: `CreateMutexW` refuses a name today, because the other
  half of the named form (opening the existing one, `ERROR_ALREADY_EXISTS`,
  `OpenMutexW`) has no kernel path. `NtOpenEvent` in `kernel/exec/nt.c` is
  the pattern for an `NtOpenMutant`.
- **MAX_PROCESSES = 64** (`kernel/include/process.h`) is shared by processes,
  threads, kthreads and idle threads. Real Win32 programs with thread pools
  will hit it; the table is scanned linearly by the scheduler, so raising it
  far means a run queue.

**Loose ends of 14(b)/(c)**
See the STILL OPEN note under item 14(c) in `ROADMAP.md`: MinGW C++
exceptions are untested (libgcc's SEH unwinder needs a CRT - `malloc`,
`abort` - that this tree does not have yet; once one exists, a `throw`/
`catch` test belongs in `src/winseh`); nested dispatch
(`EXCEPTION_NESTED_CALL`); `EXCEPTION_EXECUTE_HANDLER` from the unhandled
filter should unwind (run `__finally`s) before exiting; stack overflow
(`STATUS_STACK_OVERFLOW` needs a guard page, 14(d)).

**Item 14(d) — NT memory** (reserve/commit, `VirtualProtect`, `VirtualQuery`,
guard pages, Section objects / `MapViewOfFile`), then 16(k)-(s): keyed
events and `NtWaitForAlertByThreadId`, completion ports, `NtCreateUserProcess`,
tokens, the I/O manager, the in-kernel registry, ALPC, and the ntoskrnl/hal
export surface precompiled drivers (win32k.sys first) import.

### Phase 3 — bash understands Windows (item 17)
A patch series on upstream bash (MSYS2/Cygwin patches read first): drive-
letter paths, `.exe`/`.bat` by bare name via PATHEXT, Windows command-line
quoting and environment for PE children, CRLF scripts, the full 32-bit exit
code in a variable, ^C as `CTRL_C_EVENT`.

### Phase 4 — the libraries: major .so and DLLs (item 18; 14(e)-(o))
Written (non-GUI): the loader (14(e): `LdrLoadDll`, search path, API sets,
SxS), kernel32/kernelbase (14(f)), the C runtimes (msvcrt, ucrtbase),
advapi32, ws2_32, rpcrt4, ole32/combase, and item 18's list. First in
line: `LoadLibrary`/`GetProcAddress` at run time, and kernel32 breadth
(`CreateProcess`, `MultiByteToWideChar`/`WideCharToMultiByte`, environment,
console API, time/locale, file API) tested with real MinGW programs.
Taken (GUI): win32k.sys, user32, gdi32, comctl32, shell32, explorer, the
VGA-class display driver and the i8042prt/mouclass input stack, precompiled;
Linux GUI stacks from upstream. Genesis's part is the ntoskrnl/hal export
surface (16(r)) they load on.
The framebuffer (14(h), boot console and `/dev/fb0`) and the PS/2 mouse
(`/dev/mouse0`, `mouse_take()` in `kernel/dev/mouse.c`) are done. Open ends
there: no mode switch after boot, no PAT write-combining, one mouse queue
shared by every reader, no `O_NONBLOCK` on device reads.

### Side work, smaller, any time
- **An intermittent boot selftest failure, not yet explained**:
  `irqbalance: selftest FAILED (1)` with "irq 11 placed on cpu1 but nothing
  arrived there", about 1 boot in 4 under QEMU **TCG** (no KVM - seen in a
  Linux cloud container; not seen on the WSL machine). Evidence from one
  instrumented failure: `net_ping` returned 0, line 11 took **no** interrupt
  on any CPU within 100 ticks (`irq_delivered` 9 -> 9), its IOAPIC
  destination was unchanged and the balancer had not moved anything. So
  it is not a race with the rebalancing thread. Two leads: `if_re`'s filter
  masks the NIC's IMR and leaves unmasking to a taskqueue task, so if that
  task has not run yet the NIC raises nothing; or a level-triggered
  interrupt was lost when `smp_irq_selftest` and then `irq_balance_start`
  re-routed line 11 in quick succession. Do NOT fix it by re-pinging in the
  test. Find where the interrupt went.
- **Hardening done alongside 14(a), not covered by a test** (each is a race
  ring 3 cannot reliably provoke): `signal_send` does not wake a parked
  (suspended) thread; NtSuspendThread dequeues a READY thread directly; a
  thread killed while waiting for the big kernel lock no longer runs the
  syscall it was entering (`syscall_dispatch`); `proc_nt_thread_exit` leaves
  the unmap of a thread still running on another CPU to `proc_free`.
- **SMP**: split the big kernel lock **only when something measures it** (the
  `bkl waited` counter and lock_report show contention). No NUMA/hotplug/SMT.
- **Networking**: IPv6; `SCM_RIGHTS`; a DNS resolver in userland (the DHCP
  lease already provides the server — `net_dhcp_dns()`).
- **Filesystems**: chown should clear suid/sgid once exec honours them;
  moving a directory to a new parent should need write on the directory
  itself; symlinks; gnfs dataset directory.
- **Build**: `vendsrc/` (upstream FreeBSD source) is gitignored and only
  partly present on this machine — `src/kmod/build.sh` now skips rebuilding
  `if_rl`/`if_re` without it and keeps the staged `root/boot/kernel/ifre.ko`.
  Restore `vendsrc/sys/dev/{rl,re}` if that driver must change.

## Rules learned the hard way
- **NT syscall numbers** live in `kernel/include/nt.h` (ntdll's copy is
  generated from it). In use: `0x01`-`0x28`. Parallel work on the display/input
  side (14(h)/(j)) was asked to start at `0x40` so the two never collide.
  (win32k's own NtUser/NtGdi calls are a separate table, from `0x1000`.)
- **Never wait for time while holding the big kernel lock.** The PIT tick
  goes only to the BSP; an AP spinning in `hlt` with the lock held starves
  the BSP of the lock and so of the tick. Use `bkl_wait_for_interrupt()` or a
  real sleep. (The boot context is pinned to the BSP until init starts for
  this reason — `kernel/proc/process.c`, `kernel/flk.c`.)
- **Every new check gets a mutation test**: break the code on purpose, see
  the check fail, restore.
- **On Windows hosts**: write edit scripts with the Write tool and open files
  with `newline=''` — Python writes CRLF by default and bash heredocs mangle
  backslashes.
- Keep `ROADMAP.md`, `FEATURES.md` and this file current with each change.

## How to build and verify

Everything builds and runs in WSL (Debian) against this working copy
(`build/b.sh` is a local wrapper and is not in git - `python3 build.py all`
does the same). On a bare Linux box - a cloud container, say - the tools are
`apt-get install nasm qemu-system-x86 gcc-mingw-w64-x86-64 musl-tools
dosfstools mtools clang` (clang builds `seh.exe`: GCC has no `__try`), and each line below is the part in quotes, run from the
repository root. Rebuilding changes the committed binaries under `root/`
and `src/` whenever the toolchain version differs; commit only those whose
source changed.
```
wsl bash build/b.sh                                   # kernel -> build/kernel.elf
wsl bash -c "cd /mnt/c/Users/kevin/code/Genesis/Genesis && sh tools/build_user.sh"   # all user programs and DLLs
wsl bash -c "cd /mnt/c/Users/kevin/code/Genesis/Genesis && rm -f build/disk.img && python3 build.py disk"   # restage the FAT root from root/
wsl bash -c "cd /mnt/c/Users/kevin/code/Genesis/Genesis && python3 tools/guest_run.py > build/gr.out 2>&1"  # boot + ring-3 suites
wsl bash -c "cd /mnt/c/Users/kevin/code/Genesis/Genesis && bash tests/host/run.sh"   # host-side tests
```
`guest_run.py` boots QEMU (`-smp 4` by default; `GENESIS_SMP=N` overrides;
`GENESIS_NET=10.0.9.0/24` changes the DHCP subnet), types each program into
the shell over serial and waits for its tally. Pass program paths to run a
subset, e.g. `python3 tools/guest_run.py /bin/smp.exe`. The log is
`build/guest.log`. For `/bin/fbtest` it also drives QEMU's mouse through a
monitor on a free TCP port when the program prints `MOUSE-WAIT-n`, so run
fbtest through guest_run (by hand it asks you to move and click).
`GENESIS_VBE=WxH` or `GENESIS_VBE=off` at build time picks the graphics mode
or keeps text mode. Expected as of 2026-09-27:

| Suite | `-smp 4` | `GENESIS_SMP=1` |
|---|---|---|
| `verification` (`/bin/verif`) | 160 passed | 160 passed |
| `systest` | 510 passed | 507 passed |
| `thr` (`thr.exe`) | 45 passed | 45 passed |
| `smp` (`smp.exe`) | 61 passed | 57 passed |
| `tls` (`tls.exe`) | 24 passed | 24 passed |
| `wait` (`wait.exe`) | 46 passed | 46 passed |
| `seh` (`seh.exe`) | 17 passed | 17 passed |
| `fbtest` | 52 passed | 52 passed |
| boot selftests | `dhcp: selftest passed`, `irqbalance: selftest passed`, `fb: selftest passed`, `psm: selftest passed (4-byte packets)`, no `FAILED` | same |
| host (`tests/host/run.sh`) | exit 0 | |

A hung guest: `tools/guest_dump.py CMD` runs commands and presses Ctrl-T for a
task dump. For a hard hang, boot QEMU with `-monitor unix:/tmp/m.sock,server,nowait`
and read `info registers` per CPU, then `addr2line -f -e build/kernel.elf`
the RIPs — that is how the BKL/tick deadlock above was found.

**Do not put a gnfs volume on IDE index 1**: the first volume mounted becomes
`/`, and an empty one means no userland runs, which looks like a clean boot.
Use `guest_run.py` or `build.py run`, which attach drives correctly.
