/* Files and handles: the four calls almost every Windows program makes, plus
 * the one that answers where its output goes. */

#include "kernel32.h"

/* GetStdHandle.
 *
 * A load out of the parameters block and nothing else - no opening, no
 * caching, no lazily-created console. That is the whole argument for the
 * kernel building RTL_USER_PROCESS_PARAMETERS at execve: the handles are the
 * ones the process was STARTED with, so a PE launched from ash with its
 * output redirected writes to the redirection, exactly like every other
 * program the shell starts.
 *
 * The three constants are -10, -11 and -12, not 0, 1 and 2. A POSIX habit
 * writes GetStdHandle(1) and gets INVALID_HANDLE_VALUE, which is the right
 * answer and an easy one to misread as "no console". */
HANDLE WINAPI GetStdHandle(DWORD which) {
    PRTL_USER_PROCESS_PARAMETERS pp = k32_params();

    if (pp == NULL_PTR) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return INVALID_HANDLE_VALUE;
    }
    switch (which) {
    case STD_INPUT_HANDLE:  return pp->StandardInput;
    case STD_OUTPUT_HANDLE: return pp->StandardOutput;
    case STD_ERROR_HANDLE:  return pp->StandardError;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
}

/* CreateFileW.
 *
 * The only place in this DLL that converts a DOS path to an NT one, which is
 * why it is also the only place that has to think about what a path means.
 *
 * Disposition: only OPEN_EXISTING is honoured. NtOpenFile opens; it does not
 * create, and the kernel has no creation path behind it yet. Refusing the
 * other four is the honest answer - CREATE_ALWAYS silently opening an
 * existing file is a data-loss bug in the caller's logic, not a convenience.
 *
 * ShareAccess: passed as read-write sharing regardless of what was asked,
 * because the object layer has no sharing model at all. That is worth being
 * explicit about rather than quietly succeeding: this DLL is not enforcing
 * FILE_SHARE_READ, and a program relying on exclusive access does not get
 * it. It is the permissive direction, so nothing fails that should work. */
HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                          LPVOID security, DWORD disposition,
                          DWORD flags, HANDLE template_file) {
    UNICODE_STRING    nt_name;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK   iosb;
    HANDLE            handle = NULL_PTR;
    NTSTATUS          status;

    (void)share;
    (void)security;
    (void)flags;
    (void)template_file;

    if (name == NULL_PTR) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (disposition != OPEN_EXISTING) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return INVALID_HANDLE_VALUE;
    }
    if (!RtlDosPathNameToNtPathName_U(name, &nt_name, (PCWSTR *)NULL_PTR,
                                      NULL_PTR)) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    attrs.Length                   = sizeof(attrs);
    attrs.Reserved                 = 0;
    attrs.RootDirectory            = NULL_PTR;
    attrs.ObjectName               = &nt_name;
    attrs.Attributes               = OBJ_CASE_INSENSITIVE;
    attrs.Reserved2                = 0;
    attrs.SecurityDescriptor       = NULL_PTR;
    attrs.SecurityQualityOfService = NULL_PTR;

    iosb.Status      = 0;
    iosb.Information = 0;

    status = NtOpenFile(&handle, access, &attrs, &iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    if (!NT_SUCCESS(status)) {
        k32_set_error_from_status(status);
        /* INVALID_HANDLE_VALUE, not NULL. A failed native call leaves NULL,
         * and a Win32 program tests against INVALID_HANDLE_VALUE - the two
         * are different failures to anything that checks, and returning the
         * native one here is a bug that only shows up in the caller. */
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share,
                          LPVOID security, DWORD disposition,
                          DWORD flags, HANDLE template_file) {
    WCHAR wide[260];

    /* A wrapper, per the W/A rule: convert and call W. No second
     * implementation, and in particular no second call to NtOpenFile. */
    if (k32_ansi_to_wide(name, wide, sizeof(wide) / sizeof(wide[0])) == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(wide, access, share, security, disposition, flags,
                       template_file);
}

/* ReadFile and WriteFile.
 *
 * The count comes out of the IO_STATUS_BLOCK's Information field and is
 * written through the out-parameter. It is NOT the return value of the native
 * call, which is a status - reading a length out of a status gives 0 on
 * success, which looks exactly like a short read at end of file. */
BOOL WINAPI ReadFile(HANDLE file, LPVOID buffer, DWORD to_read,
                     LPDWORD read, LPVOID overlapped) {
    IO_STATUS_BLOCK iosb;
    NTSTATUS        status;

    (void)overlapped;

    if (read != NULL_PTR) {
        *read = 0;
    }
    iosb.Status      = 0;
    iosb.Information = 0;

    status = NtReadFile(file, NULL_PTR, NULL_PTR, NULL_PTR, &iosb,
                        buffer, to_read, (LARGE_INTEGER *)NULL_PTR,
                        (DWORD *)NULL_PTR);
    if (!NT_SUCCESS(status)) {
        k32_set_error_from_status(status);
        return FALSE;
    }
    if (read != NULL_PTR) {
        *read = (DWORD)iosb.Information;
    }
    return TRUE;
}

BOOL WINAPI WriteFile(HANDLE file, const void *buffer, DWORD to_write,
                      LPDWORD written, LPVOID overlapped) {
    IO_STATUS_BLOCK iosb;
    NTSTATUS        status;

    (void)overlapped;

    if (written != NULL_PTR) {
        *written = 0;
    }
    iosb.Status      = 0;
    iosb.Information = 0;

    status = NtWriteFile(file, NULL_PTR, NULL_PTR, NULL_PTR, &iosb,
                         (PVOID)buffer, to_write, (LARGE_INTEGER *)NULL_PTR,
                         (DWORD *)NULL_PTR);
    if (!NT_SUCCESS(status)) {
        k32_set_error_from_status(status);
        return FALSE;
    }
    if (written != NULL_PTR) {
        *written = (DWORD)iosb.Information;
    }
    return TRUE;
}

BOOL WINAPI CloseHandle(HANDLE handle) {
    NTSTATUS status;

    /* INVALID_HANDLE_VALUE is -1, which the native layer reads as the
     * current-process pseudo-handle. Closing that would be a request to
     * close the process, so it is refused here rather than passed down. */
    if (handle == INVALID_HANDLE_VALUE || handle == NULL_PTR) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    status = NtClose(handle);
    if (!NT_SUCCESS(status)) {
        k32_set_error_from_status(status);
        return FALSE;
    }
    return TRUE;
}
