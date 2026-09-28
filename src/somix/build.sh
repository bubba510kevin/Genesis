#!/bin/sh
# Build libmixa.so and libmixb.so: Linux shared objects a Windows program
# loads (ROADMAP item 19, stage 3). Staged to /lib.
#
# Freestanding on purpose: no libc means no %fs thread pointer and no musl
# startup, neither of which a Windows process provides. -fno-stack-protector
# for the same reason (the canary is read through %fs).
set -e

cd "$(dirname "$0")"
ROOT=../..
CC=${CC:-gcc}

CFLAGS="-O2 -fPIC -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-asynchronous-unwind-tables -Wall -Wextra"

$CC $CFLAGS -shared -nostdlib -Wl,-soname,libmixb.so -o libmixb.so libmixb.c
$CC $CFLAGS -shared -nostdlib -Wl,-soname,libmixa.so -o libmixa.so libmixa.c \
    -L. -l:libmixb.so

# The claims the test makes about the relocations, checked here so a
# compiler that optimised one of them away fails the build, not the test.
if command -v readelf >/dev/null 2>&1; then
    for r in R_X86_64_RELATIVE R_X86_64_64 R_X86_64_GLOB_DAT R_X86_64_JUMP_SLO; do
        readelf -r libmixa.so | grep -q "$r" || {
            echo "somix: libmixa.so has no $r relocation" >&2
            exit 1
        }
    done
    readelf -d libmixa.so | grep -q "NEEDED.*libmixb.so" || {
        echo "somix: libmixa.so does not name libmixb.so" >&2
        exit 1
    }
fi

mkdir -p "$ROOT/root/lib"
cp libmixa.so libmixb.so "$ROOT/root/lib/"
echo "built root/lib/libmixa.so, root/lib/libmixb.so"
