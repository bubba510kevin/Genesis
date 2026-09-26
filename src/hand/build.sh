#!/bin/sh
# Build hand.exe with MinGW-w64: no imports, no ntdll, no CRT.
#
# The opposite end of the pair from src/winhello. That one knows nothing - no
# syscall numbers, no register convention - and proves the import table, the
# export table and the IAT all agree. This one knows everything and imports
# nothing, so it proves the loader and the syscall convention on their own.
# Between them a failure has somewhere to be.
#
# MinGW is a host tool here on the same terms as everywhere else in this tree:
# a build dependency, never a linked one. -nostdlib and -nostartfiles mean
# nothing of its runtime reaches the image, and the assembler and linker are
# used for exactly two things - computing a section layout and emitting a
# valid PE - which is precisely the part that was hand-rolled before and
# precisely the part that broke.
set -e

cd "$(dirname "$0")"
ROOT=../..

CC=${MINGW_CC:-x86_64-w64-mingw32-gcc}

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "hand: $CC not found - skipping (set MINGW_CC to override)" >&2
    exit 0
fi

# ntsyscalls.h is generated from kernel/include/nt.h by the ntdll build. This
# image includes it rather than keeping its own list, so run the generator if
# nothing has yet - hand.exe must not be buildable against stale numbers, and
# the whole point of an anchor is that when it disagrees with the kernel, the
# kernel is what changed.
if [ ! -f ../ntdll/ntsyscalls.h ]; then
    (cd ../ntdll && python3 mknums.py)
fi

# .S rather than .s: the capital runs the C preprocessor, which is what makes
# the #include above work.
CFLAGS="-Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables"

# --entry=start because there is no CRT to provide mainCRTStartup, and
# --image-base stated rather than assumed - 0x140000000 is the executable
# default, is what the kernel's PE_USER_LIMIT check was sized against, and is
# where the loader will put this without needing .reloc.
LDFLAGS="-nostdlib -nostartfiles \
         -Wl,--entry=start \
         -Wl,--image-base,0x140000000 \
         -Wl,--subsystem,console"

$CC $CFLAGS $LDFLAGS -o hand.exe hand.S

# Refuse to stage an image with an import table. The entire value of this
# binary is that it has no dependencies past the loader, and an import
# directory arriving by accident - a stray libgcc reference, a linker default
# that changes in some future MinGW - would quietly turn the anchor into a
# second copy of hello.exe. Checked here because a check that runs is worth
# more than a comment that asks.
if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
    if x86_64-w64-mingw32-objdump -p hand.exe | grep -q "DLL Name:"; then
        echo "hand: image acquired an import table - refusing to stage" >&2
        x86_64-w64-mingw32-objdump -p hand.exe | grep "DLL Name:" >&2
        exit 1
    fi
fi

cp hand.exe "$ROOT/root/bin/hand.exe"
echo "built root/bin/hand.exe (MinGW, assembled, no imports)"
