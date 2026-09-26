#!/bin/sh
# Build kernel32.dll with MinGW-w64 and stage it into /wsr/System32.
#
# kernel32 imports from ntdll, so this is the first DLL in the tree that is
# itself a client of another one - which makes it the first test of the
# loader's dependency handling at depth two: the executable imports kernel32,
# kernel32 imports ntdll, and pe.c has to load and resolve both without
# loading ntdll twice.
#
# Same host-tool terms as everything else here: -nostdlib, -nostartfiles,
# nothing of MinGW's runtime in the image.
set -e

cd "$(dirname "$0")"
ROOT=../..

CC=${MINGW_CC:-x86_64-w64-mingw32-gcc}

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "kernel32: $CC not found - skipping (set MINGW_CC to override)" >&2
    exit 0
fi
if [ ! -f ../ntdll/libntdll.a ]; then
    echo "kernel32: ../ntdll/libntdll.a missing - build ntdll first" >&2
    exit 1
fi

CFLAGS="-std=c99 -Wall -Wextra -Os -ffreestanding -fno-builtin \
        -fno-stack-protector -fno-asynchronous-unwind-tables"

# --image-base 0x181000000: above ntdll's 0x180000000 and far enough not to
# overlap it. Two DLLs at one preferred base is exactly the case .reloc exists
# for, and this loader has that path - but a collision resolved by relocation
# on every boot is a cost paid for nothing when the bases can simply differ.
LDFLAGS="-shared -nostdlib -nostartfiles \
         -Wl,--entry=DllMainCRTStartup \
         -Wl,--image-base,0x181000000 \
         -Wl,--out-implib,libkernel32.a"

$CC $CFLAGS -c err.c  -o err.o
$CC $CFLAGS -c file.c -o file.o
$CC $CFLAGS -c proc.c -o proc.o
$CC $LDFLAGS -o kernel32.dll err.o file.o proc.o kernel32.def ../ntdll/libntdll.a

mkdir -p "$ROOT/root/wsr/System32"
cp kernel32.dll "$ROOT/root/wsr/System32/kernel32.dll"

echo "built root/wsr/System32/kernel32.dll"
