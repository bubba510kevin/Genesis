#ifndef GENESIS_KERNEL32_H
#define GENESIS_KERNEL32_H

/* kernel32.dll - the Win32 surface, over ntdll's native one.
 *
 * Clean-room, from the documented Win32 API and Windows Internals. Nothing
 * copied from ReactOS or Wine.
 *
 * --- what this layer actually is -----------------------------------------
 *
 * Almost nothing in here is a new capability. NtWriteFile already writes and
 * NtOpenFile already opens; kernel32 is the shape a Windows *program* expects
 * those to have, and the shape differs in four consistent ways:
 *
 *   1. Status vs boolean. Native calls return an NTSTATUS. Win32 calls return
 *      BOOL or a handle, and put the reason in the TEB for GetLastError. So
 *      every function here ends in the same two lines, and the mapping from
 *      NTSTATUS to a Win32 error code lives in exactly one place (err.c).
 *   2. Counts. NtReadFile reports its byte count in an IO_STATUS_BLOCK;
 *      ReadFile writes it through an out-parameter. Forgetting that the
 *      status is not the count is the mistake this shape invites.
 *   3. Paths. Win32 takes DOS paths - C:\etc\motd. The object namespace takes
 *      NT paths - \??\C:\etc\motd. RtlDosPathNameToNtPathName_U is the
 *      conversion, and CreateFileW is the only place it is called.
 *   4. Handles. Win32 has INVALID_HANDLE_VALUE for failure, which is NOT
 *      NULL, and NULL is what a failed native call leaves. The two are
 *      different failures to a program that checks.
 *
 * --- THE W/A RULE --------------------------------------------------------
 *
 * Written down here, once, before there are twenty functions and two
 * conventions to reconcile.
 *
 *   W is the implementation. A is a wrapper. Never both.
 *
 * Every function that takes text exists as a W function taking UTF-16, and
 * that is the only version with a body. The A function converts its arguments
 * to UTF-16 on the stack and calls the W function. No function is implemented
 * twice, and no A function calls the kernel.
 *
 * This is the direction Windows itself settled on, and the reason is not
 * taste. A pair of independent implementations drifts: the two get different
 * bounds checks, then different behaviour on an edge, and the bug reproduces
 * under one spelling and not the other. One implementation cannot drift from
 * itself.
 *
 * Two consequences worth stating, because they are what the rule buys:
 *
 *   - Internal code calls W. Always. A kernel32 function calling an A
 *     function converts UTF-16 to ANSI and back, which is lossy in the middle
 *     for anything this kernel will eventually have to handle.
 *   - "ANSI" here means ASCII. There is no code page in Genesis and no
 *     MultiByteToWideChar to consult one. Bytes above 0x7F are not guessed
 *     at - they become U+FFFD, the same as the kernel does when it builds the
 *     process parameters. A wrong letter is worse than an obviously wrong
 *     letter.
 *
 * --- what is deliberately absent -----------------------------------------
 *
 * No SetLastError sprinkled on success paths. Windows leaves the last error
 * alone on success for most functions, and a program that reads it after a
 * successful call is reading the previous failure - which is correct
 * behaviour and is why GetLastError is only meaningful right after a failure.
 *
 * No CreateFileW disposition other than OPEN_EXISTING. The native layer has
 * no creation path yet, so the others fail loudly rather than silently
 * opening something that was supposed to be created. */

#include "../ntdll/ntdll.h"

typedef char  CHAR;
typedef CHAR *LPSTR;
typedef const CHAR *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef void *LPVOID;
typedef DWORD *LPDWORD;

#define WINAPI
#define TRUE  1
#define FALSE 0

#define INVALID_HANDLE_VALUE  ((HANDLE)(long long)-1)

/* GetStdHandle's three arguments. They are negative and not 0/1/2, which is
 * the first place a POSIX habit shows up as a bug. */
#define STD_INPUT_HANDLE   ((DWORD)-10)
#define STD_OUTPUT_HANDLE  ((DWORD)-11)
#define STD_ERROR_HANDLE   ((DWORD)-12)

/* Win32 error codes, the handful this maps onto. Values are the documented
 * ones - a program comparing against ERROR_FILE_NOT_FOUND is comparing
 * against 2, and inventing a different number here would make every such
 * comparison quietly false. */
#define ERROR_SUCCESS               0u
#define ERROR_INVALID_FUNCTION      1u
#define ERROR_FILE_NOT_FOUND        2u
#define ERROR_PATH_NOT_FOUND        3u
#define ERROR_ACCESS_DENIED         5u
#define ERROR_INVALID_HANDLE        6u
#define ERROR_NOT_ENOUGH_MEMORY     8u
#define ERROR_INVALID_DATA         13u
#define ERROR_NOT_SUPPORTED        50u
#define ERROR_INVALID_PARAMETER    87u
#define ERROR_CALL_NOT_IMPLEMENTED 120u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_GEN_FAILURE          31u

/* CreateFile dispositions. Only OPEN_EXISTING is honoured; the rest are
 * declared so a caller's constant means what it says when it is refused. */
#define CREATE_NEW           1u
#define CREATE_ALWAYS        2u
#define OPEN_EXISTING        3u
#define OPEN_ALWAYS          4u
#define TRUNCATE_EXISTING    5u

#define FILE_SHARE_READ      0x00000001u
#define FILE_SHARE_WRITE     0x00000002u

#define MEM_RELEASE          0x00008000u

/* Threads and waiting. */
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID parameter);
#define CREATE_SUSPENDED     0x00000004u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000u
#define THREAD_ALL_ACCESS    0x001FFFFFu
#define STILL_ACTIVE         0x00000103u
#define INFINITE             0xFFFFFFFFu
#define WAIT_OBJECT_0        0x00000000u
#define WAIT_TIMEOUT         0x00000102u
#define WAIT_FAILED          0xFFFFFFFFu

/* --- error reporting (err.c) --------------------------------------------
 *
 * Every failure in this DLL goes through here. Returning the code as well as
 * storing it lets a caller write `return k32_fail(status), FALSE;` style
 * without a second statement getting separated from the first. */
DWORD k32_set_error_from_status(NTSTATUS status);

/* --- the exported surface ------------------------------------------------ */

/* Errors */
DWORD  WINAPI GetLastError(void);
void   WINAPI SetLastError(DWORD code);

/* Files and handles */
HANDLE WINAPI GetStdHandle(DWORD which);
HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                          LPVOID security, DWORD disposition,
                          DWORD flags, HANDLE template_file);
HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share,
                          LPVOID security, DWORD disposition,
                          DWORD flags, HANDLE template_file);
BOOL   WINAPI ReadFile(HANDLE file, LPVOID buffer, DWORD to_read,
                       LPDWORD read, LPVOID overlapped);
BOOL   WINAPI WriteFile(HANDLE file, const void *buffer, DWORD to_write,
                        LPDWORD written, LPVOID overlapped);
BOOL   WINAPI CloseHandle(HANDLE handle);

/* Process */
void   WINAPI ExitProcess(DWORD code);
HANDLE WINAPI GetCurrentProcess(void);
DWORD  WINAPI GetCurrentProcessId(void);
LPWSTR WINAPI GetCommandLineW(void);
LPSTR  WINAPI GetCommandLineA(void);
LPWSTR WINAPI GetEnvironmentStringsW(void);
BOOL   WINAPI FreeEnvironmentStringsW(LPWSTR block);
DWORD  WINAPI GetCurrentDirectoryW(DWORD chars, LPWSTR buffer);
HANDLE WINAPI GetModuleHandleW(LPCWSTR name);

/* Memory */
HANDLE WINAPI GetProcessHeap(void);
LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
BOOL   WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID address);
LPVOID WINAPI VirtualAlloc(LPVOID address, SIZE_T size, DWORD type,
                           DWORD protect);
BOOL   WINAPI VirtualFree(LPVOID address, SIZE_T size, DWORD type);

/* Threads (thread.c) */
HANDLE WINAPI CreateThread(LPVOID security, SIZE_T stack_size,
                           LPTHREAD_START_ROUTINE start, LPVOID parameter,
                           DWORD flags, LPDWORD thread_id);
void   WINAPI ExitThread(DWORD code);
HANDLE WINAPI GetCurrentThread(void);
DWORD  WINAPI GetCurrentThreadId(void);
DWORD  WINAPI GetThreadId(HANDLE thread);
BOOL   WINAPI GetExitCodeThread(HANDLE thread, LPDWORD code);
DWORD  WINAPI WaitForSingleObject(HANDLE handle, DWORD milliseconds);

/* --- shared internals ---------------------------------------------------- */

/* PEB->ProcessParameters, the block the kernel built at execve. Everything a
 * process knows about how it was started comes out of it, so several files
 * here need it and none of them should walk the PEB themselves. */
typedef struct _RTL_USER_PROCESS_PARAMETERS {
    DWORD           MaximumLength;
    DWORD           Length;
    DWORD           Flags;
    DWORD           DebugFlags;
    HANDLE          ConsoleHandle;
    DWORD           ConsoleFlags;
    DWORD           Reserved0;
    HANDLE          StandardInput;
    HANDLE          StandardOutput;
    HANDLE          StandardError;
    UNICODE_STRING  CurrentDirectoryPath;
    HANDLE          CurrentDirectoryHandle;
    UNICODE_STRING  DllPath;
    UNICODE_STRING  ImagePathName;
    UNICODE_STRING  CommandLine;
    PVOID           Environment;
} RTL_USER_PROCESS_PARAMETERS, *PRTL_USER_PROCESS_PARAMETERS;

PRTL_USER_PROCESS_PARAMETERS k32_params(void);

/* ANSI to UTF-16, per the rule above: used only by A wrappers, never by an
 * implementation. Returns the number of characters written excluding the
 * terminator, or 0 if it would not fit. */
SIZE_T k32_ansi_to_wide(LPCSTR src, LPWSTR dst, SIZE_T dst_chars);

/* --- the two loader-test exports (see kernel32.def) ---------------------
 *
 * Neither is a Win32 API and neither pretends to be. They exist so k32.exe
 * can exercise pe.c's ordinal-import and forwarder-export paths, which have
 * no other consumer in this tree.
 *
 * K32CurrentTeb has no implementation anywhere in kernel32 - it is a
 * forwarder to ntdll's NtCurrentTeb, resolved by the loader at link time.
 * Declaring it here is what lets a caller name it; there is deliberately no
 * definition to go with it. */
DWORD WINAPI K32OrdinalProbe(void);
PVOID WINAPI K32CurrentTeb(void);

#endif
