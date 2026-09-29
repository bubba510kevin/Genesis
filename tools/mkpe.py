#!/usr/bin/env python3
"""Emit a hand-written, no-import PE32+ executable for Genesis.

This is the PE loader's milestone made into a file, and now the NT syscall
surface's too. It exists because the first image to test a loader with should
be one you can account for byte by byte - not one a linker produced, where a
failure is ambiguous between your parser and four hundred bytes of someone
else's opinion. When a stock MinGW hello.exe eventually runs, this is what
will still be here to bisect against.

What the program does, in order:

    NtDisplayString                    -> proves the image ran at all
    NtOpenFile   \\??\\CON             -> a console handle
    NtOpenFile   \\??\\C:\\etc\\motd   -> proves the unparsed remainder
    NtReadFile   from the file         -> into .data
    NtWriteFile  it back to the console
    NtClose      the file
    three GS-relative checks           -> proves the TEB and PEB are real
    NtClose      the console
    NtTerminateProcess

Every one of those names resolves through the same object namespace ash uses,
which is the property the whole dual-personality design rests on: \\??\\CON and
/dev/console are two spellings of one object, not two consoles that behave
alike until one of them grows a setting.

Layout, deliberately the ordinary one so the parser is exercised the way a
real image would exercise it:

    0x0000  DOS header, e_lfanew at 0x3C -> 0x0080
    0x0080  PE\\0\\0, COFF header, PE32+ optional header, section table
    0x0400  .text   raw, RVA 0x1000
    0x0600  .rdata  raw, RVA 0x2000
    0x0800  .data   raw, RVA 0x3000
    0x0A00  .reloc  raw, RVA 0x4000

FileAlignment 0x200 and SectionAlignment 0x1000 differ ON PURPOSE. A loader
that conflates PointerToRawData with VirtualAddress works perfectly when they
are equal and loads garbage the moment they are not, which is the single most
common way to get this wrong.
"""

import struct
import sys

IMAGE_BASE    = 0x140000000          # what every 64-bit linker defaults to
SECTION_ALIGN = 0x1000
FILE_ALIGN    = 0x200
HEADERS_SIZE  = 0x400   # four sections push the header past 0x200

TEXT_RVA  = 0x1000
RDATA_RVA = 0x2000
DATA_RVA  = 0x3000
RELOC_RVA = 0x4000

NT_DISPLAY_STRING    = 0x01
NT_TERMINATE_PROCESS = 0x02
NT_OPEN_FILE         = 0x03
NT_CLOSE             = 0x04
NT_READ_FILE         = 0x05
NT_WRITE_FILE        = 0x06

GENERIC_READ  = 0x80000000
GENERIC_WRITE = 0x40000000

def _teb_base():
    """Read NT_TEB_BASE out of the kernel header rather than repeating it.

    The image compares GS:[0x30] against this constant, so a copy here that
    drifts from the header would make the check fail and look exactly like a
    broken TEB. There have already been two rival definitions of this address
    in this tree; a third, in another language, is not an improvement."""
    import os
    import re

    header = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "kernel", "include", "teb.h")
    try:
        with open(header) as f:
            m = re.search(r"#define\s+NT_TEB_BASE\s+(0x[0-9A-Fa-f]+)", f.read())
        if m:
            return int(m.group(1), 16)
    except OSError:
        pass
    raise SystemExit("mkpe.py: cannot read NT_TEB_BASE from kernel/include/teb.h")


NT_TEB_BASE = _teb_base()

BANNER = "hello from a PE, loaded by Genesis\n"
NOTE   = "PE read /etc/motd through \\??\\C: -> "
FAILED  = "an NT call failed - see the status line above\n"
TEB_OK  = "TEB at GS:[0x30] points at itself\n"
PEB_OK  = "PEB at GS:[0x60] knows the ImageBase\n"
ERR_OK  = "GetLastError round-trips through GS:[0x68]\n"

BUF_SIZE = 256


class Data:
    """A section being laid out, tracking where each label landed.

    Absolute pointers are recorded as they are emitted so .reloc can be
    generated from the same source of truth, rather than from a second list
    that has to be kept in step by hand.
    """

    def __init__(self, rva):
        self.rva = rva
        self.buf = bytearray()
        self.fixups = []          # RVAs holding an absolute address

    def here(self):
        return self.rva + len(self.buf)

    def align(self, n):
        while len(self.buf) % n:
            self.buf += b"\x00"

    def raw(self, b):
        at = self.here()
        self.buf += b
        return at

    def pointer(self, target_rva):
        """An absolute pointer, plus the .reloc entry that keeps it honest."""
        self.fixups.append(self.here())
        self.buf += struct.pack("<Q", IMAGE_BASE + target_rva)

    def utf16(self, text):
        return self.raw(text.encode("utf-16-le"))

    def unicode_string(self, text_rva, nbytes):
        """UNICODE_STRING: Length and MaximumLength are BYTES, not characters."""
        self.align(8)
        at = self.here()
        self.buf += struct.pack("<HH", nbytes, nbytes)
        self.buf += b"\x00" * 4
        self.pointer(text_rva)
        return at

    def object_attributes(self, name_rva):
        """OBJECT_ATTRIBUTES: 48 bytes, Length first, ObjectName at +24."""
        self.align(8)
        at = self.here()
        self.buf += struct.pack("<II", 48, 0)
        self.buf += struct.pack("<Q", 0)          # RootDirectory: none
        self.pointer(name_rva)                    # ObjectName
        self.buf += struct.pack("<II", 0x40, 0)   # OBJ_CASE_INSENSITIVE
        self.buf += struct.pack("<QQ", 0, 0)      # security descriptor, QoS
        return at


class Code:
    """A minimal emitter for the handful of instructions this program needs.

    RIP-relative throughout, so the code needs no relocation - only the
    pointers in .rdata do. That is not laziness: it keeps the instruction
    stream identical whether or not the image moved, so a relocation bug
    cannot present as a control-flow bug.
    """

    def __init__(self, rva):
        self.rva = rva
        self.buf = bytearray()

    def here(self):
        return self.rva + len(self.buf)

    def _riprel(self, opcode, target_rva):
        length = len(opcode) + 4
        disp = target_rva - (self.here() + length)
        self.buf += opcode + struct.pack("<i", disp)

    def lea(self, reg, target_rva):
        self._riprel({
            "rax": b"\x48\x8D\x05", "rcx": b"\x48\x8D\x0D",
            "rdx": b"\x48\x8D\x15", "r8":  b"\x4C\x8D\x05",
            "r9":  b"\x4C\x8D\x0D",
        }[reg], target_rva)

    def load(self, reg, target_rva):
        self._riprel({
            "rcx": b"\x48\x8B\x0D", "rax": b"\x48\x8B\x05",
        }[reg], target_rva)

    # --- the Win64 shape ------------------------------------------------
    #
    # Arguments one to four in RCX, RDX, R8, R9; five and beyond at
    # [rsp+0x28] and up, because the ABI reserves 32 bytes of shadow space
    # above the return address whether or not anyone uses it. RCX is then
    # copied to R10 because SYSCALL destroys RCX.

    def mov_r10_rcx(self):
        self.buf += b"\x49\x89\xCA"

    def sub_rsp(self, v):
        self.buf += b"\x48\x83\xEC" + struct.pack("<b", v)

    def stack_imm(self, argno, value):
        """mov qword [rsp+off], imm32 - a stack argument with a known value."""
        off = 0x28 + (argno - 5) * 8
        self.buf += b"\x48\xC7\x44\x24" + struct.pack("<B", off)
        self.buf += struct.pack("<i", value)

    def stack_rax(self, argno):
        """mov qword [rsp+off], rax - a stack argument computed at runtime."""
        off = 0x28 + (argno - 5) * 8
        self.buf += b"\x48\x89\x44\x24" + struct.pack("<B", off)

    def movimm32_ecx(self, value):
        self.buf += b"\xB9" + struct.pack("<I", value)

    def movimm32_edx(self, value):
        self.buf += b"\xBA" + struct.pack("<I", value)

    def mov_rcx_minus1(self):
        self.buf += b"\x48\xC7\xC1\xFF\xFF\xFF\xFF"

    def movimm32(self, reg, value):
        self.buf += {"eax": b"\xB8", "esi": b"\xBE"}[reg]
        self.buf += struct.pack("<I", value & 0xFFFFFFFF)

    def zero(self, reg):
        self.buf += {
            "ecx": b"\x31\xC9", "edx": b"\x31\xD2",
            "r8d": b"\x45\x31\xC0", "r9d": b"\x45\x31\xC9",
        }[reg]

    def mov_gs(self, reg, offset):
        """mov reg, gs:[offset] - the whole point of a TEB. No system call:
        a Windows program reads its own identity out of a page the kernel
        prepared, through a segment base the context switch restores."""
        self.buf += b"\x65" + {
            "rax": b"\x48\x8B\x04\x25", "rcx": b"\x48\x8B\x0C\x25",
        }[reg] + struct.pack("<I", offset)

    def mov_gs_mem_eax(self, offset):
        """mov gs:[offset], eax - SetLastError, in one instruction."""
        self.buf += b"\x65\x89\x04\x25" + struct.pack("<I", offset)

    def mov_eax_gs(self, offset):
        """mov eax, gs:[offset] - GetLastError, in one instruction."""
        self.buf += b"\x65\x8B\x04\x25" + struct.pack("<I", offset)

    def movabs_rcx(self, value):
        self.buf += b"\x48\xB9" + struct.pack("<Q", value)

    def movimm32_eax(self, value):
        self.buf += b"\xB8" + struct.pack("<I", value)

    def cmp_rax_rcx(self):
        self.buf += b"\x48\x39\xC8"

    def cmp_eax_imm32(self, value):
        self.buf += b"\x3D" + struct.pack("<I", value)

    def load_from_rax(self, offset):
        """mov rax, [rax+disp8] - one step along a pointer chain."""
        self.buf += b"\x48\x8B\x40" + struct.pack("<b", offset)

    def jne_fwd(self):
        self.buf += b"\x0F\x85" + struct.pack("<i", 0)
        return len(self.buf) - 4

    def patch_fwd(self, site):
        self.buf[site:site + 4] = struct.pack("<i", len(self.buf) - (site + 4))

    def push_imm8(self, v):
        self.buf += b"\x6A" + struct.pack("<b", v)

    def push_imm32(self, v):
        """push imm32, sign-extended to 64 bits. Needed because a buffer size
        of 256 does not fit the imm8 form, and silently truncating it would
        ask for a 0-byte read."""
        self.buf += b"\x68" + struct.pack("<i", v)

    def push_rax(self):
        self.buf += b"\x50"

    def add_rsp(self, v):
        self.buf += b"\x48\x83\xC4" + struct.pack("<b", v)

    def syscall(self, number):
        """The stub body a compiler-built ntdll emits, minus the ret."""
        self.mov_r10_rcx()
        self.movimm32("eax", number)
        self.buf += b"\x0F\x05"

    def test_eax_jnz(self):
        """`test eax,eax; jne <patched later>`. Returns the patch site.

        The rel32 form rather than rel8. The short form is two bytes cheaper
        and silently wrong the moment the body between the branch and its
        target grows past 127 - which it did, at which point the assembler
        that had no range check would have emitted a jump into the middle of
        an instruction. Cheaper to not have the failure mode."""
        self.buf += b"\x85\xC0\x0F\x85" + struct.pack("<i", 0)
        return len(self.buf) - 4

    def patch_jnz(self, site):
        delta = len(self.buf) - (site + 4)
        self.buf[site:site + 4] = struct.pack("<i", delta)

    def jmp_self(self):
        self.buf += b"\xEB\xFE"

    def push_rbx(self):
        self.buf += b"\x53"

    def pop_rbx(self):
        self.buf += b"\x5B"

    def mov_rbx_rcx(self):
        """mov rbx, rcx - saves an argument into a Win64 non-volatile
        register so it survives a call this function makes."""
        self.buf += b"\x48\x89\xCB"

    def mov_rcx_rbx(self):
        self.buf += b"\x48\x89\xD9"

    def call_iat(self, iat_rva):
        """call qword ptr [rip+disp32] - the real-world idiom for calling
        through an import-table slot, once the loader has overwritten it
        with the resolved absolute address (see kernel/pe.c's
        resolve_imports). Not a `call rel32` - that would need the
        target's address baked in at build time, which is exactly what an
        import is for not needing."""
        length = 6
        disp = iat_rva - (self.here() + length)
        self.buf += b"\xFF\x15" + struct.pack("<i", disp)

    def ret(self):
        self.buf += b"\xC3"


def build():
    rdata = Data(RDATA_RVA)
    data  = Data(DATA_RVA)

    # --- .rdata ---------------------------------------------------------
    banner16 = rdata.utf16(BANNER)
    banner_bytes = len(BANNER) * 2
    rdata.align(8)

    con_path = "\\??\\CON"
    con_name = rdata.utf16(con_path)
    con_ustr = rdata.unicode_string(con_name, len(con_path) * 2)
    con_oa   = rdata.object_attributes(con_ustr)

    motd_path = "\\??\\C:\\etc\\motd"
    motd_name = rdata.utf16(motd_path)
    motd_ustr = rdata.unicode_string(motd_name, len(motd_path) * 2)
    motd_oa   = rdata.object_attributes(motd_ustr)

    banner_ustr = rdata.unicode_string(banner16, banner_bytes)
    failed16    = rdata.utf16(FAILED)
    failed_ustr = rdata.unicode_string(failed16, len(FAILED) * 2)
    note_ascii  = rdata.raw(NOTE.encode("ascii"))
    teb_ok      = rdata.raw(TEB_OK.encode("ascii"))
    peb_ok      = rdata.raw(PEB_OK.encode("ascii"))
    err_ok      = rdata.raw(ERR_OK.encode("ascii"))
    failed      = rdata.raw(FAILED.encode("ascii"))

    # --- .data ----------------------------------------------------------
    hcon  = data.raw(struct.pack("<Q", 0))
    hfile = data.raw(struct.pack("<Q", 0))
    iosb  = data.raw(struct.pack("<QQ", 0, 0))
    buf   = data.raw(b"\x00" * BUF_SIZE)

    # --- .text ----------------------------------------------------------
    c = Code(TEXT_RVA)

    # One frame for the whole program. Arguments five and beyond live at
    # [rsp+0x28] upward, so the code needs a region below RSP it owns - the
    # same region a compiled caller would have reserved before its `call`.
    # 0x60 covers the widest call here, NtReadFile's nine arguments.
    c.sub_rsp(0x60)

    def call(number, *, rcx=None, rcx_load=None, rcx_imm=None, rcx_lea=None,
             edx=None, edx_imm=None, r8=None, r9=None, stack=()):
        if rcx_lea is not None:
            c.lea("rcx", rcx_lea)
        elif rcx_load is not None:
            c.load("rcx", rcx_load)
        elif rcx_imm is not None:
            c.mov_rcx_minus1() if rcx_imm == -1 else c.movimm32_ecx(rcx_imm)
        else:
            c.zero("ecx")
        if edx_imm is not None:
            c.movimm32_edx(edx_imm)
        elif edx is not None:
            c.lea("rdx", edx)
        else:
            c.zero("edx")
        if r8 is not None:
            c.lea("r8", r8)
        else:
            c.zero("r8d")
        if r9 is not None:
            c.lea("r9", r9)
        else:
            c.zero("r9d")
        for argno, kind, value in stack:
            if kind == "imm":
                c.stack_imm(argno, value)
            elif kind == "lea":
                c.lea("rax", value)
                c.stack_rax(argno)
            elif kind == "rax":
                c.stack_rax(argno)
        c.syscall(number)

    # NtDisplayString(&banner). The one call that needs no handle, so a
    # failure further down still leaves evidence the image ran.
    call(NT_DISPLAY_STRING, rcx_lea=banner_ustr)

    # NtOpenFile(&hcon, GENERIC_WRITE, &con_oa, &iosb, ShareAccess=0,
    #            OpenOptions=0)
    call(NT_OPEN_FILE, rcx_lea=hcon, edx_imm=GENERIC_WRITE, r8=con_oa, r9=iosb,
         stack=((5, "imm", 0), (6, "imm", 0)))
    bail_con = c.test_eax_jnz()

    call(NT_OPEN_FILE, rcx_lea=hfile, edx_imm=GENERIC_READ, r8=motd_oa, r9=iosb,
         stack=((5, "imm", 0), (6, "imm", 0)))
    bail_file = c.test_eax_jnz()

    # NtReadFile(hfile, NULL, NULL, NULL, &iosb, buf, BUF_SIZE, NULL, NULL)
    call(NT_READ_FILE, rcx_load=hfile,
         stack=((5, "lea", iosb), (6, "lea", buf), (7, "imm", BUF_SIZE),
                (8, "imm", 0), (9, "imm", 0)))

    def say_bytes(rva, nbytes):
        call(NT_WRITE_FILE, rcx_load=hcon,
             stack=((5, "lea", iosb), (6, "lea", rva), (7, "imm", nbytes),
                    (8, "imm", 0), (9, "imm", 0)))

    say_bytes(note_ascii, len(NOTE))

    # NtWriteFile(hcon, ..., buf, iosb.Information). The length comes from the
    # read's IO_STATUS_BLOCK and not from its return value - that is a status,
    # not a count.
    c.load("rax", iosb + 8)
    c.stack_rax(7)                    # Length, straight from Information
    call(NT_WRITE_FILE, rcx_load=hcon,
         stack=((5, "lea", iosb), (6, "lea", buf), (8, "imm", 0),
                (9, "imm", 0)))

    call(NT_CLOSE, rcx_load=hfile)

    # --- what the TEB is for -------------------------------------------
    #
    # Three loads, no system calls. Each is a fact a Windows program expects
    # to know about itself without asking, and each is only true because the
    # kernel built a block and the context switch restores the segment base
    # that reaches it.
    c.mov_gs("rax", 0x30)
    c.movabs_rcx(NT_TEB_BASE)
    c.cmp_rax_rcx()
    skip_teb = c.jne_fwd()
    say_bytes(teb_ok, len(TEB_OK))
    c.patch_fwd(skip_teb)

    c.mov_gs("rax", 0x60)
    c.load_from_rax(0x10)
    c.movabs_rcx(IMAGE_BASE)
    c.cmp_rax_rcx()
    skip_peb = c.jne_fwd()
    say_bytes(peb_ok, len(PEB_OK))
    c.patch_fwd(skip_peb)

    c.movimm32_eax(0x1234)
    c.mov_gs_mem_eax(0x68)
    c.movimm32_eax(0)
    c.mov_eax_gs(0x68)
    c.cmp_eax_imm32(0x1234)
    skip_err = c.jne_fwd()
    say_bytes(err_ok, len(ERR_OK))
    c.patch_fwd(skip_err)

    c.patch_jnz(bail_file)
    # Reached when opening the file failed. The console handle is still good,
    # so say so rather than exiting mutely - a program that prints nothing on
    # failure is indistinguishable from one that never ran.
    say_bytes(failed, len(FAILED))
    call(NT_CLOSE, rcx_load=hcon)

    c.patch_jnz(bail_con)
    # Reached when even the console would not open. NtDisplayString needs no
    # handle, so there is still one way to speak.
    call(NT_DISPLAY_STRING, rcx_lea=failed_ustr)
    call(NT_TERMINATE_PROCESS, rcx_imm=-1)
    c.jmp_self()                      # never reached

    text = bytes(c.buf)

    # --- .reloc ---------------------------------------------------------
    # Every absolute pointer .rdata emitted, in one block. The loader will not
    # use it - 0x140000000 fits in Genesis's user space, so the delta is zero
    # and relocation is skipped - but the image is then honest rather than
    # accidentally correct, and the directory is there for the parser to walk.
    fixups = sorted(rdata.fixups)
    assert all(f >> 12 == RDATA_RVA >> 12 for f in fixups), "fixups span pages"
    entries = [(10 << 12) | (f & 0xFFF) for f in fixups]   # DIR64
    while (8 + len(entries) * 2) % 4:
        entries.append(0)                                   # ABSOLUTE padding
    reloc = struct.pack("<II", RDATA_RVA, 8 + len(entries) * 2)
    for e in entries:
        reloc += struct.pack("<H", e)

    rdata_bytes = bytes(rdata.buf)
    data_bytes  = bytes(data.buf)
    size_of_image = (RELOC_RVA + len(reloc) + SECTION_ALIGN - 1) & ~(SECTION_ALIGN - 1)

    # Raw file offsets, computed from the sizes rather than written down.
    #
    # These were the constants 0x400, 0x600, 0x800, 0xA00 - a 0x200 gap for
    # .text, which was true when .text was under 0x200 bytes and stopped being
    # true when the Win64 calling convention landed and every call site grew a
    # stack frame. .text reached 0x377, its declared extent ran past 0x600, and
    # the header then said .rdata began at an offset that held the middle of
    # .text.
    #
    # Nothing failed. The loader mapped exactly what the headers described, so
    # every pointer the image passed to the kernel was machine code read as a
    # UNICODE_STRING - which is a STATUS_ACCESS_VIOLATION from user_ptr_ok and
    # no indication at all of where it came from. A layout that is derived
    # cannot drift from the content the way one that is asserted can.
    def file_padded(n):
        return (n + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)

    text_raw  = HEADERS_SIZE
    rdata_raw = text_raw  + file_padded(len(text))
    data_raw  = rdata_raw + file_padded(len(rdata_bytes))
    reloc_raw = data_raw  + file_padded(len(data_bytes))

    dos = bytearray(0x80)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x80)

    coff = struct.pack("<HHIIIHH", 0x8664, 4, 0, 0, 0, 240, 0x0022)

    opt = struct.pack(
        "<HBBIIIIIQIIHHHHHHIIIIHHQQQQII",
        0x020B, 0, 1,
        len(text), len(rdata_bytes), 0,
        TEXT_RVA, TEXT_RVA, IMAGE_BASE,
        SECTION_ALIGN, FILE_ALIGN,
        4, 0, 0, 0, 4, 0, 0,
        size_of_image, HEADERS_SIZE, 0,
        3, 0,
        0x100000, 0x1000, 0x100000, 0x1000,
        0, 16,
    )
    directories = [(0, 0)] * 16
    directories[5] = (RELOC_RVA, len(reloc))
    for rva, dsize in directories:
        opt += struct.pack("<II", rva, dsize)
    assert len(opt) == 240, len(opt)

    def section(name, rva, vsize, raw_ptr, raw_size, characteristics):
        return (name.encode().ljust(8, b"\x00") +
                struct.pack("<IIII", vsize, rva, raw_size, raw_ptr) +
                struct.pack("<IIHH", 0, 0, 0, 0) +
                struct.pack("<I", characteristics))

    def padded(n):
        return (n + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)

    SCN_CODE, SCN_IDATA = 0x00000020, 0x00000040
    MEM_EXECUTE, MEM_READ, MEM_WRITE = 0x20000000, 0x40000000, 0x80000000
    MEM_DISCARDABLE = 0x02000000

    sections = (
        section(".text", TEXT_RVA, len(text), text_raw, padded(len(text)),
                SCN_CODE | MEM_EXECUTE | MEM_READ) +
        section(".rdata", RDATA_RVA, len(rdata_bytes), rdata_raw,
                padded(len(rdata_bytes)), SCN_IDATA | MEM_READ) +
        section(".data", DATA_RVA, len(data_bytes), data_raw,
                padded(len(data_bytes)), SCN_IDATA | MEM_READ | MEM_WRITE) +
        section(".reloc", RELOC_RVA, len(reloc), reloc_raw, padded(len(reloc)),
                SCN_IDATA | MEM_READ | MEM_DISCARDABLE)
    )

    out = bytearray()
    out += dos
    out += b"PE\x00\x00" + coff + opt + sections
    assert len(out) <= HEADERS_SIZE, f"headers overflowed: {len(out)}"
    for offset, blob in ((text_raw, text), (rdata_raw, rdata_bytes),
                         (data_raw, data_bytes), (reloc_raw, reloc)):
        # The check, not the padding, is the point. `b"\x00" * n` with a
        # negative n is an empty string rather than an error, so a section that
        # outgrew its slot used to be written immediately after the previous
        # one - at an offset the header did not describe, with no complaint
        # from anything. Assert first and the generator stops being able to
        # emit a file whose section table is a lie.
        assert offset >= len(out), (
            f"section at 0x{offset:x} overlaps the previous one, which ends "
            f"at 0x{len(out):x}")
        out += b"\x00" * (offset - len(out))
        out += blob
    while len(out) % FILE_ALIGN:
        out += b"\x00"
    return bytes(out)


def build_sys():
    """Emit a hand-written, no-libc PE32+ "driver" for kernel/pe.c's new
    pe_load_driver (see kernel/include/pe.h) - the same "byte-accountable,
    not linker output" reasoning build() gives for hand.exe, applied to the
    kernel-mode loading path instead of execve's.

    Proves the whole chain end to end: IMAGE_FILE_DLL + IMAGE_SUBSYSTEM_
    NATIVE characteristics (so pe_load_driver accepts it and pe_load_
    executable/pe_load_into would each refuse it - the discriminator
    kernel/pe.c's new PE_ERR_SUBSYSTEM check exists for), a real import
    from the synthetic ntoskrnl.exe table (kernel/ntoskrnl_exports.c) -
    DbgPrint AND IoCreateDevice, two distinct symbols, so a loader bug
    that resolves only the first import silently would still be visible -
    called through a real IAT slot the way compiled driver code actually
    does it, not a shortcut. DriverEntry's own calling convention is Win64
    (RCX/RDX/R8/R9) - see kernel/include/wdm.h's WDM_ABI comment for why
    that specific detail is load-bearing rather than cosmetic: Genesis's
    IoCreateDriver calls DriverEntry through a WDM_ABI-typed function
    pointer, and a DriverEntry reading its arguments from the wrong
    registers would silently read garbage instead of a real DriverObject.

    Layout mirrors build()'s: same section RVAs/alignment constants, same
    4 sections. Unlike build(), .reloc here is a real, exercised path
    rather than a formality - PE_DRIVER_BASE is nowhere near this image's
    0x140000000 preferred base, so pe_load_driver's load_image always
    relocates it, and DriverEntry's own code is 100% RIP-relative (the
    only correct way to write position-independent kernel-mode driver
    code, and the reason there are zero entries in the emitted .reloc
    block - not because nothing needed relocating, but because nothing in
    this file's machine code embeds an absolute address for .reloc to
    fix up)."""
    rdata = Data(RDATA_RVA)
    data  = Data(DATA_RVA)

    # --- .rdata: DLL name, import names, DbgPrint format strings --------
    dll_name = rdata.raw(b"ntoskrnl.exe\x00")

    rdata.align(2)
    dbgprint_name = rdata.raw(struct.pack("<H", 0) + b"DbgPrint\x00")
    rdata.align(2)
    iocreatedevice_name = rdata.raw(struct.pack("<H", 0) + b"IoCreateDevice\x00")

    msg1 = rdata.raw(b"test.sys: DriverEntry running\n\x00")
    msg2 = rdata.raw(b"test.sys: IoCreateDevice returned\n\x00")

    # --- .data: the IAT (writable - resolve_imports overwrites it) and a
    # scratch slot for IoCreateDevice's output PDEVICE_OBJECT* ----------
    data.align(8)
    iat = data.here()
    data.raw(struct.pack("<Q", dbgprint_name))         # thunk[0]: DbgPrint
    data.raw(struct.pack("<Q", iocreatedevice_name))   # thunk[1]: IoCreateDevice
    data.raw(struct.pack("<Q", 0))                     # thunk terminator
    pdo_slot = data.raw(struct.pack("<Q", 0))          # &DeviceObject out-param

    iat_dbgprint_rva       = iat
    iat_iocreatedevice_rva = iat + 8

    # --- .rdata: the import directory table itself -----------------------
    # pe_import_desc_t (kernel/include/pe.h): original_first_thunk (0 - the
    # IAT above doubles as the name table too, the "OriginalFirstThunk can
    # be zero" case resolve_imports already handles), time_date_stamp,
    # forwarder_chain, name RVA, first_thunk RVA - one real descriptor for
    # "ntoskrnl.exe" plus the all-zero terminator resolve_imports' walk
    # stops on.
    rdata.align(4)
    import_dir = rdata.here()
    rdata.raw(struct.pack("<IIIII", 0, 0, 0, dll_name, iat))
    rdata.raw(struct.pack("<IIIII", 0, 0, 0, 0, 0))     # terminator

    # --- .text: DriverEntry ----------------------------------------------
    #
    # NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING
    # RegistryPath) - RCX = DriverObject, RDX = RegistryPath (NULL, see
    # kernel/wdm.c's IoCreateDriver). Called through a real `call`, not
    # jumped to like a process entry point (see build()'s comment on
    # sub_rsp) - RSP is 8-mod-16 on entry, so push rbx first (restores
    # 0-mod-16) and only then reserve a 16-byte-aligned amount of shadow
    # space, rather than reproducing build()'s process-entry-only offset.
    c = Code(TEXT_RVA)
    c.push_rbx()
    c.mov_rbx_rcx()               # rbx = DriverObject, survives both calls
    c.sub_rsp(0x20)                # 32-byte Win64 shadow space, stays aligned

    c.lea("rcx", msg1)
    c.call_iat(iat_dbgprint_rva)

    c.mov_rcx_rbx()                 # DriverObject
    c.zero("edx")                   # DeviceExtensionSize = 0
    c.lea("r8", pdo_slot)           # &DeviceObject
    c.call_iat(iat_iocreatedevice_rva)

    c.lea("rcx", msg2)
    c.call_iat(iat_dbgprint_rva)

    c.movimm32_eax(0)               # STATUS_SUCCESS
    c.add_rsp(0x20)
    c.pop_rbx()
    c.ret()

    text = bytes(c.buf)

    # --- .reloc: one valid, empty block ----------------------------------
    # DriverEntry embeds zero absolute addresses - everything above is
    # RIP-relative or an IAT-slot RVA the loader fills in itself - so there
    # is nothing to fix up. But load_image (kernel/pe.c) still requires a
    # well-formed, non-empty .reloc DIRECTORY whenever delta != 0, which it
    # always is here (see this function's docstring) - a header-only block
    # (block_size 8, zero 16-bit entries following it) is exactly that: a
    # real, structurally valid relocation block asserting zero fixups,
    # not an absent directory the loader would refuse to trust.
    reloc = struct.pack("<II", RDATA_RVA, 8)

    rdata_bytes = bytes(rdata.buf)
    data_bytes  = bytes(data.buf)
    size_of_image = (RELOC_RVA + len(reloc) + SECTION_ALIGN - 1) & ~(SECTION_ALIGN - 1)

    def file_padded(n):
        return (n + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)

    text_raw  = HEADERS_SIZE
    rdata_raw = text_raw  + file_padded(len(text))
    data_raw  = rdata_raw + file_padded(len(rdata_bytes))
    reloc_raw = data_raw  + file_padded(len(data_bytes))

    dos = bytearray(0x80)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x80)

    IMAGE_FILE_DLL = 0x2000
    coff = struct.pack("<HHIIIHH", 0x8664, 4, 0, 0, 0, 240,
                       0x0002 | 0x0020 | IMAGE_FILE_DLL)

    IMAGE_SUBSYSTEM_NATIVE = 1
    opt = struct.pack(
        "<HBBIIIIIQIIHHHHHHIIIIHHQQQQII",
        0x020B, 0, 1,
        len(text), len(rdata_bytes), 0,
        TEXT_RVA, TEXT_RVA, IMAGE_BASE,
        SECTION_ALIGN, FILE_ALIGN,
        4, 0, 0, 0, 4, 0, 0,
        size_of_image, HEADERS_SIZE, 0,
        IMAGE_SUBSYSTEM_NATIVE, 0,
        0x100000, 0x1000, 0x100000, 0x1000,
        0, 16,
    )
    directories = [(0, 0)] * 16
    directories[1] = (import_dir, 40)   # import table: 2 descriptors x 20 bytes
    directories[5] = (RELOC_RVA, len(reloc))
    for rva, dsize in directories:
        opt += struct.pack("<II", rva, dsize)
    assert len(opt) == 240, len(opt)

    def section(name, rva, vsize, raw_ptr, raw_size, characteristics):
        return (name.encode().ljust(8, b"\x00") +
                struct.pack("<IIII", vsize, rva, raw_size, raw_ptr) +
                struct.pack("<IIHH", 0, 0, 0, 0) +
                struct.pack("<I", characteristics))

    def padded(n):
        return (n + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)

    SCN_CODE, SCN_IDATA = 0x00000020, 0x00000040
    MEM_EXECUTE, MEM_READ, MEM_WRITE = 0x20000000, 0x40000000, 0x80000000
    MEM_DISCARDABLE = 0x02000000

    sections = (
        section(".text", TEXT_RVA, len(text), text_raw, padded(len(text)),
                SCN_CODE | MEM_EXECUTE | MEM_READ) +
        section(".rdata", RDATA_RVA, len(rdata_bytes), rdata_raw,
                padded(len(rdata_bytes)), SCN_IDATA | MEM_READ) +
        section(".data", DATA_RVA, len(data_bytes), data_raw,
                padded(len(data_bytes)), SCN_IDATA | MEM_READ | MEM_WRITE) +
        section(".reloc", RELOC_RVA, len(reloc), reloc_raw, padded(len(reloc)),
                SCN_IDATA | MEM_READ | MEM_DISCARDABLE)
    )

    out = bytearray()
    out += dos
    out += b"PE\x00\x00" + coff + opt + sections
    assert len(out) <= HEADERS_SIZE, f"headers overflowed: {len(out)}"
    for offset, blob in ((text_raw, text), (rdata_raw, rdata_bytes),
                         (data_raw, data_bytes), (reloc_raw, reloc)):
        assert offset >= len(out), (
            f"section at 0x{offset:x} overlaps the previous one, which ends "
            f"at 0x{len(out):x}")
        out += b"\x00" * (offset - len(out))
        out += blob
    while len(out) % FILE_ALIGN:
        out += b"\x00"
    return bytes(out)


if __name__ == "__main__":
    # root/bin/hand.exe, not hello.exe. This generator produced hello.exe when
    # it was the only PE there was; hello.exe is now the MinGW-built image that
    # imports ntdll, and running this script with no arguments used to
    # overwrite it with the hand-assembled one. The symptom of that is a
    # hello.exe that runs and prints the wrong banner - or, if it happened
    # between a build and a stage, a "my change had no effect" with no error,
    # which is the third entry on this project's list of those.
    if len(sys.argv) > 1 and sys.argv[1] == "--sys":
        # test.sys is kernel-side: it is staged from the modules/root
        # overlay, not from the userland repository's root/.
        path = sys.argv[2] if len(sys.argv) > 2 else \
            "modules/root/wsr/Windows/System32/Drivers/test.sys"
        blob = build_sys()
        with open(path, "wb") as f:
            f.write(blob)
        print(f"{path}: {len(blob)} bytes, ImageBase {IMAGE_BASE:#x}, "
              f"IMAGE_FILE_DLL, IMAGE_SUBSYSTEM_NATIVE, 2 imports")
    else:
        # hand.exe belongs to Genesis-userland now (src/hand builds it with
        # MinGW); this fallback emitter needs an explicit output path.
        if len(sys.argv) < 2:
            sys.exit("usage: mkpe.py OUTPUT.exe | mkpe.py --sys [OUTPUT.sys]")
        path = sys.argv[1]
        blob = build()
        with open(path, "wb") as f:
            f.write(blob)
        print(f"{path}: {len(blob)} bytes, ImageBase {IMAGE_BASE:#x}, no imports")
