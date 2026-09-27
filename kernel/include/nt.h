#ifndef NT_H
#define NT_H

#include "paging.h"
#include "typesk.h"

struct syscall_frame;

/* The NT system-call surface.
 *
 * --- The number space is ours -------------------------------------------
 * NT's own call numbers are not published, change between Windows releases,
 * and are not something a clean-room implementation could claim to have
 * derived from documentation anyway. What IS documented is the Nt* interface,
 * and the ntdll that loads a number into eax is built here too - so the
 * number is a private contract between this table and our own ntdll, and it
 * gets assigned when a call lands rather than guessed in advance.
 *
 * Starting at 1 rather than 0 on purpose: a jump to a zeroed page executes
 * `add %al,(%rax)` and eventually reaches a syscall with rax = 0, and having
 * that be a valid call is how a wild jump gets mistaken for a working
 * program.
 *
 * --- Why these two, and only these two, so early -------------------------
 * The PE loader's milestone is a hand-written image PRINTING to the console,
 * and an image that cannot print or exit proves only that it was mapped. So
 * the two calls the milestone cannot be observed without land with the
 * loader; the rest of the surface - NtOpenFile, NtReadFile, the
 * OBJECT_ATTRIBUTES and UNICODE_STRING path resolution, the TEB at GS:0 -
 * stays where the roadmap puts it, in its own change.
 *
 * NtDisplayString is a real documented native call, used by the session
 * manager for exactly this, rather than a throwaway invented to make a demo
 * work. It takes one argument and will not need rewriting when the rest of
 * the surface arrives. */
#define NT_SYS_DISPLAY_STRING     0x01
#define NT_SYS_TERMINATE_PROCESS  0x02
#define NT_SYS_OPEN_FILE          0x03
#define NT_SYS_CLOSE              0x04
#define NT_SYS_READ_FILE          0x05
#define NT_SYS_WRITE_FILE         0x06
#define NT_SYS_ALLOCATE_VIRTUAL   0x07

/* --- the dispatcher objects (ROADMAP item 14) ----------------------------
 *
 * The ring-3 half. The objects themselves are kernel/obj/dispatch.c and
 * predate these by one change; without a syscall a Win32 program's
 * CreateMutexW had nothing to land on, which is most of the point of having
 * named synchronisation objects at all.
 *
 * These numbers are Genesis's own, like every number above - they are not
 * Windows' syscall numbers and are not trying to be. A real ntdll's table
 * changes between builds of Windows, so matching it would be matching a
 * moving target for no benefit: the compatibility that matters is the ABI at
 * the DLL boundary, which is what src/winhello demonstrates. */
#define NT_SYS_CREATE_EVENT       0x08
#define NT_SYS_OPEN_EVENT         0x09
#define NT_SYS_SET_EVENT          0x0A
#define NT_SYS_RESET_EVENT        0x0B
#define NT_SYS_WAIT_SINGLE        0x0C
#define NT_SYS_CREATE_SEMAPHORE   0x0D
#define NT_SYS_RELEASE_SEMAPHORE  0x0E
#define NT_SYS_CREATE_MUTANT      0x0F
#define NT_SYS_RELEASE_MUTANT     0x10

/* NtQuerySecurityObject(Handle, SecurityInformation, PSECURITY_DESCRIPTOR,
 *                       Length, PULONG LengthNeeded)
 *
 * The Windows half of the permission story. A file's ACL is stored in the
 * NFSv4 form ZFS writes; this hands it back as a self-relative
 * SECURITY_DESCRIPTOR, which is what a Win32 program calling
 * GetSecurityInfo/GetFileSecurity expects to be given. See
 * kernel/include/ntsec.h for what is and is not a translation. */
#define NT_SYS_QUERY_SECURITY     0x11

/* --- threads (ROADMAP item 14(a)) ------------------------------------------
 *
 * NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK, POBJECT_ATTRIBUTES,
 *                  HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument,
 *                  ULONG CreateFlags, SIZE_T ZeroBits, SIZE_T StackSize,
 *                  SIZE_T MaximumStackSize, PVOID AttributeList)
 *
 * Eleven arguments, seven of them on the stack. ProcessHandle must be
 * NtCurrentProcess() - another process's threads need NtCreateProcess, which
 * does not exist - and that is STATUS_NOT_IMPLEMENTED, not silently ignored.
 * THREAD_CREATE_FLAGS_CREATE_SUSPENDED makes a thread with a suspend count
 * of 1 that is not put on any run queue until NtResumeThread.
 *
 * The new thread starts in ntdll!RtlUserThreadStart, as on NT, with a
 * private TEB (GS:0 is its own, so GetCurrentThreadId and GetLastError are
 * per thread) and a private stack. ONE deliberate difference from NT, in a
 * place only our own ntdll can see: NT enters RtlUserThreadStart with
 * StartRoutine in RCX and Argument in RDX. This kernel leaves ring 0 by
 * SYSRET, which spends RCX on the return RIP, so it passes StartRoutine in
 * RDX and Argument in R8 instead. */
#define NT_SYS_CREATE_THREAD      0x12

/* NtTerminateThread(HANDLE Thread, NTSTATUS ExitStatus). The calling
 * thread (NtCurrentThread(), or a handle to itself) exits the ordinary way.
 * ANOTHER thread of this process is retired from outside, like a thread
 * killed by exit_group: wherever it is - ring 3 on another CPU, blocked in a
 * wait, suspended, not yet started - it never runs another instruction, its
 * handle is signalled with the full 32-bit ExitStatus, and mutants it held
 * are abandoned. That is what TerminateThread does on Windows too, and is
 * why Microsoft's documentation says never to use it: whatever user-mode
 * state the thread was halfway through changing stays half-changed. A thread
 * that has already exited is STATUS_THREAD_IS_TERMINATING. */
#define NT_SYS_TERMINATE_THREAD   0x13

/* NtQueryInformationThread(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG)
 * for ThreadBasicInformation (0) only - which is what GetExitCodeThread and
 * GetThreadId are built on. Other classes are STATUS_INVALID_INFO_CLASS. */
#define NT_SYS_QUERY_THREAD       0x14

#define NT_CURRENT_THREAD         0xFFFFFFFFFFFFFFFEULL    /* (HANDLE)-2 */

/* --- the machine, processes and scheduling (kernel/exec/nt_sys.c) ---------
 *
 * What a Win32 program asks about the processors it runs on and how it
 * steers its threads across them: GetSystemInfo and GetLogicalProcessor-
 * Information(Ex) (NtQuerySystemInformation(Ex)), GetProcessAffinityMask /
 * SetProcessAffinityMask and GetProcessTimes (NtQuery/SetInformation-
 * Process), SetThreadAffinityMask, SetThreadIdealProcessor(Ex),
 * SetThreadPriority and SetThreadGroupAffinity (NtSetInformationThread),
 * SwitchToThread (NtYieldExecution), Sleep (NtDelayExecution),
 * GetCurrentProcessorNumber(Ex), QueryPerformanceCounter and
 * GetSystemTimeAsFileTime.
 *
 * NtQuerySystemInformation(Class, Buffer, Length, ReturnLength)
 * NtQuerySystemInformationEx(Class, InputBuffer, InputLength, Buffer,
 *                            Length, ReturnLength)
 * NtQueryInformationProcess(Handle, Class, Buffer, Length, ReturnLength)
 * NtSetInformationProcess(Handle, Class, Buffer, Length)
 * NtSetInformationThread(Handle, Class, Buffer, Length)
 * NtYieldExecution(void)
 * NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER Interval)
 * NtGetCurrentProcessorNumber(void)
 * NtGetCurrentProcessorNumberEx(PPROCESSOR_NUMBER)
 * NtQueryPerformanceCounter(PLARGE_INTEGER Counter, PLARGE_INTEGER Freq)
 * NtQuerySystemTime(PLARGE_INTEGER SystemTime) */
#define NT_SYS_QUERY_SYSTEM_INFO     0x15
#define NT_SYS_QUERY_SYSTEM_INFO_EX  0x16
#define NT_SYS_QUERY_PROCESS         0x17
#define NT_SYS_SET_PROCESS           0x18
#define NT_SYS_SET_THREAD            0x19
#define NT_SYS_YIELD                 0x1A
#define NT_SYS_DELAY                 0x1B
#define NT_SYS_CURRENT_PROCESSOR     0x1C
#define NT_SYS_CURRENT_PROCESSOR_EX  0x1D
#define NT_SYS_PERF_COUNTER          0x1E
#define NT_SYS_SYSTEM_TIME           0x1F

/* NtWaitForAlertByThreadId(PVOID Address, PLARGE_INTEGER Timeout)
 * NtAlertThreadByThreadId(HANDLE ThreadId)
 *
 * Windows 8's primitive under WaitOnAddress, SRW locks and condition
 * variables: a thread sleeps until ANOTHER thread alerts it by id (or the
 * timeout passes); an alert that arrives first is remembered, so a wake
 * racing the sleep is never lost. Address is only a hint for debuggers.
 * STATUS_ALERTED when woken, STATUS_TIMEOUT on the deadline. The
 * address-keyed waiting itself is ntdll's (RtlWaitOnAddress). */
#define NT_SYS_WAIT_ALERT_BY_TID     0x20
#define NT_SYS_ALERT_BY_TID          0x21

/* NtSuspendThread(HANDLE Thread, PULONG PreviousSuspendCount)
 * NtResumeThread(HANDLE Thread, PULONG PreviousSuspendCount)
 *
 * A counted suspension, in the calling process. Suspend raises the count
 * (STATUS_SUSPEND_COUNT_EXCEEDED past 127) and the thread stops before its
 * next instruction in ring 3: at once if it is in the kernel or blocked,
 * after a kick if it is running on another CPU, and on the way out of this
 * very call for NtCurrentThread(). Resume lowers it and lets the thread go
 * at 0. Both report the count as it was BEFORE the call - which is how
 * ResumeThread tells "resumed" (1) from "was not suspended" (0). */
#define NT_SYS_SUSPEND_THREAD        0x22
#define NT_SYS_RESUME_THREAD         0x23
#define NT_MAXIMUM_SUSPEND_COUNT     127
#define THREAD_CREATE_FLAGS_CREATE_SUSPENDED 0x00000001u
#define ThreadBasicInformation    0

/* SECURITY_INFORMATION bits, as passed in argument two. */
#define OWNER_SECURITY_INFORMATION 0x00000001u
#define GROUP_SECURITY_INFORMATION 0x00000002u
#define DACL_SECURITY_INFORMATION  0x00000004u
#define SACL_SECURITY_INFORMATION  0x00000008u

/* --- the stub calling convention ----------------------------------------
 *
 * The Win64 ABI, unchanged, with RCX copied to R10. That is what a real ntdll
 * stub does and it is now what ours does:
 *
 *     mov r10, rcx
 *     mov eax, <number>
 *     syscall
 *     ret
 *
 * Four instructions, identical for every call, and a compiler building the
 * DLL needs to know nothing about them - the stub receives its arguments
 * exactly where the ABI already put them and hands them straight on.
 *
 * R10 rather than RCX for the first argument because SYSCALL destroys RCX:
 * the CPU puts the return address there. That constraint is the hardware's,
 * and copying RCX to R10 is the only reason the stub has an instruction in it
 * at all.
 *
 * So, at the syscall instruction:
 *
 *     argument 1   R10  (copied from RCX)
 *     argument 2   RDX
 *     argument 3   R8
 *     argument 4   R9
 *     argument 5   [rsp+0x28]
 *     argument 6   [rsp+0x30]
 *     argument 7   [rsp+0x38]      ... and so on, eight bytes apart
 *
 * 0x28 and not 0 because the caller's frame is still intact: [rsp] holds the
 * return address into the caller, and the 32 bytes above it are the shadow
 * space the Win64 ABI reserves for arguments one to four whether or not
 * anyone uses it. Arguments five and beyond begin after that.
 *
 * This replaced an earlier convention of my own - arguments in RDI/RSI/RDX/
 * R10/R8/R9 with the rest pushed at [rsp+0] - which was fine while the only
 * caller was hand-written assembly and became the wrong answer the moment a
 * compiler was going to build the stubs. Matching Win64 means every stub is
 * the same four instructions; not matching it means each one hand-marshals
 * six registers and re-pushes the stack arguments, with an RSP-relative read
 * whose offsets shift as it pushes. Changed before ntdll existed to depend on
 * it, which is the only cheap moment there was going to be. */

/* Argument `n` (one-based, n >= 5) given the user RSP at the syscall. */
#define NT_STACK_ARG_OFFSET(n)  (0x28u + ((uint64)(n) - 5u) * 8u)

/* NTSTATUS values, from the published list. The top two bits are the
 * severity, so anything with bit 31 set is a failure to a caller testing
 * NT_SUCCESS - which is why a negated Linux errno must never be returned
 * here: -38 has bit 31 set for the wrong reason and means nothing to an NT
 * caller reading it as a status. */
#define STATUS_SUCCESS            0x00000000u
#define STATUS_NOT_IMPLEMENTED    0xC0000002u
#define STATUS_INVALID_HANDLE     0xC0000008u
/* Not an error in the usual sense: it is the documented way a caller asks how
 * big a security descriptor is, by calling once with a zero length and
 * reading LengthNeeded back. */
#define STATUS_BUFFER_TOO_SMALL   0xC0000023u
#define STATUS_ACCESS_VIOLATION   0xC0000005u
#define STATUS_INVALID_PARAMETER  0xC000000Du
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define STATUS_NOT_A_DIRECTORY    0xC0000103u
#define STATUS_TOO_MANY_OPENED_FILES 0xC000011Fu
#define STATUS_END_OF_FILE        0xC0000011u
#define STATUS_ACCESS_DENIED      0xC0000022u
#define STATUS_NAME_TOO_LONG      0xC0000106u
#define STATUS_NO_MEMORY          0xC0000017u
#define STATUS_PENDING            0x00000103u  /* also STILL_ACTIVE, 259 */
#define STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define STATUS_INVALID_INFO_CLASS 0xC0000003u
#define STATUS_NOT_SUPPORTED      0xC00000BBu
#define STATUS_INSUFFICIENT_RESOURCES 0xC000009Au
#define STATUS_NO_YIELD_PERFORMED 0x40000024u
#define STATUS_ALERTED            0x00000101u
#define STATUS_INVALID_CID        0xC000000Bu
#define STATUS_INVALID_PARAMETER_2 0xC00000F0u

/* NTSTATUS values the dispatcher objects need.
 *
 * STATUS_TIMEOUT is a SUCCESS code (top bit clear), not an error, and that is
 * not a curiosity: NT_SUCCESS(status) is true for it, so a caller that only
 * checks NT_SUCCESS and then assumes it holds the object has a bug Windows
 * has been catching for thirty years. Waiting is allowed to end without
 * acquiring, and the return distinguishes the two.
 *
 * STATUS_OBJECT_NAME_COLLISION is what a named create hits when the name is
 * taken. STATUS_OBJECT_NAME_EXISTS is the SUCCESS-shaped warning that comes
 * back when an OPEN-or-create found an existing one; only the first is used
 * here, because these creates do not open. */
#define STATUS_TIMEOUT            0x00000102u
#define STATUS_ALERTED            0x00000101u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_OBJECT_TYPE_MISMATCH  0xC0000024u
#define STATUS_MUTANT_NOT_OWNED   0xC0000046u
#define STATUS_SUSPEND_COUNT_EXCEEDED 0xC000004Au
#define STATUS_THREAD_IS_TERMINATING  0xC000004Bu
#define STATUS_ABANDONED_WAIT_0   0x00000080u  /* also WAIT_ABANDONED */

/* EVENT_TYPE. Upstream's spelling and upstream's values: the difference is
 * visible to a waiter, so a program that passes the wrong one gets a
 * different object rather than a differently-named one. */
#define NotificationEvent         0
#define SynchronizationEvent      1

/* NtAllocateVirtualMemory's AllocationType and Protect. Accepted and largely
 * ignored: every page this kernel hands out is committed, readable and
 * writable, so RESERVE without COMMIT would be a promise it cannot keep and
 * PAGE_READONLY a protection it cannot enforce until the VMM grows per-page
 * permission changes. Declared so the values a caller passes have names. */
#define MEM_COMMIT       0x00001000u
#define MEM_RESERVE      0x00002000u
#define PAGE_READWRITE   0x00000004u

/* UNICODE_STRING, as documented: a counted UTF-16 string whose Length is in
 * BYTES and not in characters. Reading it as characters is the classic way to
 * truncate every string in half, and it is the reason this is written out
 * rather than assumed. */
typedef struct {
    uint16 length;
    uint16 maximum_length;

    /* Four bytes the ABI puts here and nothing names. PWSTR is 8 bytes and
     * naturally aligned, so Buffer sits at offset 8 and the structure is 16
     * bytes - NOT 12.
     *
     * Spelled out rather than left to the compiler because the obvious
     * defensive move, marking this packed the way the PE headers are, DELETES
     * the padding and moves Buffer to offset 4. Every string then reads its
     * pointer from four bytes of zero and the low half of the real one, which
     * fails user_ptr_ok and turns every call taking a name into a silent
     * STATUS_ACCESS_VIOLATION. Packed is right for a FILE format, where the
     * bytes are whatever the spec says; it is wrong for an ABI structure,
     * where the bytes are whatever a compiler would have produced. */
    uint32 reserved;

    uint64 buffer;              /* PWSTR, a user-space pointer */
} nt_unicode_string_t;

/* OBJECT_ATTRIBUTES, as documented. Length is checked rather than trusted:
 * it is the struct's own declared size and a caller built against a different
 * header would otherwise have every field after it read at the wrong
 * offset. */
typedef struct __attribute__((packed)) {
    uint32 length;
    uint32 reserved;
    uint64 root_directory;        /* HANDLE; only NULL is supported     */
    uint64 object_name;           /* PUNICODE_STRING                    */
    uint32 attributes;
    uint32 reserved2;
    uint64 security_descriptor;
    uint64 security_quality_of_service;
} nt_object_attributes_t;

#define OBJ_CASE_INSENSITIVE  0x00000040u

/* IO_STATUS_BLOCK. Information is the byte count for a read or a write, and
 * it is the ONLY place that count is reported - the return value is a status,
 * not a length, which is the single biggest shape difference from read(2). */
typedef struct __attribute__((packed)) {
    uint64 status;
    uint64 information;
} nt_io_status_block_t;

/* RTL_USER_PROCESS_PARAMETERS - what a process was started WITH.
 *
 * The block PEB->ProcessParameters points at. Everything a Windows program
 * asks about its own invocation comes out of here and out of nothing else:
 * GetCommandLineW is a load of CommandLine.Buffer, GetStdHandle is a load of
 * one of the three handles below, GetEnvironmentStrings is Environment.
 *
 * That is why this had to exist before kernel32 could. GetStdHandle has no
 * other honest answer - a Windows process gets no preassigned handles the way
 * a POSIX one gets 0, 1 and 2, and the alternative of having kernel32 open
 * \??\CON lazily invents a console the caller never asked for and cannot be
 * redirected. The kernel knows what the process was started with; nothing in
 * user space does.
 *
 * --- Provenance and offsets ---------------------------------------------
 * ImagePathName at 0x60 and CommandLine at 0x70 are checkable against
 * Microsoft's own published winternl.h, which declares this structure as
 * Reserved1[16] + Reserved2[10] followed by those two fields - 16 + 80 = 0x60
 * exactly. The three standard handles at 0x20/0x28/0x30 and CurrentDirectory
 * at 0x38 are documented in Windows Internals and are what `dt
 * _RTL_USER_PROCESS_PARAMETERS` prints. Nothing here is from ReactOS or Wine.
 *
 * Declared as the prefix that is populated, like the TEB and PEB, and with
 * the same build-time offset assertions in ntproc.c. The real structure
 * continues past Environment with window and desktop fields that only a GUI
 * subsystem reads.
 *
 * CurrentDirectory is a CURDIR: a UNICODE_STRING and a HANDLE, not just a
 * path. The handle stays null - it is an open directory handle, and this
 * kernel has no way to open a directory as an object yet. A program that
 * checks it finds null and falls back to the path, which is the behaviour
 * null is for. */
typedef struct {
    uint32 maximum_length;            /* 0x000 bytes allocated             */
    uint32 length;                    /* 0x004 bytes used                  */
    uint32 flags;                     /* 0x008 */
    uint32 debug_flags;               /* 0x00C */

    uint64 console_handle;            /* 0x010 */
    uint32 console_flags;             /* 0x018 */
    uint32 reserved0;                 /* 0x01C ABI padding before a handle */

    /* The three GetStdHandle answers, in the order STD_INPUT_HANDLE,
     * STD_OUTPUT_HANDLE, STD_ERROR_HANDLE are numbered. */
    uint64 standard_input;            /* 0x020 */
    uint64 standard_output;           /* 0x028 */
    uint64 standard_error;            /* 0x030 */

    nt_unicode_string_t current_directory_path;   /* 0x038 CURDIR.DosPath  */
    uint64              current_directory_handle; /* 0x048 CURDIR.Handle   */

    nt_unicode_string_t dll_path;                 /* 0x050 */
    nt_unicode_string_t image_path_name;          /* 0x060 */
    nt_unicode_string_t command_line;             /* 0x070 */

    /* Not a UNICODE_STRING: a bare pointer to a block of NUL-separated
     * KEY=VALUE strings ended by a second NUL, exactly like the POSIX
     * environment except in UTF-16. */
    uint64 environment;               /* 0x080 */
} nt_rtl_user_process_params_t;

/* ACCESS_MASK bits. The generic ones are what a caller usually passes; the
 * specific ones are what they map onto. */
#define FILE_READ_DATA    0x00000001u
#define FILE_WRITE_DATA   0x00000002u
#define GENERIC_READ      0x80000000u
#define GENERIC_WRITE     0x40000000u

/* NtCurrentProcess(): the pseudo-handle every process has for itself. */
#define NT_CURRENT_PROCESS  ((uint64)-1)

/* --- HANDLE values -------------------------------------------------------
 * A HANDLE is an index into the same table a POSIX descriptor indexes, which
 * is the whole point of the object layer: one open file object, reachable by
 * either personality.
 *
 * It is not the index itself. NT guarantees NULL is never a valid handle, and
 * -1 is the current-process pseudo-handle - both of which index 0 and index
 * -1 would collide with. Shifting left by two also leaves the low bits free,
 * which is where NT's own tagging conventions live. */
#define NT_HANDLE_FROM_INDEX(i)  (((uint64)((i) + 1)) << 2)

/* The TEB and PEB live in teb.h, not here.
 *
 * They were briefly declared in both, with different base addresses - which
 * is the failure this codebase keeps writing comments about, arriving in the
 * one file whose entire job is the boundary between two ABIs. teb.h owns
 * them: it declares them as structures with build-time offset assertions
 * rather than as loose #defines, so a wrong offset fails the build instead of
 * producing a plausible value.
 *
 * nt.h stays about system calls. */

uint64 nt_syscall_dispatch(struct syscall_frame *frame);

/* NT has no signals. Nothing here restores a frame the way rt_sigreturn does,
 * so no call number needs excluding from signal delivery. */
int nt_is_sigreturn(uint64 nr);

/* Shared between nt.c and nt_sys.c: stack argument n (>= 5) given the user
 * RSP at the syscall, and the object behind an NT handle (NULL if none). */
struct object;
int            nt_stack_arg(uint64 rsp, int n, uint64 *out);
struct object *nt_object_of(uint64 handle);

/* nt_sys.c's calls, one per NT_SYS_ number above; each returns NTSTATUS. */
uint64 nt_sys_dispatch(struct syscall_frame *frame, int *handled);

#endif
