/* Structured exception handling, the Win32 face of it (ROADMAP 14(c)).
 *
 * The machinery is ntdll's (src/ntdll/except.c): table-based dispatch,
 * unwinding, __C_specific_handler, vectored handlers. What is left for
 * kernel32 is the Win32 shape - RaiseException's argument list, and the
 * process-wide unhandled-exception filter.
 */

#include "kernel32.h"

void WINAPI RaiseException(DWORD code, DWORD flags, DWORD nargs,
                           const ULONG_PTR *args) {
    EXCEPTION_RECORD rec;
    DWORD i;

    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
    rec.ExceptionRecord = NULL_PTR;
    rec.ExceptionAddress = NULL_PTR;            /* RtlRaiseException's job */
    /* More than 15 arguments is not an error on Windows; the rest are
     * dropped. */
    if (args == NULL_PTR) {
        nargs = 0;
    }
    if (nargs > EXCEPTION_MAXIMUM_PARAMETERS) {
        nargs = EXCEPTION_MAXIMUM_PARAMETERS;
    }
    rec.NumberParameters = nargs;
    for (i = 0; i < nargs; i++) {
        rec.ExceptionInformation[i] = args[i];
    }
    RtlRaiseException(&rec);
    /* Returns only if a handler continued execution. */
}

/* The program's filter. ntdll calls UnhandledExceptionFilter when nothing
 * on the stack handled an exception; it asks this one. */
static PTOP_LEVEL_EXCEPTION_FILTER user_filter;

LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS info) {
    if (user_filter != NULL_PTR) {
        return user_filter(info);
    }
    /* No filter: let ntdll's last chance end the process, with its report.
     * Windows would show the crash dialog here; this is that, in text. */
    return EXCEPTION_CONTINUE_SEARCH;
}

PTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(
    PTOP_LEVEL_EXCEPTION_FILTER filter) {
    PTOP_LEVEL_EXCEPTION_FILTER old = user_filter;

    user_filter = filter;
    (void)RtlSetUnhandledExceptionFilter(UnhandledExceptionFilter);
    return old;
}
