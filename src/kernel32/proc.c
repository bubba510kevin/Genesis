/* Process information and memory.
 *
 * Every function here except the two memory ones is a load out of a structure
 * the kernel filled in before the image ran. That is the point of the TEB,
 * the PEB and the parameters block: a Windows program asks these questions
 * constantly, and on a system that answered them with system calls it would
 * be asking the kernel what its own command line is. */

#include "kernel32.h"

void WINAPI ExitProcess(DWORD code) {
    /* The exiting thread's FLS destructors and every module's
     * DLL_PROCESS_DETACH TLS callback, while the process still exists to
     * run them. */
    LdrShutdownProcess();
    NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)code);

    /* Not reached. The loop is here because "not reached" is a claim about
     * the kernel, and if it is ever wrong the process should stop rather than
     * return into a caller that has already been told it exited. */
    for (;;) {
    }
}

HANDLE WINAPI GetCurrentProcess(void) {
    /* A pseudo-handle, not a real one: -1 means "me" wherever it is passed,
     * and it is not closeable. Returning a real handle here would mean
     * opening one, and a program that closed it would close its own. */
    return NtCurrentProcess();
}

DWORD WINAPI GetCurrentProcessId(void) {
    return (DWORD)NtCurrentTeb()->ClientIdProcess;
}

/* GetCommandLineW.
 *
 * Returns the block's own buffer, not a copy. That is what Windows does and
 * what callers assume: the string outlives the call, it is not freed, and
 * CommandLineToArgvW is entitled to scan it in place. Handing back a heap
 * copy would leak on every call.
 *
 * The kernel synthesised this string by joining ash's argv, and it includes
 * argv[0] - a CRT skips the program name itself, so leaving it out would eat
 * the first real argument. */
LPWSTR WINAPI GetCommandLineW(void) {
    PRTL_USER_PROCESS_PARAMETERS pp = k32_params();

    if (pp == NULL_PTR || pp->CommandLine.Buffer == NULL_PTR) {
        static WCHAR empty[1] = { 0 };
        return empty;
    }
    return pp->CommandLine.Buffer;
}

/* GetCommandLineA.
 *
 * The one A function that cannot be a pure wrapper, because it returns a
 * pointer rather than filling a caller's buffer - there is nowhere to put the
 * converted string except somewhere that outlives the call. Windows keeps a
 * per-process ANSI copy for exactly this reason; so does this, converted once
 * on first use.
 *
 * Still not a second implementation: it calls GetCommandLineW and converts
 * the result. The rule is that W has the body, and it does. */
LPSTR WINAPI GetCommandLineA(void) {
    static CHAR  ansi[1024];
    static int   built;
    LPWSTR       wide;
    SIZE_T       i;

    if (built) {
        return ansi;
    }
    wide = GetCommandLineW();
    for (i = 0; i + 1 < sizeof(ansi) && wide[i] != 0; i++) {
        /* The reverse of the ANSI-to-wide direction, with the same honesty:
         * a character outside ASCII has no byte to become, so it becomes '?'
         * rather than the low half of its code unit - which would be a
         * different, plausible-looking letter. */
        ansi[i] = (wide[i] < 0x80) ? (CHAR)wide[i] : '?';
    }
    ansi[i] = '\0';
    built = 1;
    return ansi;
}

/* GetEnvironmentStringsW.
 *
 * A pointer into the parameters block: KEY=VALUE strings, each terminated,
 * the whole run ended by a second terminator. The caller walks it until it
 * sees two zeros in a row. */
LPWSTR WINAPI GetEnvironmentStringsW(void) {
    PRTL_USER_PROCESS_PARAMETERS pp = k32_params();

    if (pp == NULL_PTR || pp->Environment == NULL_PTR) {
        static WCHAR empty[2] = { 0, 0 };
        return empty;
    }
    return (LPWSTR)pp->Environment;
}

/* Pairs with the above and frees nothing, because the above allocated
 * nothing. Windows documents the pairing, and a program that calls this is
 * correct to; it exists so that program is not wrong. */
BOOL WINAPI FreeEnvironmentStringsW(LPWSTR block) {
    (void)block;
    return TRUE;
}

/* GetCurrentDirectoryW.
 *
 * The awkward Win32 return convention, and it is worth getting exactly right
 * because callers branch on it: on success the count EXCLUDES the terminator,
 * and on "your buffer is too small" the return is the size needed INCLUDING
 * it. A caller sizes a buffer from the second and then reads the first. */
DWORD WINAPI GetCurrentDirectoryW(DWORD chars, LPWSTR buffer) {
    PRTL_USER_PROCESS_PARAMETERS pp = k32_params();
    DWORD  have;
    DWORD  i;

    if (pp == NULL_PTR || pp->CurrentDirectoryPath.Buffer == NULL_PTR) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return 0;
    }
    have = (DWORD)(pp->CurrentDirectoryPath.Length / sizeof(WCHAR));

    if (buffer == NULL_PTR || chars < have + 1) {
        return have + 1;
    }
    for (i = 0; i < have; i++) {
        buffer[i] = pp->CurrentDirectoryPath.Buffer[i];
    }
    buffer[have] = 0;
    return have;
}

/* GetModuleHandleW(NULL) is the image's own base, which the PEB has. A named
 * module needs the loader's module list, which needs LdrLoadDll - so a name
 * is refused rather than answered wrongly. */
HANDLE WINAPI GetModuleHandleW(LPCWSTR name) {
    PPEB peb = NtCurrentTeb()->ProcessEnvironmentBlock;

    if (name != NULL_PTR) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return NULL_PTR;
    }
    if (peb == NULL_PTR) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return NULL_PTR;
    }
    return (HANDLE)peb->ImageBaseAddress;
}

/* --- memory --------------------------------------------------------------
 *
 * Thin by design. HeapAlloc is RtlAllocateHeap with a Win32 name, and
 * VirtualAlloc is NtAllocateVirtualMemory with the arguments the other way
 * round. Neither adds a policy, and neither should: a second allocator
 * layered over the first is how two free lists come to disagree about one
 * pointer. */

HANDLE WINAPI GetProcessHeap(void) {
    PPEB peb = NtCurrentTeb()->ProcessEnvironmentBlock;

    if (peb == NULL_PTR) {
        return NULL_PTR;
    }
    /* Claimed by LdrInitializeThunk. Genesis jumps straight to the image
     * rather than entering ntdll first, so this is null until the image has
     * called it - which is the documented order anyway, and the reason the
     * CRT startup does it before main. */
    return (HANDLE)peb->ProcessHeap;
}

LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes) {
    LPVOID p = RtlAllocateHeap(heap, flags, bytes);

    if (p == NULL_PTR) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    }
    return p;
}

BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID address) {
    if (!RtlFreeHeap(heap, flags, address)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

LPVOID WINAPI VirtualAlloc(LPVOID address, SIZE_T size, DWORD type,
                           DWORD protect) {
    PVOID    base = address;
    SIZE_T   region = size;
    NTSTATUS status;

    status = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &region,
                                     type, protect);
    if (!NT_SUCCESS(status)) {
        k32_set_error_from_status(status);
        return NULL_PTR;
    }
    /* Note the difference in shape: the native call reports the base through
     * an in/out parameter and returns a status; Win32 returns the base and
     * reports failure as NULL. Returning `address` here instead of `base`
     * would look identical whenever the caller passed a hint and be wrong
     * whenever it passed NULL. */
    return base;
}

BOOL WINAPI VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
    (void)address;
    (void)size;
    (void)type;

    /* NtFreeVirtualMemory does not exist yet, and neither does munmap on the
     * NT side. Failing is the honest answer: a program told its memory was
     * released and then reusing the address would fault, which is far worse
     * to debug than a refused free. */
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* --- exported by ORDINAL ONLY, for the loader test ----------------------
 *
 * Reachable through no name at all: kernel32.def lists it NONAME, so it has
 * an ordinal and no entry in the export directory's name table. That is the
 * whole point - a loader that resolves ordinal imports by quietly falling
 * back to a name lookup cannot find this, and a loader that indexes
 * AddressOfFunctions without subtracting ordinal_base finds the wrong
 * function (kernel32's ordinal base is no longer 1 precisely because this
 * entry is numbered 42).
 *
 * The magic value is arbitrary but distinctive: k32.exe prints it, and a
 * wrong resolution lands on some other exported function whose return value
 * will not be this. */
DWORD WINAPI K32OrdinalProbe(void) {
    return 0xC0DE0042u;
}
