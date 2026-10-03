# Handoff — Genesis

*Where to pick the work up. For what already works, subsystem by subsystem,
see `FEATURES.md`; for why each decision was made, `ROADMAP.md` (item numbers
are never renumbered — source comments cite them).*

Last updated 2026-10-03 (phase 2 started: named NT objects). Repository: https://github.com/bubba510kevin/Genesis
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

## Two repositories (split 2026-09-28)

**This repository is the kernel alone.** Everything that runs in ring 3 is
in **Genesis-userland** (github.com/bubba510kevin/Genesis-userland), split out
with its history: ntdll, kernel32, the rtld, libgnt, systest, verif and every
test program, the committed `root/` tree, and — as submodules —
**GNTbash** (`/bin/bash`) and **GNTlibc** (`/lib/libc.so`). ntdll and
kernel32 are expected to move to repositories of their own later; each
component there is a self-contained `src/<name>/` for that reason.

Check the two out side by side:
```
code/Genesis/Genesis/            this repo   (the kernel checkout)
code/Genesis/Genesis-userland/   userland    (git clone --recurse-submodules)
```
- `build.py disk` stages `../Genesis-userland/root` (or `GENESIS_USERLAND`),
  then this repo's `modules/root` on top: the loadable modules and
  `test.sys`, which stayed here because they are kernel code
  (`modules/build.sh`, `tools/mkpe.py --sys`). A file in both trees is an
  error. `tests/host/check_staged_tree.py` checks the merged tree.
- Userland's `build.sh` reads one file from here, `kernel/include/nt.h`
  (ntdll's syscall numbers), via `GENESIS_KERNEL` (default `../Genesis`).
  **Changing an NT syscall number means rebuilding ntdll in userland.**
- `src/...` paths in kernel comments and in `ROADMAP.md` now mean
  Genesis-userland (single-file programs moved into directories:
  `src/verif/verif.c`, `src/systest/systest.c`, `src/gtrace/gtrace.c`...).
- A change that spans both (a new syscall and its test) is two commits, one
  per repository; land the kernel side first.

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
- **Phase 1 started** (item 15): GNU bash 5.3 runs interactively; a real
  termios terminal, select/pselect6, rlimits, getrusage, job control. See
  "Phase 1" below.

## What needs to be done now, in order

`ROADMAP.md` is organized into **four phases** (THE PLAN, at its top). Work
them in order; a later phase pulls forward only the pieces of an earlier one
it needs. **The GUI rule:** nothing that deals with a GUI is written from
scratch — it is taken (precompiled Windows binaries on the real win32k.sys,
or ported open source); Genesis writes the support underneath.

### Phase 1 — run bash (ROADMAP item 15) — IN PROGRESS
**Done (2026-09-27):** bash 5.3 runs interactively as `/bin/bash` — now
**GNTbash**, built by Genesis-userland from its `third_party/GNTbash`
submodule (the first bring-up used a plain upstream build; GNTbash with
winmode off is upstream bash, and the kernel work below is the same for
both): readline editing, history, tab completion,
Ctrl-C of the prompt and of a running loop. Underneath it: `select`/`pselect6`,
real rlimits (`RLIMIT_NOFILE` enforced), `getrusage`, **a real termios line
discipline** (`kernel/dev/tty.c` — raw mode, VMIN/VTIME, signal characters at
input time, TIOCSCTTY, SIGTTIN, SIGWINCH), **job control** (stop/continue,
`wait4` WUNTRACED/WCONTINUED/pid forms/WIFSIGNALED), signal handlers delivered
from the interrupt path (with FPU state), and the `rt_sigsuspend` mask bug
fixed. ROADMAP item 15 has the detail and the three bugs found.

**Tools for this work:** `/bin/gtrace CMD` traces CMD's syscalls (and its
children's) on the console; `tools/guest_sh.py -f FILE` types FILE's lines
into the guest shell (`@wait:REGEX` waits for output — bash takes ~25s to
load under TCG, see below). `guest_run.py` runs userland's
`root/usr/tests/bashtest.sh`
under bash as its last suite (non-interactive: fork/wait, signals, pipes,
here-docs, rlimits, `read -t`); the interactive side is still checked by
hand with `guest_sh.py`.

A fourth bug came out of the wait4 rework: an **ignored** signal (SIGCHLD
by default) used to be made pending, so another child's exit made a
blocking `wait4(pid)` return -EINTR. `signal_send` now discards ignored,
unblocked signals, as Linux does.

**Next, in order:**
1. (j) Something to run: BusyBox with its applets (needs links — see 2/h) or
   coreutils built the way bash is. Without it `ls | head` fails.
2. (i)/(h) Long names and symlinks: move `/` to gnfs (has neither yet) or add
   VFAT long names to fatfs; `/bin/sh -> bash`.
3. (d)/(g) A writable `/tmp` (tmpfs), staged `/etc/passwd`/`group`/`profile`,
   then bash as the login shell.
4. (e)/(f) `/proc/self/fd`, FIFOs, `/dev/fd` — process substitution.
5. (l) `#!` scripts; then bash's own `tests/` in the guest.
Open questions: `/bi<Tab>` beeps instead of completing (trace it with
gtrace); exec of a 1.4MB binary takes ~25s (polled single-sector ATA PIO).

### Phase 2 — a modern kernel, Linux and NT 10.0 parity (item 16)
A gap inventory, worked in the order phases 3-4 and real programs need it.
The pieces already queued from earlier work belong here:

**Finish item 14(a) — threads**
- **Named mutexes — DONE 2026-10-03** (kernel side, this repo): `OBJ_OPENIF`
  on the three creates (`STATUS_OBJECT_NAME_EXISTS`), `NtOpenMutant` `0x2A`,
  `NtOpenSemaphore` `0x2B`, and **temporary names** (a name goes with the
  last handle - `OB_FLAG_TEMPORARY` in `object.h`). ROADMAP 16(k) has the
  detail. **The userland half is a separate commit in Genesis-userland**
  (ntdll stubs, kernel32 `CreateMutexW` with names / `OpenMutexW` /
  `OpenEventW` / `OpenSemaphoreW`, sync.exe and wait.exe checks); land it,
  then add `/bin/sync.exe`, `/bin/ntsync.exe`, `/bin/vm.exe`,
  `/bin/proc.exe` and `/bin/reg.exe` to `guest_run.py`'s default list (they print tallies only
  from that commit on).
- **Table sizes — DONE 2026-10-03**: `MAX_PROCESSES` 64 -> 256 (kernel
  stacks to match), Windows threads per process 64 -> 128, `MAX_HANDLES`
  32 -> 256, the object and open-instance pools 64 -> 1024, dispatcher
  objects 64 -> 512. The scheduling-path scans stop at `proc_slots_used()`
  (the high-water slot), and a wait queue is now a slot bitmap (32 bytes,
  was 2KB at 256 slots). ROADMAP 16(k) has the detail. thr.exe runs 100
  threads at once. **Userland must move with it**: systest and bashtest
  pin `RLIMIT_NOFILE` (now 256), and the 100-thread check is in thr.exe -
  all in the same Genesis-userland patch as the named objects. A real run
  queue is still not needed: the scans are priced by threads alive, not by
  the table.

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
- **statx, close_range, sysinfo, sendfile, copy_file_range, signalfd,
  pidfd — DONE 2026-10-03** (16(a)): and `newfstatat` now resolves its
  path (it described every name as a character device). systest 704.
- **NT waitable timers — DONE 2026-10-03** (16(k), `timer_create` in
  `kernel/obj/dispatch.c`, NT calls `0x90`-`0x94`), on the kernel timers;
  ntsync.exe 67 (was 34), in the Genesis-userland patch with kernel32's
  waitable-timer API.
- **epoll and timerfd — DONE 2026-10-03** (16(a), `kernel/fs/epoll.c`,
  `kernel/fs/timerfd.c`): plus **kernel timers** (`ktimer.h`) - callbacks
  run from the tick, the first thing that makes an OBJECT ready at a time
  (NT waitable timers, `NtCreateTimer`, can be built on it next). systest
  634 (was 561); the systest change is in the Genesis-userland patch.
- **The in-kernel registry — DONE 2026-10-03** (16(p), `kernel/obj/registry.c`,
  NT calls `0x86`-`0x8F`): a key tree under `\Registry` built at boot and
  seeded with the CurrentVersion values, the session-manager environment, the
  computer name and the processor; it does not persist across reboots yet.
  Next for it: regf hives loaded from and written to files, then
  `NtNotifyChangeKey` and key security (with tokens, 16(n)). advapi32's Reg*
  (14(g)) can now be written over it in userland. reg.exe is in the same
  Genesis-userland patch.

### Phase 3 — bash understands Windows (item 17) — started: GNTbash
The shell is its own repository, **GNTbash**
(github.com/bubba510kevin/GNTbash): bash 5.3.9 on branch `upstream`,
Genesis commits on `main`. Done there: drive-letter paths, `pwd -W`, PATHEXT
lookup, the PE argument vector and environment, `.bat` via `cmd.exe`, CRLF
scripts (`igncr`), and `winpath`/`unixpath`/`where`. Its `genesis/build.sh`
builds the static musl binary, and Genesis-userland stages it as
`/bin/bash` (`getrlimit`, the one call it was missing, exists since phase
1). GNTbash is at bash patch level 9 (upstream is at 20+): catching up is a
rebase in that repository. Left: exact PE command-line quoting (a kernel fix
in `kernel/exec/ntproc.c`), the full 32-bit exit code and console control
events (both need kernel interfaces), completion, and a `cmd.exe`.

### Mixed images (item 19) — first version done; libc-using .so added (GNTlibc)
A `.so` that uses libc now runs inside a Windows process. **GNTlibc**
(github.com/bubba510kevin/GNTlibc) is musl 1.2.6 built as `libc.so` (pristine
musl on `upstream`, one file `src/genesis/gnt_start.c` on `main`). Its
constructor runs first in the loader's dependency order and installs a thread
pointer via `arch_prctl(ARCH_SET_FS)` from user space (musl's own
`__init_tls`/`__init_tp` are the ldso variants in the shared libc and do not
set `%fs`), plus `libc.page_size` and `libc.auxv`. No kernel change was needed:
`libc.so` has no `PT_TLS` and only relocations `elfso` already applies, and
musl's untagged syscalls route to the Linux table from a PE process.
Genesis-userland's `build.sh` builds its `third_party/GNTlibc` submodule
(or `GNTLIBC_SRC`), stages `libc.so` to `/lib`, and `src/somix/build.sh`
builds `libmixc.so` against it;
`mix.exe` proves `malloc`/`printf`/`strtol`/`errno`/`libm` (47 checks). Next
there: a `CreateThread` thread inside a PE process needs `__gnt_thread_init`
wired to the Windows thread path; TLS both ways; unloading; SEH.

Windows programs `LoadLibrary` Linux `.so` files (`kernel/exec/elfso.c`,
`src/kernel32/loader.c`, adapters in `winelf.S`); Linux programs load DLLs
with `src/libgnt` (`prctl(PR_GENESIS_PE_LOAD)`, `kernel/exec/ntmix.c`); the
kernel routes each syscall by `NT_SYSCALL_TAG`. Tests: `mix.exe`, `elfmix`.

### Phase 4 — the libraries: major .so and DLLs (item 18; 14(e)-(o))
Written (non-GUI): the loader (14(e): `LoadLibrary`/`GetProcAddress` exist
now, see item 19; left: a search path, API sets, SxS), kernel32/kernelbase (14(f)), the C runtimes (msvcrt, ucrtbase),
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
  partly present on this machine — `modules/build.sh` skips rebuilding
  `if_rl`/`if_re` without it and keeps the staged
  `modules/root/boot/kernel/ifre.ko`.
  Restore `vendsrc/sys/dev/{rl,re}` if that driver must change.

## Rules learned the hard way
- **NT syscall numbers** live in `kernel/include/nt.h` (ntdll's copy is
  generated from it). In use: `0x01`-`0x3F` and `0x80`-`0x94`.
  `0x40`-`0x7F` is the display/input range; the next core call is `0x95`. Parallel work on the display/input
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
repository root. Rebuilding changes committed binaries (userland's `root/`,
this repo's `modules/`) whenever the toolchain version differs; commit only
those whose source changed. A fresh clone on Windows: Git for Windows
defaults `core.autocrlf` to true, which breaks every shell script here - set
`git config core.autocrlf false` in this checkout (Genesis-userland forces LF
with `.gitattributes`).
```
wsl bash build/b.sh                                   # kernel -> build/kernel.elf
wsl bash -c "cd /mnt/c/Users/kevin/code/Genesis/Genesis-userland && sh build.sh"   # all user programs, DLLs, GNTbash, GNTlibc
wsl bash -c "cd /mnt/c/Users/kevin/code/Genesis/Genesis && sh modules/build.sh"   # loadable kernel modules (rarely needed)
wsl bash -c "cd /mnt/c/Users/kevin/code/Genesis/Genesis && rm -f build/disk.img && python3 build.py disk"   # restage the FAT root: userland root/ + modules/root
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
or keeps text mode. Expected as of 2026-10-03 (phase 2's first changes, with the userland patch):

| Suite | `-smp 4` | `GENESIS_SMP=1` |
|---|---|---|
| `verification` (`/bin/verif`) | 160 passed | 160 passed |
| `systest` | 704 passed | 558 passed |
| `thr` (`thr.exe`) | 47 passed | 47 passed |
| `smp` (`smp.exe`) | 70 passed | |
| `tls` (`tls.exe`) | 24 passed | 24 passed |
| `wait` (`wait.exe`) | 60 passed | 60 passed |
| `sync` (`sync.exe`) | 51 passed | |
| `ntsync` (`ntsync.exe`) | 67 passed | |
| `vm` (`vm.exe`) | 68 passed | |
| `proc` (`proc.exe`) | 40 passed | |
| `reg` (`reg.exe`) | 31 passed | |
| `seh` (`seh.exe`) | 17 passed | 17 passed |
| `fbtest` | 52 passed | 52 passed |
| `bashtest` (`/bin/bash /usr/tests/bashtest.sh`) | 20 passed | 20 passed |
| boot selftests | `dhcp: selftest passed`, `irqbalance: selftest passed`, `fb: selftest passed`, `psm: selftest passed (4-byte packets)`, no `FAILED` | same |
| host (`tests/host/run.sh`) | exit 0 | |

A hung guest: `tools/guest_dump.py CMD` runs commands and presses Ctrl-T for a
task dump. For a hard hang, boot QEMU with `-monitor unix:/tmp/m.sock,server,nowait`
and read `info registers` per CPU, then `addr2line -f -e build/kernel.elf`
the RIPs — that is how the BKL/tick deadlock above was found.

**Do not put a gnfs volume on IDE index 1**: the first volume mounted becomes
`/`, and an empty one means no userland runs, which looks like a clean boot.
Use `guest_run.py` or `build.py run`, which attach drives correctly.
