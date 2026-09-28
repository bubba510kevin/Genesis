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

# libmixc.so: a .so that USES libc (malloc, snprintf, strtol, libm, errno),
# loaded into a Windows process (item 19, the GNTlibc step). Unlike libmixa/b
# it is NOT freestanding - it is linked against GNTlibc's libc.so, and its
# DT_NEEDED of "libc.so" is what makes the kernel's .so loader pull libc in.
#
# Built only when build_user.sh found a GNTlibc checkout and exported
# GNTLIBC_OUT (its libc.so and headers). Skipped otherwise, and mix.exe's
# libc_tests notice the absence and drop those checks.
if [ -n "$GNTLIBC_OUT" ] && [ -f "$GNTLIBC_OUT/libc.so" ]; then
    # -nostdinc + the musl headers, and link against libc.so by -l:libc.so so
    # the NEEDED is the bare "libc.so" the loader resolves in /lib (libc.so
    # carries no SONAME of its own beyond what GNTlibc's build gives it).
    # -fno-stack-protector: the canary is read through %fs, which is not set up
    # until libc.so's constructor runs - the .so's own code must not assume it.
    $CC -O2 -fPIC -nostdinc -isystem "$GNTLIBC_OUT/include" \
        -fno-stack-protector -Wall -Wextra \
        -shared -Wl,-soname,libmixc.so -Wl,--no-undefined \
        -o libmixc.so libmixc.c -L"$GNTLIBC_OUT" -l:libc.so

    if command -v readelf >/dev/null 2>&1; then
        readelf -d libmixc.so | grep -q "NEEDED.*libc.so" || {
            echo "somix: libmixc.so does not name libc.so" >&2
            exit 1
        }
    fi
    cp libmixc.so "$ROOT/root/lib/"
    echo "built root/lib/libmixc.so (uses libc, needs GNTlibc's libc.so)"
else
    echo "somix: no GNTLIBC_OUT - skipping libmixc.so (the libc-using object)" >&2
fi
