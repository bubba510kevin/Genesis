[bits 16]

; The BIOS loads this sector at 0x7C00 and jumps there, so every absolute
; reference below - gdt_descriptor, the DAP, boot_drive, the far jump target -
; is relative to that address.
;
; This used to come from linker.ld, which placed section .boot at 0x7C00. Now
; that the sector is assembled standalone (nasm -f bin) there is no linker to
; supply it, and without `org` NASM assumes origin 0. The code then still runs
; - the BIOS jumps to 0x7C00 regardless - but every data reference points
; 0x7C00 too low, which decodes as garbage and raises #UD almost immediately.
org 0x7C00

global _boot_start

; Assembled standalone now (nasm -f bin), not linked with the kernel: ld
; cannot mix elf32/elf64, and a 16-bit object in an elf64 link is worse.
; So both former externs become build-time constants.
;   KERNEL_ENTRY  - fixed by linker.ld's KERNEL_LMA
;   kernel_sectors - passed by build.py with -D, computed from kernel.bin
%ifndef kernel_sectors
%error "build.py must pass -Dkernel_sectors=N"
%endif
; Where the kernel image is assembled in memory, and where boot.asm's 32-bit
; code calls into it. Must equal KERNEL_LMA in linker.ld.
;
; This was 0x7E00 - the byte after this sector - for a long time, and moving
; it is the point of the unreal-mode machinery below. Loading at 0x7E00 grows
; the image UPWARD into the 32-bit stack this sector sets up at 0x90000, so
; the kernel had a hard ceiling of 0x90000 - 0x7E00 = 557568 bytes, and it had
; reached 98% of it. At 0x100000 the ceiling is instead "however much RAM is
; mapped", which is a limit nobody has to keep raising.
KERNEL_ENTRY equ 0x100000

; INT 13h cannot write above 1MB: the DAP names its destination as a real-mode
; segment:offset. So each chunk is read HERE first and then copied up. 0x8000
; is above this sector's stack (0x7C00, growing down) and below 0x10000, so
; the whole 32KB chunk sits inside one real-mode segment.
;
; The AP trampoline is copied to this same address later (see
; kernel/arch/ap_trampoline.c) - long after this buffer is finished with.
LOAD_BUF     equ 0x8000
LOAD_BUF_SEG equ 0x0800

_boot_start:
    jmp 0x0000:flush_cs

; Falls straight through to __start. There used to be a `jmp __start` here;
; it went when the sector had to be shrunk below 446 bytes to leave the MBR
; partition-table area clear - see the note above the padding at the end.
flush_cs:
__start:
    cli
    mov [boot_drive], dl
    xor ax, ax
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov sp, 0x7C00

    call detect_memory

    ; NMI off and A20 on, BOTH BEFORE the kernel is loaded.
    ;
    ; A20 used to be enabled after load_kernel, and had to move: the kernel is
    ; now copied to 0x100000, and with the A20 gate still closed every address
    ; with bit 20 set wraps to the megabyte below it. The copy would appear to
    ; succeed and would in fact be scribbling over the IVT, the BDA, this
    ; sector and the buffer it is copying out of.
    in al, 0x70
    or al, 0x80
    out 0x70, al

    in al, 0x92
    or al, 2
    out 0x92, al

    call load_kernel

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:init_32bit

; Loads the kernel image in chunks to KERNEL_ENTRY (0x100000).
;
; The destination is above 1MB and INT 13h cannot reach it - the DAP's
; destination is a real-mode segment:offset pair, so the BIOS can only write
; the first megabyte. Each chunk is therefore read into LOAD_BUF down in low
; memory and then copied up by this code, which CAN address 4GB because of
; enter_unreal below.
;
; The previous version read straight to the final address and advanced the
; destination *segment* per chunk (offset fixed at 0), which sidestepped the
; 64KB segment-wrap problem but pinned the kernel to low memory. The chunking
; is kept for the same reason it existed: one INT 13h read must land inside a
; single 64KB real-mode segment.
;
; Bounded only by the 16-bit sector-count field per call - up to 32MB per
; chunk, nowhere near a real constraint - and now by how much physical memory
; the kernel's own page tables cover, not by anything in this file.
CHUNK_SECTORS equ 64        ; 64 sectors = 32KB per read, one buffer's worth

load_kernel:
    mov cx, kernel_sectors      ; cx = sectors remaining
    mov dword [cur_dest], KERNEL_ENTRY
    mov dword [cur_lba], 1           ; LBA 1 = first sector after the boot sector

.load_loop:
    cmp cx, 0
    je .done

    mov ax, CHUNK_SECTORS
    cmp cx, ax
    jae .have_chunk
    mov ax, cx                  ; last, partial chunk
.have_chunk:
    mov [dap_count], ax

    ; dap_segment, dap_offset and dap_lba_hi are constant for every read - the
    ; buffer never moves and no LBA here is above 4 billion - so they are set
    ; in the DAP's own initialiser rather than rewritten each iteration. That
    ; is three fewer instructions in a sector that has to stay under 446 bytes.
    mov eax, [cur_lba]
    mov [dap_lba_lo], eax

    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    ; Re-arm the big ES limit AFTER every BIOS call, not once before the
    ; loop. INT 13h runs in real mode and is entitled to load ES itself; a
    ; real-mode load of a segment register rewrites its base, and on the
    ; return path there is no way to tell whether the 4GB limit survived. Re-
    ; entering unreal mode costs a few instructions and no BIOS involvement,
    ; so it is cheaper than being wrong here would be to diagnose.
    call enter_unreal

    push cx                     ; sectors remaining - rep movsd wants ECX
    movzx ecx, word [dap_count]
    shl ecx, 7                  ; sectors * 512 / 4 = dwords to copy
    mov esi, LOAD_BUF           ; DS base is 0, so this is a flat address
    mov edi, [cur_dest]         ; ES has a 4GB limit: edi may exceed 1MB
    cld
    a32 rep movsd               ; a32: 32-bit addressing inside 16-bit code
    pop cx

    movzx eax, word [dap_count] ; sectors just transferred
    sub cx, ax                  ; sectors remaining
    add [cur_lba], eax          ; advance LBA by sectors read
    shl eax, 9                  ; sectors * 512 = bytes just copied
    add [cur_dest], eax

    jmp .load_loop

.done:
    ret

; --- unreal mode --------------------------------------------------------
; Gives ES a 4GB limit while the CPU stays in real mode, so the copy loop
; above can write to 0x100000 and beyond with a 32-bit offset.
;
; The mechanism is the descriptor cache. A segment register's base, limit and
; attributes live in a hidden cache that is only reloaded when the register is
; written. Entering protected mode long enough to load ES from a
; 4GB-limit descriptor, then leaving without touching ES again, leaves that
; limit in the cache while everything else is real mode once more.
;
; Two things this must NOT do, both of which undo it silently:
;   - reload ES afterwards. A real-mode segment load rewrites the cached base
;     from the value written and drops the limit back to 64KB. So ES is left
;     holding the selector 0x10, whose descriptor base happens to be 0, which
;     is exactly the base wanted.
;   - reload CS or SS. CS is 0x0000 here (the far jump at _boot_start made it
;     so), which is the null selector in protected mode - but the cache still
;     holds a valid base-0 code descriptor and is never rewritten, so
;     execution continues across both transitions. That is the whole trick,
;     and a far jump anywhere in this routine would end it.
enter_unreal:
    cli
    push eax
    push bx                     ; the copy loop below does not use BX, but a
                                ; routine that silently eats a register is a
                                ; trap for the next caller

    lgdt [gdt_descriptor]

    mov eax, cr0
    or  al, 1                   ; CR0.PE - 16-bit protected mode, same CS
    mov cr0, eax
    jmp short $+2               ; flush the prefetch queue

    mov bx, 0x10                ; the flat 4GB data descriptor
    mov es, bx                  ; loads its limit into ES's hidden cache

    and al, 0xFE                ; CR0.PE back off - real mode, ES untouched
    mov cr0, eax
    jmp short $+2

    pop bx
    pop eax
    ret

; Queries the BIOS memory map into E820_BUFFER, in the format kernel/e820.c
; expects: a 64-bit entry count, then up to E820_MAX_ENTRIES 24-byte entries.
;
; This has to happen here. E820 is int 0x15, and there is no int 0x15 after
; the switch to protected mode - the kernel cannot go back and ask later. The
; buffer sits at 0x5000: below the kernel load address, above the BDA, and
; inside the region the PMM reserves at boot, so it survives until parsed.
;
; On failure the count is left at 0 and e820_load() substitutes a conservative
; built-in map, so a machine without E820 still boots.
E820_BUFFER      equ 0x5000
E820_ENTRIES     equ E820_BUFFER + 8
E820_MAX         equ 32
E820_SMAP        equ 0x534D4150      ; 'SMAP', echoed back in eax

detect_memory:
    ; Zero the count first: every failure path below just returns, and the
    ; kernel reads a count of 0 as "the BIOS did not answer".
    mov dword [E820_BUFFER], 0
    mov dword [E820_BUFFER + 4], 0

    mov di, E820_ENTRIES
    xor ebx, ebx                     ; continuation value, 0 = start over
    xor si, si                       ; entries accepted so far

.loop:
    mov eax, 0xE820
    mov edx, E820_SMAP
    mov ecx, 24                      ; ask for the ACPI 3.0 24-byte form
    ; A BIOS that only knows the 20-byte form leaves the attribute dword
    ; untouched. Presetting it to 1 means such an entry reads back as "valid"
    ; rather than as whatever happened to be in the buffer.
    mov dword [es:di + 20], 1
    int 0x15
    jc .done                         ; CF on the FIRST call = unsupported;
                                     ; on a later one = list finished
    cmp eax, E820_SMAP
    jne .done

    ; A zero-length entry is legal and means nothing; skip without counting.
    mov ecx, [es:di + 8]
    or  ecx, [es:di + 12]
    jz .next

    ; 24-byte entries may set bit 0 of the attributes to 0 to say "ignore me".
    test byte [es:di + 20], 1
    jz .next

    inc si
    add di, 24
    cmp si, E820_MAX
    jae .done                        ; buffer full; keep what we have

.next:
    test ebx, ebx                    ; zero continuation = that was the last
    jnz .loop

.done:
    mov [E820_BUFFER], si
    ret

disk_error:
    mov si, err_msg
    call print_string_rm
    cli
    hlt

print_string_rm:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print_string_rm
.done:
    ret

boot_drive: db 0
err_msg: db "Disk read error!", 0
cur_dest: dd 0          ; flat 32-bit address the next chunk is copied to
cur_lba: dd 0

align 4
dap:
    db 0x10
    db 0
dap_count:   dw 0
dap_offset:  dw 0                ; constant: LOAD_BUF is segment-aligned
dap_segment: dw LOAD_BUF_SEG     ; constant: every read lands in the buffer
dap_lba_lo:  dd 0
dap_lba_hi:  dd 0                ; constant: no LBA here is above 2TB

[bits 32]
init_32bit:
    mov ax, 0x10
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ebp, 0x90000
    mov esp, ebp

    call KERNEL_ENTRY          ; 32-bit entry in .lowtext
    cli
hang:
    hlt
    jmp hang

[bits 16]
align 4
gdt_start:
    dd 0x0
    dd 0x0
gdt_code:
    dw 0xFFFF
    dw 0x0
    db 0x0
    db 10011010b
    db 11001111b
    db 0x0
gdt_data:
    dw 0xFFFF
    dw 0x0
    db 0x0
    db 10010010b
    db 11001111b
    db 0x0
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; --- MBR partition table ------------------------------------------------
; Bytes 446..509 of a boot sector are where a partitioned disk keeps its four
; 16-byte partition entries, and Genesis's own partition scanner (kernel/dev/
; part.c) reads them off every disk it finds - including this one, because
; os.img is what the machine boots from.
;
; They were all zero here by accident for a long time: the code was short
; enough that the tail padding covered them. When the unreal-mode loader made
; the sector 464 bytes long, the GDT and its descriptor landed inside the
; first entry, whose type byte read back as 0xCF - and the boot disk suddenly
; grew a partition that was really a code fragment. Nothing crashed; the
; volume numbering just shifted by one and C: moved to HarddiskVolume2.
;
; So this is now explicit rather than incidental: pad to 446, declare four
; zeroed entries, and let NASM fail the build if the code ever grows into
; them again. "This image is not partitioned" is a thing worth stating.
times 446-($-$$) db 0
times 64 db 0
dw 0xAA55