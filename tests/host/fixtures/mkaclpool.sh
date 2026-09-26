#!/bin/sh
# Build genesisacl.dat.gz - the ZPL v5 fixture with a non-trivial ACL.
#
# Needs root (zpool create) and a host with OpenZFS installed. See
# tests/host/fixtures/README.md for what is on the resulting pool and why.
#
# The layout deliberately MIRRORS genesispool.dat: /etc/motd exists on both,
# because systest's fs_checks() runs the same assertions against every mounted
# volume and a pool missing the file it looks for fails as a filesystem bug
# rather than as the fixture mismatch it is.
set -e

WORK=${WORK:-build/aclwork}
POOL=genesisacl
IMG=$WORK/pool.img
OUT=${OUT:-tests/host/fixtures/genesisacl.dat.gz}
MNT=/mnt/$POOL

mkdir -p "$WORK"
zpool destroy "$POOL" 2>/dev/null || true
rm -f "$IMG"
truncate -s 192M "$IMG"

# -d creates the pool with NO features enabled. The vendored reader gates on
# eighteen features_for_read and refuses anything else; without -d the pool
# gets whatever this host's OpenZFS enables by default and will not mount.
zpool create -d -o ashift=9 \
    -O acltype=nfsv4 -O aclmode=passthrough -O aclinherit=passthrough \
    -O compression=off -O atime=off -O xattr=sa -O normalization=none \
    -m "$MNT" "$POOL" "$(readlink -f "$IMG")"

mkdir -p "$MNT/etc"
printf 'genesis reads zfs acls\n'  > "$MNT/readable.txt"
printf 'owner only\n'              > "$MNT/secret.txt"
printf 'world writable\n'          > "$MNT/open.txt"
printf 'hello from a zfs volume\n' > "$MNT/etc/motd"
chmod 0644 "$MNT/readable.txt"
chmod 0600 "$MNT/secret.txt"
chmod 0666 "$MNT/open.txt"
chmod 0755 "$MNT/etc"
chmod 0644 "$MNT/etc/motd"
chown 1000:1000 "$MNT/readable.txt" "$MNT/secret.txt"

# The object number of secret.txt, which is what zplsetacl takes. Read from
# the filesystem rather than assumed: object numbers depend on creation order
# and this script has already been edited once.
OBJ=$(stat -c %i "$MNT/secret.txt")
echo "secret.txt is object $OBJ"

zpool export "$POOL"

# The non-trivial ACL. Linux cannot set an NFSv4 ACL - there is no setfacl for
# them and no system.nfs4_acl xattr - so this goes in through libzpool, which
# is still OpenZFS's own SA writer laying out the bytes.
mkdir -p "$WORK/inc"
ln -sfn /usr/include/libzpool "$WORK/inc/sys"
gcc -o "$WORK/zplsetacl" tests/host/zplsetacl.c \
    -I/usr/include/libzfs -I/usr/include/libspl -I"$WORK/inc" \
    -D_GNU_SOURCE -lzpool -lnvpair -lzfs_core -luutil
"$WORK/zplsetacl" "$(readlink -f "$WORK")" "$POOL" "$OBJ"

gzip -9 -c "$IMG" > "$OUT"
ls -la "$OUT"
echo "done - remember to remove build/zfsacl.img so build.py unpacks the new one"
