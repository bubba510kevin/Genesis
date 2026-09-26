import struct, zlib, sys, os

SEC = 512
def blank(n): return bytearray(n*SEC)

def fat_superfloppy(nsec=1024):
    d = blank(nsec)
    d[0:3] = b'\xeb\x3c\x90'          # jmp short + nop
    d[3:11] = b'MSWIN4.1'
    struct.pack_into('<H', d, 11, 512)   # bytes/sector
    d[13] = 4                             # sectors/cluster
    struct.pack_into('<H', d, 14, 1)      # reserved sectors
    d[510:512] = b'\x55\xaa'
    return d

def mbr(nsec=2048, parts=((0x80,0x83,64,512),(0x00,0x0b,1024,512))):
    d = blank(nsec)
    d[0:3] = b'\xfa\x33\xc0'          # cli; xor ax,ax  -- boot code, no BPB
    for i,(flag,typ,start,cnt) in enumerate(parts):
        o = 446 + i*16
        d[o] = flag; d[o+4] = typ
        struct.pack_into('<I', d, o+8, start)
        struct.pack_into('<I', d, o+12, cnt)
    d[510:512] = b'\x55\xaa'
    return d

GUID_LINUX = bytes.fromhex('af3dc60f838472478e793d69d8477de4')
GUID_EFI   = bytes([0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,
                    0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B])

def gpt(nsec=4096, entries=((GUID_EFI,2048,2559),(GUID_LINUX,2560,4000)),
        protective=True, break_primary=False, hybrid=False):
    d = blank(nsec)
    # protective MBR
    d[0:3] = b'\xfa\x33\xc0'
    if protective:
        o = 446
        d[o]=0x00; d[o+4]=0xEE
        struct.pack_into('<I', d, o+8, 1)
        struct.pack_into('<I', d, o+12, nsec-1)
        if hybrid:
            o2 = 446+16
            d[o2]=0x80; d[o2+4]=0x83
            struct.pack_into('<I', d, o2+8, 2048)
            struct.pack_into('<I', d, o2+12, 512)
    d[510:512] = b'\x55\xaa'

    ent_lba, nent, esz = 2, 128, 128
    arr = bytearray(nent*esz)
    for i,(tguid,first,last) in enumerate(entries):
        o = i*esz
        arr[o:o+16] = tguid
        arr[o+16:o+32] = bytes(range(16))
        struct.pack_into('<Q', arr, o+32, first)
        struct.pack_into('<Q', arr, o+40, last)
    acrc = zlib.crc32(bytes(arr)) & 0xFFFFFFFF
    for i in range(nent*esz//SEC):
        d[(ent_lba+i)*SEC:(ent_lba+i+1)*SEC] = arr[i*SEC:(i+1)*SEC]

    def hdr(my_lba, alt_lba, elba):
        h = bytearray(92)
        h[0:8] = b'EFI PART'
        struct.pack_into('<I', h, 8, 0x00010000)
        struct.pack_into('<I', h, 12, 92)
        struct.pack_into('<Q', h, 24, my_lba)
        struct.pack_into('<Q', h, 32, alt_lba)
        struct.pack_into('<Q', h, 40, 34)
        struct.pack_into('<Q', h, 48, nsec-34)
        struct.pack_into('<Q', h, 72, elba)
        struct.pack_into('<I', h, 80, nent)
        struct.pack_into('<I', h, 84, esz)
        struct.pack_into('<I', h, 88, acrc)
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
        return h

    d[1*SEC:1*SEC+92] = hdr(1, nsec-1, ent_lba)
    backup_ent = nsec-1-32
    d[(nsec-1)*SEC:(nsec-1)*SEC+92] = hdr(nsec-1, 1, backup_ent)
    for i in range(nent*esz//SEC):
        d[(backup_ent+i)*SEC:(backup_ent+i+1)*SEC] = arr[i*SEC:(i+1)*SEC]
    if break_primary:
        d[1*SEC:2*SEC] = bytes(SEC)     # wipe the primary header only
    return d

out = {
 'fat_superfloppy.img': fat_superfloppy(),
 'mbr.img': mbr(),
 'gpt.img': gpt(),
 'gpt_backup.img': gpt(break_primary=True),
 'gpt_hybrid.img': gpt(hybrid=True),
 'raw.img': blank(64),
}
outdir = sys.argv[1] if len(sys.argv) > 1 else '/tmp/imgs'
os.makedirs(outdir, exist_ok=True)
for name, data in out.items():
    open(os.path.join(outdir, name), 'wb').write(bytes(data))
    print(name, len(data)//SEC, "sectors")
