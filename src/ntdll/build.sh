#!/bin/sh
# Build ntdll.dll with MinGW-w64 and stage it into /wsr/System32.
#
# MinGW is a HOST TOOL here: permissive-licensed, a build dependency and not a
# linked one. Nothing of its runtime reaches the image - -nostdlib and
# -nostartfiles see to that, and the DLL provides its own entry point. The
# only thing being used is its ability to emit a real PE with a real export
# table, which is the part that would otherwise take weeks.
set -e

cd "$(dirname "$0")"
ROOT=../..

CC=${MINGW_CC:-x86_64-w64-mingw32-gcc}

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "ntdll: $CC not found - skipping (set MINGW_CC to override)" >&2
    exit 0
fi

# The syscall numbers, read out of the kernel header rather than repeated.
python3 mknums.py

CFLAGS="-std=c99 -Wall -Wextra -Os -ffreestanding -fno-builtin \
        -fno-stack-protector"

# --image-base 0x180000000 is where a 64-bit DLL conventionally lands, above
# an executable's 0x140000000 so the two do not collide before anything
# relocates. Well below PE_USER_LIMIT either way.
LDFLAGS="-shared -nostdlib -nostartfiles \
         -Wl,--entry=DllMainCRTStartup \
         -Wl,--image-base,0x180000000 \
         -Wl,--out-implib,libntdll.a"

$CC $CFLAGS -c stubs.c -o stubs.o
$CC $CFLAGS -c rtl.c   -o rtl.o
$CC $CFLAGS -c sync.c  -o sync.o
$CC $CFLAGS -c tls.c   -o tls.o
$CC $CFLAGS -c waitaddr.c -o waitaddr.o
$CC $CFLAGS -c except.c -o except.o
$CC $LDFLAGS -o ntdll.dll stubs.o rtl.o sync.o tls.o waitaddr.o except.o ntdll.def

# /wsr/System32 is the Windows side of the volume, and the name is 8.3-clean:
# WSR and SYSTEM32 both fit, as does NTDLL.DLL. A DLL that cannot be named by
# the filesystem is a problem to find out about now rather than when the
# loader goes looking for it.
mkdir -p "$ROOT/root/wsr/System32"
cp ntdll.dll "$ROOT/root/wsr/System32/ntdll.dll"

echo "built root/wsr/System32/ntdll.dll"
