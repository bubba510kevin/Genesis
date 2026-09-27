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

## What needs to be done now, in order

### 1. Finish item 14(a) — threads (small, self-contained)
- **Named mutexes**: `CreateMutexW` refuses a name today, because the other
  half of the named form (opening the existing one, `ERROR_ALREADY_EXISTS`,
  `OpenMutexW`) has no kernel path. `NtOpenEvent` in `kernel/exec/nt.c` is
  the pattern for an `NtOpenMutant`.
- **MAX_PROCESSES = 64** (`kernel/include/process.h`) is shared by processes,
  threads, kthreads and idle threads. Real Win32 programs with thread pools
  will hit it; the table is scanned linearly by the scheduler, so raising it
  far means a run queue.

### 2. Loose ends of 14(b)/(c) — small, pick up alongside 14(d)
See the STILL OPEN note under item 14(c) in `ROADMAP.md`: MinGW C++
exceptions are untested (libgcc's SEH unwinder needs a CRT - `malloc`,
`abort` - that this tree does not have yet; once one exists, a `throw`/
`catch` test belongs in `src/winseh`); nested dispatch
(`EXCEPTION_NESTED_CALL`); `EXCEPTION_EXECUTE_HANDLER` from the unhandled
filter should unwind (run `__finally`s) before exiting; stack overflow
(`STATUS_STACK_OVERFLOW` needs a guard page, 14(d)).

### 3. Items 14(d)–(f) — memory, loader, kernel32
- (d) NT page-state model (reserve/commit, `VirtualProtect`, `VirtualQuery`),
  Section objects / `MapViewOfFile`.
- (e) `LdrLoadDll`/`LdrGetProcedureAddress` at run time (`LoadLibrary`,
  `GetProcAddress`), a DLL search path.
- (f) kernel32 breadth: `CreateProcess`, `MultiByteToWideChar`/
  `WideCharToMultiByte`, environment, console API, time/locale, file API
  breadth. Test with real MinGW programs, not only purpose-built ones.

### 4. Then the graphical stack — 14(g) onward
Registry (advapi32); the ntoskrnl/hal export surface so precompiled drivers
load (win32k.sys first, then the VGA-class display driver and the
i8042prt/mouclass input stack); the real gdi32/user32/comctl32/explorer on
top; COM. Mouse: precompiled stack, or FreeBSD's psm ported meanwhile. See
item 14.

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
`build/guest.log`. Expected as of 2026-09-27:

| Suite | `-smp 4` | `GENESIS_SMP=1` |
|---|---|---|
| `verification` (`/bin/verif`) | 160 passed | 160 passed |
| `systest` | 510 passed | 507 passed |
| `thr` (`thr.exe`) | 45 passed | 45 passed |
| `smp` (`smp.exe`) | 61 passed | 57 passed |
| `tls` (`tls.exe`) | 24 passed | 24 passed |
| `wait` (`wait.exe`) | 46 passed | 46 passed |
| `seh` (`seh.exe`) | 17 passed | 17 passed |
| boot selftests | `dhcp: selftest passed`, `irqbalance: selftest passed`, no `FAILED` | same |
| host (`tests/host/run.sh`) | exit 0 | |

A hung guest: `tools/guest_dump.py CMD` runs commands and presses Ctrl-T for a
task dump. For a hard hang, boot QEMU with `-monitor unix:/tmp/m.sock,server,nowait`
and read `info registers` per CPU, then `addr2line -f -e build/kernel.elf`
the RIPs — that is how the BKL/tick deadlock above was found.

**Do not put a gnfs volume on IDE index 1**: the first volume mounted becomes
`/`, and an empty one means no userland runs, which looks like a clean boot.
Use `guest_run.py` or `build.py run`, which attach drives correctly.
