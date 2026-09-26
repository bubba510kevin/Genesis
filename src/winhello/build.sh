#!/bin/sh
# Build hello.exe with MinGW-w64, importing from ntdll.dll.
#
# The point of this one is that it knows nothing. No syscall numbers, no
# register convention, no structure offsets - it calls NtOpenFile the way any
# Windows program does and the linker emits an import table naming ntdll.dll.
# If it runs, then the import directory, the export directory, the IAT and the
# DLL all agree, which no amount of hand-assembly can demonstrate.
set -e

cd "$(dirname "$0")"
ROOT=../..

CC=${MINGW_CC:-x86_64-w64-mingw32-gcc}

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "winhello: $CC not found - skipping (set MINGW_CC to override)" >&2
    exit 0
fi
if [ ! -f ../ntdll/libntdll.a ]; then
    echo "winhello: ../ntdll/libntdll.a missing - build ntdll first" >&2
    exit 1
fi

CFLAGS="-std=c99 -Wall -Wextra -Os -ffreestanding -fno-builtin \
        -fno-stack-protector -fno-asynchronous-unwind-tables"

# --entry=start because there is no CRT to provide mainCRTStartup, and
# --image-base is the executable default stated rather than assumed.
LDFLAGS="-nostdlib -nostartfiles \
         -Wl,--entry=start \
         -Wl,--image-base,0x140000000 \
         -Wl,--subsystem,console"

$CC $CFLAGS $LDFLAGS -o hello.exe hello.c ../ntdll/libntdll.a

cp hello.exe "$ROOT/root/bin/hello.exe"
echo "built root/bin/hello.exe (MinGW, imports ntdll.dll)"
