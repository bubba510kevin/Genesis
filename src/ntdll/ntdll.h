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

/* --- the runtime library ------------------------------------------------- */

PTEB     NtCurrentTeb(void);
void     RtlInitUnicodeString(PUNICODE_STRING Destination, PCWSTR Source);
BOOL     RtlDosPathNameToNtPathName_U(PCWSTR DosName, PUNICODE_STRING NtName,
                                      PCWSTR *FilePart, PVOID Reserved);
PVOID    RtlAllocateHeap(PVOID Heap, DWORD Flags, SIZE_T Size);
BOOL     RtlFreeHeap(PVOID Heap, DWORD Flags, PVOID Address);
void     LdrInitializeThunk(void);

#define HEAP_ZERO_MEMORY 0x00000008u

#endif
