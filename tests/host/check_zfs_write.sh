#!/bin/sh
# Verify a pool Genesis wrote to, using real OpenZFS.
#
# This is the only verification of a write path that counts. Genesis reading
# back its own writes proves the two halves of one program agree; it says
# nothing about whether what landed on disk is ZFS. A pool that only Genesis
# can read is a pool Genesis has corrupted in a way only Genesis forgives.
#
# Run AFTER tests/host/run.sh, which produces the committed image. Needs root
# (zpool import) and a host with OpenZFS. Exits 0 when real ZFS agrees.
set -e

IMG=${IMG:-build/hosttest/zfs/genesisacl.dat}
POOL=genesisacl
WORK=$(mktemp -d)
trap 'zpool export $POOL 2>/dev/null || true; rm -rf "$WORK"' EXIT

if [ ! -f "$IMG" ]; then
    echo "check_zfs_write: $IMG missing - run tests/host/run.sh first" >&2
    exit 1
fi
cp "$IMG" "$WORK/pool.img"

# 1. The uberblock. zdb picks the highest-txg VALID uberblock, so seeing the
#    one Genesis wrote means its self-checksum verified - and that checksum is
#    computed over the block with a verifier derived from the block's own
#    physical offset, so it also means the block went to the right slot in the
#    right label.
TXG=$(zdb -e -p "$WORK" -u "$POOL" 2>/dev/null | awk '/^\ttxg = /{print $3}')
echo "uberblock txg reported by zdb: ${TXG:-none}"
[ -n "$TXG" ] || { echo "FAIL: zdb found no valid uberblock" >&2; exit 1; }

# 2. Import it. This is real ZFS deciding the pool is coherent, not a dump
#    tool being lenient.
zpool import -d "$WORK" -N "$POOL" >/dev/null
zpool status "$POOL" | grep -q "state: ONLINE" || {
    echo "FAIL: the pool did not import ONLINE" >&2; exit 1; }

# 3. Scrub. Every block, every checksum, verified by the implementation that
#    defines what correct means.
zpool scrub "$POOL"
while zpool status "$POOL" | grep -q "scrub in progress"; do sleep 1; done
zpool status "$POOL" | grep -E "scan:|errors:"
zpool status "$POOL" | grep -q "errors: No known data errors" || {
    echo "FAIL: the scrub found errors" >&2; exit 1; }

echo "OK: real OpenZFS imported and scrubbed a pool Genesis committed to"
