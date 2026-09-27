#!/usr/bin/env python3
"""Build the 64-bit Genesis kernel.

Two things differ structurally from the 32-bit build:

1. The boot sector is assembled standalone with `nasm -f bin` and concatenated
   onto the front of the image, rather than linked. ld cannot mix elf32 and
   elf64 objects, and 16-bit code with absolute relocations inside an elf64
   link is worse still. The boot sector shares no symbols with the kernel, so
   there was never anything to gain from linking it.

2. kernel_sectors used to come from the linker script. It now comes from the
   size of the built kernel image and is passed to nasm with -D.

Everything that runs before long mode - the 32-bit entry, the page tables, the
trampoline - lives inside 64-bit object files via .code32, in kernel/boot64.c.
"""

import os
import gzip
import shutil
import subprocess
import sys

CC, LD, NASM, OBJCOPY, NM = "gcc", "ld", "nasm", "objcopy", "nm"

CFLAGS = [
    "-m64", "-ffreestanding", "-fno-pie", "-fno-stack-protector",
    # The kernel lives in the top 2GB, so GCC can use 32-bit sign-extended
    # offsets instead of a movabs per symbol reference. Tied to KERNEL_VMA in
    # linker.ld: move one and the other breaks.
    "-mcmodel=kernel",
    # The ABI's 128-byte red zone below RSP is unusable in kernel code:
    # interrupts push straight through it. Forgetting this does NOT fail to
    # build - it corrupts whatever function was running when an IRQ landed.
    "-mno-red-zone",
    # GCC will otherwise emit SSE for ordinary struct copies and x87 for
    # anything float-shaped. Both fault until CR4.OSFXSR and the FPU are set up.
    "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-80387",
    "-fno-asynchronous-unwind-tables",
    # No host headers. -ffreestanding does NOT do this: it changes what the
    # compiler assumes about the library, not where #include looks, so
    # /usr/include stays on the search path and a header this tree has not
    # provided is silently answered by glibc.
    #
    # That is not hypothetical. Compiling vendored FreeBSD network source
    # without it reached /usr/include/x86_64-linux-gnu/bits/types.h, which
    # then "conflicted" with FreeBSD's own __int64_t - and the error named the
    # FreeBSD header, not the glibc one. Worse is the case that does NOT
    # error: a kernel built against a host header that happens to parse is a
    # kernel built against the wrong ABI, and nothing says so.
    #
    # -isystem keeps GCC's own freestanding headers (stddef.h, stdarg.h), which
    # are part of the compiler rather than part of the host system.
    "-nostdinc",
    "-isystem", subprocess.run(
        ["gcc", "-print-file-name=include"],
        check=True, capture_output=True, text=True).stdout.strip(),
    "-Wall", "-Wextra",
    "-c", "-Ikernel/include",
]

# Per-subtree include paths, prepended to CFLAGS for sources under the given
# prefix and for nothing else.
#
# kernel/bsd/compat/ answers #include <sys/param.h>, <sys/types.h> and
# <sys/queue.h> the way FreeBSD kernel source expects. It is scoped to
# kernel/bsd/ rather than made global because kernel/include has its own
# sys/ directory (sys/bus.h, the Newbus source-compat shim), and a vendored
# tree's headers must not answer Genesis's own includes - the failure is not
# a missing file but a file that parses and is wrong. (A second vendored tree,
# the ZFS reader, had its own compat/ with DIFFERENT sys/param.h; it was
# removed 2026-09-26, and the per-subtree shape is what made that clean.)
#
# Prepended, not appended: these must beat -Ikernel/include, which has its
# own sys/ directory (sys/bus.h, the Newbus source-compat shim).
EXTRA_INCLUDES = {
    # -D_KERNEL alongside the include path, because FreeBSD kernel source is
    # written to be compiled with it and large parts of these headers are
    # inside "#ifdef _KERNEL". net/if_media.h is entirely so: without the
    # define it yields the license block and nothing else, and the failure is
    # "unknown type name ifm_change_cb_t" rather than anything that points at
    # a missing flag.
    #
    # The vendored mbuf and UMA trees happened not to need it - their headers
    # put the kernel-only parts behind other guards - which is why it was
    # absent until the first vendored DRIVER was built. Adding it makes those
    # two see more of their own upstream headers, not less, so it is the
    # correct-by-construction direction rather than a widening to be careful
    # about.
    "kernel/bsd": ["-Ikernel/bsd/compat", "-D_KERNEL"],
}

# Warnings switched off for vendored subtrees, and ONLY for vendored subtrees.
#
# -Wall -Wextra stays on for every line of Genesis's own code. It comes off
# here because the alternative is editing FreeBSD source to satisfy a warning
# set FreeBSD does not build with - and an edited vendored file is one that
# cannot be re-fetched, which is the entire value of vendoring it.
#
# Both of these are noise rather than signal in this code: -Wsign-compare
# fires on upstream's habit of comparing an int length against a u_int count
# (m_pullup, m_copydata), and -Wunused-parameter fires on the UMA ctor/dtor
# signature, where every callback takes (mem, size, arg, how) whether or not
# it uses all four.
VENDOR_WARN_OFF = ["-Wno-sign-compare", "-Wno-unused-parameter",
                   "-Wno-missing-field-initializers",
                   "-Wno-pointer-sign", "-Wno-unused-value"]

EXTRA_WARNINGS = {
    # kernel/bsd/mbuf.c #includes the vendored .inc files, so the vendored
    # code's warnings are attributed to the includer - which is why the whole
    # subtree is listed rather than just vendor/.
    "kernel/bsd": VENDOR_WARN_OFF,
}


def flags_for(src):
    """The per-subtree flags that apply to one source file.

    Returned to be placed BEFORE CFLAGS so an -I here beats -Ikernel/include
    and a -Wno- here beats the -Wall/-Wextra there.
    """
    norm = src.replace(os.sep, "/")
    out = []
    for table in (EXTRA_INCLUDES, EXTRA_WARNINGS):
        for prefix, flags in table.items():
            if norm.startswith(prefix + "/"):
                out.extend(flags)
    return out

# -n (nmagic) stops ld page-aligning segments, which otherwise adds padding
# and an extra LOAD segment ahead of .lowtext in the flat binary.
LDFLAGS = ["-n", "-m", "elf_x86_64", "-T", "linker.ld", "-nostdlib"]

BUILD = "build"
KERNEL_ELF = f"{BUILD}/kernel.elf"
KERNEL_BIN = f"{BUILD}/kernel.bin"
BOOT_BIN = f"{BUILD}/boot.bin"
OS_IMG = f"{BUILD}/os.img"
DISK_IMG = f"{BUILD}/disk.img"
DISK_MB = 32
ROOT_DIR = "root"      # staged onto disk.img by cmd_disk
BOOT_ASM = "bootloader/boot/boot.asm"
LINKER_SCRIPT = "linker.ld"
PAGING_H = "kernel/include/paging.h"


def _read_constant(path, pattern, what):
    """Pull one hex constant out of a source file, or fail loudly.

    Read rather than duplicated on purpose. Both numbers below already exist
    in exactly one authoritative place each, and a copy here would be a third
    place to keep in step - which is how the old hand-raised MAX_IMG_SIZE
    ended up with six paragraphs of comment explaining what it was really a
    proxy for.
    """
    import re

    m = re.search(pattern, open(path).read(), re.MULTILINE)
    if m is None:
        raise SystemExit(
            f"build.py: could not find {what} in {path} - it was renamed or "
            f"reformatted, and the image-size check depends on it")
    return int(m.group(1), 16)


def image_limits():
    """(load address, window size, largest kernel that fits) - all derived.

    There used to be a MAX_IMG_SIZE constant here that got raised by hand
    every time the kernel grew, five times, each raise carrying a paragraph
    justifying it. It was a proxy for a real ceiling: the image loaded at
    0x7E00 and grew upward toward the 32-bit stack boot.asm sets up at
    0x90000, so 557568 bytes was the actual wall and the last raise landed
    within 512 bytes of it.

    The image loads at 0x100000 now (boot.asm copies it there through unreal
    mode, since INT 13h cannot write above 1MB). Nothing sits above it in
    physical memory, so the only remaining bound is how much the kernel's own
    page tables cover - KERNEL_MAP_SIZE. That is a .bss cost, not an image
    cost, so it is set far past what is needed and raising it further is one
    edit in one file.

    Both numbers are READ from the files that define them, so this cannot
    drift out of step with the linker script or the page tables the way three
    hand-copied constants would.
    """
    lma = _read_constant(LINKER_SCRIPT,
                         r"^KERNEL_LMA\s*=\s*(0x[0-9A-Fa-f]+)\s*;",
                         "KERNEL_LMA")
    window = _read_constant(PAGING_H,
                            r"^#define\s+KERNEL_MAP_SIZE\s+(0x[0-9A-Fa-f]+)",
                            "KERNEL_MAP_SIZE")
    if window <= lma:
        raise SystemExit(
            f"build.py: KERNEL_MAP_SIZE {window:#x} is not above KERNEL_LMA "
            f"{lma:#x} - the kernel window does not contain the load address")
    return lma, window, window - lma


def run(cmd):
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


def sources(root, ext):
    out = []
    for dirpath, dirnames, files in os.walk(root):
        # A vendor/ directory holds files that upstream #includes into a
        # wrapper translation unit (kernel/bsd/mbuf.c pulls in
        # kernel/bsd/vendor/). Compiling them separately would be compiling
        # fragments, so every vendor/ directory is skipped and the wrapper is
        # what gets built.
        dirnames[:] = [d for d in dirnames if d != "vendor"]
        out.extend(os.path.join(dirpath, f) for f in sorted(files) if f.endswith(ext))
    return out


def obj_for(src):
    return os.path.join(BUILD, src.replace(os.sep, "_").rsplit(".", 1)[0] + ".o")


# Must match linker.ld's .ksyms reservation and kernel/lib/ksyms_data.c's
# __ksyms_reserved[] EXACTLY - three places, and gen_ksyms below asserts the
# real table fits rather than silently truncating it.
#
# This reservation is a fixed hole in kernel.bin whether or not the symbols
# fill it, so it was squeezed to 96KB when the boot image had a hard ceiling
# 512 bytes away and 57KB of it was being shipped as padding.
#
# That trade is gone. The image loads above 1MB now and is bounded by
# KERNEL_MAP_SIZE (see image_limits above), which is measured in megabytes and
# costs .bss rather than image bytes - so reserved-but-unused space here is
# cheap again, and the table was at 83KB of 96KB with a protocol layer about
# to be added to it. 256KB against ~2800 symbols leaves room for roughly 5000
# more.
#
# Must match linker.ld's .ksyms reservation and kernel/lib/ksyms_data.c's
# __ksyms_reserved[] EXACTLY - three places - and gen_ksyms() below asserts
# the real table fits rather than silently truncating it if the kernel ever
# grows past it.
KSYMS_RESERVED_SIZE = 262144


def gen_ksyms(elf_path, bin_path):
    """Patch kernel.elf's real symbol table into the .ksyms space
    linker.ld reserved inside the already-built kernel.bin.

    No second link: nm reads kernel.elf (still has full ELF/symbol info -
    only kernel.bin, a separate objcopy output, is flat) after the normal
    link, and the blob is written directly into kernel.bin's bytes at the
    file offset that corresponds to __ksyms_reserved's address. See
    kernel/ksyms.c for the reader and the exact on-disk format this
    writes: u32 count, u32 strtab_size, then `count` 16-byte {addr,
    name_offset, pad} entries sorted ascending by addr, then the name
    bytes themselves.
    """
    out = subprocess.run([NM, elf_path], check=True,
                         capture_output=True, text=True).stdout

    kernel_vma = None
    kernel_lma = None
    ksyms_vma = None
    entries = []  # (addr, name), sorted by nm -n order below

    # Types worth keeping: real code/data/bss/rodata symbols, upper or
    # lower case (lower = local linkage). Everything else (U undefined,
    # A absolute - KERNEL_VMA itself is one - a/w/... ) is either not a
    # real address or is one of the linker-script constants this file
    # reads separately, by name, below.
    kept_types = set("TtDdBbRr")

    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        addr_s, typ, name = parts
        addr = int(addr_s, 16)
        if name == "KERNEL_VMA":
            kernel_vma = addr
        elif name == "kernel_phys_start":
            kernel_lma = addr
        elif name == "__ksyms_reserved":
            ksyms_vma = addr
        if typ in kept_types:
            entries.append((addr, name))

    if kernel_vma is None or kernel_lma is None or ksyms_vma is None:
        raise SystemExit("gen_ksyms: linker.ld symbol(s) missing from nm output "
                         "(KERNEL_VMA/kernel_phys_start/__ksyms_reserved) - "
                         "linker.ld or the .ksyms section was renamed?")

    entries.sort(key=lambda e: e[0])

    strtab = bytearray()
    name_offsets = []
    for _, name in entries:
        name_offsets.append(len(strtab))
        strtab += name.encode("ascii", "replace") + b"\x00"

    import struct
    blob = struct.pack("<II", len(entries), len(strtab))
    for (addr, _), name_off in zip(entries, name_offsets):
        blob += struct.pack("<QI4x", addr, name_off)
    blob += bytes(strtab)

    if len(blob) > KSYMS_RESERVED_SIZE:
        raise SystemExit(
            f"gen_ksyms: symbol table blob is {len(blob)} bytes, but "
            f"linker.ld only reserved {KSYMS_RESERVED_SIZE} - raise the "
            f".ksyms reservation in linker.ld (and KSYMS_RESERVED_SIZE "
            f"here to match) rather than truncating it")

    # __ksyms_reserved's file offset inside kernel.bin: objcopy -O binary lays
    # bytes out by LOAD ADDRESS (LMA), and linker.ld's AT(ADDR(x) -
    # KERNEL_VMA) makes every high-half section's LMA equal to its own
    # future physical/flat-binary offset - which is exactly what
    # kernel_phys_end (used by the PMM) already relies on elsewhere.
    ksyms_offset = (ksyms_vma - kernel_vma) - kernel_lma

    with open(bin_path, "r+b") as f:
        f.seek(0, os.SEEK_END)
        bin_size = f.tell()
        if ksyms_offset + KSYMS_RESERVED_SIZE > bin_size:
            raise SystemExit(
                f"gen_ksyms: computed .ksyms offset {ksyms_offset:#x} + "
                f"{KSYMS_RESERVED_SIZE:#x} runs past kernel.bin's actual "
                f"size {bin_size:#x} - the offset math above is wrong")
        f.seek(ksyms_offset)
        f.write(blob)

    print(f"  ksyms: {len(entries)} symbols, {len(blob)} of "
         f"{KSYMS_RESERVED_SIZE} bytes reserved")


def cmd_kernel():
    os.makedirs(BUILD, exist_ok=True)
    objs = []
    for src in sources("kernel", ".c"):
        obj = obj_for(src)
        run([CC, *flags_for(src), *CFLAGS, src, "-o", obj])
        objs.append(obj)

    run([LD, *LDFLAGS, *objs, "-o", KERNEL_ELF])
    run([OBJCOPY, "-O", "binary", KERNEL_ELF, KERNEL_BIN])
    gen_ksyms(KERNEL_ELF, KERNEL_BIN)


def cmd_image():
    cmd_kernel()

    kernel_size = os.path.getsize(KERNEL_BIN)
    sectors = (kernel_size + 511) // 512

    run([NASM, "-f", "bin", f"-Dkernel_sectors={sectors}", BOOT_ASM, "-o", BOOT_BIN])

    boot = open(BOOT_BIN, "rb").read()
    if len(boot) != 512:
        sys.exit(f"boot sector is {len(boot)} bytes, must be exactly 512")

    lma, window, limit = image_limits()
    if kernel_size > limit:
        sys.exit(
            f"kernel.bin is {kernel_size} bytes, which does not fit between "
            f"the load address {lma:#x} and the end of the kernel window "
            f"{window:#x}. Raise KERNEL_MAP_SIZE in {PAGING_H} - the cost is "
            f".bss page tables, not image bytes - or shrink the kernel.")

    # os.img is padded to a whole sector and no further.
    #
    # It used to be padded out to the full cap, which quietly covered a bug:
    # the boot sector reads ceil(kernel_size / 512) sectors, so a kernel whose
    # size is not a sector multiple makes the last read run past the end of
    # the file. With a fixed-size padded image there was always something
    # there; without one, that read fails and the machine prints "Disk read
    # error!" for a reason that has nothing to do with the disk.
    #
    # Padding to the cap is also what made the image a fixed 544KB blob no
    # matter how small the kernel was. It grows with the kernel now.
    padded = sectors * 512
    with open(OS_IMG, "wb") as out:
        out.write(boot)
        out.write(open(KERNEL_BIN, "rb").read())
        out.write(b"\0" * (padded - kernel_size))

    print(f"os.img: {512 + padded} bytes, kernel {sectors} sectors "
          f"loaded at {lma:#x}, {limit - kernel_size} bytes of the "
          f"{window:#x} kernel window still free "
          f"({100 * kernel_size // limit}% used)")


def cmd_disk():
    """Create a FAT16 data disk, if it does not already exist.

    Deliberately a SEPARATE image rather than a filesystem embedded in os.img.
    Embedding means the filesystem's start LBA moves every time the kernel
    changes size, and it means the only thing that ever writes that filesystem
    is your own half-written driver.

    A standalone image can be formatted by mkfs.fat and mounted by Linux, so
    you can drop real files in from the host and your driver has to agree with
    a reference implementation to read them back. Same reason the allocator got
    a test harness: a bug found by disagreeing with something known-correct
    beats a bug found by staring at a hex dump.

    Not regenerated if present - it holds your files.
    """
    if os.path.exists(DISK_IMG):
        print(f"{DISK_IMG} exists, leaving it alone "
              f"(delete it to re-stage from {ROOT_DIR}/)")
        return

    os.makedirs(BUILD, exist_ok=True)
    run(["dd", "if=/dev/zero", f"of={DISK_IMG}", "bs=1M", f"count={DISK_MB}",
         "status=none"])
    # -F 16: FAT16 is the sane first target. The root directory is a
    # fixed-size array at a known offset rather than a cluster chain, which
    # removes a whole layer from the first version of the driver.
    run(["mkfs.fat", "-F", "16", "-n", "GENESIS", DISK_IMG])
    print(f"{DISK_IMG}: {DISK_MB}MB FAT16")

    # Stage the tree from root/ so the on-disk layout is in version control
    # rather than being something assembled by hand once and unreproducible.
    # mkfs.fat still owns the geometry - this only allocates clusters and
    # writes directory entries - so the driver is still reading a filesystem
    # laid out by a reference implementation.
    if os.path.isdir(ROOT_DIR):
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "tools"))
        import fatfs
        vol = fatfs.Fat16(DISK_IMG)
        try:
            vol.stage(ROOT_DIR)
        finally:
            vol.close()
    else:
        print(f"  no {ROOT_DIR}/ to stage")

    print("  mount it with:  sudo mount -o loop,uid=$(id -u) "
          f"{DISK_IMG} /mnt/genesis")


def qemu_args():
    cmd_image()
    args = ["qemu-system-x86_64",
            "-drive", f"format=raw,file={OS_IMG},if=ide,index=0"]
    # Drives that ride the AHCI controller rather than IDE.
    ahci_extra = []

    # The FAT16 root. index=1 is the primary slave, which the driver
    # enumerates as ata1; the first volume to mount becomes the root.
    if os.path.exists(DISK_IMG):
        args += ["-drive", f"format=raw,file={DISK_IMG},if=ide,index=1"]

    # THE gnfs FIXTURE, as the second volume - attached by default, because
    # the checks that need it degrade silently without it: with no second
    # volume systest prints "skip" and still reports "all passed", which is
    # the failure mode this tree has already had once.
    #
    # tests/host/fixtures/gnfsfix.img.gz is built by the kernel's own gnfs
    # code (tests/host/gnfs_fixture.c) and run.sh proves the committed copy
    # still matches what gnfs writes. It holds /etc/motd for systest's
    # "every volume behaves the same" section and secret.txt - mode 0600
    # with an ACL granting uid 1001 read, which no mode word can say - for its
    # access-control section; systest's gnfs section then mutates the same
    # volume as real uids. So it is unpacked FRESH every run: a second run
    # against the first run's leftovers would be testing its own history.
    # It mounts at /mnt/d. (It replaced the two ZFS pools that used to sit
    # here; ZFS was removed from the tree 2026-09-26.)
    gnfs_src = "tests/host/fixtures/gnfsfix.img.gz"
    gnfs_img = f"{BUILD}/gnfsfix.img"
    if os.path.exists(gnfs_src):
        os.makedirs(BUILD, exist_ok=True)
        with gzip.open(gnfs_src, "rb") as src, open(gnfs_img, "wb") as dst:
            shutil.copyfileobj(src, dst)
        # index=2 is the secondary master, enumerated as ata2.
        args += ["-drive", f"format=raw,file={gnfs_img},if=ide,index=2"]

    # An AHCI controller (8086:2922), present for two reasons and used as
    # storage for neither.
    #
    # It is the only MSI-capable function QEMU's default machine can be given,
    # and pci_msi_selftest has been printing "no MSI-capable function -
    # programming not exercised (try -device ich9-ahci)" since MSI was built.
    # A test that says it did not run is honest; leaving it saying that
    # forever is not.
    #
    # It is also the one PCI function no driver in the tree claims, which is
    # what a loadable driver module needs in order to prove it reached real
    # hardware. Everything else on this machine - the host bridge, the ISA
    # bridge, IDE, VGA, the e1000 - is already attached by the time modules
    # load, and bus_driver_added deliberately will not take a device away
    # from an attached driver.
    # ...and a DISK BEHIND IT, which it did not have.
    #
    # The controller was attached for MSI and for a loadable module to claim,
    # and nothing was plugged into it - so kernel/dev/ahci.c would find an HBA
    # with zero occupied ports and its read/write path would never run. A
    # driver whose transfer code has never executed is a driver that has not
    # been tested, however many registers it read correctly.
    #
    # Separate from the IDE drives rather than instead of them: ata.c must keep
    # working, and having both means a boot proves the two storage paths
    # coexist rather than one having quietly replaced the other.
    ahci_img = f"{BUILD}/ahci.img"
    if not os.path.exists(ahci_img):
        os.makedirs(BUILD, exist_ok=True)
        with open(ahci_img, "wb") as fh:
            fh.truncate(16 * 1024 * 1024)
    args += ["-device", "ich9-ahci,id=ahci0"]
    args += ["-drive", f"format=raw,file={ahci_img},if=none,id=ahcidisk"]
    args += ["-device", "ide-hd,drive=ahcidisk,bus=ahci0.0"]
    for n, extra in enumerate(ahci_extra):
        args += ["-drive", f"format=raw,file={extra},if=none,id=ahcix{n}"]
        args += ["-device", f"ide-hd,drive=ahcix{n},bus=ahci0.{n + 1}"]

    # An RTL8139 NIC (10ec:8139), for the same reason as the AHCI controller
    # above: a PCI function no built-in driver claims, so the FreeBSD-idiom
    # loadable module has real hardware to attach to and read registers off.
    #
    # Chosen over the e1000 already on this machine because that one is
    # claimed by lkpi_demo at boot, and over virtio because virtio needs a
    # backend. Its BAR 0 is an I/O-PORT range, which is the point - it
    # exercises the port half of bus_read_N, where the AHCI driver exercises
    # the memory half through ioremap.
    # The NIC, now with a REAL NETWORK BEHIND IT.
    #
    # -netdev user is QEMU's user-mode network: a userspace TCP/IP stack that
    # answers ARP for 10.0.2.2, replies to ICMP echo, and NATs outbound
    # traffic. It needs no privileges and no host configuration, which is why
    # it is the right default here.
    #
    # That matters for what can be PROVEN: with a netdev attached, a frame the
    # driver transmits is really parsed by something that will really answer,
    # so an ARP reply or an ICMP echo reply arriving back is evidence the
    # whole path works. Without it the NIC transmits into a void and receive
    # can never be exercised at all.
    # GENESIS_NET=10.0.9.0/24 (say) puts QEMU's user network - its DHCP
    # server, gateway and DNS - on another subnet: the address the guest ends
    # up with then says whether DHCP configured it (10.0.9.15) or the static
    # fallback did (10.0.2.15, which cannot reach anything there).
    netdev = "user,id=n0"
    if os.environ.get("GENESIS_NET"):
        netdev += ",net=" + os.environ["GENESIS_NET"]
    args += ["-netdev", netdev]
    args += ["-device", "rtl8139,netdev=n0"]

    # COM1 to this terminal. The kernel mirrors everything print_string writes
    # to it, so the boot log and any test output are scrollable, greppable and
    # redirectable - none of which an 80x25 text buffer can be.
    #
    # The graphical window stays: keyboard input still comes from the emulated
    # PS/2 controller, so -display none would leave the shell unreachable.
    # Four CPUs unless told otherwise: the kernel runs processes on every
    # one, and a single-CPU boot would leave all of that untested. GENESIS_SMP=1
    # is the uniprocessor configuration, which must keep working too.
    args += ["-smp", os.environ.get("GENESIS_SMP", "4")]
    args += ["-serial", "stdio"]
    return args


def cmd_usb():
    """Build ONE bootable disk image: kernel and root filesystem together.

    Under QEMU, Genesis boots from os.img as drive 0 and finds its FAT16 root
    on disk.img as drive 1 - two separate disks, because attaching two is free.
    Real hardware does not work that way: a machine has the disk you wrote, and
    the kernel has to find its root on the same one it booted from.

    The layout, and why each piece is where it is:

      sector 0            the boot sector, WITH a partition table
      sectors 1..N        the kernel, read by INT 13h from absolute LBA 1
      sector PART_START   partition 1: the FAT16 root

    bootloader/boot/boot.asm already reserves bytes 446..509 for four zeroed
    partition entries and makes NASM fail if code grows into them - its own
    comment explains that they were accidentally non-zero once and the boot
    disk "grew a partition that was really a code fragment". That reservation
    is what makes this possible without touching the boot sector's code: the
    entry is filled in here, in the image builder, where the partition's real
    offset is known.

    The kernel is read from ABSOLUTE LBA 1 (see boot.asm's cur_lba), so it must
    stay below PART_START or it would run into the filesystem. That is asserted
    rather than assumed.
    """
    cmd_image()
    if not os.path.exists(DISK_IMG):
        cmd_disk()

    out = f"{BUILD}/genesis-usb.img"
    # 4MB in. The kernel is about 1.2MB today; this leaves it room to triple
    # before the check below starts failing, and 4MB is a round alignment for
    # anything that cares.
    part_start = 8192                      # sectors
    os_bytes = os.path.getsize(OS_IMG)
    if os_bytes > part_start * 512:
        sys.exit(f"{OS_IMG} is {os_bytes} bytes, which runs past the start of "
                 f"the root partition at sector {part_start}. Raise part_start "
                 f"in cmd_usb.")

    root = open(DISK_IMG, "rb").read()
    root_sectors = (len(root) + 511) // 512

    img = bytearray(open(OS_IMG, "rb").read())
    img += b"\x00" * (part_start * 512 - len(img))
    img += root
    if len(img) % 512:
        img += b"\x00" * (512 - len(img) % 512)

    # Partition entry 1, at offset 446. CHS is left as 0xFE 0xFF 0xFF - the
    # "use LBA instead" convention - because the geometry a BIOS reports for a
    # USB stick is invented anyway and Genesis's part.c reads the LBA fields.
    ent = bytearray(16)
    ent[0] = 0x80                                    # bootable
    ent[1:4] = b"\xFE\xFF\xFF"
    ent[4] = 0x0E                                    # FAT16 LBA
    ent[5:8] = b"\xFE\xFF\xFF"
    ent[8:12] = part_start.to_bytes(4, "little")
    ent[12:16] = root_sectors.to_bytes(4, "little")
    img[446:462] = ent

    with open(out, "wb") as f:
        f.write(img)
    print(f"{out}: {len(img)} bytes  "
          f"(kernel {os_bytes} bytes, root {len(root)} bytes at sector "
          f"{part_start})")
    print("  write it with:  dd if=%s of=/dev/sdX bs=4M conv=fsync" % out)
    return out


def cmd_usbrun():
    """Boot the combined image as the machine's ONLY disk.

    The point is to prove the single-disk layout before it is written to real
    hardware: no second drive, so if the kernel cannot find its root on the
    disk it booted from, it fails here rather than on a machine that has to be
    walked to.
    """
    img = cmd_usb()
    run(["qemu-system-x86_64",
         "-drive", f"format=raw,file={img},if=ide,index=0",
         "-serial", "stdio", "-display", "none"])


def cmd_run():
    #run(["tools/build_user.sh"])
    run(qemu_args())


def cmd_debug():
    run(qemu_args() + ["-d", "int,cpu_reset", "-no-reboot", "-s", "-S"])


def cmd_test():
    """Allocator test suite, run natively at 64-bit.

    KHEAP_START is overridden because the real one, 0xFFFFFFFF90000000, is a
    kernel address the host cannot mmap. That the allocator passes at a
    completely different base is not a compromise - it is the property being
    tested. The heap was already proved base-independent when it moved from
    0x800000 to 0xD0000000, and that same move is what exposed the integer
    overflow in the block-size checks.
    """
    os.makedirs(BUILD, exist_ok=True)
    run([CC, "-ffreestanding", "-fno-pie", "-no-pie", "-fno-stack-protector",
         "-nostdlib", "-Ikernel/include", "-Wall", "-Wextra",
         "-DKHEAP_START=0x70000000ULL",
         "-o", f"{BUILD}/kheap_test", "tests/kheap_test.c", "kernel/mm/kheap.c"])
    run([f"{BUILD}/kheap_test"])


def cmd_clean():
    """Remove build products, but NOT the data disk.

    disk.img lives in build/ but is not a build product - it holds files you
    put there from the host, and rebuilding it means losing them. Everything
    else in here is regenerated from source in seconds; this one thing is not
    regenerated at all.
    """
    if not os.path.exists(BUILD):
        return

    kept = 0
    for name in os.listdir(BUILD):
        path = os.path.join(BUILD, name)
        if os.path.abspath(path) == os.path.abspath(DISK_IMG):
            kept = 1
            continue
        os.remove(path)

    if kept:
        print(f"removed build products, kept '{DISK_IMG}'")
    else:
        os.rmdir(BUILD)
        print(f"removed '{BUILD}'")


if __name__ == "__main__":
    targets = {
        "all": cmd_image, "kernel": cmd_kernel, "image": cmd_image,
        "disk": cmd_disk, "usb": cmd_usb, "usbrun": cmd_usbrun, "run": cmd_run, "debug": cmd_debug,
        "test": cmd_test, "clean": cmd_clean,
    }
    target = sys.argv[1] if len(sys.argv) > 1 else "all"
    if target not in targets:
        sys.exit(f"unknown target '{target}'; try: {', '.join(targets)}")
    targets[target]()
