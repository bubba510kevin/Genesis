#!/bin/sh
# Rebuild genesispool.dat.gz - the ZPL version 1 fixture.
#
# Version 1 on purpose: its attributes live in a znode_phys_t bonus buffer at
# fixed offsets rather than in the System Attribute registry, and its ACL is
# in that buffer's zp_acl too. That is the format no modern pool uses and the
# one nothing else in the tree exercises. See tests/host/fixtures/README.md.
#
# Needs no root and no ZFS kernel module: libzpool is OpenZFS's own code built
# for userland, which is how ztest works.
set -e

WORK=${WORK:-build/aclwork}
OUT=${OUT:-tests/host/fixtures/genesispool.dat.gz}

mkdir -p "$WORK/inc"
ln -sfn /usr/include/libzpool "$WORK/inc/sys"
gcc -o "$WORK/mkzpl" tests/host/mkzpl.c \
    -I/usr/include/libzfs -I/usr/include/libspl -I"$WORK/inc" \
    -D_GNU_SOURCE -lzpool -lnvpair -lzfs_core -luutil

# A stale cache file makes spa_create fail with EEXIST on a pool name it has
# seen before, which reads as "the pool already exists" for a pool that does
# not.
rm -f /etc/zfs/zpool.cache 2>/dev/null || true
rm -f "$WORK/v1pool.img"
truncate -s 128M "$WORK/v1pool.img"
"$WORK/mkzpl" "$(readlink -f "$WORK/v1pool.img")" genesispool

gzip -9 -c "$WORK/v1pool.img" > "$OUT"
ls -la "$OUT"
echo "done - remember to remove build/zfs.img so build.py unpacks the new one"
