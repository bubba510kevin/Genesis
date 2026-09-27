#!/bin/sh
# Build the ring-3 programs in src/ and stage them onto the data disk.
#
# Separate from build.py on purpose. These are userspace ELF binaries with a
# completely different set of flags from the kernel - no -mcmodel=kernel, no
# KERNEL_VMA link address - and folding them into the kernel build would mean
# one script maintaining two contradictory toolchain configurations.
#
# The flags that matter, and why:
#   -static -no-pie   the ELF loader handles ET_EXEC with fixed vaddrs. A PIE
#                     is ET_DYN and needs relocation the loader does not do
#                     yet, and it links at address 0, which fails the
#                     ELF_USER_LIMIT checks in a confusing way.
#   -nostdlib         there is no libc on the volume. Every syscall in these
#                     programs is a raw `syscall` instruction.
#   -nostartfiles     the entry point is _start, called with the stack the
#                     kernel built. No crt1.o, no __libc_start_main.
#   -mno-red-zone     signal delivery writes below RSP; the kernel skips 128
#                     bytes for the red zone, but not relying on it is free.
#   -fno-builtin      stops gcc turning a loop into a memcpy call that does
#                     not exist here.
set -e

cd "$(dirname "$0")/.."

CC=${CC:-gcc}
CFLAGS="-std=c99 -Wall -Wextra -O1 -static -no-pie -nostdlib -nostartfiles \
        -ffreestanding -fno-stack-protector -fno-builtin -mno-red-zone"

DISK_IMG=build/disk.img
MNT=${MNT:-/mnt/genesis}

mkdir -p root/bin

# The freestanding programs. ls.c is NOT here any more - it moved to musl, and
# is built below with the other libc-linked binaries.
#
# systest STAYS freestanding, deliberately and permanently. It tests the
# syscall interface, and a test that reaches the kernel through libc cannot
# tell a kernel bug from a libc workaround: musl retries on EINTR, translates
# errnos, and emulates calls the kernel does not have. All three are exactly
# what a syscall test must not do. Two programs, two reasons, and they do not
# converge.
#
# utlib.c is gone. It was the beginning of a from-scratch libc (libgenesis) and
# it never became one - an empty struct, a FILE_t that was never defined, and a
# function with no return statement. Nothing included it and it would not have
# compiled if anything had. musl works; the stub was a placeholder for a
# decision that has now been made the other way.
for src in src/hello.c src/systest.c src/verif.c src/mkprobe.c; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    # 8.3 on the FAT volume: names longer than eight characters are not
    # reachable through the kernel's path lookup, so catch it here rather
    # than as a mysterious ENOENT at runtime.
    if [ ${#name} -gt 8 ]; then
        echo "$name: too long for 8.3, rename it" >&2
        exit 1
    fi
    $CC $CFLAGS -o "root/bin/$name" "$src"
    echo "built root/bin/$name"
done

# --- the musl-linked ELF --------------------------------------------------
#
# mhello is an ORDINARY C program - stdio, malloc, time, no syscall numbers,
# no _start - linked against musl. It is the other half of the pair with
# src/hello.c above: that one proves the kernel's syscall entry works, this
# one proves a real libc's assumptions about the kernel hold.
#
# STATIC, and nothing musl gets staged onto the volume.
#
# That is worth saying plainly because it is the opposite of the intuition.
# There is no libc.so on the disk and there does not need to be: -static
# copies everything the program uses out of libc.a and into the binary, so
# what lands on the volume is one self-contained file. A DYNAMIC musl binary
# would need /lib/ld-musl-x86_64.so.1 staged AND an ELF dynamic linker in the
# kernel to run it - and that second one does not exist yet (roadmap item 7,
# the ELF half). Staging the interpreter without it would turn a clean
# "toolchain missing" into an exec that fails inside the kernel for reasons
# that look nothing like the cause.
#
# The toolchain is looked for in four places before giving up, because the
# most likely reason this is skipped is that musl is installed under a name
# the script did not think of:
#
#   $MUSL_CC              - set it and nothing below is consulted
#   musl-gcc              - what Debian/Alpine's musl-tools installs
#   x86_64-linux-musl-gcc - what a cross toolchain calls itself
#   $MUSL_SRC             - a musl SOURCE tree, built once into build/musl
#
# The last one is why having the tarball is enough. musl builds with a stock
# host gcc in about a minute and needs no cross compiler, and the wrapper it
# installs IS the toolchain - which is the whole reason no custom triplet is
# required here: Genesis's ELF personality is the Linux x86-64 ABI, so
# x86_64-linux-musl is already the right target.
MUSL_CC_FOUND=""
if [ -f src/hello_musl.c ]; then

    if [ -n "$MUSL_CC" ] && command -v "$MUSL_CC" >/dev/null 2>&1; then
        MUSL_CC_FOUND="$MUSL_CC"
    elif command -v musl-gcc >/dev/null 2>&1; then
        MUSL_CC_FOUND=musl-gcc
    elif command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
        MUSL_CC_FOUND=x86_64-linux-musl-gcc
    else
        # Look for a source tree to build from. $MUSL_SRC wins; otherwise the
        # two places a tarball usually gets unpacked to.
        for d in "$MUSL_SRC" third_party/musl ../musl; do
            [ -n "$d" ] || continue
            [ -f "$d/configure" ] && [ -f "$d/VERSION" ] || continue
            MUSL_SRC_FOUND="$d"
            break
        done

        if [ -n "$MUSL_SRC_FOUND" ]; then
            MUSL_PREFIX="$(pwd)/build/musl"
            # Absolute, resolved BEFORE the subshell cds away from here. A
            # relative source path evaluated after the cd finds nothing, and
            # configure's failure message would be about the wrong directory.
            MUSL_SRC_ABS="$(cd "$MUSL_SRC_FOUND" && pwd)"

            # Cached on the wrapper's existence: musl takes about a minute to
            # build and nothing in Genesis changes it, so rebuilding it on
            # every staging run would make this script feel broken. Delete
            # build/musl to force it again.
            if [ ! -x "$MUSL_PREFIX/bin/musl-gcc" ]; then
                echo "musl: building $MUSL_SRC_ABS once into build/musl (~1 min)"
                # Out of tree, so the source you unpacked stays clean and two
                # Genesis checkouts do not fight over one set of objects.
                mkdir -p build/musl-obj
                ( cd build/musl-obj &&
                  "$MUSL_SRC_ABS/configure" --disable-shared \
                      --prefix="$MUSL_PREFIX" >/dev/null &&
                  make -j"$(nproc 2>/dev/null || echo 2)" >/dev/null &&
                  make install >/dev/null ) || {
                    echo "musl: build failed - see build/musl-obj" >&2
                    exit 1
                }
            fi
            MUSL_CC_FOUND="$MUSL_PREFIX/bin/musl-gcc"
        fi
    fi

    if [ -z "$MUSL_CC_FOUND" ]; then
        echo "mhello: no musl toolchain - skipping (set MUSL_CC or MUSL_SRC)" >&2
    else
        # -static -no-pie for the same reason as the raw programs above, and
        # -no-pie is NOT redundant with -static: a gcc configured with
        # --enable-default-pie produces a static-PIE, which is ET_DYN, links
        # at address 0 and needs relocation this kernel's ELF loader does not
        # do. It loads, jumps to address zero-ish and dies with no hint that
        # the problem was a linker default.
        # -std=c11 with _POSIX_C_SOURCE, not the -std=c99 the raw programs
        # use. nanosleep, gmtime_r and environ are POSIX rather than ISO C,
        # and a strict-C99 musl header set hides them - which does not fail
        # the build, it produces an IMPLICIT declaration: int return, no
        # prototype, arguments passed however the call site happens to look.
        # nanosleep taking two pointers survives that by luck; a function
        # returning a pointer would not.
        "$MUSL_CC_FOUND" -std=c11 -D_POSIX_C_SOURCE=200809L \
            -Wall -Wextra -Werror=implicit-function-declaration \
            -O1 -static -no-pie \
            -o root/bin/mhello src/hello_musl.c

        # Verify what actually came out rather than trusting the flags. Three
        # bytes of the ELF header answer both questions that matter, and
        # checking here turns a confusing runtime failure into a build error
        # that names the cause.
        #
        #   e_type at offset 16: 02 00 is ET_EXEC, 03 00 is ET_DYN (PIE)
        #   a PT_INTERP program header means it wants a dynamic linker
        if [ "$(od -A n -t x1 -j 16 -N 2 root/bin/mhello | tr -d ' ')" != "0200" ]; then
            echo "mhello: not ET_EXEC - a static-PIE slipped through" >&2
            exit 1
        fi
        if command -v readelf >/dev/null 2>&1 &&
           readelf -l root/bin/mhello 2>/dev/null | grep -q INTERP; then
            echo "mhello: wants a dynamic linker - the ELF rtld does not exist yet" >&2
            exit 1
        fi
        echo "built root/bin/mhello (musl, static, $("$MUSL_CC_FOUND" -dumpversion 2>/dev/null))"
    fi
fi

# --- the dynamic linker ---------------------------------------------------
#
# Built before anything that might be linked against it, and staged to
# root/lib. Its own script, because its flags share nothing with any other
# target here - see the commentary in it.
if [ -f src/rtld/build.sh ]; then
    sh src/rtld/build.sh
fi

# --- ls, on musl ----------------------------------------------------------
#
# Built with the same toolchain search as mhello above, and STATIC by default
# for the same reason: static is what proves the program is correct, and
# dynamic is what proves the loader is. Keeping them separate means a broken
# rtld does not remove ls from the volume.
#
# Set GENESIS_DYNAMIC=1 to build it dynamic instead. That is the run that
# exercises the whole of item 2 end to end - PT_INTERP in the kernel,
# ELF_INTERP_BASE, AT_BASE, and every relocation type in rtld.c - against a
# program whose correct output you can check by eye.
if [ -f src/ls.c ] && [ -n "$MUSL_CC_FOUND" ]; then
    if [ "$GENESIS_DYNAMIC" = "1" ]; then
        "$MUSL_CC_FOUND" -std=c11 -D_POSIX_C_SOURCE=200809L \
            -Wall -Wextra -Werror=implicit-function-declaration -O1 \
            -Wl,--dynamic-linker=/lib/ld-gen.so \
            -o root/bin/ls src/ls.c
        echo "built root/bin/ls (musl, DYNAMIC - needs the rtld on the volume)"
    else
        "$MUSL_CC_FOUND" -std=c11 -D_POSIX_C_SOURCE=200809L \
            -Wall -Wextra -Werror=implicit-function-declaration -O1 \
            -static -no-pie -o root/bin/ls src/ls.c
        echo "built root/bin/ls (musl, static)"
    fi
elif [ -f src/ls.c ]; then
    echo "ls: no musl toolchain - skipping (it needs a libc now)" >&2
fi

# ntdll.dll, built with MinGW-w64 as a host tool and staged into
# /wsr/System32. Skips itself with a message if MinGW is not installed, so
# this script still works on a machine that only builds the Linux side.
if [ -f src/ntdll/build.sh ]; then
    sh src/ntdll/build.sh
fi

# kernel32.dll, which imports ntdll - so it follows the ntdll build for the
# same reason, and everything below follows this one.
if [ -f src/kernel32/build.sh ]; then
    sh src/kernel32/build.sh
fi

# The MinGW-built PE that imports ntdll. Must follow the ntdll build, since
# it links against the import library that build produces.
if [ -f src/winhello/build.sh ]; then
    sh src/winhello/build.sh
fi

# The PE that imports kernel32 and nothing else. Last of the Windows-side
# builds, because it depends on the import library kernel32's build emits -
# and because it is the one whose failure means the most: it is the only
# image here that makes the loader resolve a dependency of a dependency.
# The loadable kernel module. Built with the KERNEL's flags, not userspace's -
# it runs in the kernel - and staged into /boot/kernel for kldload.c to find.
if [ -f src/kmod/build.sh ]; then
    sh src/kmod/build.sh
fi

# sync.exe, the ring-3 check for the NT dispatcher objects. After winhello,
# because it depends on everything winhello proves - if the loader or the
# import table is broken, that one says so first and this one only says so
# again with more noise.
if [ -f src/winsync/build.sh ]; then
    sh src/winsync/build.sh
fi

if [ -f src/k32demo/build.sh ]; then
    sh src/k32demo/build.sh
fi

# thr.exe and smp.exe: Win32 threads, and the multiprocessor from Win32 -
# affinity, processor queries, and synchronisation under real parallelism.
if [ -f src/winthread/build.sh ]; then
    sh src/winthread/build.sh
fi
if [ -f src/winsmp/build.sh ]; then
    sh src/winsmp/build.sh
fi
if [ -f src/wintls/build.sh ]; then
    sh src/wintls/build.sh
fi
# wait.exe: WaitForMultipleObjects, alertable waits and APCs (item 14(b)).
if [ -f src/winwait/build.sh ]; then
    sh src/winwait/build.sh
fi

# hand.exe, the bisect anchor: no imports, no ntdll, no CRT, so when
# hello.exe breaks this says whether the loader broke or the linking did.
#
# Two producers, and which one runs matters enough to be explicit about.
#
#   src/hand/hand.S  - assembled and linked by MinGW. Preferred. This file
#                      contains only what is actually Genesis's own: the
#                      syscall numbers (included from the generated header,
#                      not repeated), the register convention and the
#                      structure shapes. The section layout is computed by
#                      tools that have been getting it right for decades.
#
#   src/mkpe.py      - emits the whole PE byte by byte, headers included.
#                      Needs no toolchain at all, which is the reason to keep
#                      it: it is the only image buildable on a machine with
#                      nothing installed. It is also the one that failed -
#                      its hardcoded raw file offsets stopped matching a
#                      .text that had grown, so it described a .rdata
#                      beginning inside .text and the loader faithfully
#                      mapped the lie. Its offsets are derived now and it
#                      asserts before writing, so it is sound again.
#
# The two print different banners on purpose - "a hand-written PE" is the
# assembled one - so a screenshot always says which was staged. Whichever ran
# last wins, so the fallback runs only when MinGW is genuinely absent.
if [ -f src/hand/build.sh ]; then
    sh src/hand/build.sh
fi
if [ ! -f root/bin/hand.exe ] && [ -f src/mkpe.py ]; then
    echo "hand: falling back to the byte-by-byte emitter (no MinGW)" >&2
    python3 src/mkpe.py root/bin/hand.exe
fi

# --- staging onto the volume ---------------------------------------------
#
# Everything below is what makes a rebuild visible to the kernel, and it is
# where a rebuilt binary can silently fail to arrive. Three things had to
# change:
#
# 1. TRAILING SLASH ON THE SOURCE. `rsync src/root /mnt` copies the DIRECTORY,
#    producing /root/bin/systest on the volume; /bin/systest - the path ash
#    actually execs - is never touched and stays at whatever was staged when
#    disk.img was first created. `rsync root/ /mnt/` copies the CONTENTS,
#    which is what was meant. This is a one-character bug that looks exactly
#    like "my change did not take effect".
#
# 2. RELATIVE PATH. Absolute ~/git/Genesis/root stages one particular checkout
#    no matter which tree the script was run from. The cd above already puts
#    us at the top of the right one.
#
# 3. CHECK THE MOUNT. Without it, a failed mount means rsync writes into the
#    host's own /mnt/genesis directory and reports success - which is
#    indistinguishable from a good run until the kernel boots the old binary.
#
# -rt rather than -a: the archive flag tries to preserve ownership, groups and
# permissions, none of which exist on FAT, so every file produces an error
# that --ignore-errors was there to paper over. Times and recursion are all
# this filesystem can carry anyway.
#
# No --delete: the point of a separate data disk is that you can drop files
# onto it from the host, and this script does not know which of them matter.

if [ ! -f "$DISK_IMG" ]; then
    echo "$DISK_IMG missing - run ./build.py disk first" >&2
    exit 1
fi

sudo mkdir -p "$MNT"
sudo mount -o loop,uid=$(id -u),gid=$(id -g) "$DISK_IMG" "$MNT"

# Unmount even if the copy fails, so a bad run does not leave the image
# mounted and the next one fail with "already mounted" instead of the real
# error.
trap 'sudo umount "$MNT" 2>/dev/null || true' EXIT INT TERM

if ! mountpoint -q "$MNT"; then
    echo "$MNT is not a mount point - refusing to stage into the host tree" >&2
    exit 1
fi

rsync -rt --no-perms --no-owner --no-group root/ "$MNT"/
sync

echo "staged root/ onto $DISK_IMG"

# --- what the volume can actually hold ------------------------------------
#
# Last, after everything is staged, because it checks the tree rather than any
# one build step.
#
# This exists because the rtld was first staged as ld-genesis-x86_64.so.1 -
# the musl convention - and FAT16 with no long-filename support cannot hold
# that name. fatfs.py refused it, correctly, but at image-build time: three
# steps and one tool removed from the line that chose the name. Failing here
# names the file and the fix.
#
# It also catches case collisions, which are the more dangerous half. 8.3
# names fold to upper case, so System32 and system32 are ONE directory entry -
# and that has already happened once in /wsr. Nothing reports it: the two
# directories merge, and the tree on the volume stops matching the tree on
# disk with no error anywhere.
if [ -f tools/check_root.py ]; then
    python3 tools/check_root.py root || exit 1
fi
