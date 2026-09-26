#!/usr/bin/env python3
"""Write a directory tree into an existing FAT16 image.

Populate, not format. The geometry comes from whatever mkfs.fat wrote into the
boot sector, and this only allocates clusters and fills in directory entries.
That split is deliberate and worth keeping: build.py's comment on cmd_disk
makes the point that a filesystem your own driver both writes and reads proves
nothing, because a shared misunderstanding looks exactly like agreement. Having
mkfs.fat own the layout means the C driver still has to agree with a reference
implementation about where everything is.

--format exists for environments without mkfs.fat, and gives up that property.
Use it only for test fixtures.

Names are 8.3, uppercased, and long filenames are NOT written - the kernel
driver skips LFN entries, so a name that needs one would be invisible to it.
A source file whose name does not fit is an error here rather than a silently
truncated entry on disk.
"""

import os
import struct
import sys

DIR_ENTRY = 32
ATTR_DIRECTORY = 0x10
FREE_CLUSTER = 0x0000
EOC = 0xFFFF


class Fat16:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "r+b")
        self._read_bpb()

    def _read_bpb(self):
        self.f.seek(0)
        bs = self.f.read(512)
        if bs[510:512] != b"\x55\xaa":
            raise ValueError(f"{self.path}: no boot signature; not formatted")

        self.bytes_per_sector = struct.unpack_from("<H", bs, 11)[0]
        self.sectors_per_cluster = bs[13]
        self.reserved_sectors = struct.unpack_from("<H", bs, 14)[0]
        self.num_fats = bs[16]
        self.root_entries = struct.unpack_from("<H", bs, 17)[0]
        self.fat_sectors = struct.unpack_from("<H", bs, 22)[0]

        total16 = struct.unpack_from("<H", bs, 19)[0]
        total32 = struct.unpack_from("<I", bs, 32)[0]
        self.total_sectors = total16 if total16 else total32

        if not self.bytes_per_sector or not self.sectors_per_cluster:
            raise ValueError(f"{self.path}: implausible BPB; is this FAT16?")

        self.fat_start = self.reserved_sectors
        self.root_start = self.fat_start + self.num_fats * self.fat_sectors
        self.root_sectors = (self.root_entries * DIR_ENTRY
                             + self.bytes_per_sector - 1) // self.bytes_per_sector
        self.data_start = self.root_start + self.root_sectors
        self.cluster_bytes = self.sectors_per_cluster * self.bytes_per_sector
        self.total_clusters = (self.total_sectors - self.data_start) // self.sectors_per_cluster

    # --- raw access -------------------------------------------------------

    def _read_sectors(self, lba, count):
        self.f.seek(lba * self.bytes_per_sector)
        return self.f.read(count * self.bytes_per_sector)

    def _write_sectors(self, lba, data):
        self.f.seek(lba * self.bytes_per_sector)
        self.f.write(data)

    def cluster_lba(self, cluster):
        return self.data_start + (cluster - 2) * self.sectors_per_cluster

    # --- the FAT ----------------------------------------------------------

    def _fat_get(self, cluster):
        off = cluster * 2
        lba = self.fat_start + off // self.bytes_per_sector
        idx = off % self.bytes_per_sector
        return struct.unpack_from("<H", self._read_sectors(lba, 1), idx)[0]

    def _fat_set(self, cluster, value):
        off = cluster * 2
        # Every copy of the FAT, not just the first. A driver is entitled to
        # read either one, and fsck will report a mismatch as corruption.
        for copy in range(self.num_fats):
            base = self.fat_start + copy * self.fat_sectors
            lba = base + off // self.bytes_per_sector
            idx = off % self.bytes_per_sector
            sector = bytearray(self._read_sectors(lba, 1))
            struct.pack_into("<H", sector, idx, value)
            self._write_sectors(lba, bytes(sector))

    def alloc_cluster(self):
        for c in range(2, self.total_clusters + 2):
            if self._fat_get(c) == FREE_CLUSTER:
                self._fat_set(c, EOC)
                self._write_sectors(self.cluster_lba(c),
                                    b"\x00" * self.cluster_bytes)
                return c
        raise RuntimeError("volume full")

    def alloc_chain(self, nbytes):
        need = max(1, (nbytes + self.cluster_bytes - 1) // self.cluster_bytes)
        chain = []
        for _ in range(need):
            chain.append(self.alloc_cluster())
        for a, b in zip(chain, chain[1:]):
            self._fat_set(a, b)
        self._fat_set(chain[-1], EOC)
        return chain

    # --- directories ------------------------------------------------------

    def _dir_sectors(self, cluster):
        """LBAs holding a directory's entries. cluster 0 means the root."""
        if cluster == 0:
            return [self.root_start + i for i in range(self.root_sectors)]
        out = []
        while cluster < 0xFFF8:
            for i in range(self.sectors_per_cluster):
                out.append(self.cluster_lba(cluster) + i)
            cluster = self._fat_get(cluster)
        return out

    def _dir_entries(self, cluster):
        for lba in self._dir_sectors(cluster):
            data = self._read_sectors(lba, 1)
            for off in range(0, len(data), DIR_ENTRY):
                yield lba, off, data[off:off + DIR_ENTRY]

    def _put_entry(self, dir_cluster, entry):
        for lba, off, existing in self._dir_entries(dir_cluster):
            if existing[0] in (0x00, 0xE5):
                sector = bytearray(self._read_sectors(lba, 1))
                sector[off:off + DIR_ENTRY] = entry
                self._write_sectors(lba, bytes(sector))
                return
        raise RuntimeError("directory full (the root is a fixed-size array)")

    def find(self, dir_cluster, name11):
        for _, _, e in self._dir_entries(dir_cluster):
            if e[0] == 0x00:
                return None
            if e[0] == 0xE5 or (e[11] & 0x0F) == 0x0F or (e[11] & 0x08):
                continue
            if e[0:11] == name11:
                return {"attr": e[11],
                        "cluster": struct.unpack_from("<H", e, 26)[0],
                        "size": struct.unpack_from("<I", e, 28)[0]}
        return None

    @staticmethod
    def name_to_11(name):
        stem, _, ext = name.partition(".")
        if len(stem) > 8 or len(ext) > 3:
            raise ValueError(
                f"'{name}' does not fit 8.3, and long filenames are not "
                f"written because the kernel driver cannot read them")
        return (stem.upper().ljust(8) + ext.upper().ljust(3)).encode("ascii")

    @staticmethod
    def _entry(name11, attr, cluster, size):
        e = bytearray(DIR_ENTRY)
        e[0:11] = name11
        e[11] = attr
        struct.pack_into("<H", e, 26, cluster)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    def mkdir(self, parent_cluster, name):
        name11 = self.name_to_11(name)
        found = self.find(parent_cluster, name11)
        if found:
            if not found["attr"] & ATTR_DIRECTORY:
                raise ValueError(f"{name} exists and is not a directory")
            return found["cluster"]

        cluster = self.alloc_cluster()
        # "." and ".." are real entries, and their absence is why a driver
        # that follows ".." from a subdirectory lands nowhere. ".." pointing
        # at the root is stored as 0, which is also how the root is named
        # everywhere else in FAT16.
        dot = self._entry(b".          ", ATTR_DIRECTORY, cluster, 0)
        dotdot = self._entry(b"..         ", ATTR_DIRECTORY, parent_cluster, 0)
        block = bytearray(self.cluster_bytes)
        block[0:DIR_ENTRY] = dot
        block[DIR_ENTRY:DIR_ENTRY * 2] = dotdot
        self._write_sectors(self.cluster_lba(cluster), bytes(block))

        self._put_entry(parent_cluster, self._entry(name11, ATTR_DIRECTORY, cluster, 0))
        return cluster

    def addfile(self, parent_cluster, name, data):
        name11 = self.name_to_11(name)
        if self.find(parent_cluster, name11):
            raise ValueError(f"{name} already exists")

        if len(data) == 0:
            self._put_entry(parent_cluster, self._entry(name11, 0x20, 0, 0))
            return

        chain = self.alloc_chain(len(data))
        for i, cluster in enumerate(chain):
            chunk = data[i * self.cluster_bytes:(i + 1) * self.cluster_bytes]
            chunk = chunk.ljust(self.cluster_bytes, b"\x00")
            self._write_sectors(self.cluster_lba(cluster), chunk)
        self._put_entry(parent_cluster, self._entry(name11, 0x20, chain[0], len(data)))

    def mkpath(self, path):
        """Create every directory along an absolute path; return its cluster."""
        cluster = 0
        for part in path.strip("/").split("/"):
            if part:
                cluster = self.mkdir(cluster, part)
        return cluster

    def stage(self, srcdir, verbose=True):
        """Mirror a host directory tree onto the volume."""
        srcdir = os.path.abspath(srcdir)
        for root, dirs, files in os.walk(srcdir):
            dirs.sort()
            rel = os.path.relpath(root, srcdir)
            vpath = "/" if rel == "." else "/" + rel.replace(os.sep, "/")
            cluster = self.mkpath(vpath)
            for name in sorted(files):
                with open(os.path.join(root, name), "rb") as fh:
                    data = fh.read()
                self.addfile(cluster, name, data)
                if verbose:
                    sep = "" if vpath.endswith("/") else "/"
                    print(f"  {vpath}{sep}{name.upper()}  {len(data)} bytes")

    def close(self):
        self.f.flush()
        self.f.close()


def format_image(path, size_mb=32, label="GENESIS"):
    """Minimal FAT16 formatter, for environments without mkfs.fat.

    Prefer mkfs.fat. A volume written and read by the same author is not a
    cross-check; this exists so tests can build a fixture, not to replace it.
    """
    bps, spc, reserved, num_fats, root_entries = 512, 4, 1, 2, 512
    total_sectors = size_mb * 1024 * 1024 // bps

    # Sector count and FAT size are mutually dependent; two passes converge.
    fat_sectors = 1
    for _ in range(8):
        root_sectors = (root_entries * DIR_ENTRY + bps - 1) // bps
        data_sectors = total_sectors - reserved - num_fats * fat_sectors - root_sectors
        clusters = data_sectors // spc
        needed = ((clusters + 2) * 2 + bps - 1) // bps
        if needed == fat_sectors:
            break
        fat_sectors = needed

    bs = bytearray(bps)
    bs[0:3] = b"\xeb\x3c\x90"
    bs[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", bs, 11, bps)
    bs[13] = spc
    struct.pack_into("<H", bs, 14, reserved)
    bs[16] = num_fats
    struct.pack_into("<H", bs, 17, root_entries)
    struct.pack_into("<H", bs, 19, total_sectors if total_sectors < 0x10000 else 0)
    bs[21] = 0xF8
    struct.pack_into("<H", bs, 22, fat_sectors)
    struct.pack_into("<H", bs, 24, 63)
    struct.pack_into("<H", bs, 26, 255)
    struct.pack_into("<I", bs, 32, 0 if total_sectors < 0x10000 else total_sectors)
    bs[36] = 0x80
    bs[38] = 0x29
    struct.pack_into("<I", bs, 39, 0x12345678)
    bs[43:54] = label.upper().ljust(11).encode("ascii")
    bs[54:62] = b"FAT16   "
    bs[510:512] = b"\x55\xaa"

    with open(path, "wb") as f:
        f.truncate(total_sectors * bps)
        f.write(bytes(bs))
        for copy in range(num_fats):
            f.seek((reserved + copy * fat_sectors) * bps)
            fat = bytearray(fat_sectors * bps)
            struct.pack_into("<H", fat, 0, 0xFFF8)
            struct.pack_into("<H", fat, 2, 0xFFFF)
            f.write(bytes(fat))


def main(argv):
    if len(argv) >= 2 and argv[1] == "--format":
        if len(argv) not in (3, 4):
            print("usage: fatfs.py --format IMAGE [SIZE_MB]", file=sys.stderr)
            return 2
        format_image(argv[2], int(argv[3]) if len(argv) == 4 else 32)
        print(f"{argv[2]}: formatted FAT16")
        return 0

    if len(argv) != 3:
        print("usage: fatfs.py IMAGE SRCDIR\n"
              "       fatfs.py --format IMAGE [SIZE_MB]", file=sys.stderr)
        return 2

    vol = Fat16(argv[1])
    try:
        vol.stage(argv[2])
    finally:
        vol.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
