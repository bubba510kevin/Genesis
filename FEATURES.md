# Genesis — Feature Guide

*Last updated 2026-09-26. A complete tour of what Genesis is, what it can do
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
sysctl, the UMA allocator, mbufs, and more (`kernel/bsd/`). A **ZFS reader** is
vendored too, kept behind a license boundary (`kernel/zfs/`, CDDL). Genesis
also has its own native copy-on-write filesystem, **gnfs**.

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
| Multiprocessor | 🟡 | 2 CPUs brought up (ACPI, LAPIC, IOAPIC, TLB shootdown); only the boot CPU runs programs |
| Memory | ✅ | 4-level paging, NX, W^X, vendored UMA slab allocator, low-memory reclaim |
| Processes | ✅ | fork, vfork, clone, execve (ELF and PE), signals, wait4/waitid, sessions, groups |
| POSIX threads | ✅ | clone() threads with TLS and `pthread_join` wakeups |
| Windows threads | ✅ | CreateThread / WaitForSingleObject, a TEB and stack per thread |
| Scheduler | ✅ | FreeBSD-style ULE with interactivity scoring; preemptive for user code |
| Linux syscalls | ✅ | ~130 real Linux x86-64 syscall numbers |
| FAT16 | ✅ | Full read and write; the default root filesystem |
| ZFS | 🟡 | Read-only, 18 pool features, reads real ACLs; experimental write that doesn't finish |
| gnfs (native COW FS) | 🟡 | Read, write and ACLs; crash-safe commits; files capped at 48KB |
| Permissions | ✅ | One NFSv4/NT ACL model with POSIX and Windows views; chmod, chown, umask, setgid, sticky |
| Windows programs | 🟡 | PE loading, ntdll + kernel32 subset, 20 NT syscalls; no GUI |
| NT object manager | ✅ | Namespace, named events/semaphores/mutexes, \ObjectTypes |
| Driver models | ✅ | Newbus, LinuxKPI and WDM, loadable from 4 directories, with unload |
| Networking | 🟡 | IPv4, ICMP, ARP, UDP, sockets (FreeBSD's own code); no TCP, no DHCP |
| Storage drivers | ✅ | ATA (PIO), AHCI (DMA), MBR partitions |
| Console | ✅ | VGA text, PS/2 keyboard, serial console (usable as the only input) |
| GUI / desktop | ❌ | Nothing graphical yet; see §20 |

**Latest test results** (2026-09-26, full machine under QEMU):
`verification: 160 passed, 0 failed` · `systest: 443 passed, 0 failed` ·
`thr: 16 passed, 0 failed` · host suite: all checks passed · 20+ boot-time
self-tests all passing.

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

### Multiprocessor (SMP) 🟡
`kernel/arch/smp.c` and `acpi.c`: CPUs are found in ACPI's MADT, and the
secondary CPUs start with INIT-SIPI-SIPI through a real-mode trampoline. Each
gets per-CPU GDT/TSS/IST and a per-CPU `%gs`, and **IPI-driven TLB
shootdown** works. **Limit:** the secondary CPUs come up, answer IPIs and then
idle in `hlt`. **Only the boot CPU runs processes today**, so threads take
turns on one CPU rather than running in parallel. There is also no interrupt
balancing: every line goes to CPU 0.

### PCI ✅
Enumeration with **PCI-to-PCI bridge recursion**, BAR sizing, and config-space
read/write (`kernel/dev/pci.c`). A fallback `pci_generic` driver claims
anything no real driver takes.

### Timers and clocks ✅
PIT/LAPIC tick (`kernel/dev/timer.c`), **RTC** wall clock (`kernel/dev/rtc.c`),
and TSC-based short delays (a driver may call `DELAY()` before interrupts are
on). Clock resolution is **one tick** and is reported honestly.

### Console and input ✅
- **VGA text** screen and a **tty** with a line discipline, echo and Ctrl-C
  (`kernel/dev/screen.c`, `tty.c`).
- **PS/2 keyboard** (`kernel/dev/keyboard.c`).
- **Serial console** (`kernel/dev/serial.c`): everything the kernel prints is
  mirrored to COM1, and **received serial bytes feed the same input ring as
  the keyboard**. A machine with no PS/2 keyboard (the bare-metal target has
  only USB) is therefore still usable over serial, and the automated test
  harness types into the machine this way.

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

**Not yet:** Windows TLS (`TlsAlloc`), `CREATE_SUSPENDED`/`ResumeThread`,
terminating *another* thread, and marking a mutex *abandoned* when its owner
thread dies.

### Kernel threads ✅
`kernel/proc/kthread.c`: schedulable threads that run only in the kernel, used
by the taskqueue, low-memory reclaim and tests. `sleep(9)` really deschedules a
kernel thread. **Kernel code is deliberately not preemptible** (nothing in the
kernel locks against it), so a kernel thread must yield or block.

### Scheduler ✅
**ULE**, the FreeBSD scheduler design (`kernel/proc/sched_ule.c`), behind a
small four-function policy interface (`sched.c`):
- per-CPU run queues;
- an **interactivity score** from sleep-time vs run-time, so programs that
  mostly wait on a person are favoured over CPU hogs;
- **preemptive for user programs**: the timer tick forces a switch;
- CPU-time accounting that never runs backwards (`times(2)`,
  `CLOCK_PROCESS_CPUTIME_ID`).

**Limit:** `MAX_PROCESSES` is **16**, shared by every process, thread and
kernel thread.

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
| Waiting | `poll`, `ppoll`, `futex`, `sched_yield`, `eventfd2`, `pipe`, `pipe2`, `socketpair` |
| Sockets | `socket`, `bind`, `connect`, `sendto`, `recvfrom`, `getsockname` |
| Misc | `uname`, `arch_prctl`, `set_tid_address`, `set_robust_list`, `getrandom`, `prctl`, `reboot` |

**Honest errors.** A call that can't be supported fails with the correct
errno rather than faking success. For example, `rseq` and `prlimit64` return
`-ENOSYS`, and a TCP socket returns `-EPROTONOSUPPORT`, refused by the protocol
switch itself, so it will start working the day TCP is added.

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

### ZFS 🟡 — read-only, real pools
`kernel/zfs/` holds FreeBSD's standalone ZFS reader (~10k lines), **kept
behind a CDDL licence boundary** that a build check enforces in both
directions.
- Mounts real pools made by OpenZFS, and supports **18 pool read features**
  (lz4, zstd, blake3, skein, sha512, encryption, large blocks, large dnodes,
  embedded data, …). A pool using any other feature is **refused**, never
  mounted and hoped for the best.
- Reads **real NFSv4 ACLs** in *both* on-disk formats (the old ZPL v1 layout
  and the modern v5 system-attribute layout), and enforces them. This is
  something Linux-on-ZFS doesn't do.
- **Write, experimental:** Genesis can allocate space, write checksummed
  blocks, and commit a transaction group that **real OpenZFS imports and
  scrubs clean**, but the full copy-on-write chain doesn't settle, so no file
  can be changed yet. Writes stay switched off.

### gnfs 🟡 — Genesis's own copy-on-write filesystem
`kernel/gnfs/`, BSD-2-Clause, new code; design notes in
`kernel/include/gnfs_layout.h`.
- **Crash-safe by construction:** a ring of root records (the ZFS uberblock
  idea, reused), copy-on-write for every block, and a whole-structure
  ping-pong for the free-space bitmap and object table. A torn write leaves the
  previous state mountable.
- A flat object table, where an object number is a direct array index.
- Files and directories with create, read, write (gaps read as zero),
  truncate, mkdir, rmdir, unlink, statfs.
- **Full ACL support**, stored per object in its own COW block, with
  inheritance on create, chmod, chown, the creator owning new objects, and
  setgid/sticky directories. See §9.
- Formatted by `tools/mkgnfs.c`, which uses the same formatting code the kernel
  does.
- **Limits:** files and directories are capped at **48KB** (no indirect blocks
  yet), `rename` isn't implemented, and snapshots and datasets aren't done.

### Other file-like objects ✅
**Pipes** (with SIGPIPE/EPIPE), **eventfd**, **socketpair** (two crossed
pipes), **sockets**, `/dev/console`, `/dev/null`, all behind the same object
vtable, so `read`/`write`/`poll` work on all of them.

---

## 9. Security and permissions

Genesis's permission system is one of its most distinctive parts.

### One ACL model, two views ✅
Every object's permissions are an **NFSv4 ACL**, the same model ZFS stores and
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

### NT system calls ✅ (20)
| Area | Calls |
|---|---|
| Process | `NtTerminateProcess` |
| Threads | `NtCreateThreadEx`, `NtTerminateThread`, `NtQueryInformationThread` |
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
`RtlDosPathNameToNtPathName_U`, `RtlAllocateHeap`/`RtlFreeHeap`,
`LdrInitializeThunk`, `RtlUserThreadStart` and `RtlExitUserThread`.

### kernel32.dll 🟡 (clean-room, `src/kernel32/`)
| Area | Exports |
|---|---|
| Errors | `GetLastError`, `SetLastError` (NTSTATUS → Win32 error mapping) |
| Files | `GetStdHandle`, `CreateFileW`/`A`, `ReadFile`, `WriteFile`, `CloseHandle` |
| Process | `ExitProcess`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetCommandLineW`/`A`, `GetEnvironmentStringsW`, `FreeEnvironmentStringsW`, `GetCurrentDirectoryW`, `GetModuleHandleW` |
| Threads | `CreateThread`, `ExitThread`, `GetCurrentThread`, `GetCurrentThreadId`, `GetThreadId`, `GetExitCodeThread`, `WaitForSingleObject` |
| Memory | `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `VirtualAlloc`, `VirtualFree` |

### Windows test programs in `/bin` ✅
| Program | Proves |
|---|---|
| `hello.exe` | The loader and the native ntdll interface |
| `hand.exe` | A PE assembled by hand, not by a linker |
| `k32.exe` | kernel32-only imports two levels deep; command line and parameters |
| `sync.exe` | Events, semaphores and mutexes by name, through ntdll (29 checks) |
| `thr.exe` | Win32 threads end to end (16 checks) |

### Not yet ❌
GUI (user32, gdi32), structured exception handling, the registry, COM, TLS,
`NtCreateProcess` (a Windows program can't start another one yet), and most of
kernel32. See §20.

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
| Shell | 🟡 | **BusyBox ash** runs as the interactive shell, but this BusyBox build has no applets (no `echo`/`cat`/`rm`); a fuller build is needed |
| vDSO | ❌ | |

Programs in `/bin`: `busybox` (shell), `ls` (musl), `mhello` (musl), `hello`
(freestanding), `mkprobe`, `systest`, `verif`, plus the Windows ones in §10.

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
  setup/teardown.

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
real AHCI controller's registers through a memory BAR. The Linux in-kernel
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
| Routing table, default route, `SIOCAIFADDR` address setup | ✅ |
| **Sockets from user programs**: `socket`, `bind`, `connect`, `sendto`, `recvfrom`, and plain `read`/`write` | ✅ systest sends a real DNS query to QEMU's resolver and checks the reply (the reply needs the host to have a working DNS upstream) |
| `poll` on sockets | 🟡 the wake-up is implemented but has no test yet: proving it needs data arriving on demand, which needs a loopback interface |
| NIC | ✅ RTL8139C+ via the unmodified FreeBSD `if_re` driver |
| Address | 🟡 static `10.0.2.15/24`, gateway `10.0.2.2` (QEMU user networking) |
| **TCP** | ❌ not vendored yet; everything below it exists |
| DHCP, loopback interface, `listen`/`accept`, IPv6 | ❌ |

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
  dumps, and passes a two-CPU contention test with no lost increments.

---

## 16. Debugging aids

- **ksyms**: the kernel's own symbol table is embedded and readable at runtime
  (`kernel/lib/ksyms.c`).
- **backtrace** with symbol names (`kernel/lib/backtrace.c`).
- **kprintf** with field widths and colours, mirrored to serial.
- **NT syscall trace**: failing NT calls print their number and status.
- **Lock diagnostics**: deadlock reports and held-lock dumps.
- `build.py debug` starts QEMU paused with a GDB stub.

---

## 17. How Genesis is tested

Four layers, and every feature above lives in at least one:

| Layer | What it is | Runs |
|---|---|---|
| **Host suite** (`tests/host/run.sh`) | Kernel code (VFS, FAT, gnfs, ACLs, ZFS reader, bcache, PE, paths, volumes, …) compiled natively and tested directly, plus build-time checks: the ZFS licence boundary and staged-tree name collisions | On the build machine |
| **Boot self-tests** | 20+ checks that run every boot: SMP TLB shootdown, IOAPIC, IDT, IRQ, MSI, locks, ULE, kernel threads, condvars, taskqueue, callout, mbuf, bus, W^X, module unload, WDM IRPs, AHCI DMA, page cache, low memory, dispatcher objects, ACL privilege, network ARP/ICMP | Inside the kernel |
| **systest** (`src/systest.c`) | 443 checks of the syscall interface from a real user program, deliberately without libc so errnos aren't hidden. Permission checks run in child processes as real uids | Inside the booted machine |
| **verif** (`src/verif.c`) | 160 checks plus the bug postmortems and the "needs a human" queue | Inside the booted machine |
| **Windows programs** | `sync.exe` (29), `thr.exe` (16), `k32.exe`, `hello.exe`, `hand.exe` | Inside the booted machine |

**`tools/guest_run.py`** boots the full machine (FAT root, two ZFS pools, a
fresh gnfs volume, AHCI, the NIC), types commands into the shell over the
serial port, and collects each program's `N passed, M failed` tally. The whole
run takes about five minutes:

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
| `python3 build.py disk` | Build the FAT16 data disk from `root/` (kept if it exists) |
| `python3 build.py run` | Boot the full machine in QEMU (window + serial console) |
| `python3 build.py debug` | The same, paused, with a GDB stub |
| `python3 build.py usb` / `usbrun` | Build / test the single-disk bare-metal image |
| `python3 build.py test` | The kernel heap allocator's native test |
| `python3 build.py clean` | Remove build products, keeping `disk.img` |
| `bash tests/host/run.sh` | The host test suite |
| `python3 tools/guest_run.py` | Boot and run verif, systest and thr.exe |
| `sh src/<ntdll\|kernel32\|winthread\|…>/build.sh` | Build a Windows DLL or program (MinGW-w64) |
| `tools/build_user.sh` | Build every user program (its final staging step needs `sudo`) |

Toolchain: gcc, nasm, ld, QEMU, mkfs.fat, **MinGW-w64** (Windows programs),
**musl-gcc** (musl programs). Commit history is in git on branch `main`.

---

## 19. Known limitations

Collected in one place so nobody has to discover them the hard way:

- **One CPU runs programs.** The second CPU is up but idle.
- **16 process slots in total**, shared by processes, threads and kernel
  threads.
- **Kernel code is not preemptible**; a kernel loop that doesn't yield hangs
  the machine.
- **No GUI**: text console only.
- **No TCP, no DHCP, no loopback.**
- **ZFS is read-only**; gnfs files are capped at 48KB; there are no symlinks
  on any filesystem; FAT is 8.3-only.
- **No file-backed mmap, no swap, no demand paging.**
- **No USB** (no controller or HID drivers); input is PS/2 or serial.
- **No NVMe, no GPT**; ATA is PIO-only.
- **BusyBox has no applets**, just the shell.
- **Windows**: no SEH, registry, COM, TLS, child processes, or GUI DLLs;
  kernel32 is a small subset.
- **Clock resolution is one timer tick.**
- `exec` doesn't honour setuid/setgid bits.

---

## 20. The road to the goal

The destination is a **daily-drivable desktop running Genesis's own
reimplementation of the Windows 7 desktop**, running real Win32 programs. The
key decision is already made: Genesis will write its own clean-room
`user32`/`gdi32`/`shell32` against the documented Win32 API, as it already did
for `ntdll` and `kernel32`, rather than reverse-engineer Windows' undocumented
`win32k.sys`. ReactOS takes the same approach.

ROADMAP item 14's dependency-ordered list:

| Step | What | Status |
|---|---|---|
| (a) | **Full multithreading** | 🟡 started: POSIX and Win32 threads work; TLS, more slots and multi-CPU remain |
| (b) | Dispatcher objects completed: waits blocking threads, APCs | 🟡 objects and waits exist; APCs don't |
| (c) | Structured exception handling (x64 table-based) | ❌ |
| (d) | NT memory model: VirtualAlloc states, Section objects, a full PEB | ❌ |
| (e) | The loader: `LdrLoadDll`/`GetProcAddress` for real | ❌ |
| (f) | kernel32, completed | 🟡 |
| (g) | advapi32 (registry, security APIs) | ❌ |
| (h) | The display: a linear framebuffer (VESA/VBE) | ❌ |
| (i) | gdi32 | ❌ |
| (j) | user32 (windows, messages, input) | ❌ |
| (k) | COM/OLE | ❌ |
| (l) | RPC | ❌ |
| (m) | comctl32 | ❌ |
| (n) | Session/service architecture (smss, services) | ❌ |
| (o) | A shell: explorer-like, or a native fallback | ❌ |
| (p) | NTFS (read at least) | ❌ |
| (q) | TCP | ❌ |
| (r) | USB (controllers, HID, mass storage) | ❌ |

ROADMAP.md is candid about the scale: this list is larger than everything built
so far combined, and ReactOS has worked on almost exactly this problem since
1996. The foundations here (the object manager, the PE loader, WDM, the unified
ACL model, and now threads) are the parts everything above sits on.

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
│   ├── zfs/                   vendored ZFS reader (CDDL, kept separate)
│   ├── dev/                   ATA, AHCI, disks, partitions, volumes, PCI, IRQ, console, serial, timer, RTC
│   ├── driver/                Newbus, LinuxKPI, WDM, module loaders, hints
│   ├── exec/                  ELF, PE, NT syscalls, TEB/PEB construction
│   ├── obj/                   NT object manager, namespace, dispatcher objects
│   ├── bsd/                   vendored FreeBSD (network stack, UMA, sysctl, taskqueue, …)
│   ├── lib/                   kprintf, ksyms, backtrace, locks
│   └── include/               all headers (flat, on purpose; see kernel/README.md)
├── src/
│   ├── ntdll/  kernel32/      clean-room Windows DLLs
│   ├── winhello/ hand/ k32demo/ winsync/ winthread/   Windows test programs
│   ├── rtld/                  the ELF dynamic linker (ld-gen.so)
│   ├── kmod/                  loadable driver modules (if_re, lkpi_ahci, nb_rtl, …)
│   ├── systest.c  verif.c     the in-machine test suites
│   └── hello.c hello_musl.c ls.c mkprobe.c
├── root/                      the files staged onto the boot disk (/bin, /lib, /boot, /wsr)
├── tests/host/                host test suite and ZFS fixtures
├── tools/                     guest_run.py, mkgnfs.c, fatfs.py, build_user.sh, …
├── build.py                   the build
└── ROADMAP.md  ROADMAP-archive.md  handoff.md  FEATURES.md
```
