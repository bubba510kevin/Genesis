#!/bin/sh
# Build wait.exe: multi-object waits and APCs (item 14(b)), a PE importing kernel32.dll alone.
#
# The ring-3 check for ROADMAP item 14(b): WaitForMultipleObjects, alertable
# waits and APCs, reached the way any Win32 program reaches them.
set -e

cd "$(dirname "$0")"
ROOT=../..

CC=${MINGW_CC:-x86_64-w64-mingw32-gcc}

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "winwait: $CC not found - skipping (set MINGW_CC to override)" >&2
    exit 0
fi
if [ ! -f ../kernel32/libkernel32.a ]; then
    echo "winwait: ../kernel32/libkernel32.a missing - build kernel32 first" >&2
    exit 1
fi

# MinGW's wchar_t is 16 bits, so L"C:\\etc\\motd" is already UTF-16 and no
# -fshort-wchar is needed. It IS needed to syntax-check this on a Linux host,
# where wchar_t is 32 bits and every L"" would be twice the width the target
# expects - which is worth knowing before someone checks it the quick way and
# concludes the string handling is broken.
CFLAGS="-std=c99 -Wall -Wextra -Os -ffreestanding -fno-builtin \
        -fno-stack-protector -fno-asynchronous-unwind-tables"

LDFLAGS="-nostdlib -nostartfiles \
         -Wl,--entry=start \
         -Wl,--image-base,0x140000000 \
         -Wl,--subsystem,console"

$CC $CFLAGS $LDFLAGS -o wait.exe wait.c ../kernel32/libkernel32.a

# The import table is the claim this binary makes. Check it: exactly one DLL,
# and it is kernel32. A stray ntdll import would mean the compiler emitted a
# call this file did not write - and would quietly turn the depth-two test
# into a depth-one one.
if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
    dlls=$(x86_64-w64-mingw32-objdump -p wait.exe | sed -n 's/^\tDLL Name: //p')
    if [ "$dlls" != "kernel32.dll" ]; then
        echo "winwait: expected to import kernel32.dll alone, got:" >&2
        echo "$dlls" >&2
        exit 1
    fi
fi

cp wait.exe "$ROOT/root/bin/wait.exe"
echo "built root/bin/wait.exe (imports kernel32.dll only)"
