#!/bin/sh
# Build a ZFS pool containing Genesis's root tree, for booting with / on ZFS.
#
# Needs root (zpool create) and a host with OpenZFS.
#
# -d creates the pool with NO features enabled. The vendored reader gates on
# eighteen features_for_read and refuses anything else, so a pool built without
# -d will not mount and the failure looks like a reader bug rather than a pool
# built wrong. Same reason as tests/host/fixtures/mkaclpool.sh.
#
# compression=off and no large blocks for the same reason: everything here has
# to stay inside what a boot-loader-class reader implements.
set -e

WORK=${WORK:-build/zfsroot}
POOL=${POOL:-genesisroot}
IMG=$WORK/pool.img
SRC=${SRC:-root}
MNT=/mnt/$POOL
SIZE=${SIZE:-256M}

mkdir -p "$WORK"
zpool destroy "$POOL" 2>/dev/null || true
rm -f "$IMG"
truncate -s "$SIZE" "$IMG"

zpool create -d -o ashift=9 \
    -O compression=off -O atime=off -O xattr=sa -O normalization=none \
    -O acltype=nfsv4 -O aclmode=passthrough \
    -m "$MNT" "$POOL" "$(readlink -f "$IMG")"

# The staged tree, exactly as it is written to the FAT image. cp -a keeps
# permissions and would keep symlinks, though there are none today - a symlink
# here would be a file the VFS cannot represent, since fs_ops_t has no symlink
# slot at all.
cp -a "$SRC"/. "$MNT"/
sync
ls -la "$MNT"
zpool export "$POOL"

echo "built $IMG"
