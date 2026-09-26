# kernel/ layout

Source is grouped by subsystem. Nothing about this is load-bearing for the
build - `build.py`'s `sources()` walks `kernel/` recursively and `obj_for()`
flattens the path into the object name, so a file compiles from wherever it
sits. The grouping is for readers.

| directory | what lives there |
|---|---|
| `arch/` | x86-64 machine specifics: boot, GDT/IDT, interrupts, PIC/LAPIC/IOAPIC, SMP bring-up, ACPI, E820 |
| `mm/` | physical and virtual memory: PMM, paging, kernel heap, kernel VA allocator, kernel stacks |
| `proc/` | processes, scheduling, signals, futex, wait queues, the syscall table |
| `fs/` | VFS, FAT16, file objects, path resolution, pipes, block cache |
| `dev/` | hardware drivers and the buses they hang off: ATA, disk/partition/volume, PCI, IRQ, console, timer, RTC |
| `driver/` | the three driver MODELS and their loaders: Newbus (`bus.c`), LinuxKPI, WDM, plus `kldload`/`sysload`/`hints` |
| `exec/` | binary loaders and the personalities above them: ELF, PE, NT |
| `obj/` | the NT object manager and namespace |
| `lib/` | kernel-internal utilities with no subsystem of their own: kprintf, ksyms, backtrace, locks |
| `bsd/` | vendored FreeBSD - see `bsd/README.md` for the manifest |
| `zfs/` | vendored ZFS, CDDL, kept separate on purpose (ROADMAP item 7) |
| `include/` | all headers |

`flk.c` stays at the root: it is the kernel's main, and it reaches into every
directory above by design.

## Why headers are flat and .c files are not

`include/` is deliberately NOT mirrored into the same subdirectories. There is
exactly one `-Ikernel/include` for Genesis's own code, so `#include "kheap.h"`
has exactly one possible answer.

That matters more here than in most trees, because `include/` already carries
three compat subdirectories - `sys/`, `linux/`, `dev/` - whose whole job is to
answer an include the way FreeBSD or Linux source expects. Two vendored trees
already disagree about what `<sys/param.h>` means, which is why `build.py`
has per-subtree include paths at all (see `EXTRA_INCLUDES`, and the comment
above it about the afternoon that cost). Adding `mm/`, `dev/` and `arch/`
directories to the *search path* would put Genesis's own headers into the same
namespace those compat directories are fighting over - `dev/` in particular
already exists and means "FreeBSD's `dev/pci/pcivar.h`", not "Genesis's device
headers".

So: `.c` files are organized by directory, headers are organized by name.
