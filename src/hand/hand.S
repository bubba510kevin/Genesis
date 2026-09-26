/* hand.exe - the bisect anchor.
 *
 * WHAT THIS IS FOR
 *
 * When hello.exe fails, the question is always "which layer". hello.exe is a
 * MinGW-compiled PE that imports from ntdll.dll, so a failure there could be
 * the PE loader, the import directory, the export directory, the IAT, ntdll's
 * stubs, the syscall convention, or the kernel's NT surface - seven answers,
 * and the symptom is usually the same silence for all of them.
 *
 * This image cuts that in half in one step. It has NO imports, NO ntdll, NO
 * CRT and no dependency on anything the loader does after mapping sections
 * and jumping to the entry point. It issues raw `syscall` instructions with
 * the numbers and register convention written out below, where they can be
 * read. So:
 *
 *     hand.exe works, hello.exe does not  ->  imports, ntdll, or the IAT
 *     both fail                           ->  the loader or the kernel
 *
 * That is its whole job, and everything else here is subordinate to it. Each
 * step also prints, so a partial failure says how far it got rather than
 * leaving the same silence the anchor exists to break.
 *
 * WHY THIS IS ASSEMBLED RATHER THAN EMITTED BYTE BY BYTE
 *
 * It used to be src/mkpe.py, which built the whole PE - headers, section
 * table, file offsets and machine code - from scratch, on the reasoning that
 * the first image to test a loader with should be one you can account for
 * completely rather than four hundred bytes of a linker's opinion. That
 * reasoning is sound and it is not what went wrong.
 *
 * What went wrong is that the generator hardcoded its raw file offsets with a
 * 0x200 gap for .text. When the Win64 convention landed and every call site
 * grew a stack frame, .text reached 0x377, and the section table began
 * describing a .rdata that started inside .text. The loader mapped exactly
 * what it was told, so the image read its own string constants as machine
 * code, and what came back was STATUS_ACCESS_VIOLATION from a pointer check
 * in nt.c with nothing pointing at the cause. The anchor had quietly acquired
 * a third dependency - its own generator - which is the one thing an anchor
 * must not have.
 *
 * The trade made here: MinGW's assembler and linker compute the layout, which
 * they have been doing correctly for decades, and this file contains only the
 * part that is actually Genesis's own - the syscall numbers, the register
 * convention, and the structure shapes. A linker bug is now shared with
 * hello.exe and no longer isolated by this image. That is a real loss, and it
 * is a smaller one than a hand-rolled layout that can drift from its own
 * content: the difference this anchor is actually asked to isolate is
 * imports/ntdll/IAT, and that difference is fully intact.
 *
 * THE CONVENTION, WRITTEN OUT
 *
 * Win64, unchanged, with RCX copied to R10 - the same one ntdll's stubs use,
 * because `syscall` destroys RCX (it holds the return RIP) and R11 (RFLAGS).
 *
 *     RAX             system call number
 *     R10             argument 1  (copied from RCX)
 *     RDX, R8, R9     arguments 2, 3, 4
 *     [rsp+0x28]      argument 5, then +8 per argument after it
 *
 * 0x28 and not 0x00: the ABI puts the return address at [rsp] and reserves 32
 * bytes of shadow space above it. Nothing here executes a CALL before a
 * syscall, so the frame allocated at entry stands in for both - which is
 * exactly what a compiler would have emitted at a call site, and is why the
 * kernel needs no special case for hand-written callers.
 *
 * RAX comes back holding an NTSTATUS. It is a status and not a count: the
 * byte count for a read or a write is reported only in the IO_STATUS_BLOCK's
 * Information field, which is why the second write below takes its length
 * from there.
 */

/* Generated from kernel/include/nt.h by src/ntdll/mknums.py, and included
 * rather than repeated. This file is the fourth language in the tree that
 * needs these numbers; a hand-kept list here would be the fourth rival copy
 * of a constant that already caused this project one silent failure. */
#include "../ntdll/ntsyscalls.h"

/* These are Windows ABI constants rather than Genesis's, which is why they
 * are written here instead of read out of the kernel: their values are fixed
 * by the platform being reimplemented, not chosen by this project. The
 * syscall numbers above are the opposite, and are generated for that reason. */
    .set GENERIC_READ,          0x80000000
    .set GENERIC_WRITE,         0x40000000
    .set OBJ_CASE_INSENSITIVE,  0x00000040
    .set OA_LENGTH,             48          /* sizeof(OBJECT_ATTRIBUTES)   */
    .set BUF_SIZE,              512
    .set NT_CURRENT_PROCESS,    -1

/* One system call. The RCX->R10 copy is here rather than at each call site so
 * that it cannot be the thing that was forgotten. */
    .macro  ntcall num
    mov     %rcx, %r10
    mov     $\num, %eax
    syscall
    .endm

/* NtWriteFile(hcon, NULL, NULL, NULL, &iosb, src, len, NULL, NULL).
 *
 * Arguments two to four select asynchronous completion - an event, an APC
 * routine and its context - and NULL for all three is a synchronous write,
 * which is what this wants and what the kernel implements. */
    .macro  write_con src, len
    movq    hcon(%rip), %rcx
    xor     %edx, %edx
    xor     %r8d, %r8d
    xor     %r9d, %r9d
    lea     iosb(%rip), %rax
    mov     %rax, 0x28(%rsp)
    lea     \src(%rip), %rax
    mov     %rax, 0x30(%rsp)
    movq    $\len, 0x38(%rsp)
    movq    $0, 0x40(%rsp)
    movq    $0, 0x48(%rsp)
    ntcall  NT_SYS_WRITE_FILE
    .endm

/* NtOpenFile(&handle, access, &attrs, &iosb, ShareAccess=0, OpenOptions=0). */
    .macro  open_file handle, access, attrs
    lea     \handle(%rip), %rcx
    mov     $\access, %edx
    lea     \attrs(%rip), %r8
    lea     iosb(%rip), %r9
    movq    $0, 0x28(%rsp)
    movq    $0, 0x30(%rsp)
    ntcall  NT_SYS_OPEN_FILE
    .endm

/* Print through NtDisplayString and stop. Used before a console handle
 * exists, and on every path where one could not be got - a bail that needs
 * the thing that failed is a bail that says nothing. */
    .macro  die msg
    lea     \msg(%rip), %rcx
    ntcall  NT_SYS_DISPLAY_STRING
    mov     $NT_CURRENT_PROCESS, %rcx
    mov     $1, %edx
    ntcall  NT_SYS_TERMINATE_PROCESS
    .endm

    .text
    .globl  start
start:
    /* 16-align, then reserve the argument area every syscall below writes
     * into: 8 for the absent return address, 32 of shadow space, and room for
     * arguments five to nine at 0x28..0x48. */
    and     $-16, %rsp
    sub     $0x60, %rsp

    /* Step one: prove the image ran at all. NtDisplayString is the only call
     * here that needs no handle, so it works before anything has been opened
     * and still works when everything else has failed. */
    lea     banner_us(%rip), %rcx
    ntcall  NT_SYS_DISPLAY_STRING

    /* Step two: \??\CON. The same object /dev/console resolves to - one
     * console with two spellings, which is the property the dual-personality
     * design rests on. */
    open_file hcon, GENERIC_WRITE, con_oa
    test    %eax, %eax
    jz      con_ok
    die     err_con
con_ok:

    /* Step three: \??\C:\etc\motd. \??\C: names the volume and \etc\motd is
     * the unparsed remainder handed to it, which is what the object namespace
     * was built for. */
    open_file hfile, GENERIC_READ, motd_oa
    test    %eax, %eax
    jz      file_ok
    write_con err_file_txt, err_file_len
    jmp     finish
file_ok:

    /* Step four: read it. Arguments five to seven are IoStatusBlock, Buffer
     * and Length. */
    movq    hfile(%rip), %rcx
    xor     %edx, %edx
    xor     %r8d, %r8d
    xor     %r9d, %r9d
    lea     iosb(%rip), %rax
    mov     %rax, 0x28(%rsp)
    lea     buf(%rip), %rax
    mov     %rax, 0x30(%rsp)
    movq    $BUF_SIZE, 0x38(%rsp)
    movq    $0, 0x40(%rsp)
    movq    $0, 0x48(%rsp)
    ntcall  NT_SYS_READ_FILE
    test    %eax, %eax
    jz      read_ok
    write_con err_read_txt, err_read_len
    jmp     close_file
read_ok:

    /* Step five: write it back out. The length comes from the read's
     * IO_STATUS_BLOCK Information field and NOT from RAX, which is a status.
     * Reading a count out of a status is a bug that looks like a short read. */
    write_con note_txt, note_len

    movq    hcon(%rip), %rcx
    xor     %edx, %edx
    xor     %r8d, %r8d
    xor     %r9d, %r9d
    lea     iosb(%rip), %rax
    mov     %rax, 0x28(%rsp)
    lea     buf(%rip), %rax
    mov     %rax, 0x30(%rsp)
    movq    iosb+8(%rip), %rax          /* IoStatusBlock.Information       */
    mov     %rax, 0x38(%rsp)
    movq    $0, 0x40(%rsp)
    movq    $0, 0x48(%rsp)
    ntcall  NT_SYS_WRITE_FILE

close_file:
    movq    hfile(%rip), %rcx
    ntcall  NT_SYS_CLOSE

    /* Step six: the TEB and the PEB, with no system calls at all. Each check
     * is a fact a Windows program expects to know about itself without
     * asking, and each is only true because the kernel built the block at
     * execve time and the context switch restores the GS base that reaches
     * it.
     *
     * Both checks are written to need no address constant. NT_TEB_BASE lives
     * in kernel/include/teb.h, and the previous version of this program
     * hardcoded it - a second home for the one constant this project has
     * already had three copies of. Self-consistency and the image's own
     * headers prove more, and prove it without anything to keep in sync. */
    mov     %gs:0x30, %rax              /* TEB->Self                       */
    test    %rax, %rax
    jz      teb_bad
    mov     0x30(%rax), %rcx            /* read Self again THROUGH itself  */
    cmp     %rax, %rcx
    jne     teb_bad
    write_con teb_txt, teb_len
    jmp     peb_check
teb_bad:
    write_con err_teb_txt, err_teb_len

peb_check:
    /* PEB->ImageBaseAddress must point at this image's own headers. That
     * checks the PEB, the field offset and the loader's mapping of
     * SizeOfHeaders in one go - and the headers are mapped read-only and
     * no-execute now, so it also proves a data read of an NX page works. */
    mov     %gs:0x60, %rax              /* PEB                             */
    test    %rax, %rax
    jz      peb_bad
    mov     0x10(%rax), %rax            /* PEB->ImageBaseAddress           */
    test    %rax, %rax
    jz      peb_bad
    cmpw    $0x5A4D, (%rax)             /* 'MZ'                            */
    jne     peb_bad
    movl    0x3C(%rax), %ecx            /* e_lfanew, zero-extended         */
    cmpl    $0x00004550, (%rax,%rcx)    /* 'PE\0\0'                        */
    jne     peb_bad
    write_con peb_txt, peb_len
    jmp     finish
peb_bad:
    write_con err_peb_txt, err_peb_len

finish:
    movq    hcon(%rip), %rcx
    ntcall  NT_SYS_CLOSE

    mov     $NT_CURRENT_PROCESS, %rcx
    xor     %edx, %edx
    ntcall  NT_SYS_TERMINATE_PROCESS

    /* NtTerminateProcess does not return. If it ever does, stopping here is
     * better than falling into whatever follows .text. */
hang:
    jmp     hang

/* --- read-only data ------------------------------------------------------
 *
 * UNICODE_STRING is Length, MaximumLength, four bytes of padding nothing
 * names, then Buffer at offset 8 - sixteen bytes, not twelve. The padding is
 * written out rather than left implicit because deleting it is exactly the
 * mistake that made every name-taking call fail silently once before: Buffer
 * moves to offset 4, and the kernel reads a pointer made of four zero bytes
 * and the low half of the real one.
 *
 * .string16 emits each character zero-extended to sixteen bits, which is
 * UTF-16LE for ASCII, and appends a terminating null word - hence the -2 in
 * every length below.
 *
 * Each structure is .balign 8'd. x86 would not fault on a misaligned Buffer
 * and the kernel would read it correctly anyway, but these are ABI structures
 * and an ABI structure is whatever a compiler would have produced - which is
 * the same rule that says the padding above stays. */
    .section .rdata,"dr"
    .balign 8

banner_txt:
    .string16 "hello from a hand-written PE, loaded by Genesis\n"
banner_end:
    .balign 8
banner_us:
    .short  banner_end - banner_txt - 2     /* Length                      */
    .short  banner_end - banner_txt - 2     /* MaximumLength               */
    .long   0                               /* the padding, named          */
    .quad   banner_txt                      /* Buffer                      */

con_txt:
    .string16 "\\??\\CON"
con_end:
    .balign 8
con_us:
    .short  con_end - con_txt - 2
    .short  con_end - con_txt - 2
    .long   0
    .quad   con_txt
    .balign 8
con_oa:
    .long   OA_LENGTH                       /* Length                      */
    .long   0                               /* RootDirectory is NULL only  */
    .quad   0
    .quad   con_us                          /* ObjectName                  */
    .long   OBJ_CASE_INSENSITIVE            /* Attributes                  */
    .long   0
    .quad   0                               /* SecurityDescriptor          */
    .quad   0                               /* SecurityQualityOfService    */

motd_txt:
    .string16 "\\??\\C:\\etc\\motd"
motd_end:
    .balign 8
motd_us:
    .short  motd_end - motd_txt - 2
    .short  motd_end - motd_txt - 2
    .long   0
    .quad   motd_txt
    .balign 8
motd_oa:
    .long   OA_LENGTH
    .long   0
    .quad   0
    .quad   motd_us
    .long   OBJ_CASE_INSENSITIVE
    .long   0
    .quad   0
    .quad   0

/* Console output is bytes, not UTF-16: NtWriteFile hands them to the same
 * console object ash writes through. */
note_txt:
    .ascii  "PE read /etc/motd through \\??\\C: -> "
    .set    note_len, . - note_txt
teb_txt:
    .ascii  "TEB self-pointer is consistent\n"
    .set    teb_len, . - teb_txt
peb_txt:
    .ascii  "PEB ImageBaseAddress points at this image's MZ/PE headers\n"
    .set    peb_len, . - peb_txt
err_file_txt:
    .ascii  "could not open \\??\\C:\\etc\\motd\n"
    .set    err_file_len, . - err_file_txt
err_read_txt:
    .ascii  "read failed\n"
    .set    err_read_len, . - err_read_txt
err_teb_txt:
    .ascii  "TEB self-pointer is WRONG\n"
    .set    err_teb_len, . - err_teb_txt
err_peb_txt:
    .ascii  "PEB ImageBaseAddress is WRONG\n"
    .set    err_peb_len, . - err_peb_txt

/* The one failure that cannot be reported through the console, because it is
 * the console that failed. NtDisplayString goes straight to the kernel's own
 * output path and needs no handle. */
err_con_txt:
    .string16 "could not open \\??\\CON\n"
err_con_end:
    .balign 8
err_con:
    .short  err_con_end - err_con_txt - 2
    .short  err_con_end - err_con_txt - 2
    .long   0
    .quad   err_con_txt

/* --- writable data ------------------------------------------------------- */
    .section .data,"dw"
    .balign 8
hcon:   .quad   0
hfile:  .quad   0
iosb:   .quad   0                           /* Status                      */
        .quad   0                           /* Information: the byte count */
buf:    .space  BUF_SIZE, 0
