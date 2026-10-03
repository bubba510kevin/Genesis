# Genesis — Feature Guide

*Last updated 2026-09-27. A complete tour of what Genesis is, what it can do
today, how each piece works, how each piece is verified, and what is not
there yet.*

This file is the **map**. The other documents are the **record**:

| Document | What it is for |
|---|---|
| `FEATURES.md` (this file) | What exists, explained, organized by subsystem |
| `ROADMAP.md` | What is unfinished, and the reasoning behind every decision (dense prose) |
| `ROADMAP-archive.md` | The completed items, with their history |
| `handoff.md` | Where to pick the work up, and how to build and test |
| `src/verif.c` | The bug postmortems, and checks that need a human or the boot log |
| `kernel/README.md`, `kernel/bsd/README.md` | Source layout, and the vendored-FreeBSD manifest |

**Status markers used below**

- ✅ **Working** — implemented and covered by a test that runs.
- 🟡 **Partial** — works within stated limits; the limits are listed.
- ❌ **Not yet** — absent; listed so nobody assumes it exists.

---

## Contents

1. [What Genesis is](#1-what-genesis-is)
2. [At a glance](#2-at-a-glance)
3. [Booting and the hardware platform](#3-booting-and-the-hardware-platform)
4. [Memory management](#4-memory-management)
5. [Processes, threads and scheduling](#5-processes-threads-and-scheduling)
6. [The Linux system-call interface](#6-the-linux-system-call-interface)
7. [Storage: disks, partitions and volumes](#7-storage-disks-partitions-and-volumes)
8. [Filesystems](#8-filesystems)
9. [Security and permissions](#9-security-and-permissions)
10. [The Windows (NT) personality](#10-the-windows-nt-personality)
11. [The NT object manager and namespace](#11-the-nt-object-manager-and-namespace)
12. [Linux userland: ELF, the dynamic linker, musl](#12-linux-userland-elf-the-dynamic-linker-musl)
13. [The three driver models](#13-the-three-driver-models)
14. [Networking](#14-networking)
15. [Vendored FreeBSD kernel infrastructure](#15-vendored-freebsd-kernel-infrastructure)
16. [Debugging aids](#16-debugging-aids)
17. [How Genesis is tested](#17-how-genesis-is-tested)
18. [Building and running](#18-building-and-running)
19. [Known limitations](#19-known-limitations)
20. [The road to the goal](#20-the-road-to-the-goal)
21. [Where things live](#21-where-things-live)

---

## 1. What Genesis is

Genesis is a **from-scratch x86-64 operating-system kernel** that boots in QEMU
and on real hardware, and that deliberately supports **three software
ecosystems at once**:

1. **Linux programs** — native ELF binaries, statically or dynamically
   linked, including ones built against the real **musl** C library, running
   on the real Linux x86-64 system-call numbers.
2. **Windows programs** — real PE executables (built with MinGW), running on
   Genesis's own **clean-room `ntdll.dll` and `kernel32.dll`**, with a TEB and
   PEB built by the kernel exactly where Windows programs expect them.
3. **Three kinds of drivers** — FreeBSD **Newbus** drivers (including an
   *unmodified* 4,295-line FreeBSD network driver), **Linux** drivers written
   against a LinuxKPI source-compatibility layer, and **Windows WDM** drivers
   (`.sys` files loaded as kernel-mode PE images).

Large parts of the kernel are **real, vendored FreeBSD source** rather than
reimplementations: the IPv4 network stack, the socket layer, the routing table,
sysctl, the UMA allocator, mbufs, TCP, and more (`kernel/bsd/`). Genesis has
its own native copy-on-write filesystem, **gnfs**, with snapshots. It runs
programs on **every CPU at once** (§3, §5).

**The long-term goal**: a daily-drivable desktop running Genesis's own
reimplementation of the Windows 7 desktop, able to run real Win32 programs.
That goal is far off, and §20 lays out the remaining steps in order.

**Engineering culture.** Almost every feature carries a test that runs: at
boot, in the host test suite, or from a real user program inside the booted
machine. New checks are routinely confirmed by breaking the code on purpose and
watching the test fail. Bugs get written up where the next person will find
them. See §17.

---

## 2. At a glance

| Area | Status | Summary |
|---|---|---|
| Boot (QEMU + bare metal) | ✅ | BIOS MBR bootloader → 64-bit long mode; single-disk USB image for real machines |
| Multiprocessor | ✅ | Every CPU runs programs (up to 8; QEMU boots 4); affinity, per-CPU timers, targeted TLB shootdown, interrupt binding; kernel serialised by a big lock |
| Memory | ✅ | 4-level paging, NX, W^X, vendored UMA slab allocator, low-memory reclaim |
| Processes | ✅ | fork, vfork, clone, execve (ELF and PE), signals, wait4/waitid, sessions, groups |
| POSIX threads | ✅ | clone() threads with TLS and `pthread_join` wakeups |
| Windows threads | ✅ | CreateThread / WaitForSingleObject, a TEB and stack per thread; critical sections, SRW locks, condition variables |
| Scheduler | ✅ | FreeBSD-style ULE with interactivity scoring, on every CPU; preemptive for user code; affinity masks |
| Linux syscalls | ✅ | ~145 real Linux x86-64 syscall numbers |
| FAT16 | ✅ | Full read and write; the default root filesystem |
| gnfs (native COW FS) | ✅ | Read, write, ACLs, rename, ~1GB files, growing directories, snapshots; crash-safe commits |
| Permissions | ✅ | One NFSv4/NT ACL model with POSIX and Windows views; chmod, chown, umask, setgid, sticky |
| Windows programs | 🟡 | PE loading, ntdll + kernel32 subset, 31 NT syscalls; no GUI |
| Mixed images | ✅ | Windows programs `LoadLibrary` Linux `.so` files, including libc-using ones ([GNTlibc](https://github.com/bubba510kevin/GNTlibc): musl as `libc.so`); Linux programs load DLLs (`libgnt`); syscalls routed per call |
| NT object manager | ✅ | Namespace, named events/semaphores/mutexes, \ObjectTypes |
| Driver models | ✅ | Newbus, LinuxKPI and WDM, loadable from 4 directories, with unload; each with its multiprocessor API |
| Networking | 🟡 | IPv4, ICMP, ARP, UDP, **TCP**, loopback (FreeBSD's own code); no DHCP, no IPv6 |
| Storage drivers | ✅ | ATA (PIO), AHCI (DMA), MBR partitions |
| Display | ✅ | VESA/VBE linear framebuffer (1024x768x32 by default), set by the bootloader; `/dev/fb0` with Linux fbdev ioctls and `mmap` |
| Console | ✅ | Drawn into the framebuffer (128x48, BIOS 8x16 font) or VGA text; PS/2 keyboard; serial console (usable as the only input) |
| Mouse | ✅ | PS/2 mouse (Newbus `psm` on `atkbdc`), wheel; `/dev/mouse0` delivers evdev `input_event` records |
| GUI / desktop | ❌ | A framebuffer and a mouse exist; no window system yet; see §20 |

**Latest test results** (2026-09-27, full machine under QEMU, `-smp 4`):
`verification: 160 passed, 0 failed` · `systest: 510 passed, 0 failed` ·
`thr: 16 passed, 0 failed` · `smp: 53 passed, 0 failed` · host suite: all
checks passed · 25+ boot-time self-tests all passing. The same suites pass on
one CPU (`GENESIS_SMP=1`).

---

## 3. Booting and the hardware platform

### Bootloader ✅
`bootloader/boot/boot.asm` is a BIOS MBR boot sector. It switches to
**unreal mode** so it can copy the kernel above 1MB (a BIOS disk read cannot
write there itself), enables the A20 line *before* the copy, and loads the
kernel at `0x100000`. The kernel then enters **64-bit long mode**
(`kernel/arch/boot64.c`). The boot sector keeps the MBR partition-table bytes
zeroed and padded, so the kernel's own partition scanner can never mistake boot
code for a partition. The build fails if the code ever grows into that area.

It also sets the **graphics mode**, in real mode while the BIOS can still be
called (ROADMAP 14(h)). The boot sector has eight bytes to spare, so after
loading the kernel it makes one far call through the High Memory Area
(`0xFFFF:0x0020` = linear `0x100010`) into a stub carried at the front of the
kernel image (`kernel/arch/vbe_boot.c`). The stub copies the BIOS 8x16 font,
walks the VBE mode list for the largest 32bpp direct-colour mode with a linear
framebuffer that fits the request (1024x768 unless `GENESIS_VBE=WxH` or
`GENESIS_VBE=off` says otherwise at build time), sets it, and leaves a
description at physical `0x5400` (`kernel/include/bootvbe.h`). Any failure
leaves the machine in VGA text mode, which is still fully supported.

### Real hardware ✅
`python3 build.py usb` produces **one bootable disk image**: kernel and FAT16
root together, with a real partition table, ready to `dd` onto a USB stick.
`build.py usbrun` tests exactly that image as a machine's *only* disk before it
goes near real hardware.

### CPU setup ✅
- **GDT/TSS per CPU** and **IST stacks per CPU**. A shared TSS would silently
  corrupt state across CPUs, so this is required, not tidiness.
- **IDT** with a **dynamic vector allocator** (`kernel/arch/idt_alloc.c`).
- **NX** (`EFER.NXE`) enabled on every CPU.
- **E820** memory map parsing (`kernel/arch/e820.c`).

### Interrupt controllers ✅
- **8259 PIC** for early boot, then **masked**.
- **Local APIC** on every CPU (`kernel/arch/lapic.c`). Each CPU needs its own
  LAPIC enabled.
- **IOAPIC** (`kernel/arch/ioapic.c`), programmed from ACPI's MADT, including
  **interrupt source overrides**. On QEMU the timer arrives on GSI 2, not 0.
- **MSI** for PCI devices that support it.
- **IRQ sharing** between drivers on one line (`kernel/dev/irq.c`).

### Multiprocessor (SMP) ✅
`kernel/arch/smp.c`, `kernel/proc/bkl.c`, `kernel/proc/sched.c`. CPUs are found
in ACPI's MADT and started with INIT-SIPI-SIPI through a real-mode trampoline.
**Every CPU runs user programs**, and threads of one program run on several
CPUs at the same moment. That is proven by a ping-pong between two spinning
threads, which exchange 200,000 times in about 150ms and could not finish at
all on one CPU.

- **Per CPU:** GDT/TSS/IST, a `%gs` block (current thread, idle thread,
  reschedule flag, time slice, loaded address space), the SYSCALL MSRs, the
  FPU/SSE enables, and its own **LAPIC timer**, calibrated against the PIT,
  for preemption. The TSC is calibrated too and backs the performance
  counters.
- **The big kernel lock.** One ticket lock serialises the kernel; user code
  runs in parallel. It is taken on every entry from ring 3 and dropped on
  every exit, and it is passed across a context switch rather than released.
  This is the Giant/BKL model FreeBSD 5 and Linux 2.x shipped SMP with. It
  makes all existing kernel code, and every vendored subsystem, correct on N
  CPUs at once.
- **IPIs:** TLB shootdown, remote function call, and reschedule, through
  per-CPU mailboxes. A CPU spinning for the big lock services them, so the
  holder can never deadlock waiting for its answer.
- **TLB shootdown is targeted.** A user-space change goes only to the CPUs
  that have that address space loaded; a kernel-half change goes to all. A
  stale copy-on-write fault from another CPU is retried, not fatal.
- **Scheduling:** an idle thread per CPU, affinity masks, NT's ideal
  processor, wake-up IPIs to idle CPUs, and migration (a thread can move
  itself to another CPU mid-call). A thread killed while running on another
  CPU is stopped by an IPI, and is never reaped until that CPU has let go of it.
- **Interrupt binding:** a device line can be moved to any CPU
  (`bus_bind_intr`); the boot self-test moves the NIC's interrupt to CPU 3 and
  sees it arrive there.
- **Interrupt balancing** (`kernel/arch/irqbalance.c`): at boot every device
  line is spread across the CPUs, then an `irqbalance` kernel thread re-plans
  every 2 seconds from how many interrupts each line actually took (heaviest
  line first, onto the least-loaded CPU; applied only when it cuts the busiest
  CPU's load by a quarter). A line a driver bound itself is left alone. The
  self-test checks the planner against known loads and that the NIC's
  interrupt arrives on the CPU the policy chose.
- **Ctrl-T** on the console prints every task's state and every CPU's current
  thread.

### PCI ✅
Enumeration with **PCI-to-PCI bridge recursion**, BAR sizing, and config-space
read/write (`kernel/dev/pci.c`). A fallback `pci_generic` driver claims
anything no real driver takes.

### Timers and clocks ✅
PIT/LAPIC tick (`kernel/dev/timer.c`), **RTC** wall clock (`kernel/dev/rtc.c`),
and TSC-based short delays (a driver may call `DELAY()` before interrupts are
on). Clock resolution is **one tick** and is reported honestly.

### Console and input ✅
- A **terminal with a real termios** (`kernel/dev/tty.c`, new 2026-09-27):
  canonical mode with erase/kill/word-erase/literal-next/reprint, and **raw
  mode** with `VMIN`/`VTIME` exactly as termios(3) defines them - what
  readline needs. Input is processed **when it is typed** (as Linux's n_tty
  does), so **Ctrl-C, Ctrl-Z and Ctrl-backslash raise SIGINT, SIGTSTP and
  SIGQUIT** in the
  foreground process group even when nothing is reading the terminal, and
  type-ahead is echoed as typed. `TCGETS`/`TCSETS[WF]`, `TCFLSH`, `FIONREAD`,
  `TIOC[GS]PGRP`, `TIOCSCTTY`/`TIOCNOTTY`/`TIOCGSID`, `TIOC[GS]WINSZ` (a new
  size sends SIGWINCH). A background process group reading the terminal gets
  **SIGTTIN**. PS/2 arrow and navigation keys send the VT100 sequences
  (`ESC [ A`...). The console draws into the **framebuffer** when there is one
  (8x16 BIOS font, 16 VGA colours, 128x48 at 1024x768, repainting only the
  cells that changed) and into **VGA text** otherwise. `TIOCGWINSZ` reports
  the real grid. A program that draws to the screen takes it with Linux's
  `KDSETMODE KD_GRAPHICS` and gives it back with `KD_TEXT` (the console then
  repaints); a program that exits holding it loses it at the next line of
  console output.
- **PS/2 keyboard** (`kernel/dev/keyboard.c`).
- **Serial console** (`kernel/dev/serial.c`): everything the kernel prints is
  mirrored to COM1, and **received serial bytes feed the same input ring as
  the keyboard**. A machine with no PS/2 keyboard (the bare-metal target has
  only USB) is therefore still usable over serial, and the automated test
  harness types into the machine this way.

### Display: the linear framebuffer ✅ (`kernel/dev/fb.c`)
ROADMAP 14(h). The kernel maps what the bootloader set (uncached-minus, so a
write-combining MTRR still applies) and serves it three ways: to the console,
to other kernel code (`fb_get()`), and to user space as **`/dev/fb0`** -
`FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` in Linux's layout, `read`/`write`
at an offset, and **`mmap(MAP_SHARED)`** of the pixels. Device pages carry a
`PAGE_DEVICE` bit in the page tables, so `munmap` and exit never free them and
`fork` shares them writable instead of copy-on-write. No mode switching after
boot (the BIOS is gone by then); `FBIOPUT_VSCREENINFO` accepts only the mode
already set.

**Verified by** the `fb` boot selftest (the bootloader's block and font, the
mode really active - read back from the Bochs/QEMU adapter's own registers
where it has them, the kernel mapping page by page, pixel readback, the
console's glyph renderer checked pixel-for-pixel against the font, and
`/dev/fb0`'s read and mmap paths) and by `/bin/fbtest` from ring 3 (below).

### Mouse ✅ (`kernel/dev/atkbdc.c`, `psm.c`, `mouse.c`)
The mouse half of ROADMAP 14(j). `atkbdc` is the i8042 controller as a bus;
**`psm`** is the PS/2 mouse, written as a FreeBSD Newbus driver (DEVMETHOD
table, `DRIVER_MODULE(psm, atkbdc, ...)`, `bus_alloc_resource` for IRQ 12,
`bus_setup_intr` with a filter). It turns on the IntelliMouse wheel when the
mouse has one, and its decoder handles the parts that go wrong: framing (bit 3
of the first byte, plus a timeout for a half-received packet), the 9-bit
signed motion, and overflowed packets. Reports go to a device-independent
layer (`mouse.c`) that a USB HID driver will feed too, which serves
**`/dev/mouse0`**: Linux evdev `struct input_event` records (`EV_REL`
`REL_X`/`REL_Y`/`REL_WHEEL`, `EV_KEY` `BTN_LEFT`/`RIGHT`/`MIDDLE`, `EV_SYN`),
down and right positive, blocking `read` and `poll`. One shared queue: a
second reader steals from the first.

**Verified by** the `psm` boot selftest, which injects packets through the
controller's loopback command (0xD3) so they arrive exactly as a mouse's would
(IRQ 12, the handler, the decoder, the queue) and checks every event: signs,
the ninth bit, Y flipped to screen coordinates, button changes only, a stray
byte, an overflow, the wheel, an abandoned packet, and the interrupt-driven
command path. `/bin/fbtest` then reads **real QEMU mouse input** injected
through the monitor by `guest_run.py`.

---

## 4. Memory management

| Feature | Status | Where |
|---|---|---|
| Physical frame allocator | ✅ | `kernel/mm/pmm.c` |
| 4-level paging, per-process address spaces, direct map of RAM | ✅ | `kernel/mm/paging.c` |
| `ioremap` for device memory, mapped uncached | ✅ | `paging.c` |
| Kernel heap | ✅ | `kernel/mm/kheap.c` |
| Kernel virtual-address allocator (no hand-picked, collision-prone constants) | ✅ | `kernel/mm/vmalloc.c` |
| Kernel stacks with guard pages | ✅ | `kernel/mm/kstack.c` |
| **UMA** slab allocator — FreeBSD's real `uma_core.c`, 6,042 lines | ✅ | `kernel/bsd/uma_vendor.c` |
| Low-memory reclaim (a kernel thread raises `vm_lowmem` so caches shrink) | ✅ | `kernel/bsd/kern_lowmem.c` |
| **W^X for user programs** (no page both writable and executable) | ✅ | `kernel/exec/elf.c` |
| **W^X for loaded kernel modules** (text read-exec, data no-exec, checked at boot) | ✅ | `kernel/driver/kldload.c` |
| `mmap` (anonymous), `munmap`, `mprotect` (honours PROT_EXEC), `mremap` (grow in place, shrink, move) | ✅ | `kernel/proc/syscall.c` |
| File-backed `mmap` | ❌ | returns `-ENODEV` |
| Demand paging / swap | ❌ | memory is committed up front |

---

## 5. Processes, threads and scheduling

### Processes ✅
A process (`process_t`, `kernel/include/process.h`) holds an address space, a
descriptor table, signal state, credentials, a working directory and a
personality (Linux or Windows). Supported:

- **fork**, **vfork** (the parent really is suspended until the child execs
  or exits) and **clone** (fork-shaped or thread-shaped).
- **execve** / **execveat** for ELF and PE images. The file's magic number
  picks the loader, and the personality is set from the same decision.
- **wait4** / **waitid**, including `WNOHANG`, correct `si_pid` zeroing, and
  zombies kept until reaped.
- **Process groups and sessions** (`setpgid`, `setsid`, `getsid`), with
  terminal signals delivered to the foreground group.
- **Credentials**: uid, gid, supplementary groups, umask (default **022**),
  inherited across fork and by threads.

### Signals ✅
`kernel/proc/signal.c`: `rt_sigaction`, `rt_sigprocmask`, `rt_sigsuspend`,
`rt_sigpending`, `kill`, `tgkill`, `pause`, restartable delivery and
`rt_sigreturn`. **`sigaltstack`** is supported, with correct nesting: a second
signal on the alternate stack stacks *below* the first instead of overwriting
it. Signal handlers are shared by a thread group and signal masks are
per-thread, as POSIX specifies.
- **Handlers run from the interrupt path too** (new 2026-09-27): a program
  that never makes a system call still gets its handler at the next timer
  tick (bash's `while :; do :; done` stops on Ctrl-C). The frame saves every
  register and the **FPU/SSE state**, and `rt_sigreturn` returns by `iretq`.
- **Temporary masks done right**: `rt_sigsuspend`, `pselect6` and `ppoll`
  deliver the signal that ends the wait under the temporary mask and restore
  the caller's afterwards (Linux's `TIF_RESTORE_SIGMASK`).
- **Job control**: SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU stop the whole process,
  SIGCONT resumes it, SIGKILL reaches a stopped one; the parent sees stops
  and continues through `wait4(WUNTRACED|WCONTINUED)` and `waitid`, and a
  death by signal is reported as `WIFSIGNALED`. `wait4` honours all four pid
  forms (pid, 0, -1, -pgid).

### POSIX threads ✅
`clone()` with the thread flags creates a task that **shares** the address
space, descriptors and signal handlers, with:
- **CLONE_SETTLS** (per-thread TLS via the FS base register),
- **CLONE_CHILD_CLEARTID + futex wake** (how `pthread_join` works),
- **exit** vs **exit_group** (one thread vs the whole program),
- **futex** (`kernel/proc/futex.c`) for musl's locking,
- automatic freeing of exited threads, and threads inheriting their creator's
  credentials (both fixed 2026-09-26; see §5 "Windows threads").

### Windows threads ✅ (new, 2026-09-26)
`CreateThread`, `ExitThread`, `WaitForSingleObject(hThread)`,
`GetExitCodeThread`, `GetCurrentThreadId` and `GetThreadId` all work, on real
NT system calls (`NtCreateThreadEx`, `NtTerminateThread`,
`NtQueryInformationThread`). Each Windows thread gets:
- **its own TEB**, so `GetCurrentThreadId` and `GetLastError` are per thread;
- **its own stack**, in a 1MB slot above an unmapped guard page;
- a **waitable Thread object** that becomes signalled, carrying the full 32-bit
  exit code, when the thread ends.

Threads start in **`ntdll!RtlUserThreadStart`**, as on Windows, so a thread
function can simply `return` its exit code. `ExitProcess` ends every thread in
the process.

Two long-standing bugs in the Linux-side thread code were found and fixed while
building this: threads used to run **as root** regardless of who created them,
and threads that exited were **never freed**, so a program could create only
about a dozen before hitting "resource unavailable".

**Thread-local storage ✅** (2026-09-27): `TlsAlloc`/`TlsFree`/`TlsGetValue`/
`TlsSetValue` (64 slots in the TEB plus 1024 expansion slots), fiber-local
storage (`FlsAlloc` with destructor callbacks, run on thread exit), and
**implicit TLS** - `__declspec(thread)` variables in `.tls` sections of the
program and of every DLL it loads, each thread getting its own copy of the
initialised template, plus TLS callbacks for `DLL_PROCESS_ATTACH`,
`DLL_THREAD_ATTACH/DETACH` and `DLL_PROCESS_DETACH`. `TlsFree` clears the slot
in every thread, as Windows does.

**Waiting on several objects ✅** (2026-09-27): `WaitForMultipleObjects`, both
wait-any (lowest index wins) and wait-all (all or nothing), plus `CreateEvent`,
`SetEvent`, `ResetEvent`, `CreateSemaphore` and `ReleaseSemaphore` (unnamed).

**Structured exception handling ✅** (2026-09-27): x64 table-based SEH. A
fault in a Windows program (access violation, divide by zero, breakpoint, ...)
becomes an exception the program handles itself: `__try`/`__except`/
`__finally`, `RaiseException`, vectored handlers, `SetUnhandledExceptionFilter`.
ntdll has the real unwinder (`RtlVirtualUnwind`, `RtlUnwindEx`,
`__C_specific_handler`); tested with clang-compiled `__try` code. An unhandled
exception ends the process with a report instead of a silent kill.

**APCs ✅** (2026-09-27): `QueueUserAPC`, and the alertable waits that run
them - `SleepEx`, `WaitForSingleObjectEx`, `WaitForMultipleObjectsEx`
(`WAIT_IO_COMPLETION`). Underneath, the kernel can hand a thread a full
`CONTEXT` on its own stack and `NtContinue` puts every register back.

**Mutexes ✅** (2026-09-27): `CreateMutexW`/`CreateMutexA` and
`ReleaseMutex`. A mutex whose owner thread dies holding it is **abandoned**:
released, and the next `WaitForSingleObject` gets `WAIT_ABANDONED` once.

**Suspension ✅** (2026-09-27): `CREATE_SUSPENDED`, `SuspendThread` and
`ResumeThread` (counted, up to 127), including a thread suspending itself
and one spinning in ring 3 on another CPU.

**`TerminateThread` ✅** (2026-09-27) on another thread of the process,
wherever it is: running on another CPU, blocked in a wait, or suspended.
Mutexes it held are abandoned.

**NT virtual memory ✅** (2026-10-03, ROADMAP 16(l)/14(d)): reserve, commit,
decommit, release; `VirtualProtect` with enforced READONLY / NOACCESS /
EXECUTE (DEP) and one-shot `PAGE_GUARD` (`STATUS_GUARD_PAGE_VIOLATION`);
`VirtualQuery` over allocations, images, stacks and free space. Commit is
eager (frames at commit time). Sections: `CreateFileMapping`/`MapViewOfFile`
(pagefile and file-backed, copy-on-write views, named, flushed to the file).

**KUSER_SHARED_DATA and the version ✅** (2026-10-03, ROADMAP 16(s)): the
shared page at `0x7FFE0000` (time, tick count, NT 10.0.19045, processor
features), the PEB's version fields, `GetVersion(Ex)`, `RtlGetVersion`,
`IsProcessorFeaturePresent`; `GetTickCount` reads the page.
`NtQuerySystemInformation` gained the time of day and the process list
(`SystemProcessInformation`).

**Keyed events and I/O completion ports ✅** (2026-10-03, ROADMAP 16(k)):
`NtCreateKeyedEvent`/`NtWaitForKeyedEvent`/`NtReleaseKeyedEvent` (a
rendezvous per key, the NULL handle being the global one) and completion
ports (`NtCreateIoCompletion`, `NtSetIoCompletion`,
`NtRemoveIoCompletion(Ex)`, `NtQueryIoCompletion`; kernel32's
`CreateIoCompletionPort`/`PostQueuedCompletionStatus`/
`GetQueuedCompletionStatus(Ex)`, alertable included). Not yet: a file
associated with a port (needs overlapped I/O), the concurrency limit.

**Named objects ✅** (2026-10-03, phase 2 / ROADMAP 16(k)): events,
semaphores and mutexes by name in `\BaseNamedObjects`. The kernel honours
`OBJ_OPENIF` on `NtCreateEvent`/`NtCreateSemaphore`/`NtCreateMutant` (the
existing object of the same type is opened, its create arguments ignored,
`STATUS_OBJECT_NAME_EXISTS`; another type's name is
`STATUS_OBJECT_TYPE_MISMATCH`) and has `NtOpenMutant`/`NtOpenSemaphore`
(NT syscalls `0x2A`/`0x2B`) beside `NtOpenEvent`. Names are NT's
**temporary** kind: the namespace entry goes when the last open instance
closes (`OB_FLAG_TEMPORARY` and `handle_count` in `kernel/include/object.h`),
so a single-instance program's mutex does not outlive it. The kernel32 side
(`CreateMutexW` with a name and `ERROR_ALREADY_EXISTS`, `OpenMutexW`,
`OpenEventW`, `OpenSemaphoreW`, A forms) is in Genesis-userland.

### Kernel threads ✅
`kernel/proc/kthread.c`: schedulable threads that run only in the kernel, used
by the taskqueue, low-memory reclaim, LinuxKPI workqueues and tests. Kernel
threads run on any CPU. `sleep(9)` really deschedules the caller, and that now
includes a process inside a system call, which is what makes a blocking TCP
connect work. **Kernel code is deliberately not preemptible** (the big kernel
lock covers it), so a kernel thread must yield or block.

### Scheduler ✅
**ULE**, the FreeBSD scheduler design (`kernel/proc/sched_ule.c`), behind a
small policy interface (`sched.c`):
- one shared queue (the process table), from which each CPU picks what it may
  run: READY and allowed by the thread's **affinity mask**, or already its own;
- an **interactivity score** from sleep-time vs run-time, so programs that
  mostly wait on a person are favoured over CPU hogs;
- **preemptive for user programs**: each CPU's timer tick forces a switch;
- CPU-time accounting that never runs backwards (`times(2)`,
  `CLOCK_PROCESS_CPUTIME_ID`).

**Limits** (raised 2026-10-03): `MAX_PROCESSES` is **256**, shared by every
process, thread, kernel thread and per-CPU idle thread; a Windows process
may have **128** threads; a process has **256** handles/descriptors
(`RLIMIT_NOFILE`). The scheduler's scans stop at the highest slot ever
used, so the table's size costs memory, not time per switch.

---

## 6. The Linux system-call interface

Genesis uses the **real Linux x86-64 syscall numbers** and invents none of its
own. Genesis-specific operations go through `prctl`, Linux's own extension
point. About **130** syscall numbers are dispatched, in these groups:

| Group | Calls |
|---|---|
| Files & I/O | `open`, `openat` (`O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY`, `O_CLOEXEC`), `read`, `write`, `readv`, `writev`, `pread64`, `pwrite64`, `lseek`, `close`, `dup`/`dup2`/`dup3`, `fcntl`, `ioctl`, `fsync`, `fdatasync`, `msync` |
| Metadata | `stat`, `fstat`, `lstat`, `newfstatat`, `statfs`, `fstatfs`, `access`, `faccessat`, `faccessat2`, `getdents64`, `readlinkat` |
| Namespace | `mkdir`, `mkdirat`, `rmdir`, `unlink`, `unlinkat` (incl. `AT_REMOVEDIR`), `rename`, `renameat`, `truncate`, `ftruncate`, `chdir`, `fchdir`, `getcwd` |
| Permissions | `chmod`, `fchmod`, `chown`, `fchown`, `umask` |
| Processes | `fork`, `vfork`, `clone`, `execve`, `execveat`, `exit`, `exit_group`, `wait4`, `waitid`, `getpid`, `getppid`, `gettid`, `kill`, `tgkill` |
| Identity | `getuid`/`geteuid`/`getgid`/`getegid`, `setuid`, `setgid`, `setresuid`/`getresuid`, `setresgid`/`getresgid`, `getgroups`/`setgroups`, `setpgid`/`getpgid`/`getpgrp`, `setsid`/`getsid` |
| Memory | `brk`, `mmap`, `munmap`, `mprotect`, `mremap`, `madvise` |
| Signals | `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `rt_sigsuspend`, `rt_sigpending`, `sigaltstack`, `pause` |
| Time | `clock_gettime`, `clock_getres`, `clock_nanosleep`, `nanosleep`, `gettimeofday`, `times` |
| Waiting | `poll`, `ppoll` (with a signal mask), `select`, `pselect6`, `futex`, `sched_yield`, `eventfd2`, `pipe`, `pipe2`, `socketpair` |
| Limits & usage | `getrlimit`, `setrlimit`, `prlimit64` (per process, inherited; `RLIMIT_NOFILE` enforced, `RLIMIT_STACK` is the real stack), `getrusage`, `times` (with reaped children's time) |
| CPUs | `sched_setaffinity`, `sched_getaffinity`, `getcpu` |
| Sockets | `socket`, `bind`, `connect`, `listen`, `accept`, `accept4`, `shutdown`, `sendto`, `recvfrom`, `sendmsg`, `recvmsg`, `getsockname`, `getpeername`, `setsockopt`, `getsockopt` |
| Misc | `uname`, `arch_prctl`, `set_tid_address`, `set_robust_list`, `getrandom`, `prctl`, `reboot` |

**Honest errors.** A call that can't be supported fails with the correct
errno rather than faking success. For example, `rseq` returns `-ENOSYS`. The socket calls translate at the boundary between the FreeBSD stack
and Linux programs: sockaddr layout, errno numbers (ECONNREFUSED is 111, not
BSD's 61), `MSG_` flags and socket-option numbers.

**Genesis `prctl` extensions:** `PR_GENESIS_GRANT_SUPREME`, `_REVOKE_SUPREME`
and `_QUERY_SUPREME`. See §9 "the supreme privilege".

---

## 7. Storage: disks, partitions and volumes

| Feature | Status | Notes |
|---|---|---|
| **ATA** disks | ✅ | PIO, polled (`kernel/dev/ata.c`) |
| **AHCI** SATA | ✅ | Native, polled, **DMA**; self-test does a real write/read round trip (`kernel/dev/ahci.c`) |
| NVMe | ❌ | |
| **MBR** partition tables | ✅ | `kernel/dev/part.c` |
| GPT | ❌ | |
| Block cache | ✅ | 64 × 4KB, keyed by (device, LBA) (`kernel/fs/bcache.c`) |
| **Page cache** | ✅ | 64 pages keyed by (volume, file, page), write-through, invalidated on every change (`kernel/fs/pcache.c`) |
| Volumes and **drive letters** | ✅ | Each partition becomes `\Device\HarddiskVolumeN`. The first mounted volume is `/` and `C:`; others become `D:`, `E:`… and mount at `/mnt/d`, `/mnt/e`… |
| **Surprise removal** | ✅ | Open files on a removed volume get `-ENODEV` rather than someone else's data; slots are retired, not reused unsafely |

---

## 8. Filesystems

All filesystems plug into one **VFS** (`kernel/fs/vfs.c`, `kernel/include/fs.h`)
through an `fs_ops_t` table: lookup, read, write, iterate, statfs, create,
truncate, mkdir, rmdir, unlink, rename, unmount, getacl, setacl, setowner. A
filesystem leaves a slot empty for what it can't do, and the VFS answers
honestly from that (a missing `write` slot means `access(W_OK)` says no).
There is a **mount table** with nested mounts; `rmdir` on a mount point is
refused with `-EBUSY`.

### FAT16 ✅ — the default root filesystem
`kernel/fs/fat.c`, `fatfs.c`.
- Full **read and write**: files grow, clusters are allocated, and the gap
  when writing past the end **reads back as zeroes** (including stale bytes in
  the old last cluster).
- `O_CREAT`/`O_EXCL`/`O_TRUNC`/`O_APPEND`, `truncate`, `ftruncate`, `statfs`
  (real free space, not cached).
- `mkdir`, `rmdir`, `unlink`, `rename`, **updating every FAT copy**.
- Verified against the reference tools: the host runs `fsck.fat` on images
  Genesis wrote and finds no errors.
- **Limits:** 8.3 names only. FAT has no owners or permission bits, so it
  presents itself as root-owned 0755. There are no symlinks.

### gnfs ✅ — Genesis's own copy-on-write filesystem (version 2)
`kernel/gnfs/`, BSD-2-Clause, new code; design notes in
`kernel/include/gnfs_layout.h`. (ZFS was removed on 2026-09-26; gnfs replaced
it everywhere, including the ACL test fixtures.)
- **Crash-safe by construction:** a ring of root records, copy-on-write for
  every block, and ping-pong regions for the free-space bitmap and object
  table, committed **incrementally** (only what changed). A torn write leaves
  the previous state mountable.
- An object table sized to the volume; freed object numbers are reused.
- Files up to about **1GB** through direct, indirect and double-indirect
  block maps; directories that grow across blocks; create, read, write (gaps
  read as zero), truncate (a shrink really forgets the old bytes), **rename**,
  mkdir, rmdir, unlink, statfs.
- **Snapshots**: `mkdir /.snapshots/NAME` takes one and `rmdir` deletes it; a
  snapshot is a read-only view of the whole volume as it was, and the blocks
  it holds are never reused until it is gone.
- **Full ACL support**, stored per object in its own COW block, with
  inheritance on create, chmod, chown, the creator owning new objects, and
  setgid/sticky directories. See §9.
- Formatted by `tools/mkgnfs.c`, which uses the same formatting code the kernel
  does.
- **Limits:** one dataset per volume (no named datasets), no symlinks.

### Other file-like objects ✅
**Pipes** (with SIGPIPE/EPIPE), **eventfd**, **socketpair** (two crossed
pipes), **sockets**, `/dev/console`, `/dev/null`, all behind the same object
vtable, so `read`/`write`/`poll` work on all of them.

---

## 9. Security and permissions

Genesis's permission system is one of its most distinctive parts.

### One ACL model, two views ✅
Every object's permissions are an **NFSv4 ACL**, the same model ZFS uses and
the same one Windows uses (NFSv4 took it from NT, and the access-mask bits are
identical). There's no separate "Unix permissions" system to keep in sync:
- the **POSIX mode** (`rwxr-xr-x`) is a *projection* computed from the ACL
  (OpenZFS's own `zfs_mode_compute`, reproduced);
- the **Windows view** is a real self-relative `SECURITY_DESCRIPTOR` built from
  the same ACL (`NtQuerySecurityObject`, `kernel/fs/ntsec.c`), with the ACE
  flag bits translated correctly between the two formats.

Every access decision goes through **one function**, `fs_access`, so `open`,
`access(2)` and every mutation get the same answer. That includes the rule that
an ACL entry naming a specific user can grant what no mode bits could. For
example, a mode-0600 file can still be readable by one other named user.

### What is enforced ✅
| Rule | Detail |
|---|---|
| Open/read/write/execute | From the ACL, in NFSv4 order: the first entry to mention a permission decides it |
| **Descriptor access mode** | Writing through an `O_RDONLY` descriptor is refused |
| **Creating** a file/dir | Needs write + search on the parent. An existing name returns `-EEXIST` first, so opening your own existing file never needs the directory's write bit |
| **Deleting / renaming** | Delete permission on the object, or delete-child + search on its parent (NFSv4's either/or rule; POSIX's rule for plain files) |
| **Sticky directories** (`/tmp`-style) | Only the entry's owner, the directory's owner, or a supreme caller may remove entries (`-EPERM`) |
| **chmod** | Owner or supreme; sets rwx **and** setuid/setgid/sticky; setgid is silently dropped if you're not in the file's group (as Linux does); `-EPERM` for non-owners |
| **chown** | Only root/supreme can give a file away. The Windows "take ownership" right lets you make only *yourself* the owner. An owner may change the group to one they belong to |
| **umask** | Applied by `open(O_CREAT)` and `mkdir`; default 022 |
| **New object ownership** | The creator's uid and gid, or the directory's group in a **setgid directory** (and new subdirectories inherit setgid) |
| **ACL inheritance** | Real NT/NFSv4 file-inherit / directory-inherit / no-propagate semantics |
| **Read-only media** | Refused with `-EROFS` before any permission question |
| **Group membership** | Supplementary groups count; only root can change them |

### The supreme privilege ✅
Genesis fuses root, `NT AUTHORITY\SYSTEM` and `TrustedInstaller` into one
exemption. **Root, plus exactly one user that root designates**, bypass every
ACL check, including explicit deny entries. It is granted and revoked through
`prctl(PR_GENESIS_GRANT_SUPREME)`, which only root may call; a non-root attempt
gets `-EPERM`, and a test checks that from a real user program. Every grant and
revoke is logged.

### Memory protections ✅
W^X for programs and for kernel modules, NX everywhere, guard pages under every
kernel stack and every Windows thread stack.

### Not yet ❌
Clearing setuid/setgid on chown (it only matters once `exec` honours setuid,
which it doesn't yet), auditing ACEs (SACLs), per-handle access masks the NT
way (Genesis checks access on the open file, the POSIX way), and security
descriptors on named kernel objects.

---

## 10. The Windows (NT) personality

### PE loader ✅
`kernel/exec/pe.c` loads **real PE32+ executables and DLLs**:
- relocations when an image can't load at its preferred base;
- **import resolution through dependency chains** (a program importing only
  `kernel32.dll` gets `ntdll.dll` pulled in as kernel32's dependency);
- **ordinal-only imports** (with the ordinal-base subtraction) and
  **forwarded exports** (`kernel32.K32CurrentTeb → ntdll.NtCurrentTeb`);
- DLLs are loaded from `/wsr/System32/`.

### What the kernel builds for a Windows program ✅
Exactly as Windows does, before the first instruction runs
(`kernel/exec/ntproc.c`, `kernel/include/teb.h`):
- a **TEB**: GS:0x30 is its own address, GS:0x60 the PEB, GS:0x68 the last
  error, and the stack bounds (one TEB per thread);
- a **PEB**;
- **`RTL_USER_PROCESS_PARAMETERS`**: the command line, environment, current
  directory, image path, and standard handles **inherited from the shell**
  (so redirection like `hello.exe > out.txt` works).

Every structure offset is checked at build time against the documented ABI.

### NT system calls ✅ (31)
| Area | Calls |
|---|---|
| Process | `NtTerminateProcess`, `NtQueryInformationProcess` (basic info, affinity, times, priority class), `NtSetInformationProcess` (affinity, priority class) |
| Threads | `NtCreateThreadEx`, `NtTerminateThread` (self or another thread), `NtSuspendThread`, `NtResumeThread`, `NtQueryInformationThread` (basic, times, priorities, group affinity, ideal processor), `NtSetInformationThread` (affinity, group affinity, ideal processor, priorities) |
| System | `NtQuerySystemInformation(Ex)` (basic info, processor info, per-CPU performance, logical-processor information in both forms) |
| Scheduling & time | `NtYieldExecution`, `NtDelayExecution`, `NtGetCurrentProcessorNumber(Ex)`, `NtQueryPerformanceCounter` (the calibrated TSC), `NtQuerySystemTime` |
| Files | `NtOpenFile`, `NtReadFile`, `NtWriteFile`, `NtClose` |
| Memory | `NtAllocateVirtualMemory` |
| Console | `NtDisplayString` |
| Events | `NtCreateEvent`, `NtOpenEvent`, `NtSetEvent`, `NtResetEvent` |
| Semaphores | `NtCreateSemaphore`, `NtReleaseSemaphore` |
| Mutexes | `NtCreateMutant`, `NtReleaseMutant` |
| Waiting | `NtWaitForSingleObject` (infinite, relative timeout, poll) |
| Security | `NtQuerySecurityObject` |

The numbers live in one header (`kernel/include/nt.h`); ntdll's stubs are
**generated from it**, so the DLL and the kernel can't disagree.

### ntdll.dll ✅ (clean-room, `src/ntdll/`)
The system-call stubs above, plus `NtCurrentTeb`, `RtlInitUnicodeString`,
`RtlDosPathNameToNtPathName_U`, `RtlAllocateHeap`/`RtlFreeHeap` (thread-safe:
the heap is locked, because threads of one process now run concurrently),
`LdrInitializeThunk`, `RtlUserThreadStart`, `RtlExitUserThread`, and the
user-mode synchronisation built for real parallelism (`src/ntdll/sync.c`):
**critical sections** (spin, then block on an auto-reset event created on
first contention, as NT does), **SRW locks** and **condition variables** (a
short spin, then sleep in the kernel on the lock word itself through
`RtlWaitOnAddress` over `NtWaitForAlertByThreadId` - how Windows 8 and later
build them), **`RtlWaitOnAddress`/`RtlWakeAddressSingle/All`**,
**interlocked SLists**, `RtlGetCurrentProcessorNumber(Ex)`,
`RtlQueryPerformanceCounter/Frequency`, the TLS machinery (TLS callbacks,
`RtlFlsAlloc` family, `LdrShutdownProcess`).

### kernel32.dll 🟡 (clean-room, `src/kernel32/`)
| Area | Exports |
|---|---|
| Errors | `GetLastError`, `SetLastError` (NTSTATUS → Win32 error mapping) |
| Files | `GetStdHandle`, `CreateFileW`/`A`, `ReadFile`, `WriteFile`, `CloseHandle` |
| Process | `ExitProcess`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetCommandLineW`/`A`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `GetCurrentDirectoryW`, `GetModuleHandleW` |
| Threads | `CreateThread`, `ExitThread`, `GetCurrentThread`, `GetCurrentThreadId`, `GetThreadId`, `GetExitCodeThread`, `WaitForSingleObject`, `SwitchToThread`, `Sleep`, `SleepEx` |
| Processors | `GetSystemInfo`, `GetNativeSystemInfo`, `GetActiveProcessorCount`, `GetMaximumProcessorCount`, the group counts, `GetCurrentProcessorNumber(Ex)`, `GetLogicalProcessorInformation(Ex)` |
| Affinity & priority | `Get/SetProcessAffinityMask`, `SetThreadAffinityMask`, `Set/GetThreadGroupAffinity`, `SetThreadIdealProcessor(Ex)`, `GetThreadIdealProcessorEx`, `Set/GetThreadPriority`, `Set/GetPriorityClass`, `GetProcessTimes`, `GetThreadTimes` |
| Synchronisation | `Initialize/Enter/TryEnter/Leave/DeleteCriticalSection` (+ `AndSpinCount`, `Ex`), the SRW lock family, `InitializeConditionVariable`, `Wake(All)ConditionVariable`, `SleepConditionVariableCS/SRW`, `WaitOnAddress`, `WakeByAddressSingle/All`, the SList family. Most are **forwarders** into ntdll, as on Windows |
| Thread-local storage | `TlsAlloc`, `TlsFree`, `TlsGetValue`, `TlsSetValue`, `FlsAlloc`, `FlsFree`, `FlsGetValue`, `FlsSetValue` |
| Time | `QueryPerformanceCounter/Frequency`, `GetTickCount(64)`, `GetSystemTimeAsFileTime`, `GetSystemTimePreciseAsFileTime` |
| Memory | `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `VirtualAlloc`, `VirtualFree` |

### Windows test programs in `/bin` ✅
| Program | Proves |
|---|---|
| `hello.exe` | The loader and the native ntdll interface |
| `hand.exe` | A PE assembled by hand, not by a linker |
| `k32.exe` | kernel32-only imports two levels deep; command line and parameters |
| `sync.exe` | Events, semaphores and mutexes by name, through ntdll (29 checks) |
| `thr.exe` | Win32 threads end to end (16 checks) |
| `smp.exe` | The multiprocessor from Win32: processor queries, affinity moves, parallel threads, critical sections/SRW/condition variables/SLists and the heap under real contention, `WaitOnAddress`, and SRW waiters that really sleep (61 checks) |
| `tls.exe` | Thread-local storage: `TlsAlloc` family including the expansion slots, `FlsAlloc` with callbacks, and implicit `.tls` sections with TLS callbacks for process/thread attach and detach (24 checks) |

### Not yet ❌
GUI (the precompiled user32/gdi32 and the win32k.sys support under them),
the registry, COM, `NtCreateProcess` (a Windows program can't start another
one yet), and most of kernel32. See §20.

---

## 11. The NT object manager and namespace

`kernel/obj/`. Genesis's single naming system is **NT's**; the POSIX names are
layered on top of it, not kept beside it. `/dev/console` and `\??\CON` reach the
same object.

- **Objects** (`object.c`): reference-counted, behind a type vtable (read,
  write, getdents, poll, wait, signal, destroy). Files, pipes, sockets, eventfds
  and devices are all objects, and so are Windows handles.
- **The namespace** (`ns.c`): directories, objects and symbolic links,
  backslash-separated and case-insensitive, with NT's **unparsed-remainder**
  rule (`\Device\HarddiskVolume1\bin\ls` goes to the volume, which parses
  `\bin\ls`).
- **Standard directories**: `\Device`, `\??` (drive letters `C:`/`D:`…, plus
  `CON`, `NUL`, `sda`…), `\BaseNamedObjects`, `\KernelObjects`, `\Sessions`,
  and **`\ObjectTypes`**, which lists every object type in use, including
  `Thread`.
- **Dispatcher objects** (`dispatch.c`): **events** (notification and
  synchronisation), **semaphores** (a release past the limit is *refused*,
  never clamped), **mutants** (recursive, owner-checked) and **threads**
  (signalled on exit). Waits really block, and objects can be **named** under
  `\BaseNamedObjects`. Creating a name that already exists is an error, not a
  silent open of someone else's mutex.
- **ioctl** dispatches to devices through the object, so `isatty()` on a
  redirected stdin correctly says no.

---

## 12. Linux userland: ELF, the dynamic linker, musl

| Feature | Status | Notes |
|---|---|---|
| Static ELF executables | ✅ | `kernel/exec/elf.c`, with per-page W^X computed across all segments |
| **Dynamic ELF** | ✅ | Genesis's own dynamic linker, `ld-gen.so` (`src/rtld/`): load order, symbol scope and relocations, resolving everything at startup (no lazy binding) |
| Auxiliary vector | ✅ | `AT_PHDR`, `AT_ENTRY`, `AT_PAGESZ`, … |
| TLS (initial-exec) | ✅ | Through `arch_prctl`/FS base |
| **musl** C library | ✅ | `mhello` and `ls` are ordinary C programs linked against real musl |
| Shell | 🟡 | **GNTbash** (GNU bash 5.3 + a switchable Windows layer; Genesis-userland's `third_party/GNTbash`, static musl) runs interactively as `/bin/bash`: readline editing, history, tab completion, Ctrl-C. Not yet the login shell (BusyBox ash is), and there are no utilities to run (no `cat`/`head`) - ROADMAP item 15 |
| vDSO | ❌ | |

Programs in `/bin`: `bash` (GNU bash), `busybox` (shell), `ls` (musl),
`mhello` (musl), `gtrace` (runs a command with the kernel's syscall trace on),
`hello` (freestanding), `mkprobe`, `systest`, `verif`, plus the Windows ones
in §10.

**Filesystem layout.** A standard Linux tree plus **`/wsr`**, the "Windows
system repository", holding Genesis's rewritten NT components and a Windows
tree (`/wsr/System32`, `/wsr/Windows/System32/Drivers`).

---

## 13. The three driver models

Genesis runs drivers written for three different operating systems side by
side, each picked for what it's good at (the reasoning is in ROADMAP item 12b).
The matching engine underneath all three is Newbus-shaped.

### FreeBSD Newbus ✅ (`kernel/driver/bus.c`, `newbus_compat.c`)
- devclasses, probe/attach with FreeBSD's full probe-priority scale, **custom
  interface methods** (KOBJ-style dispatch), **resource allocation** with
  overlap detection and shareable resources, **multi-pass attach ordering**,
  **devclass inheritance**, device names/units (`re0`), a **hints** file in
  FreeBSD's format, and `BUS_PROBE_NOWILDCARD`;
- bus_space and bus_dma, per-device sysctl trees (`dev.re.0.*`), interrupt
  setup/teardown, **`bus_bind_intr`** (really moves the line to a CPU) and
  `bus_describe_intr`;
- **the multiprocessor KPI** (`kernel/bsd/kern_smp.c`): `mp_ncpus`, `curcpu`,
  `CPU_FOREACH` over the CPUs that exist, `all_cpus`, `smp_rendezvous(_cpus)`
  with real setup/action/teardown barriers, `DPCPU_*` per-CPU variables, and a
  `sched_bind` that really moves the calling thread.

**The proof:** `if_re.ko` is FreeBSD's **own, unmodified** `if_re.c` (4,295
lines). It loads at runtime, attaches to QEMU's RTL8139C+, drives its PHY over
bit-banged MDIO, reads the real MAC address and carries the network traffic in
§14. `if_rl.ko` (FreeBSD's `if_rl.c`) loads too and correctly *declines* the
same chip, as its own matching logic says it should.

### Linux (LinuxKPI source compatibility) ✅ (`kernel/driver/lkpi.c`, `kernel/include/linux/`)
Linux driver **source** compiles against Genesis's headers: `struct
pci_driver`, `module_pci_driver`, `pr_info`, `kzalloc`, `ioremap`,
`readl`/`writel`, spinlocks and mutexes, lists, atomics, bitops, delays and
jiffies. `lkpi_ahci.ko` is ordinary Linux-style driver source that reads a
real AHCI controller's registers through a memory BAR.

**The multiprocessor surface** (`kernel/driver/lkpi_smp.c`):
`smp_processor_id`, `get_cpu`/`put_cpu`, cpumasks and the `for_each_*_cpu`
iterators, `smp_call_function(_single/_many/_any)`, `on_each_cpu`,
`preempt_*`, `local_irq_*`; **per-CPU variables done Linux's way**
(`DEFINE_PER_CPU`, `per_cpu`, `this_cpu_*`, `alloc_percpu`: CPU n's copy is at
`__per_cpu_offset[n]` from CPU 0's, and a *loaded module's* per-CPU section is
placed and replicated by the module loader, checked by `lkpi_pcpu.ko`);
**kernel threads** with the Linux lifecycle (`kthread_run`,
`kthread_create_on_cpu`, `kthread_stop`, `wake_up_process`, the
`set_current_state`/`schedule` sleep protocol); **completions**; and
**workqueues** with a bound worker per CPU, delayed work, flush and cancel;
`late_initcall`. The Linux in-kernel
binary interface isn't stable even on Linux, so compatibility is at the source
level; that was a deliberate decision.

### Windows WDM ✅ (`kernel/driver/wdm.c`, `sysload.c`, `kernel/exec/ntoskrnl_exports.c`)
- `.sys` files are loaded as **kernel-mode PE images** from
  `/wsr/Windows/System32/Drivers/`;
- a clean-room **ntoskrnl export surface**: `IoCreateDevice`,
  `IoDeleteDevice`, `IoCompleteRequest`, `IoGetCurrentIrpStackLocation`,
  `ExAllocatePool2`, `ExFreePool`, `KeAcquire/ReleaseSpinLock`,
  `KeRaise/LowerIrql`, `KeGetCurrentIrql`, `DbgPrint`;
- **real IRP dispatch** with **device stacking** (a filter driver on top of a
  function driver) and completion routines; **IRQL mapped onto the LAPIC's
  task-priority register**. The self-test sends an IRP through filter →
  function → completion.
- **the multiprocessor surface** (`kernel/driver/wdm_smp.c`, all exported to
  loaded drivers): `KeGetCurrentProcessorNumber(Ex)`,
  `KeQueryActiveProcessor*`, the group queries, `KeNumberProcessors`,
  `KeIpiGenericCall`, `KeGenericCallDpc` with its barrier, **DPC objects** with
  per-CPU queues and `KeSetTargetProcessorDpc`, `KeSetSystemAffinityThread(Ex)`
  and its revert, the DPC-level and **in-stack queued** spin locks,
  `ExInterlocked*List`, `KeStallExecutionProcessor` and
  `KeQueryPerformanceCounter`.

### Loading and unloading ✅ (`kernel/driver/kldload.c`)
- Modules are loaded from **four directories**: `/boot/kernel`,
  `/boot/modules`, `/lib/modules` and `/wsr/Windows/System32/Drivers`.
- A real **ET_REL (relocatable ELF) loader** resolves undefined symbols against
  the kernel's own symbol table.
- **Unload** is supported: `module_exit` runs, the drivers come off the bus,
  and the module is refused if any of five registries (driver tables, IRQ
  handlers, callouts, taskqueue, sysctl) still points into it. Reloading lands
  at the same address.
- Module text is W^X, checked at boot by reading the page tables back.

**Not yet:** PHY drivers (link state is read from standard registers, which
works on QEMU), and walking eventhandler lists on unload.

---

## 14. Networking

The protocol stack is **FreeBSD's own code**, vendored file for file:
`net/if.c`, the whole `net/route` tree, `netinet/ip_*.c`, `in.c`, `in_pcb.c`,
`if_ether.c`, `igmp.c`, `udp_usrreq.c`, `raw_ip.c`, the socket layer
`uipc_socket.c`/`uipc_sockbuf.c`, and ~250 headers. Nothing in the protocol
layer is hand-written.

| Feature | Status |
|---|---|
| IPv4, ICMP (ping replies), ARP, UDP | ✅ verified end to end at boot |
| **TCP** | ✅ FreeBSD's own `tcp_input/output/subr/timer/usrreq/reass/sack/syncache/hostcache/timewait/ecn` and NewReno congestion control, vendored whole |
| **Loopback** | ✅ `lo0` at `127.0.0.1/8` (FreeBSD's `if_loop.c`); packets a host sends itself go through a real deferred netisr queue |
| Routing table, default route, `SIOCAIFADDR` address setup | ✅ |
| **Sockets from user programs**: `socket`, `bind`, `connect` (blocking and non-blocking), `listen`, `accept4`, `shutdown`, `send*`/`recv*` including `sendmsg`/`recvmsg`, socket options, and plain `read`/`write` | ✅ systest runs TCP over lo0: handshake, data both ways, `MSG_PEEK`/`MSG_WAITALL`, half-close, `ECONNREFUSED`, non-blocking connect with `SO_ERROR` |
| `poll` on sockets | ✅ listeners report `POLLIN` for a waiting connection; connecting sockets report `POLLOUT` when done |
| NIC | ✅ RTL8139C+ via the unmodified FreeBSD `if_re` driver |
| **DHCP** | ✅ `kernel/bsd/dhcp.c`: DISCOVER / OFFER / REQUEST / ACK at boot; address, netmask, default route and DNS server from the lease; a `dhclient` kernel thread renews at T1 (and starts over if the server refuses). Falls back to static `10.0.2.15/24` if nothing answers. Checked on QEMU's default subnet and on a different one (`GENESIS_NET=10.0.9.0/24` leases `10.0.9.15`) |
| IPv6 | ❌ |

---

## 15. Vendored FreeBSD kernel infrastructure

These pieces came in because the vendored network stack and drivers need the
real thing, not a stub (`kernel/bsd/`):

- **SYSINIT** (53 initializers found through linker sets) and
  **EVENTHANDLER**, both reported at boot;
- **sysctl**, a real MIB tree readable by name, plus per-device trees;
- **sleep(9)** (`tsleep`/`msleep`/`wakeup`), **condition variables**, a real
  **taskqueue** backed by a kernel thread, and the **callout** timer wheel;
- **mbufs**, **SMR** (safe memory reclamation), per-CPU **counters**, the
  kernel environment and `log()`, the `sys/time.h` clock family;
- **locks**: mutexes, rwlocks and sx over Genesis's own `mtx` implementation
  (`kernel/lib/mtx.c`), which has bounded-spin deadlock reports and held-lock
  dumps, and passes a four-CPU contention test with no lost increments;
- **modules**: `DECLARE_MODULE` delivers `MOD_LOAD` at boot (that is how
  NewReno registers itself).

---

## 16. Debugging aids

- **ksyms**: the kernel's own symbol table is embedded and readable at runtime
  (`kernel/lib/ksyms.c`).
- **backtrace** with symbol names (`kernel/lib/backtrace.c`).
- **kprintf** with field widths and colours, mirrored to serial.
- **NT syscall trace**: failing NT calls print their number and status.
- **Lock diagnostics**: deadlock reports and held-lock dumps.
- **Ctrl-T** on the console: every task (state, CPU, what it waits on) and
  every CPU's current thread. `tools/guest_dump.py` runs commands in the guest
  and presses it.
- `build.py debug` starts QEMU paused with a GDB stub.

---

## 17. How Genesis is tested

Four layers, and every feature above lives in at least one:

| Layer | What it is | Runs |
|---|---|---|
| **Host suite** (`tests/host/run.sh`) | Kernel code (VFS, FAT, gnfs, ACLs, bcache, PE, paths, volumes, …) compiled natively and tested directly, plus a byte-for-byte check of the gnfs ACL fixture and staged-tree name collisions | On the build machine |
| **Boot self-tests** | 25+ checks that run every boot: the framebuffer and the PS/2 mouse, SMP TLB shootdown, the WDM, LinuxKPI and FreeBSD multiprocessor APIs, interrupt migration, IOAPIC, IDT, IRQ, MSI, locks on 4 CPUs, ULE, kernel threads, condvars, taskqueue, callout, mbuf, bus, W^X, module unload, WDM IRPs, AHCI DMA, page cache, low memory, dispatcher objects, ACL privilege, network ARP/ICMP/UDP | Inside the kernel |
| **systest** (`src/systest.c`) | 510 checks of the syscall interface from a real user program, deliberately without libc so errnos aren't hidden: including TCP over lo0, gnfs v2, and threads running on two CPUs at once. Permission checks run in child processes as real uids | Inside the booted machine |
| **verif** (`src/verif.c`) | 160 checks plus the bug postmortems and the "needs a human" queue | Inside the booted machine |
| **Windows programs** | `sync.exe` (29), `thr.exe` (16), `smp.exe` (53), `k32.exe`, `hello.exe`, `hand.exe` | Inside the booted machine |
| **fbtest** (`src/fbtest.c`) | 52 checks of `/dev/fb0` and `/dev/mouse0` from ring 3, freestanding like systest: draws a picture through `mmap` and checks it back through the mapping, `read(2)`, a second mapping and a forked child; the mmap refusals; then mouse events from real (monitor-injected) QEMU input, through `poll` and through a blocking `read` | Inside the booted machine |

**`tools/guest_run.py`** boots the full machine (4 CPUs, FAT root, a fresh
gnfs volume and the gnfs ACL fixture, AHCI, the NIC), types commands into the
shell over the serial port, and collects each program's `N passed, M failed`
tally (verif, systest, thr.exe, smp.exe, tls.exe and fbtest by default).
For fbtest it also moves and clicks QEMU's mouse through the monitor
(`mouse_move`, `mouse_button`) when the program prints its `MOUSE-WAIT`
lines. `GENESIS_SMP=1` runs
it on one CPU. The whole run takes about ten minutes:

```
wsl.exe -d Debian -- bash -lc "cd '/mnt/c/Users/kevin/code/Genesis/Genesis' && python3 tools/guest_run.py > build/guest.txt 2>&1"
```

**The rules the tests follow** (ROADMAP's "standing instruction"):
- a test that explains away its own zero isn't a test, so measure a control;
- a negative-only test can't tell enforcement from blanket refusal, so assert
  the positive case too;
- a new check is confirmed by **breaking the code on purpose** and watching it
  fail (a "mutation"), because a check that passes against broken code is a
  green light that means nothing.

---

## 18. Building and running

Development happens on Windows with the toolchain inside **WSL Debian**, which
points at this same working copy.

| Command (inside WSL, from the repo root) | Does |
|---|---|
| `python3 build.py image` | Build the kernel and the bootable `build/os.img` |
| `python3 build.py disk` | Build the FAT16 data disk from Genesis-userland's `root/` (`GENESIS_USERLAND`, default `../Genesis-userland`) plus `modules/root` (kept if it exists) |
| `python3 build.py run` | Boot the full machine in QEMU (window + serial console) |
| `python3 build.py debug` | The same, paused, with a GDB stub |
| `python3 build.py usb` / `usbrun` | Build / test the single-disk bare-metal image |
| `python3 build.py test` | The kernel heap allocator's native test |
| `python3 build.py clean` | Remove build products, keeping `disk.img` |
| `bash tests/host/run.sh` | The host test suite |
| `python3 tools/guest_run.py` | Boot and run every ring-3 suite: verif, systest, the Windows suites, mix/elfmix, fbtest and bashtest (`GENESIS_SMP=N` picks the CPU count, default 4) |
| `python3 tools/guest_sh.py -f FILE` | Boot and type FILE's lines into the guest shell (`@wait:REGEX` waits for output) |
| `python3 tools/guest_dump.py CMD...` | Run commands, then press Ctrl-T and print the task dump (for a guest that hangs) |
| `sh modules/build.sh` | Build the loadable kernel modules into `modules/root` |
| `sh build.sh` *(in Genesis-userland)* | Build every user program, DLL, GNTbash and GNTlibc into its `root/` |

`GENESIS_VBE=WxH` (build time) caps the graphics mode the bootloader picks;
`GENESIS_VBE=off` keeps VGA text mode.

Toolchain: gcc, nasm, ld, QEMU, mkfs.fat, **MinGW-w64** (Windows programs),
**musl-gcc** (musl programs). Commit history is in git on branch `main`.

---

## 19. Known limitations

Collected in one place so nobody has to discover them the hard way:

- **The kernel runs on one CPU at a time** (the big kernel lock); programs
  run on all of them. System-call-heavy workloads do not scale with CPUs;
  compute does.
- **64 process slots in total**, shared by processes, threads, kernel threads
  and the per-CPU idle threads.
- **Kernel code is not preemptible**; a kernel loop that doesn't yield holds
  the big lock and stalls every CPU's system calls.
- **No window system**: a framebuffer, a console drawn into it and a mouse;
  no mode switching after boot, and nothing yet above the raw pixels.
- **No IPv6.**
- gnfs has one dataset per volume; there are no symlinks on any filesystem;
  FAT is 8.3-only.
- **No file-backed mmap, no swap, no demand paging.** Devices (`/dev/fb0`)
  can be mapped; files cannot.
- **No USB** (no controller or HID drivers); input is PS/2 or serial.
- **No NVMe, no GPT**; ATA is PIO-only.
- **BusyBox has no applets**, just the shell - so bash has no `cat`, `head`
  or `ls -l` to run either.
- **Loading a large binary is slow under emulation**: exec reads the whole
  file through polled ATA PIO, about 25 seconds for bash's 1.4MB under TCG.
- **Windows**: no registry, COM, child processes, or GUI DLL support yet;
  kernel32 is a small subset.
- **Clock resolution is one timer tick.**
- `exec` doesn't honour setuid/setgid bits.
- A system call interrupted by a job-control stop returns `-EINTR` after
  SIGCONT rather than restarting; the orphaned-process-group rule for
  SIGTSTP/SIGTTIN/SIGTTOU is not applied.

---

## 20. The road to the goal

The destination is a **daily-drivable desktop running the Windows 7 desktop**,
running real Win32 programs. The key decision is made: Genesis does **not**
write GUI DLLs or drivers from scratch. It runs the **precompiled** ones —
Microsoft's own `user32`/`gdi32`/`comctl32`/`shell32` and the `.sys` drivers
under them, taken from the user's own Windows 7 install — and writes the
**support** they need: the loader, the NT system calls, and the kernel-mode
export surface (`ntoskrnl`/`hal` and the per-family port libraries) that
precompiled drivers, `win32k.sys` included, import. A driver whose source
exists (FreeBSD, Linux) is ported and modified rather than rewritten.
`ntdll` and `kernel32` stay Genesis's own: they are the boundary between the
precompiled binaries and this kernel.

The work is organized into **four phases** (THE PLAN in `ROADMAP.md`):

| Phase | What | ROADMAP | Status |
|---|---|---|---|
| 1 | **Run GNU bash** as the login shell (readline, job control, its own test suite) | item 15 | 🟡 bash 5.3 runs interactively; select, rlimits, termios, job-control stops done; utilities, /tmp, /proc, symlinks and long names remain |
| 2 | **A modern kernel**: the feature set of current Linux and of NT 10.0 (Windows 10/11) — syscalls, demand paging and a page cache, namespaces/cgroups, tmpfs/procfs/ext4, UEFI/ACPI/NVMe/USB, IPv6; NT's I/O manager, registry, tokens, ALPC, completion ports, and the ntoskrnl surface precompiled drivers import | item 16 (+ 4, 6, 9, 12b, 13, 14 kernel halves) | 🟡 a large base exists; the inventory is open |
| 3 | **bash understands Windows**: drive-letter paths, `.exe`/`.bat` by bare name, Windows command lines and environment for PE children, CRLF scripts, NT exit codes, ^C as a console event | item 17 | 🟡 [GNTbash](https://github.com/bubba510kevin/GNTbash): paths, PATHEXT, PE argv/env, CRLF done; exit codes, console events and quoting need kernel work |
| 4 | **The libraries**: non-GUI DLLs written by Genesis (loader, kernel32/kernelbase, C runtimes, advapi32, ws2_32, rpcrt4, COM); GUI DLLs and Linux GUI stacks **taken**, never written; Linux `.so` files from upstream | item 18 (+ 14 (e)-(o)) | 🟡 ntdll and kernel32 subsets; [GNTlibc](https://github.com/bubba510kevin/GNTlibc) brings upstream musl in as `libc.so` for `.so` files that use libc |

ROADMAP item 14's dependency-ordered list (its letters now fall under phases 2
and 4):

| Step | What | Status |
|---|---|---|
| (a) | **Full multithreading** | 🟡 POSIX and Win32 threads on every CPU, TLS, suspend/resume/terminate, named mutexes, 256 threads machine-wide |
| (b) | Dispatcher objects completed: waits blocking threads, APCs | ✅ |
| (c) | Structured exception handling (x64 table-based) | ✅ C `__try`; C++ exceptions and stack overflow remain |
| (d) | NT memory model: VirtualAlloc states, Section objects, a full PEB | ❌ |
| (e) | The loader: `LdrLoadDll`/`GetProcAddress` for real | 🟡 `LoadLibrary`/`GetProcAddress` at run time (DLLs and .so files); no search path, API sets or SxS |
| (f) | kernel32, completed | 🟡 |
| (g) | advapi32 (registry, security APIs) | ❌ |
| (h) | The display: a VESA/VBE framebuffer (console), then precompiled display drivers under win32k | 🟡 framebuffer set at boot, `/dev/fb0` with mmap, console drawn into it; the Windows display driver path doesn't exist |
| (i) | gdi32 (precompiled; win32k's NtGdi side underneath) | ❌ |
| (j) | user32 (precompiled; win32k's NtUser side) and input | 🟡 the PS/2 mouse driver and `/dev/mouse0` exist; user32 support doesn't |
| (k) | COM/OLE | ❌ |
| (l) | RPC | ❌ |
| (m) | comctl32 (precompiled, v5 and v6 SxS) | ❌ |
| (n) | Session/service architecture (smss, services) | ❌ |
| (o) | The shell: the real explorer.exe/shell32 | ❌ |
| (p) | NTFS (read at least) | ❌ |
| (q) | TCP | ✅ |
| (r) | USB (FreeBSD's stack ported; precompiled Windows stack later) | ❌ |

ROADMAP.md is candid about the scale: this list is larger than everything built
so far combined, and ReactOS has worked on almost exactly this problem since
1996. The foundations here (the object manager, the PE loader, WDM, the unified
ACL model, threads, SMP and TCP) are the parts everything above sits on.

---

## 21. Where things live

```
Genesis/
├── bootloader/boot/boot.asm   BIOS MBR bootloader
├── kernel/
│   ├── flk.c                  kernel main
│   ├── arch/                  x86-64: boot, GDT/IDT, APIC/IOAPIC, SMP, ACPI, E820
│   ├── mm/                    physical/virtual memory, heap, kernel VA, stacks
│   ├── proc/                  processes, threads, ULE scheduler, signals, futex, syscalls
│   ├── fs/                    VFS, FAT16, ACLs, NT security descriptors, caches, pipes
│   ├── gnfs/                  gnfs, Genesis's native COW filesystem
│   ├── dev/                   ATA, AHCI, disks, partitions, volumes, PCI, IRQ, console, framebuffer, i8042 + PS/2 mouse, serial, timer, RTC
│   ├── driver/                Newbus, LinuxKPI, WDM, module loaders, hints
│   ├── exec/                  ELF, PE, NT syscalls, TEB/PEB construction
│   ├── obj/                   NT object manager, namespace, dispatcher objects
│   ├── bsd/                   vendored FreeBSD (network stack, UMA, sysctl, taskqueue, …)
│   ├── lib/                   kprintf, ksyms, backtrace, locks
│   └── include/               all headers (flat, on purpose; see kernel/README.md)
├── modules/                   loadable driver modules (if_re, lkpi_ahci, lkpi_pcpu, nb_rtl, …)
│   └── root/                  their staged .ko files and test.sys, overlaid on userland's root
├── tests/host/                host test suite and the gnfs ACL fixture
├── tools/                     guest_run.py, guest_sh.py, mkgnfs.c, fatfs.py, mkpe.py, …
├── build.py                   the build
└── ROADMAP.md  ROADMAP-archive.md  handoff.md  FEATURES.md

Genesis-userland/              everything in ring 3 (its own repository since 2026-09-28)
├── build.sh                   builds all of it into root/
├── src/
│   ├── ntdll/  kernel32/      clean-room Windows DLLs
│   ├── winhello/ hand/ k32demo/ winsync/ winthread/ winsmp/ …   Windows test programs
│   ├── rtld/  libgnt/         the ELF dynamic linker (ld-gen.so); DLL loading for ELF programs
│   ├── systest/  verif/       the in-machine test suites
│   └── fbtest/ ls/ mhello/ mkprobe/ gtrace/ somix/ winmix/ elfmix/
├── third_party/GNTbash        submodule: /bin/bash
├── third_party/GNTlibc        submodule: /lib/libc.so
└── root/                      the files staged onto the boot disk (/bin, /lib, /usr, /wsr)
```
