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
it, (a) through (s). Genesis writes its **own** clean-room user32/gdi32/shell32
rather than run Microsoft's (those need win32k.sys). Be honest about scale:
item 14 is bigger than everything built so far.

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
- **Networking** (item 6): TCP + lo0, Linux-ABI sockets, **DHCP** with renewal.
- **gnfs v2**: big files, rename, snapshots; ZFS removed.

## What needs to be done now, in order

### 1. Finish item 14(a) — threads (small, self-contained)
- **Mutant abandonment**: a mutex held by a thread that dies must be released
  with `WAIT_ABANDONED` for the next waiter. The hook is `proc_nt_thread_exit`
  (`kernel/proc/process.c`); mutant state is in `kernel/obj/dispatch.c`.
- **`CREATE_SUSPENDED` / `ResumeThread` / `SuspendThread`**: `NtCreateThreadEx`
  (`kernel/exec/nt.c`) ignores the flag today.
- **`NtTerminateThread` on another thread** returns `STATUS_NOT_IMPLEMENTED`
  (`kernel/exec/nt.c`); the killing-a-thread-on-another-CPU machinery already
  exists for signals — reuse it.
- **MAX_PROCESSES = 64** (`kernel/include/process.h`) is shared by processes,
  threads, kthreads and idle threads. Real Win32 programs with thread pools
  will hit it; the table is scanned linearly by the scheduler, so raising it
  far means a run queue.

### 2. Item 14(b) — dispatcher objects, completed
`WaitForMultipleObjects` (wait-any and wait-all), alertable waits and **APCs**
(`QueueUserAPC`, `NtQueueApcThread`, APC delivery on alertable wait / return
to user mode). I/O completion and thread termination lean on APCs.
`NtWaitForAlertByThreadId` (nt_sys.c) is a good pattern for the kernel side.

### 3. Item 14(c) — structured exception handling (x64 table-based)
`RtlDispatchException`, `RtlUnwindEx`, `RtlVirtualUnwind` over `.pdata`/
`.xdata`, `KiUserExceptionDispatcher` entry from the kernel on a fault in a
Windows process (today a fault kills it), vectored handlers,
`RaiseException`, `SetUnhandledExceptionFilter`. Not optional: `__try` is
everywhere in real Windows code, and MinGW's C++ exceptions use it.

### 4. Items 14(d)–(f) — memory, loader, kernel32
- (d) NT page-state model (reserve/commit, `VirtualProtect`, `VirtualQuery`),
  Section objects / `MapViewOfFile`.
- (e) `LdrLoadDll`/`LdrGetProcedureAddress` at run time (`LoadLibrary`,
  `GetProcAddress`), a DLL search path.
- (f) kernel32 breadth: `CreateProcess`, `MultiByteToWideChar`/
  `WideCharToMultiByte`, environment, console API, time/locale, file API
  breadth. Test with real MinGW programs, not only purpose-built ones.

### 5. Then the graphical stack — 14(g) onward
Registry (advapi32), framebuffer display (VESA), gdi32, a mouse driver
(PS/2 first; USB later), user32, COM, comctl32, a shell. See item 14.

### Side work, smaller, any time
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

Everything builds and runs in WSL (Debian) against this working copy:
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
| `thr` (`thr.exe`) | 16 passed | 16 passed |
| `smp` (`smp.exe`) | 61 passed | 57 passed |
| `tls` (`tls.exe`) | 24 passed | 24 passed |
| boot selftests | `dhcp: selftest passed`, `irqbalance: selftest passed`, no `FAILED` | same |
| host (`tests/host/run.sh`) | exit 0 | |

A hung guest: `tools/guest_dump.py CMD` runs commands and presses Ctrl-T for a
task dump. For a hard hang, boot QEMU with `-monitor unix:/tmp/m.sock,server,nowait`
and read `info registers` per CPU, then `addr2line -f -e build/kernel.elf`
the RIPs — that is how the BKL/tick deadlock above was found.

**Do not put a gnfs volume on IDE index 1**: the first volume mounted becomes
`/`, and an empty one means no userland runs, which looks like a clean boot.
Use `guest_run.py` or `build.py run`, which attach drives correctly.
