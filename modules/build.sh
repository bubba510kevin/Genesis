#!/bin/sh
# Build the loadable kernel modules into modules/root/ - an overlay that
# build.py disk stages on top of the userland root (Genesis-userland), at the
# paths kldload.c looks in. They live here, not in the userland repository,
# because they are kernel code: built with the kernel's flags against its
# headers, and run in ring 0. (This was src/kmod/ before the split.)
#
# -c, not -shared: kernel/driver/kldload.c wants ET_REL. The rest of the flags
# match the kernel's own (build.py's CFLAGS) because this code runs IN the
# kernel - same code model, same absence of SSE, same freestanding
# environment. A mismatch here is not a link error, it is a fault at run time.
#
# -mcmodel=kernel in particular: it makes the compiler emit 32-bit
# sign-extended offsets on the assumption everything lives in the top 2GB,
# which is where kldload.c's kmalloc puts the module too.
set -e
cd "$(dirname "$0")"
ROOT=..

CFLAGS="-m64 -ffreestanding -fno-pie -fno-stack-protector -mcmodel=kernel \
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
    -fno-asynchronous-unwind-tables -Wall -Wextra"

# hello_kmod is built against NOTHING - it declares the two kernel functions
# it calls by hand. That is deliberate: it proves the loader works with no
# compat layer involved at all.
gcc $CFLAGS -c hello_kmod.c -o hello_kmod.ko
mkdir -p "$ROOT/modules/root/boot/kernel"
cp hello_kmod.ko "$ROOT/modules/root/boot/kernel/hellokm.ko"

# lkpi_ahci is the opposite case: ordinary Linux driver source, compiled
# against kernel/include/linux/ with -Ikernel/include and nothing else. It
# goes in /lib/modules, which is where Linux puts modules and which
# kldload.c scans.
#
# 8.3 name on the FAT root, upper-cased on read - see kldload.c's
# case-insensitive .ko test, which exists because the first version of that
# scan found nothing on a volume that plainly had a module on it.
gcc $CFLAGS -I"$ROOT/kernel/include" -c lkpi_ahci.c -o lkpi_ahci.ko
mkdir -p "$ROOT/modules/root/lib/modules"
cp lkpi_ahci.ko "$ROOT/modules/root/lib/modules/lkpiahci.ko"

# nb_rtl is the FreeBSD half: ordinary Newbus driver source, compiled against
# kernel/include/sys/bus.h and kernel/include/dev/pci/pcivar.h. It goes in
# /boot/kernel, which is where FreeBSD puts modules.
# nb_rtl.c is COMPILED but no longer STAGED. It claims the same 10ec:8139 the
# real if_rl.c does, and only one driver can own a device - keeping both on
# disk would mean whichever registered first silently won, which reads as a
# bug in the log. It stays built so it cannot rot, and stays in the tree as
# the small hand-written counterpart to the 2095-line vendored one.
gcc $CFLAGS -I"$ROOT/kernel/include" -c lkpi_pcpu.c -o lkpi_pcpu.ko
cp lkpi_pcpu.ko "$ROOT/modules/root/lib/modules/lkpipcpu.ko"

gcc $CFLAGS -I"$ROOT/kernel/include" -c nb_rtl.c -o nb_rtl.ko
rm -f "$ROOT/modules/root/boot/kernel/nbrtl.ko"

# THE REAL THING: vendsrc/sys/dev/rl/if_rl.c, 2095 lines of unmodified
# FreeBSD driver source, compiled against Genesis's compat headers.
#
# -nostdinc is load-bearing and not tidiness: without it a #include <sys/...>
# finds the HOST's /usr/include, and "it compiled" would mean "it compiled
# against glibc". -D_KERNEL because FreeBSD kernel source is written to be
# compiled with it and large parts of these headers are inside #ifdef _KERNEL.
# The extra -I is for "miibus_if.h", which upstream GENERATES into the object
# directory and which is hand-written here (see that file).
#
# vendsrc/ is the upstream tree and is NOT in version control (.gitignore):
# a checkout without it keeps the staged ifre.ko and skips these two, loudly,
# rather than stopping tools/build_user.sh before the programs after it.
if [ -d "$ROOT/vendsrc/sys/dev/rl" ] && [ -d "$ROOT/vendsrc/sys/dev/re" ]; then
gcc $CFLAGS -nostdinc -D_KERNEL \
    -isystem "$(gcc -print-file-name=include)" \
    -I"$ROOT/kernel/bsd/compat" -I"$ROOT/kernel/bsd/compat/dev/mii" \
    -I"$ROOT/kernel/include" \
    -c "$ROOT/vendsrc/sys/dev/rl/if_rl.c" -o if_rl.ko

# if_re.c - 4295 more lines, and the one that MATTERS on this machine.
# QEMU's rtl8139 reports revision 0x20, which is an 8139C+, and if_rl.c
# correctly declines it ("let re(4) take care of this device"). re(4) is
# that driver.
gcc $CFLAGS -nostdinc -D_KERNEL \
    -isystem "$(gcc -print-file-name=include)" \
    -I"$ROOT/kernel/bsd/compat" -I"$ROOT/kernel/bsd/compat/dev/mii" \
    -I"$ROOT/kernel/include" \
    -c "$ROOT/vendsrc/sys/dev/re/if_re.c" -o if_re.ko
cp if_re.ko "$ROOT/modules/root/boot/kernel/ifre.ko"
echo "built if_rl.ko (UNMODIFIED vendsrc/sys/dev/rl/if_rl.c, compiled not staged)"
echo "built modules/root/boot/kernel/ifre.ko (UNMODIFIED vendsrc/sys/dev/re/if_re.c)"
else
echo "SKIPPED if_rl.ko and ifre.ko: vendsrc/sys/dev/{rl,re} not present - the staged modules/root/boot/kernel/ifre.ko is kept"
fi

echo "built modules/root/boot/kernel/hellokm.ko"
echo "built nb_rtl.ko (compiled, not staged - if_rl.c owns that device)"
echo "built modules/root/lib/modules/lkpiahci.ko"
echo "built modules/root/lib/modules/lkpipcpu.ko"
