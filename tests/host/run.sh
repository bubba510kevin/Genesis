#!/bin/sh
# Compile pmm.c and paging.c for the host and run tests/host/vmm_test.c.
#
# Two mechanical rewrites make that possible, both confined to a scratch copy
# so the kernel sources stay untouched:
#
#   1. KERNEL_VMA and PHYSMAP_BASE become 0, so a physical address IS a host
#      address and the harness can mmap a region to stand in for RAM.
#      KERNEL_MAP_SIZE is widened to cover that region, since the direct map's
#      bootstrap frames have to come from inside it.
#
#   2. Inline asm is stripped. invlpg, the CR3 reload and the CR3 load in
#      paging_init have no meaning here; the harness tests the bookkeeping
#      around them, not the CPU.
#
# The point of the rewrite-a-copy approach rather than #ifdefs is that the
# kernel builds with no test scaffolding in it at all.
set -e

cd "$(dirname "$0")/../.."
OUT=build/hosttest
rm -rf "$OUT"
mkdir -p "$OUT/include"

cp kernel/include/*.h "$OUT/include/"
# Flattened into $OUT deliberately: the harness compiles each of these as a
# standalone translation unit against $OUT/include, so the directory a file
# came from is not part of its identity here.
#
# These paths used to be kernel/*.c and every one of them was wrong - the
# kernel was reorganised into mm/, dev/, fs/, proc/, obj/, exec/ and arch/,
# and `cp` failing on twenty-two files is not an error this script stops on.
# It kept going and compiled whatever was left, so the suite reported on a
# subset without saying which subset. Worth stating rather than just fixing:
# a test harness that silently tests less than it claims is worse than one
# that does not run.
cp kernel/mm/pmm.c kernel/mm/paging.c kernel/arch/e820.c kernel/dev/keyboard.c \
   kernel/dev/tty.c \
   kernel/proc/waitq.c kernel/fs/pipe.c kernel/exec/pe.c kernel/fs/fat.c \
   kernel/fs/path.c kernel/obj/object.c kernel/mm/kstack.c kernel/obj/ns.c \
   kernel/dev/devices.c kernel/exec/ntproc.c kernel/dev/rtc.c kernel/dev/part.c \
   kernel/dev/device.c kernel/fs/bcache.c kernel/dev/volume.c kernel/fs/vfs.c \
   kernel/fs/fatfs.c kernel/fs/fileobj.c kernel/fs/pcache.c kernel/fs/acl.c kernel/fs/ntsec.c \
   kernel/gnfs/gnfs_format.c kernel/gnfs/gnfs_object.c kernel/gnfs/gnfs_vfs.c "$OUT/"

# Anchored on the MACRO NAME, not on the value it currently has.
#
# These used to spell out the value - KERNEL_MAP_SIZE 0x00400000ULL - and the
# kernel then grew its window to 0x01000000ULL. sed does not fail when a
# pattern does not match; it succeeds and changes nothing. So the harness went
# on building the direct map into a window a quarter the size it needed, and
# reported three failures in paging.c that had nothing to do with paging.c.
#
# A rewrite that silently does not apply is the same bug as a `cp` that
# silently does not copy, and it is worth fixing the same way: match the thing
# that is stable (the name) rather than the thing that is not (the value).
sed -i \
  -e 's/^#define KERNEL_VMA .*/#define KERNEL_VMA 0ULL/' \
  -e 's/^#define PHYSMAP_BASE .*/#define PHYSMAP_BASE 0ULL/' \
  -e 's/^#define KERNEL_MAP_SIZE .*/#define KERNEL_MAP_SIZE 0x44000000ULL/' \
  "$OUT/include/paging.h"
grep -q '#define KERNEL_MAP_SIZE 0x44000000ULL' "$OUT/include/paging.h" || {
    echo "run.sh: the paging.h rewrite did not apply - the macros moved" >&2
    exit 1
}

# Kernel stacks are mapped at a real kernel virtual address, which on the host
# is not a valid pointer - kstack_alloc zeroes the stack through it. Point the
# region at a scratch area the harness mmaps and the PMM never hands out
# (it is outside every usable E820 entry), so the mapping bookkeeping is
# exercised without the zeroing writing somewhere fatal.
sed -i -e 's/^#define KSTACK_BASE .*/#define KSTACK_BASE     0x50000000ULL/' \
  "$OUT/include/kstack.h"
grep -q '#define KSTACK_BASE     0x50000000ULL' "$OUT/include/kstack.h" || {
    echo "run.sh: the kstack.h rewrite did not apply - KSTACK_BASE moved" >&2
    exit 1
}

# Every __asm__ statement in paging.c is a single line; drop them and give
# flush_tlb's local a use so -Wall stays quiet.
# typesk.h defines size_t and NULL, which the host's own headers also define.
sed -i \
  -e 's/typedef uintptr size_t;/typedef uintptr kernel_size_t;/' \
  -e 's|#define NULL ((void\*)0)|#ifndef NULL\n#define NULL ((void*)0)\n#endif|' \
  "$OUT/include/typesk.h"

# Every __asm__ statement in paging.c is a single line; drop them and give
# the arguments they consumed a use so -Wall stays quiet.
# The EFER write in paging_enable_nx goes the same way. It is a wrmsr called
# through a static inline in io.h rather than an __asm__ line in this file, so
# the blanket strip above does not catch it - and a wrmsr executed in a user
# process is a #GP, not a failed test. Deleting the write leaves the flag it
# sets, which is the half the tests are about.
sed -i \
  -e '/__asm__ volatile/d' \
  -e '/wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);/d' \
  -e 's/^static void invlpg(virt_addr_t virt_addr) {$/static void invlpg(virt_addr_t virt_addr) { (void)virt_addr;/' \
  -e 's/^    uint64 cr3;$/    uint64 cr3 = 0; (void)cr3;/' \
  "$OUT/paging.c"

# waitq.c: the same treatment. The wait loop's "sti; hlt; cli" is privileged
# and meaningless here - stripping it turns the blocking wait into a spin,
# which is fine because every test feeds a complete line before reading.
# (This used to be in keyboard.c; the loop moved to waitq.c and the stripping
# has to follow it, or the host build assembles a privileged instruction.)
sed -i -e '/__asm__ volatile/d' "$OUT/waitq.c"

# Build the fixture the FAT tests read. Staged from a tree created here rather
# than from root/ in the repo, so the assertions can depend on exact sizes and
# contents instead of on whatever happens to be checked in.
FIX="$OUT/fixture"
mkdir -p "$FIX"/bin "$FIX"/sbin "$FIX"/usr/bin "$FIX"/usr/local/bin \
         "$FIX"/wsr/system32 "$FIX"/etc
printf 'hello from /etc/motd\n' > "$FIX/etc/motd"
: > "$FIX/etc/empty"
printf 'small\n' > "$FIX/usr/bin/tiny"
python3 - "$FIX" <<'PY'
import sys
d = sys.argv[1]
open(d + "/bin/busybox", "wb").write(bytes(range(256)) * 600)
open(d + "/wsr/system32/kernel32.dll", "wb").write(b"W" * 9000)
PY
python3 tools/fatfs.py --format "$OUT/test.img" 32 >/dev/null
python3 tools/fatfs.py "$OUT/test.img" "$FIX" >/dev/null

# Partition-table fixtures. Generated rather than committed as binaries, so the
# expected values in part_test.c can be read against the script that produced
# them instead of against a hexdump - and so the GPT checksums are computed by
# a different implementation (Python's zlib) than the one under test, which is
# the only way a CRC bug is visible at all.
mkdir -p "$OUT/imgs"
python3 tests/host/mkimg.py "$OUT/imgs"

gcc -std=c99 -Wall -Wextra -g -no-pie -fno-pie \
    -I"$OUT/include" -Itests/host \
    "$OUT/pmm.c" "$OUT/paging.c" "$OUT/e820.c" "$OUT/keyboard.c" "$OUT/tty.c" "$OUT/waitq.c" "$OUT/pipe.c" "$OUT/pe.c" "$OUT/fat.c" "$OUT/path.c" "$OUT/object.c" "$OUT/kstack.c" "$OUT/ns.c" "$OUT/devices.c" "$OUT/ntproc.c" "$OUT/rtc.c" "$OUT/part.c" "$OUT/device.c" "$OUT/bcache.c" \
    "$OUT/volume.c" "$OUT/vfs.c" "$OUT/fatfs.c" "$OUT/fileobj.c" "$OUT/pcache.c" "$OUT/acl.c" "$OUT/ntsec.c" \
    "$OUT/gnfs_format.c" "$OUT/gnfs_object.c" "$OUT/gnfs_vfs.c" \
    tests/host/vmm_test.c tests/host/kbd_test.c tests/host/fat_test.c \
    tests/host/path_test.c tests/host/object_test.c tests/host/kstack_test.c \
    tests/host/ns_test.c tests/host/pipe_test.c tests/host/pe_test.c \
    tests/host/ntproc_test.c tests/host/rtc_test.c \
    tests/host/part_test.c tests/host/dev_stub.c tests/host/time_stub.c \
    tests/host/kernel_stub.c tests/host/fat_write_test.c \
    tests/host/volume_test.c tests/host/bcache_test.c \
    tests/host/gnfs_test.c tests/host/gnfs_fixture.c \
    -o "$OUT/vmm_test"

# A separate, writable copy of the FAT fixture for the write tests. Made by
# copying rather than by formatting a second image, so the two are byte
# identical at the start and any difference the write tests see is theirs.
cp "$OUT/test.img" "$OUT/test-rw.img"

"$OUT/vmm_test" "$OUT/test.img" "$OUT/imgs" "" "$OUT/test-rw.img"

# The gnfs ACL fixture the guest mounts as /mnt/d (tests/host/gnfs_fixture.c).
# Committed gzipped so build.py needs nothing but the tree, and REGENERATED
# here by the kernel's own gnfs code and compared byte for byte - so the
# committed image cannot drift from what the current gnfs would write. After
# an intentional format change, refresh it with UPDATE_FIXTURES=1.
"$OUT/vmm_test" --gnfs-fixture "$OUT/gnfsfix.img"
if [ "${UPDATE_FIXTURES:-0}" = 1 ]; then
    gzip -9 -n -c "$OUT/gnfsfix.img" > tests/host/fixtures/gnfsfix.img.gz
    echo "gnfs fixture: tests/host/fixtures/gnfsfix.img.gz refreshed"
fi
gunzip -c tests/host/fixtures/gnfsfix.img.gz > "$OUT/gnfsfix.committed" || {
    echo "gnfs fixture: tests/host/fixtures/gnfsfix.img.gz is missing -" \
         "run with UPDATE_FIXTURES=1" >&2
    exit 1
}
cmp -s "$OUT/gnfsfix.img" "$OUT/gnfsfix.committed" || {
    echo "gnfs fixture: the committed image differs from what gnfs writes" \
         "now - rerun with UPDATE_FIXTURES=1 if the change is intended" >&2
    exit 1
}
echo "gnfs fixture: committed image matches the kernel's gnfs output"

# What FAT16 can actually represent, checked against root/ BEFORE an image is
# built from it. ROADMAP item 10: the /wsr System32-vs-system32 collision
# already happened once and was silent, because 8.3 names are uppercased on
# write and the second one staged simply replaced the first. A run-time check
# cannot see that - by then one of the two names is gone - which is why this
# one runs here.
python3 tests/host/check_staged_tree.py
