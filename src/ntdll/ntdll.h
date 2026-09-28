#ifndef GENESIS_NTDLL_H
#define GENESIS_NTDLL_H

/* Types, kept minimal and local.
 *
 * The DLL is built -nostdlib against MinGW-w64 as a HOST TOOL: permissive,
 * a build dependency and not a linked one, so nothing of its runtime ends up
 * in the shipped image. That also means windows.h is not available and would
 * not be wanted - every structure below is one this kernel already defines on
 * its own side, and the two have to agree exactly. */

typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef unsigned int        DWORD;
typedef unsigned long long  QWORD;
typedef unsigned long long  SIZE_T;
typedef int                 BOOL;
typedef void               *PVOID;
typedef void               *HANDLE;
typedef unsigned short      WCHAR;
typedef WCHAR              *PWSTR;
typedef const WCHAR        *PCWSTR;
typedef DWORD               NTSTATUS;
/* LONG and BOOLEAN, which the dispatcher-object calls take. Spelled out
 * because this header defines its own types rather than including Windows',
 * and a BOOLEAN that is accidentally four bytes wide is an argument the
 * kernel reads the wrong half of. */
typedef int                 LONG;
typedef unsigned char       BOOLEAN;

#define NULL_PTR ((void *)0)

#define STATUS_SUCCESS              0x00000000u
#define STATUS_NO_MEMORY            0xC0000017u
#define STATUS_INVALID_PARAMETER    0xC000000Du
#define STATUS_NAME_TOO_LONG        0xC0000106u
#define STATUS_TIMEOUT              0x00000102u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_OBJECT_TYPE_MISMATCH  0xC0000024u
#define STATUS_MUTANT_NOT_OWNED     0xC0000046u
#define STATUS_INVALID_HANDLE       0xC0000008u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_NOT_IMPLEMENTED      0xC0000002u
#define STATUS_NOT_IMPLEMENTED      0xC0000002u

#define NT_SUCCESS(s)  ((NTSTATUS)(s) < 0x80000000u)

#define NtCurrentProcess()  ((HANDLE)(long long)-1)
#define NtCurrentThread()   ((HANDLE)(long long)-2)

/* ACCESS_MASK. The generic bits are what a caller passes; the kernel maps
 * them onto its own read/write access. */
#define GENERIC_READ    0x80000000u
#define GENERIC_WRITE   0x40000000u
#define FILE_READ_DATA  0x00000001u
#define FILE_WRITE_DATA 0x00000002u

#define MEM_COMMIT      0x00001000u
#define MEM_RESERVE     0x00002000u
#define PAGE_READWRITE  0x00000004u

/* Counted, not terminated - Length is in BYTES. The four bytes of padding
 * before Buffer are the ABI's, and this must NOT be packed: the kernel side
 * of this exact structure was packed once, which moved Buffer from offset 8
 * to offset 4 and made every call taking a name fail silently. */
typedef struct _UNICODE_STRING {
    WORD   Length;
    WORD   MaximumLength;
    DWORD  Reserved;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    DWORD            Length;
    DWORD            Reserved;
    HANDLE           RootDirectory;
    PUNICODE_STRING  ObjectName;
    DWORD            Attributes;
    DWORD            Reserved2;
    PVOID            SecurityDescriptor;
    PVOID            SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

#define OBJ_CASE_INSENSITIVE 0x00000040u

typedef struct _IO_STATUS_BLOCK {
    QWORD  Status;
    QWORD  Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _LARGE_INTEGER { long long QuadPart; } LARGE_INTEGER;

/* The parts of the TEB and PEB this DLL reads. Offsets are the published
 * x86-64 layout and match kernel/include/teb.h, which is where the kernel
 * writes them. */
typedef struct _PEB {
    BYTE   InheritedAddressSpace;
    BYTE   ReadImageFileExecOptions;
    BYTE   BeingDebugged;
    BYTE   BitField;
    DWORD  Reserved0;
    PVOID  Mutant;
    PVOID  ImageBaseAddress;
    PVOID  Ldr;
    PVOID  ProcessParameters;
    PVOID  SubSystemData;
    PVOID  ProcessHeap;
    PVOID  FastPebLock;
} PEB, *PPEB;

typedef struct _TEB {
    PVOID  ExceptionList;
    PVOID  StackBase;
    PVOID  StackLimit;
    PVOID  SubSystemTib;
    PVOID  FiberData;
    PVOID  ArbitraryUserPointer;
    struct _TEB *Self;
    PVOID  EnvironmentPointer;
    QWORD  ClientIdProcess;
    QWORD  ClientIdThread;
    PVOID  ActiveRpcHandle;
    PVOID  ThreadLocalStoragePointer;
    PPEB   ProcessEnvironmentBlock;
    DWORD  LastErrorValue;
    DWORD  CountOfOwnedCriticalSections;
    PVOID  CsrClientThread;
    PVOID  Win32ThreadInfo;
} TEB, *PTEB;

/* --- the native interface ------------------------------------------------ */

NTSTATUS NtDisplayString(PUNICODE_STRING String);
NTSTATUS NtTerminateProcess(HANDLE Process, NTSTATUS ExitStatus);
NTSTATUS NtClose(HANDLE Handle);
NTSTATUS NtOpenFile(HANDLE *FileHandle, DWORD DesiredAccess,
                    POBJECT_ATTRIBUTES ObjectAttributes,
                    PIO_STATUS_BLOCK IoStatusBlock,
                    DWORD ShareAccess, DWORD OpenOptions);
NTSTATUS NtReadFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine,
                    PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                    PVOID Buffer, DWORD Length, LARGE_INTEGER *ByteOffset,
                    DWORD *Key);
NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine,
                     PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                     PVOID Buffer, DWORD Length, LARGE_INTEGER *ByteOffset,
                     DWORD *Key);
NTSTATUS NtAllocateVirtualMemory(HANDLE Process, PVOID *BaseAddress,
                                 QWORD ZeroBits, SIZE_T *RegionSize,
                                 DWORD AllocationType, DWORD Protect);

/* --- the dispatcher objects ----------------------------------------------
 *
 * Events, semaphores and mutants. A Win32 program's CreateEventW,
 * CreateSemaphoreW, CreateMutexW and WaitForSingleObject are all thin wrappers
 * over these, which is why they are the first things a great many programs
 * call - often before they open a file.
 *
 * OBJECT_ATTRIBUTES may carry a name or be NULL. A named object goes into the
 * kernel namespace, which is what lets two unrelated processes agree on one -
 * the whole reason \BaseNamedObjects exists.
 */
typedef LONG EVENT_TYPE;
#define NotificationEvent     0     /* manual reset: stays set, wakes all   */
#define SynchronizationEvent  1     /* auto reset: wakes one, then clears   */

NTSTATUS NtCreateEvent(HANDLE *EventHandle, DWORD DesiredAccess,
                       POBJECT_ATTRIBUTES ObjectAttributes,
                       EVENT_TYPE EventType, BOOLEAN InitialState);
NTSTATUS NtOpenEvent(HANDLE *EventHandle, DWORD DesiredAccess,
                     POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS NtSetEvent(HANDLE EventHandle, LONG *PreviousState);
NTSTATUS NtResetEvent(HANDLE EventHandle, LONG *PreviousState);

/* Timeout: NULL waits forever; a NEGATIVE value is a relative interval in
 * 100-nanosecond units; zero does not wait at all. A POSITIVE value is an
 * absolute time and is not implemented - it returns STATUS_NOT_IMPLEMENTED
 * rather than being read as a relative interval, which is what dropping the
 * sign would silently do.
 *
 * STATUS_TIMEOUT is a SUCCESS code. NT_SUCCESS() is true for it, so a caller
 * that only tests NT_SUCCESS and then uses the object has a bug. */
NTSTATUS NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable,
                               LARGE_INTEGER *Timeout);

/* Up to 64 objects. WaitAll takes every one in a single step once all are
 * signalled; WaitAny takes the lowest-indexed signalled one and returns
 * STATUS_WAIT_0 + its index. */
typedef LONG WAIT_TYPE;
#define WaitAll                      0
#define WaitAny                      1
#define MAXIMUM_WAIT_OBJECTS         64
NTSTATUS NtWaitForMultipleObjects(DWORD Count, HANDLE *Handles,
                                  WAIT_TYPE WaitType, BOOLEAN Alertable,
                                  LARGE_INTEGER *Timeout);

NTSTATUS NtCreateSemaphore(HANDLE *SemaphoreHandle, DWORD DesiredAccess,
                           POBJECT_ATTRIBUTES ObjectAttributes,
                           LONG InitialCount, LONG MaximumCount);
/* Releasing past MaximumCount is an ERROR and the count does not move. A
 * caller that over-releases has a counting bug, and clamping would hide it. */
NTSTATUS NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount,
                            LONG *PreviousCount);

NTSTATUS NtCreateMutant(HANDLE *MutantHandle, DWORD DesiredAccess,
                        POBJECT_ATTRIBUTES ObjectAttributes,
                        BOOLEAN InitialOwner);
/* A mutant is RECURSIVE: taken twice by its owner, released twice. Releasing
 * one you do not hold is STATUS_MUTANT_NOT_OWNED, not a no-op. */
NTSTATUS NtReleaseMutant(HANDLE MutantHandle, LONG *PreviousCount);

#define MUTANT_ALL_ACCESS            0x001F0001u
#define EVENT_ALL_ACCESS             0x001F0003u
#define SEMAPHORE_ALL_ACCESS         0x001F0003u
/* A wait that acquired a mutant whose owner died holding it. A SUCCESS code:
 * the caller owns the mutant now, but what it guards may be half-updated. */
#define STATUS_ABANDONED_WAIT_0      0x00000080u

/* --- threads ------------------------------------------------------------- */

#define STATUS_PENDING               0x00000103u   /* STILL_ACTIVE */
#define THREAD_CREATE_FLAGS_CREATE_SUSPENDED 0x00000001u
#define ThreadBasicInformation       0

typedef DWORD (*PUSER_THREAD_START_ROUTINE)(PVOID Parameter);

typedef struct _CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID;

typedef struct _THREAD_BASIC_INFORMATION {
    NTSTATUS  ExitStatus;
    PVOID     TebBaseAddress;
    CLIENT_ID ClientId;
    SIZE_T    AffinityMask;
    LONG      Priority;
    LONG      BasePriority;
} THREAD_BASIC_INFORMATION;

/* The new thread begins in RtlUserThreadStart, not at StartRoutine; see
 * kernel/include/nt.h for what the kernel supports and refuses. */
NTSTATUS NtCreateThreadEx(HANDLE *ThreadHandle, DWORD DesiredAccess,
                          POBJECT_ATTRIBUTES ObjectAttributes,
                          HANDLE ProcessHandle, PVOID StartRoutine,
                          PVOID Argument, DWORD CreateFlags, SIZE_T ZeroBits,
                          SIZE_T StackSize, SIZE_T MaximumStackSize,
                          PVOID AttributeList);
NTSTATUS NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus);
NTSTATUS NtQueryInformationThread(HANDLE ThreadHandle, DWORD InfoClass,
                                  PVOID Info, DWORD InfoLength,
                                  DWORD *ReturnLength);
/* Counted: each reports the count as it was BEFORE the call. */
NTSTATUS NtSuspendThread(HANDLE ThreadHandle, DWORD *PreviousSuspendCount);
NTSTATUS NtResumeThread(HANDLE ThreadHandle, DWORD *PreviousSuspendCount);

/* --- CONTEXT, APCs -----------------------------------------------------------
 *
 * CONTEXT in Microsoft's exact x64 layout (0x4D0 bytes, 16-aligned): every
 * exception handler is handed one and compiled code reads it by windows.h's
 * offsets. See kernel/include/nt_context.h for the kernel's side. */
typedef struct __attribute__((aligned(16))) _M128A {
    QWORD Low;
    long long High;
} M128A;

typedef struct __attribute__((aligned(16))) _CONTEXT {
    QWORD P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    DWORD ContextFlags;
    DWORD MxCsr;
    WORD  SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    DWORD EFlags;
    QWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    QWORD Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    QWORD R8, R9, R10, R11, R12, R13, R14, R15;
    QWORD Rip;
    BYTE  FltSave[512];             /* XMM_SAVE_AREA32: FXSAVE's image */
    M128A VectorRegister[26];
    QWORD VectorControl;
    QWORD DebugControl;
    QWORD LastBranchToRip, LastBranchFromRip;
    QWORD LastExceptionToRip, LastExceptionFromRip;
} CONTEXT, *PCONTEXT;

#define CONTEXT_AMD64           0x00100000u
#define CONTEXT_CONTROL         (CONTEXT_AMD64 | 0x1u)
#define CONTEXT_INTEGER         (CONTEXT_AMD64 | 0x2u)
#define CONTEXT_SEGMENTS        (CONTEXT_AMD64 | 0x4u)
#define CONTEXT_FLOATING_POINT  (CONTEXT_AMD64 | 0x8u)
#define CONTEXT_FULL            (CONTEXT_CONTROL | CONTEXT_INTEGER | \
                                 CONTEXT_FLOATING_POINT)

#define STATUS_USER_APC         0x000000C0u

typedef void (*PPS_APC_ROUTINE)(PVOID Arg1, PVOID Arg2, PVOID Arg3);

/* Resume in exactly `Context` - every register. Does not return on success.
 * TestAlert TRUE runs any queued APC first (see NtQueueApcThread). */
NTSTATUS NtContinue(PCONTEXT Context, BOOLEAN TestAlert);
/* Queue Routine(Arg1, Arg2, Arg3) on a thread of this process; it runs the
 * next time that thread waits alertably or calls NtTestAlert. */
NTSTATUS NtQueueApcThread(HANDLE Thread, PPS_APC_ROUTINE Routine,
                          PVOID Arg1, PVOID Arg2, PVOID Arg3);
NTSTATUS NtTestAlert(void);
/* The caller's registers as they will be when this returns. */
void     RtlCaptureContext(PCONTEXT Context);
/* Where the kernel sends a thread to run an APC. Not called by anybody. */
void     KiUserApcDispatcher(void);

/* --- structured exception handling (ROADMAP 14(c)) ----------------------
 *
 * x64 SEH is TABLE-BASED: nothing is registered at run time. Every function
 * that needs unwinding has a RUNTIME_FUNCTION in its image's .pdata naming
 * an UNWIND_INFO in .xdata that describes its prologue - and optionally a
 * language handler (for C, __C_specific_handler, whose scope table lists
 * the __try ranges). Dispatching an exception is walking those tables up
 * the stack. */
typedef struct _EXCEPTION_RECORD {
    DWORD  ExceptionCode;
    DWORD  ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID  ExceptionAddress;
    DWORD  NumberParameters;
    QWORD  ExceptionInformation[15];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT          ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

typedef struct _RUNTIME_FUNCTION {
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD UnwindData;
} RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;

typedef enum _EXCEPTION_DISPOSITION {
    ExceptionContinueExecution = 0,
    ExceptionContinueSearch    = 1,
    ExceptionNestedException   = 2,
    ExceptionCollidedUnwind    = 3
} EXCEPTION_DISPOSITION;

struct _DISPATCHER_CONTEXT;
typedef EXCEPTION_DISPOSITION (*PEXCEPTION_ROUTINE)(
    PEXCEPTION_RECORD rec, PVOID EstablisherFrame, PCONTEXT ctx,
    struct _DISPATCHER_CONTEXT *dc);

typedef struct _DISPATCHER_CONTEXT {
    QWORD              ControlPc;
    QWORD              ImageBase;
    PRUNTIME_FUNCTION  FunctionEntry;
    QWORD              EstablisherFrame;
    QWORD              TargetIp;
    PCONTEXT           ContextRecord;
    PEXCEPTION_ROUTINE LanguageHandler;
    PVOID              HandlerData;
    PVOID              HistoryTable;
    DWORD              ScopeIndex;
    DWORD              Fill0;
} DISPATCHER_CONTEXT, *PDISPATCHER_CONTEXT;

typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(PEXCEPTION_POINTERS info);
typedef LONG (*PTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS info);

#define EXCEPTION_NONCONTINUABLE      0x01u
#define EXCEPTION_UNWINDING           0x02u
#define EXCEPTION_EXIT_UNWIND         0x04u
#define EXCEPTION_TARGET_UNWIND       0x20u
#define EXCEPTION_MAXIMUM_PARAMETERS  15

#define EXCEPTION_EXECUTE_HANDLER      1
#define EXCEPTION_CONTINUE_SEARCH      0
#define EXCEPTION_CONTINUE_EXECUTION  (-1)

#define UNW_FLAG_NHANDLER  0x0u
#define UNW_FLAG_EHANDLER  0x1u
#define UNW_FLAG_UHANDLER  0x2u
#define UNW_FLAG_CHAININFO 0x4u

#define STATUS_ACCESS_VIOLATION        0xC0000005u
#define STATUS_NONCONTINUABLE_EXCEPTION 0xC0000025u
#define STATUS_INVALID_DISPOSITION     0xC0000026u
#define STATUS_UNWIND                  0xC0000027u

/* NtGenesisLoadImage - Genesis's own call (ROADMAP items 14(e) and 19).
 * Map the image at the POSIX path `path` - a DLL, or a Linux ELF shared
 * object - and whatever it needs that is not loaded yet. `out` lists the
 * new modules dependencies first, for kernel32 to run their initialisers:
 * a DLL's DllMain in `entry`; a .so's DT_INIT in `entry` and its
 * .init_array at `init_array` (init_count pointers). The layout matches
 * kernel/include/ntmix.h. */
#define GNT_LOAD_MAX 16

typedef struct {
    unsigned long long base;
    unsigned long long size;
    unsigned long long entry;
    unsigned long long init_array;
    DWORD              init_count;
    DWORD              kind;             /* 1 PE, 2 ELF */
} GNT_LOAD_MODULE;

typedef struct {
    unsigned long long base;             /* the image asked for */
    DWORD              count;
    DWORD              reserved;
    GNT_LOAD_MODULE    mods[GNT_LOAD_MAX];
} GNT_LOAD_OUT;

NTSTATUS NtGenesisLoadImage(const char *path, GNT_LOAD_OUT *out);

NTSTATUS NtRaiseException(PEXCEPTION_RECORD rec, PCONTEXT ctx,
                          BOOLEAN FirstChance);

PRUNTIME_FUNCTION RtlLookupFunctionEntry(QWORD ControlPc, QWORD *ImageBase,
                                         PVOID HistoryTable);
PEXCEPTION_ROUTINE RtlVirtualUnwind(DWORD HandlerType, QWORD ImageBase,
                                    QWORD ControlPc,
                                    PRUNTIME_FUNCTION FunctionEntry,
                                    PCONTEXT Context, PVOID *HandlerData,
                                    QWORD *EstablisherFrame,
                                    PVOID ContextPointers);
BOOLEAN  RtlDispatchException(PEXCEPTION_RECORD rec, PCONTEXT ctx);
void     RtlUnwindEx(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD rec,
                     PVOID ReturnValue, PCONTEXT OriginalContext,
                     PVOID HistoryTable);
void     RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD rec,
                   PVOID ReturnValue);
void     RtlRaiseException(PEXCEPTION_RECORD rec);
void     RtlRaiseStatus(NTSTATUS status);
PVOID    RtlAddVectoredExceptionHandler(DWORD First,
                                        PVECTORED_EXCEPTION_HANDLER handler);
DWORD    RtlRemoveVectoredExceptionHandler(PVOID handle);
/* What runs when nothing on the stack handled an exception (kernel32's
 * SetUnhandledExceptionFilter installs its UnhandledExceptionFilter here).
 * Returns the previous one. */
PTOP_LEVEL_EXCEPTION_FILTER RtlSetUnhandledExceptionFilter(
    PTOP_LEVEL_EXCEPTION_FILTER filter);
EXCEPTION_DISPOSITION __C_specific_handler(PEXCEPTION_RECORD rec,
                                           PVOID EstablisherFrame,
                                           PCONTEXT ctx,
                                           PDISPATCHER_CONTEXT dc);
void     KiUserExceptionDispatcher(void);

/* --- the runtime library ------------------------------------------------- */

PTEB     NtCurrentTeb(void);
void     RtlInitUnicodeString(PUNICODE_STRING Destination, PCWSTR Source);
BOOL     RtlDosPathNameToNtPathName_U(PCWSTR DosName, PUNICODE_STRING NtName,
                                      PCWSTR *FilePart, PVOID Reserved);
PVOID    RtlAllocateHeap(PVOID Heap, DWORD Flags, SIZE_T Size);
BOOL     RtlFreeHeap(PVOID Heap, DWORD Flags, PVOID Address);
void     LdrInitializeThunk(void);
void     RtlExitUserThread(NTSTATUS ExitStatus);

#define HEAP_ZERO_MEMORY 0x00000008u
#define HEAP_NO_SERIALIZE 0x00000001u

/* --- the machine, processes and scheduling ------------------------------
 *
 * See kernel/include/nt.h (NT_SYS_QUERY_SYSTEM_INFO and after) for the
 * contract of each call and kernel/exec/nt_sys.c for the structures. */
typedef unsigned long long KAFFINITY;
typedef long long          LONGLONG;
typedef unsigned short     USHORT;
typedef unsigned char      UCHAR;

#define STATUS_NO_YIELD_PERFORMED   0x40000024u
#define STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define STATUS_INVALID_INFO_CLASS   0xC0000003u
#define STATUS_ACCESS_VIOLATION     0xC0000005u

#define SystemBasicInformation                    0
#define SystemProcessorInformation                1
#define SystemProcessorPerformanceInformation     8
#define SystemLogicalProcessorInformation         73
#define SystemLogicalProcessorAndGroupInformation 107

#define ProcessBasicInformation   0
#define ProcessTimes              4
#define ProcessPriorityClass      18
#define ProcessAffinityMask       21

#define ThreadTimes               1
#define ThreadPriority            2
#define ThreadBasePriority        3
#define ThreadAffinityMask        4
#define ThreadIdealProcessor      13
#define ThreadHideFromDebugger    17
#define ThreadGroupInformation    30
#define ThreadIdealProcessorEx    33

typedef struct _SYSTEM_BASIC_INFORMATION {
    DWORD     Reserved;
    DWORD     TimerResolution;
    DWORD     PageSize;
    DWORD     NumberOfPhysicalPages;
    DWORD     LowestPhysicalPageNumber;
    DWORD     HighestPhysicalPageNumber;
    DWORD     AllocationGranularity;
    SIZE_T    MinimumUserModeAddress;
    SIZE_T    MaximumUserModeAddress;
    KAFFINITY ActiveProcessorsAffinityMask;
    char      NumberOfProcessors;
} SYSTEM_BASIC_INFORMATION;

typedef struct _SYSTEM_PROCESSOR_INFORMATION {
    USHORT ProcessorArchitecture;
    USHORT ProcessorLevel;
    USHORT ProcessorRevision;
    USHORT MaximumProcessors;
    DWORD  ProcessorFeatureBits;
} SYSTEM_PROCESSOR_INFORMATION;

typedef struct _SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    DWORD         InterruptCount;
} SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION;

typedef struct _PROCESS_BASIC_INFORMATION {
    NTSTATUS  ExitStatus;
    PVOID     PebBaseAddress;
    KAFFINITY AffinityMask;
    LONG      BasePriority;
    SIZE_T    UniqueProcessId;
    SIZE_T    InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION;

typedef struct _KERNEL_USER_TIMES {
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER ExitTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
} KERNEL_USER_TIMES;

typedef struct _PROCESSOR_NUMBER {
    WORD Group;
    BYTE Number;
    BYTE Reserved;
} PROCESSOR_NUMBER, *PPROCESSOR_NUMBER;

typedef struct _GROUP_AFFINITY {
    KAFFINITY Mask;
    WORD      Group;
    WORD      Reserved[3];
} GROUP_AFFINITY, *PGROUP_AFFINITY;

NTSTATUS NtQuerySystemInformation(DWORD Class, PVOID Buffer, DWORD Length,
                                  DWORD *ReturnLength);
NTSTATUS NtQuerySystemInformationEx(DWORD Class, PVOID InputBuffer,
                                    DWORD InputLength, PVOID Buffer,
                                    DWORD Length, DWORD *ReturnLength);
NTSTATUS NtQueryInformationProcess(HANDLE Process, DWORD Class, PVOID Buffer,
                                   DWORD Length, DWORD *ReturnLength);
NTSTATUS NtSetInformationProcess(HANDLE Process, DWORD Class, PVOID Buffer,
                                 DWORD Length);
NTSTATUS NtSetInformationThread(HANDLE Thread, DWORD Class, PVOID Buffer,
                                DWORD Length);
NTSTATUS NtYieldExecution(void);
NTSTATUS NtDelayExecution(BOOLEAN Alertable, LARGE_INTEGER *Interval);
DWORD    NtGetCurrentProcessorNumber(void);
DWORD    NtGetCurrentProcessorNumberEx(PPROCESSOR_NUMBER ProcNumber);
NTSTATUS NtQueryPerformanceCounter(LARGE_INTEGER *Counter,
                                   LARGE_INTEGER *Frequency);
NTSTATUS NtQuerySystemTime(LARGE_INTEGER *SystemTime);
#define STATUS_ALERTED 0x00000101u
NTSTATUS NtWaitForAlertByThreadId(PVOID Address, LARGE_INTEGER *Timeout);
NTSTATUS NtAlertThreadByThreadId(HANDLE ThreadId);

/* Wait until *Address differs from *Compare (Size 1, 2, 4 or 8 bytes),
 * sleeping in the kernel; woken by RtlWakeAddressSingle/All on the same
 * address. Spurious returns are possible - re-test. waitaddr.c. */
NTSTATUS RtlWaitOnAddress(const volatile void *Address, PVOID Compare,
                          SIZE_T Size, LARGE_INTEGER *Timeout);
void     RtlWakeAddressSingle(PVOID Address);
void     RtlWakeAddressAll(PVOID Address);

/* --- synchronisation, in user mode ----------------------------------------
 *
 * The primitives a multithreaded Win32 program is built on, which on SMP
 * are the difference between a program that works and one that corrupts
 * itself: critical sections, slim reader/writer locks, condition variables,
 * and the lock-free singly linked list. Layouts are Windows' (a CRITICAL_
 * SECTION is 40 bytes, an SRWLOCK and a CONDITION_VARIABLE one pointer, an
 * SLIST_HEADER 16 bytes aligned to 16). See sync.c. */
typedef struct _RTL_CRITICAL_SECTION {
    PVOID           DebugInfo;
    volatile LONG   LockCount;          /* -1 free; else owner + waiters - 1 */
    volatile LONG   RecursionCount;
    volatile HANDLE OwningThread;       /* thread id of the owner           */
    volatile HANDLE LockSemaphore;      /* auto-reset event, made on demand */
    SIZE_T          SpinCount;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;

typedef struct _RTL_SRWLOCK {
    volatile SIZE_T Value;              /* bit 0: exclusive; count << 1     */
} RTL_SRWLOCK, *PRTL_SRWLOCK;

typedef struct _RTL_CONDITION_VARIABLE {
    volatile SIZE_T Value;              /* a generation, bumped per wake    */
} RTL_CONDITION_VARIABLE, *PRTL_CONDITION_VARIABLE;

typedef struct _SLIST_ENTRY {
    struct _SLIST_ENTRY *Next;
} SLIST_ENTRY, *PSLIST_ENTRY;

typedef struct __attribute__((aligned(16))) _SLIST_HEADER {
    volatile PSLIST_ENTRY Next;
    volatile QWORD        DepthAndLock; /* bits 0-15 depth; bit 63 lock     */
} SLIST_HEADER, *PSLIST_HEADER;

#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1u

NTSTATUS RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSTATUS RtlInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION cs,
                                                  DWORD spin);
NTSTATUS RtlInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION cs, DWORD spin,
                                        DWORD flags);
NTSTATUS RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs);
BOOLEAN  RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSTATUS RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSTATUS RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION cs);
DWORD    RtlSetCriticalSectionSpinCount(PRTL_CRITICAL_SECTION cs, DWORD spin);
BOOLEAN  RtlIsCriticalSectionLockedByThread(PRTL_CRITICAL_SECTION cs);

void     RtlInitializeSRWLock(PRTL_SRWLOCK l);
void     RtlAcquireSRWLockExclusive(PRTL_SRWLOCK l);
void     RtlAcquireSRWLockShared(PRTL_SRWLOCK l);
void     RtlReleaseSRWLockExclusive(PRTL_SRWLOCK l);
void     RtlReleaseSRWLockShared(PRTL_SRWLOCK l);
BOOLEAN  RtlTryAcquireSRWLockExclusive(PRTL_SRWLOCK l);
BOOLEAN  RtlTryAcquireSRWLockShared(PRTL_SRWLOCK l);

void     RtlInitializeConditionVariable(PRTL_CONDITION_VARIABLE cv);
void     RtlWakeConditionVariable(PRTL_CONDITION_VARIABLE cv);
void     RtlWakeAllConditionVariable(PRTL_CONDITION_VARIABLE cv);
NTSTATUS RtlSleepConditionVariableCS(PRTL_CONDITION_VARIABLE cv,
                                     PRTL_CRITICAL_SECTION cs,
                                     LARGE_INTEGER *timeout);
NTSTATUS RtlSleepConditionVariableSRW(PRTL_CONDITION_VARIABLE cv,
                                      PRTL_SRWLOCK l, LARGE_INTEGER *timeout,
                                      DWORD flags);

void         RtlInitializeSListHead(PSLIST_HEADER h);
PSLIST_ENTRY RtlInterlockedPushEntrySList(PSLIST_HEADER h, PSLIST_ENTRY e);
PSLIST_ENTRY RtlInterlockedPopEntrySList(PSLIST_HEADER h);
PSLIST_ENTRY RtlInterlockedFlushSList(PSLIST_HEADER h);
PSLIST_ENTRY RtlFirstEntrySList(const SLIST_HEADER *h);
WORD         RtlQueryDepthSList(PSLIST_HEADER h);

/* --- thread-local storage (tls.c) --------------------------------------- */
typedef void (*PFLS_CALLBACK_FUNCTION)(PVOID Data);
NTSTATUS RtlFlsAlloc(PFLS_CALLBACK_FUNCTION Callback, DWORD *Index);
NTSTATUS RtlFlsFree(DWORD Index);
NTSTATUS RtlFlsGetValue(DWORD Index, PVOID *Value);
NTSTATUS RtlFlsSetValue(DWORD Index, PVOID Value);
void     LdrShutdownProcess(void);

#define TEB_TLS_SLOTS_OFFSET     0x1480
#define TEB_TLS_EXPANSION_OFFSET 0x1780
#define TLS_MINIMUM_AVAILABLE    64
#define TLS_EXPANSION_SLOTS      1024
#define ThreadZeroTlsCell        10

DWORD    RtlGetCurrentProcessorNumber(void);
void     RtlGetCurrentProcessorNumberEx(PPROCESSOR_NUMBER ProcNumber);
BOOLEAN  RtlQueryPerformanceCounter(LARGE_INTEGER *Counter);
BOOLEAN  RtlQueryPerformanceFrequency(LARGE_INTEGER *Frequency);

#endif
