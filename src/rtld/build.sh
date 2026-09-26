#!/bin/sh
# Build the ELF dynamic linker and stage it to /lib.
#
# Separate from the rest of build_user.sh because the flags are unlike any
# other target in the tree, and every one of them is load-bearing.
#
#   -shared -fPIC    the interpreter is an ET_DYN loaded at a base the kernel
#                    chooses (ELF_INTERP_BASE). A non-PIC build has absolute
#                    addresses that no bias can fix.
#
#   -Bsymbolic       bind this object's own references to its own definitions
#                    at LINK time. Without it, rtld_start's call to lookup()
#                    goes through a PLT slot that nothing has filled in - the
#                    linker would need itself to be linked. This flag is what
#                    makes the bootstrap terminate.
#
#   -z norelro       RELRO asks the interpreter to mprotect its own GOT after
#                    relocation. There is nobody to do that for the
#                    interpreter itself, and the kernel's mprotect cannot drop
#                    write on mapped text yet anyway. Asking for a protection
#                    that will not be applied is worse than not asking.
#
#   --no-undefined   an undefined symbol here is a call into a libc that does
#                    not exist at this point in startup. Catching it at link
#                    time turns it into a build error instead of a jump to
#                    zero inside the loader, which is the least debuggable
#                    place in the system.
#
#   -nostdlib        there is no libc for the thing that loads libc.
#
#   -fno-builtin     gcc turns a byte-copy loop into a call to memcpy. There
#                    is no memcpy here, and in self_relocate there could not
#                    be one - it would be a call through a GOT slot that has
#                    not been relocated yet. This flag is the difference
#                    between the loader working and it faulting on its own
#                    first instruction.
#
#   -fno-stack-protector
#                    the guard value is read from the TLS block, and there is
#                    no TLS until the program the linker is about to load sets
#                    one up.
set -e

cd "$(dirname "$0")/../.."

CC=${CC:-gcc}
# 8.3, because the volume is FAT16 with no LFN support. ld-genesis-x86_64.so.1
# has a seventeen-character stem and two dots; fatfs.py refuses it, which is
# how this was found. See the note in rtld.c.
OUT=root/lib/ld-gen.so

mkdir -p root/lib build

$CC -c -fPIC -O1 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector \
    -Wall -Wextra -Werror=implicit-function-declaration \
    -o build/rtld.o src/rtld/rtld.c

$CC -c -o build/rtld_start.o src/rtld/start.S

ld -shared -Bsymbolic -z norelro --no-undefined \
   -o "$OUT" build/rtld_start.o build/rtld.o

# Verify what came out rather than trusting the flags, the same way the mhello
# build checks its own ELF header. Three properties, and each has failed at
# least once during development:
#
#   ET_DYN            a non-PIC link produces ET_EXEC, which the kernel's
#                     elf_load_biased refuses - correctly, and with an error
#                     that says TYPE when the cause is a missing -shared.
#   no PT_INTERP      an interpreter that needs an interpreter is a loop the
#                     kernel refuses, one level down from here.
#   no UNDEF symbols  --no-undefined should have caught it; checking the
#                     output as well is what catches the case where somebody
#                     removes the flag to make a build pass.
if [ "$(od -A n -t x1 -j 16 -N 2 "$OUT" | tr -d ' ')" != "0300" ]; then
    echo "rtld: not ET_DYN - the -shared link did not take" >&2
    exit 1
fi
if command -v readelf >/dev/null 2>&1; then
    if readelf -l "$OUT" 2>/dev/null | grep -q INTERP; then
        echo "rtld: has a PT_INTERP of its own" >&2
        exit 1
    fi
    if readelf --dyn-syms -W "$OUT" 2>/dev/null | awk '$7 == "UND" && $8 != ""' | grep -q .; then
        echo "rtld: undefined symbols remain" >&2
        readelf --dyn-syms -W "$OUT" | awk '$7 == "UND" && $8 != ""' >&2
        exit 1
    fi
fi

echo "built $OUT"
