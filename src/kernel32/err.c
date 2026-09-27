/* The NTSTATUS-to-Win32 translation, and the two things every other file
 * here needs.
 *
 * One table, one caller-visible function. The alternative - each function
 * deciding what its own failures mean - is how CreateFileW comes to report
 * ERROR_ACCESS_DENIED for a missing file while ReadFile reports
 * ERROR_FILE_NOT_FOUND for a denied one, and nothing ever notices because
 * both are plausible. */

#include "kernel32.h"

DWORD WINAPI GetLastError(void) {
    return NtCurrentTeb()->LastErrorValue;
}

void WINAPI SetLastError(DWORD code) {
    NtCurrentTeb()->LastErrorValue = code;
}

/* The mapping is deliberately short. Windows has a table of hundreds; this
 * covers what this kernel's NT surface can actually return, and everything
 * else becomes ERROR_GEN_FAILURE rather than a guess that a program might act
 * on. A wrong-but-specific error is worse than a vague one: it sends the
 * reader looking in the wrong place. */
DWORD k32_set_error_from_status(NTSTATUS status) {
    DWORD code;

    switch (status) {
    case 0x00000000u:                    /* STATUS_SUCCESS               */
        code = ERROR_SUCCESS;            break;
    case 0xC0000034u:                    /* OBJECT_NAME_NOT_FOUND        */
        code = ERROR_FILE_NOT_FOUND;     break;
    case 0xC000003Au:                    /* OBJECT_PATH_NOT_FOUND        */
        code = ERROR_PATH_NOT_FOUND;     break;
    case 0xC0000022u:                    /* ACCESS_DENIED                */
        code = ERROR_ACCESS_DENIED;      break;
    case 0xC0000008u:                    /* INVALID_HANDLE               */
        code = ERROR_INVALID_HANDLE;     break;
    case 0xC0000017u:                    /* NO_MEMORY                    */
        code = ERROR_NOT_ENOUGH_MEMORY;  break;
    case 0xC000000Du:                    /* INVALID_PARAMETER            */
        code = ERROR_INVALID_PARAMETER;  break;
    case 0xC0000106u:                    /* NAME_TOO_LONG                */
        code = ERROR_INSUFFICIENT_BUFFER; break;
    case 0xC0000002u:                    /* NOT_IMPLEMENTED              */
        code = ERROR_CALL_NOT_IMPLEMENTED; break;
    case 0xC0000004u:                    /* INFO_LENGTH_MISMATCH         */
    case 0xC0000023u:                    /* BUFFER_TOO_SMALL             */
        code = ERROR_INSUFFICIENT_BUFFER; break;
    case 0xC0000003u:                    /* INVALID_INFO_CLASS           */
        code = ERROR_INVALID_PARAMETER;  break;
    case 0x00000102u:                    /* STATUS_TIMEOUT               */
        code = ERROR_TIMEOUT;            break;
    case 0xC0000046u:                    /* MUTANT_NOT_OWNED             */
        code = ERROR_NOT_OWNER;          break;
    case 0xC0000005u:                    /* ACCESS_VIOLATION             */
        /* A bad pointer handed to the kernel. ERROR_NOACCESS is what
         * Windows uses; ERROR_INVALID_PARAMETER is closer to what a caller
         * of this API can act on, and the distinction has never once helped
         * anyone. Reported as invalid parameter, which is what it is. */
        code = ERROR_INVALID_PARAMETER;  break;
    default:
        code = ERROR_GEN_FAILURE;        break;
    }

    SetLastError(code);
    return code;
}

PRTL_USER_PROCESS_PARAMETERS k32_params(void) {
    PPEB peb = NtCurrentTeb()->ProcessEnvironmentBlock;

    if (peb == NULL_PTR) {
        return (PRTL_USER_PROCESS_PARAMETERS)NULL_PTR;
    }
    return (PRTL_USER_PROCESS_PARAMETERS)peb->ProcessParameters;
}

SIZE_T k32_ansi_to_wide(LPCSTR src, LPWSTR dst, SIZE_T dst_chars) {
    SIZE_T n = 0;

    if (src == NULL_PTR || dst == NULL_PTR || dst_chars == 0) {
        return 0;
    }
    while (src[n] != '\0') {
        unsigned char c = (unsigned char)src[n];

        if (n + 1 >= dst_chars) {
            return 0;                    /* would not fit, terminator and all */
        }
        /* No code page here, so a byte above ASCII is not translated - it is
         * marked as untranslatable. See the W/A rule in kernel32.h. */
        dst[n] = (c < 0x80) ? (WCHAR)c : (WCHAR)0xFFFD;
        n++;
    }
    dst[n] = 0;
    return n;
}

/* --- the two functions the compiler may call without being asked ---------
 *
 * GCC is entitled to turn a copy loop or a struct initialisation into a call
 * to memcpy or memset even under -ffreestanding, because both are part of the
 * freestanding environment it assumes exists. With -nostdlib there is no CRT
 * to provide them, and the failure is an undefined reference at link time
 * naming a function that appears nowhere in this source.
 *
 * -fno-builtin makes it unlikely rather than impossible; the emission depends
 * on the optimiser, so it can appear when a loop is edited or the compiler is
 * upgraded. Twenty lines here turns a confusing link error into nothing at
 * all. They are not exported: kernel32.def does not name them, so they are
 * available to this DLL's own code and invisible outside it.
 */
void *memcpy(void *dst, const void *src, SIZE_T n) {
    BYTE       *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    SIZE_T      i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int c, SIZE_T n) {
    BYTE  *d = (BYTE *)dst;
    SIZE_T i;

    for (i = 0; i < n; i++) {
        d[i] = (BYTE)c;
    }
    return dst;
}

/* The DLL's entry point.
 *
 * Genesis's loader maps sections and resolves imports; it does not call
 * DllMain, so nothing runs through here today. It exists because a PE needs
 * an AddressOfEntryPoint and the linker needs the symbol named in
 * --entry - and because when the loader does start calling DLL entry points,
 * the place for kernel32's initialisation is already the right one rather
 * than wherever it got bolted on.
 *
 * Returning TRUE is "initialisation succeeded", which is true of doing
 * nothing. */
BOOL DllMainCRTStartup(PVOID instance, DWORD reason, PVOID reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
