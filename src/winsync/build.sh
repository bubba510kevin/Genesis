#!/bin/sh
# Build sync.exe with MinGW-w64, importing from ntdll.dll.
#
# The ring-3 check for the NT dispatcher objects. src/winhello proves the
# loader and the import machinery; this one assumes those work and exercises
# what the boot-time dispatch_selftest cannot see - the syscall surface:
# argument marshalling across the Win64 boundary, OBJECT_ATTRIBUTES parsing,
# handle allocation, and the NTSTATUS values.
#
# Like winhello it knows no syscall number and no register convention. It
# calls NtCreateEvent the way any Windows program does.
set -e

cd "$(dirname "$0")"
ROOT=../..

CC=${MINGW_CC:-x86_64-w64-mingw32-gcc}

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "winsync: $CC not found - skipping (set MINGW_CC to override)" >&2
    exit 0
fi
if [ ! -f ../ntdll/libntdll.a ]; then
    echo "winsync: ../ntdll/libntdll.a missing - build ntdll first" >&2
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

$CC $CFLAGS $LDFLAGS -o sync.exe sync.c ../ntdll/libntdll.a

cp sync.exe "$ROOT/root/bin/sync.exe"
echo "built root/bin/sync.exe (MinGW, imports ntdll.dll)"
