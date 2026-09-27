#!/bin/sh
# Build seh.exe: structured exception handling (ROADMAP item 14(c)).
#
# Compiled by CLANG for the MinGW target, then linked by MinGW's toolchain
# like every other PE here. GCC has no __try/__except/__finally at all;
# clang implements them the MSVC way - scope tables in .xdata naming
# __C_specific_handler - which is the data a real Windows program hands
# ntdll, and so the data worth testing against. -fasync-exceptions makes a
# hardware fault inside a __try body catchable, not only an exception
# raised by a call. -mno-stack-arg-probe: there is no __chkstk to call.
#
# Imports kernel32 AND ntdll: __C_specific_handler is an ntdll export, as
# on Windows, and the compiler emits the reference itself.
set -e

cd "$(dirname "$0")"
ROOT=../..

CLANG=${CLANG:-clang}
CC=${MINGW_CC:-x86_64-w64-mingw32-gcc}

if ! command -v "$CLANG" >/dev/null 2>&1 || ! command -v "$CC" >/dev/null 2>&1; then
    echo "winseh: needs $CLANG and $CC - skipping" >&2
    exit 0
fi
if [ ! -f ../kernel32/libkernel32.a ] || [ ! -f ../ntdll/libntdll.a ]; then
    echo "winseh: build ntdll and kernel32 first" >&2
    exit 1
fi

$CLANG --target=x86_64-w64-windows-gnu -fms-extensions -fasync-exceptions \
       -std=gnu99 -Wall -Wextra -Wno-unused-function -O1 -ffreestanding \
       -fno-builtin -fno-stack-protector -mno-stack-arg-probe \
       -c seh.c -o seh.o

$CC -nostdlib -nostartfiles -Wl,--entry=start -Wl,--image-base,0x140000000 \
    -Wl,--subsystem,console -o seh.exe seh.o \
    ../kernel32/libkernel32.a ../ntdll/libntdll.a

# The claim this binary makes: it really uses table-based SEH.
if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
    if ! x86_64-w64-mingw32-objdump -p seh.exe | grep -q "__C_specific_handler"; then
        echo "winseh: seh.exe does not import __C_specific_handler" >&2
        exit 1
    fi
fi

cp seh.exe "$ROOT/root/bin/seh.exe"
echo "built root/bin/seh.exe (clang SEH; imports kernel32.dll, ntdll.dll)"
