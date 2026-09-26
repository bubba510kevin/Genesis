/* k32.exe - a Windows program that knows nothing about Genesis.
 *
 * The next rung of the milestone ladder. hello.exe imports ntdll and calls
 * the native interface; this imports kernel32 and calls Win32, which is what
 * an actual Windows program does. Between them they cover both halves of the
 * surface.
 *
 * Two things it proves that hello.exe cannot:
 *
 *   - The loader resolves imports at DEPTH TWO. This image imports kernel32;
 *     kernel32 imports ntdll. Nothing here mentions ntdll at all, so if
 *     RtlDosPathNameToNtPathName_U ends up being called - and it does, inside
 *     CreateFileW - the loader found and linked a dependency of a dependency.
 *
 *   - RTL_USER_PROCESS_PARAMETERS is real. GetStdHandle, GetCommandLineW and
 *     GetCurrentDirectoryW are loads out of the block the kernel built at
 *     execve, and there is no other way for this program to learn any of it.
 *     If the command line comes back right, the join, the UTF-16 conversion
 *     and the user-address arithmetic in ntproc.c were all correct.
 *
 * Deliberately no heap: HeapAlloc needs PEB->ProcessHeap, which
 * LdrInitializeThunk claims, which would mean importing ntdll and spoiling
 * the "only kernel32" property. */

#include "../kernel32/kernel32.h"

static void say(HANDLE out, const char *s) {
    DWORD n = 0, written = 0;

    while (s[n] != '\0') {
        n++;
    }
    WriteFile(out, s, n, &written, NULL_PTR);
}

/* UTF-16 to bytes, for printing. Not a kernel32 function and not pretending
 * to be one - WriteFile takes bytes and the strings in the parameters block
 * are wide, so something has to bridge them. A real program would use
 * WriteConsoleW. */
static void say_wide(HANDLE out, const WCHAR *w) {
    char   buf[512];
    DWORD  n = 0, written = 0;

    while (w[n] != 0 && n + 1 < sizeof(buf)) {
        buf[n] = (w[n] < 0x80) ? (char)w[n] : '?';
        n++;
    }
    buf[n] = '\0';
    WriteFile(out, buf, n, &written, NULL_PTR);
}

void start(void) {
    HANDLE out, file;
    WCHAR  cwd[260];
    char   buf[512];
    DWORD  got = 0, written = 0;

    /* Note the constant. STD_OUTPUT_HANDLE is -11, not 1 - and this is the
     * one line that would silently work on a system where GetStdHandle
     * ignored its argument. */
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE) {
        /* No handle means no way to report that there is no handle. Exit
         * with a distinguishable code so the shell can say what happened. */
        ExitProcess(2);
    }

    say(out, "hello from a PE that imports only kernel32.dll\n");

    say(out, "GetCommandLineW: ");
    say_wide(out, GetCommandLineW());
    say(out, "\n");

    if (GetCurrentDirectoryW(sizeof(cwd) / sizeof(cwd[0]), cwd) != 0) {
        say(out, "GetCurrentDirectoryW: ");
        say_wide(out, cwd);
        say(out, "\n");
    } else {
        say(out, "GetCurrentDirectoryW failed\n");
    }

    /* A DOS path, converted by CreateFileW through ntdll and resolved by the
     * object namespace - the whole stack in one call, from a program that
     * names none of it. */
    file = CreateFileW(L"C:\\etc\\motd", GENERIC_READ, FILE_SHARE_READ,
                       NULL_PTR, OPEN_EXISTING, 0, NULL_PTR);
    if (file == INVALID_HANDLE_VALUE) {
        say(out, "CreateFileW(C:\\etc\\motd) failed, GetLastError = ");
        {
            DWORD e = GetLastError();
            char  d[12];
            int   i = 0, j;

            if (e == 0) {
                d[i++] = '0';
            }
            while (e > 0 && i < 11) {
                d[i++] = (char)('0' + (e % 10));
                e /= 10;
            }
            for (j = 0; j < i; j++) {
                buf[j] = d[i - 1 - j];
            }
            buf[i] = '\0';
            say(out, buf);
        }
        say(out, "\n");
        ExitProcess(1);
    }

    if (ReadFile(file, buf, sizeof(buf) - 1, &got, NULL_PTR)) {
        say(out, "ReadFile through CreateFileW: ");
        WriteFile(out, buf, got, &written, NULL_PTR);
    } else {
        say(out, "ReadFile failed\n");
    }

    CloseHandle(file);

    /* --- the two loader paths this program exists to exercise -----------
     *
     * Both calls look ordinary here, which is the point: what is unusual is
     * how the LOADER had to resolve them, and neither is visible in the C.
     *
     * K32OrdinalProbe is exported NONAME, so this call site imports it as
     * kernel32.dll#42 with no name anywhere in the import table. kernel32's
     * ordinal base is 40-something rather than 1 because of it, so the right
     * answer needs AddressOfFunctions[42 - ordinal_base] - a loader skipping
     * that subtraction lands on a different exported function entirely and
     * prints whatever that one returns.
     *
     * K32CurrentTeb has no code in kernel32 at all. Its export entry is the
     * string "ntdll.NtCurrentTeb", so the loader has to notice the RVA lands
     * inside the export directory, parse the string, and resolve the name in
     * a second DLL. A non-NULL TEB back means it followed it correctly - a
     * forwarder returned as-is would be a pointer into kernel32's .edata,
     * and calling it would not return at all. */
    {
        DWORD probe = K32OrdinalProbe();

        say(out, "K32OrdinalProbe (imported by ordinal 42) = ");
        if (probe == 0xC0DE0042u) {
            say(out, "0xC0DE0042 - correct\n");
        } else {
            say(out, "WRONG - the ordinal resolved to another export\n");
        }
    }

    say(out, "K32CurrentTeb (forwarded to ntdll.NtCurrentTeb) = ");
    say(out, K32CurrentTeb() != NULL_PTR ? "non-NULL - followed\n"
                                         : "NULL - not followed\n");

    ExitProcess(0);
}
