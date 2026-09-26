#!/usr/bin/env bash
#
# mkzfsimg.sh -- build a ZFS pool image and stage a root tree into it.
#
# Produces an image whose feature set stays inside what a boot-loader-class
# ZFS reader can handle: no large_dnode, no blake3/skein/edonr checksums,
# no zstd, no encryption, little-endian only, cleanly exported.
#
# Usage:
#   sudo ./mkzfsimg.sh [staging-dir]
#
# Environment overrides:
#   IMG=Build/genesis     output image path
#   STAGE=Build/root      directory whose contents become the pool root
#   POOL=genesis          pool name (must not collide with an imported pool)
#   SIZE=512M             image size
#   ASHIFT=12             vdev sector shift
#   COMPRESS=lz4          lz4 | lzjb | zle | off
#   LAYOUT=gpt            gpt | raw   (raw = pool at offset 0, no loop device)
#   VERIFY=1              re-import read-only and diff against the staging dir
#
set -euo pipefail

IMG="${IMG:-build/disk.img}"
STAGE="${STAGE:-${1:-root}}"
POOL="${POOL:-genesis}"
SIZE="${SIZE:-1G}"
ASHIFT="${ASHIFT:-12}"
COMPRESS="${COMPRESS:-lz4}"
LAYOUT="${LAYOUT:-gpt}"
VERIFY="${VERIFY:-1}"

MNT="$(mktemp -d /tmp/genesis-pool.XXXXXX)"
LOOP=""
IMPORTED=0

die() { printf '%s: %s\n' "${0##*/}" "$*" >&2; exit 1; }
note() { printf '==> %s\n' "$*"; }

# ---------------------------------------------------------------- teardown --
# Order matters: the pool must be exported before the loop device goes away,
# or ZFS is left holding a vanished vdev and the pool suspends -- which on
# Linux can wedge processes in uninterruptible sleep until reboot.
cleanup() {
    local rc=$?
    if [ "$IMPORTED" -eq 1 ]; then
        zpool export "$POOL" 2>/dev/null || zpool export -f "$POOL" 2>/dev/null || true
    fi
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
    [ $rc -ne 0 ] && printf '%s: failed (exit %d)\n' "${0##*/}" "$rc" >&2
    return $rc
}
trap cleanup EXIT

# ------------------------------------------------------------ preflight ----
[ "$(id -u)" -eq 0 ] || die "must run as root (losetup and zpool need it)"
[ -d "$STAGE" ] || die "staging directory '$STAGE' does not exist"
command -v zpool >/dev/null || die "zpool not found -- install OpenZFS userland"

# A big-endian pool is refused by the reader rather than misread, so catching
# it here is cheaper than catching it as 'all block copies unavailable' later.
[ "$(printf '\1\2' | od -An -tx2 | tr -d ' ')" = "0201" ] \
    || die "host is big-endian; the resulting pool would be unreadable"

case "$COMPRESS" in
    lz4|lzjb|zle|off) ;;
    *) die "compression '$COMPRESS' is outside the supported set (lz4|lzjb|zle|off)" ;;
esac

# The failure the name collision produces is 'pool already exists', which
# reads like a problem with the image and is not one.
if zpool list -H -o name 2>/dev/null | grep -qx "$POOL"; then
    die "a pool named '$POOL' is already imported on this host; export it or set POOL="
fi

mkdir -p "$(dirname "$IMG")"

# ------------------------------------------------------------- the image ---
note "creating $IMG ($SIZE)"
rm -f "$IMG"
truncate -s "$SIZE" "$IMG"

VDEV="$IMG"
if [ "$LAYOUT" = gpt ]; then
    command -v sgdisk >/dev/null || die "sgdisk not found (package: gdisk)"
    # Partition the FILE, then attach -- so the partition node exists the
    # moment losetup -P scans, with no partprobe race in between.
    note "writing GPT (one BF01 partition)"
    sgdisk -Z "$IMG" >/dev/null
    sgdisk -n 1:1M:0 -t 1:BF01 -c 1:genesis "$IMG" >/dev/null
    LOOP="$(losetup -fP --show "$IMG")"
    VDEV="${LOOP}p1"
    [ -b "$VDEV" ] || die "expected partition node $VDEV did not appear"
    note "attached $LOOP -> $VDEV"
fi

# --------------------------------------------------------------- the pool --
# compatibility=grub2 is the widest set a loader-class reader is tested
# against: it carries lz4_compress, embedded_data, extensible_dataset and
# hole_birth, while excluding large_dnode, zstd and the newer checksums.
note "creating pool '$POOL' (ashift=$ASHIFT compression=$COMPRESS)"
zpool create \
    -o ashift="$ASHIFT" \
    -o compatibility=grub2 \
    -O compression="$COMPRESS" \
    -O checksum=fletcher4 \
    -O atime=off \
    -O dnodesize=legacy \
    -O mountpoint=/ \
    -R "$MNT" \
    "$POOL" "$VDEV"
IMPORTED=1

# --------------------------------------------------------------- staging ---
# cp -a rather than rsync: no trailing-slash semantics to get wrong, and it
# preserves symlinks as symlinks (which is the whole point of putting the
# root here rather than on FAT).
note "staging $STAGE -> pool root"
cp -a "$STAGE"/. "$MNT"/
sync

USED="$(zfs get -H -o value -p used "$POOL")"
AVAIL="$(zfs get -H -o value -p available "$POOL")"
note "used $(numfmt --to=iec "$USED"), free $(numfmt --to=iec "$AVAIL")"

note "features enabled:"
zpool get -H -o property,value all "$POOL" \
    | awk '/^feature@/ && $2 != "disabled" { printf "      %s = %s\n", $1, $2 }'

# ---------------------------------------------------------------- export ---
# A clean export is not cosmetic: it settles the uberblocks and leaves no ZIL
# to replay, which a read-only reader cannot do.
note "exporting"
zpool export "$POOL"
IMPORTED=0

if [ -n "$LOOP" ]; then
    losetup -d "$LOOP"
    LOOP=""
fi

# ---------------------------------------------------------------- verify ---
# Re-read what was actually written, through a fresh import, rather than
# trusting that the writes above succeeded because they returned zero.
if [ "$VERIFY" = 1 ]; then
    note "verifying (read-only import)"
    VVDEV="$IMG"
    if [ "$LAYOUT" = gpt ]; then
        LOOP="$(losetup -fP --show "$IMG")"
        VVDEV="${LOOP}p1"
    fi
    zpool import -o readonly=on -d "$VVDEV" -R "$MNT" "$POOL"
    IMPORTED=1

    if diff -qr --no-dereference "$STAGE" "$MNT" >/tmp/genesis-verify.diff 2>&1; then
        note "verify OK: pool root matches $STAGE"
    else
        sed 's/^/      /' /tmp/genesis-verify.diff >&2
        die "verify FAILED: pool contents differ from staging tree"
    fi

    zpool export "$POOL"
    IMPORTED=0
    losetup -d "$LOOP"; LOOP=""
fi

note "done: $IMG"