/* seh.exe - structured exception handling, from real __try/__except/__finally
 * code. ROADMAP item 14(c)'s ring-3 check.
 *
 * Compiled by clang for the MinGW target (see build.sh): GCC has no __try,
 * and clang's is the genuine article - scope tables in .xdata naming
 * __C_specific_handler, exactly what MSVC emits, so what passes here is
 * ntdll's dispatcher and unwinder working on the same data a Windows
 * program would hand it. -fasync-exceptions makes a hardware fault inside a
 * __try body catchable, not only an exception raised by a call.
 *
 * What it proves:
 *
 *   - a fault (access violation, divide by zero, breakpoint) reaches the
 *     program as an exception with the right code and parameters, and
 *     __except catches it - in the faulting function or several frames up;
 *   - the order is filter, then __finally blocks (during the unwind), then
 *     the __except body - and unwound frames' __finally blocks all run;
 *   - RaiseException carries its code and arguments; a filter returning
 *     EXCEPTION_CONTINUE_EXECUTION resumes after it;
 *   - vectored handlers run first, in order, and can fix the context and
 *     continue (skipping an int3);
 *   - it all works on a second thread's stack, and an exception inside an
 *     __except block is caught by an outer __try;
 *   - SetUnhandledExceptionFilter's filter sees an exception nothing else
 *     handled.
 */

#include "../kernel32/kernel32.h"

#define GetExceptionCode()        _exception_code()
#define GetExceptionInformation() ((PEXCEPTION_POINTERS)_exception_info())
unsigned long _exception_code(void);
void *_exception_info(void);

#define STATUS_BREAKPOINT_      0x80000003u
#define STATUS_DIV_ZERO_        0xC0000094u
#define MY_CODE                 0xE0000042u

/* clang may emit these for struct copies even freestanding. */
void *memset(void *d, int c, SIZE_T n) {
    unsigned char *p = (unsigned char *)d;

    while (n--) {
        *p++ = (unsigned char)c;
    }
    return d;
}

void *memcpy(void *d, const void *s, SIZE_T n) {
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;

    while (n--) {
        *p++ = *q++;
    }
    return d;
}

static HANDLE out;
static int passes, failures;

static void say(const char *s) {
    DWORD n = 0, written = 0;

    while (s[n] != '\0') {
        n++;
    }
    WriteFile(out, s, n, &written, NULL_PTR);
}

static void say_u(DWORD v) {
    char buf[12];
    int i = 11;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    say(&buf[i]);
}

static void say_x(QWORD v) {
    char buf[19];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xF];
    }
    buf[18] = '\0';
    say(buf);
}

static void check(int cond, const char *what) {
    say(cond ? "  ok    " : "  FAIL  ");
    say(what);
    say("\r\n");
    if (cond) passes++; else failures++;
}

/* Nothing here may be optimised into knowing the answer. */
static volatile int *volatile null_ptr;
static volatile int zero;
static int *volatile bad_ptr = (int *)(SIZE_T)0x10;

/* --- faults caught where they happen ------------------------------------ */

static DWORD caught_code;
static QWORD caught_params[2];
static QWORD caught_addr, caught_rip;

static LONG capture(PEXCEPTION_POINTERS ep) {
    caught_code = ep->ExceptionRecord->ExceptionCode;
    caught_params[0] = ep->ExceptionRecord->ExceptionInformation[0];
    caught_params[1] = ep->ExceptionRecord->ExceptionInformation[1];
    caught_addr = (QWORD)ep->ExceptionRecord->ExceptionAddress;
    caught_rip = ep->ContextRecord->Rip;
    return EXCEPTION_EXECUTE_HANDLER;
}

static int write_fault(void) {
    int reached = 0;

    __try {
        *bad_ptr = 1;
        reached = 1;
    } __except (capture(GetExceptionInformation())) {
        return reached ? -1 : (int)GetExceptionCode();
    }
    return 0;
}

static int read_fault(void) {
    __try {
        return *null_ptr;
    } __except (capture(GetExceptionInformation())) {
        return 7;
    }
}

static int divide(void) {
    __try {
        return 100 / zero;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return (int)GetExceptionCode();
    }
}

/* --- through frames, with __finally ------------------------------------- */

static char order[8];
static int order_n;

static void mark(char c) {
    if (order_n < 7) {
        order[order_n++] = c;
        order[order_n] = '\0';
    }
}

static __attribute__((noinline)) void deepest(void) {
    __try {
        *bad_ptr = 2;
    } __finally {
        mark('1');                   /* innermost finally first */
    }
}

static __attribute__((noinline)) void middle(void) {
    __try {
        deepest();
    } __finally {
        mark('2');
    }
}

static LONG filter_marks(void) {
    mark('F');
    return EXCEPTION_EXECUTE_HANDLER;
}

static int through_frames(void) {
    volatile int keep = 12345;       /* must survive the unwind */

    __try {
        middle();
        mark('X');                   /* never */
    } __except (filter_marks()) {
        mark('E');
    }
    return keep;
}

/* --- RaiseException ------------------------------------------------------ */

static LONG check_args(PEXCEPTION_POINTERS ep) {
    PEXCEPTION_RECORD r = ep->ExceptionRecord;

    if (r->ExceptionCode == MY_CODE && r->NumberParameters == 2 &&
        r->ExceptionInformation[0] == 111 && r->ExceptionInformation[1] == 222) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int raise_caught(void) {
    ULONG_PTR args[2] = { 111, 222 };

    __try {
        RaiseException(MY_CODE, 0, 2, args);
        return -1;
    } __except (check_args(GetExceptionInformation())) {
        return (int)GetExceptionCode();
    }
}

static int continued;

static int raise_continued(void) {
    int after = 0;

    __try {
        RaiseException(MY_CODE, 0, 0, NULL_PTR);
        after = 1;                   /* reached only by continuing */
    } __except (continued++, EXCEPTION_CONTINUE_EXECUTION) {
        return -1;
    }
    return after;
}

/* --- vectored handlers --------------------------------------------------- */

static int vec_order[4];
static int vec_n;

static LONG vec_a(PEXCEPTION_POINTERS ep) {
    (void)ep;
    if (vec_n < 4) vec_order[vec_n++] = 'a';
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG vec_b(PEXCEPTION_POINTERS ep) {
    (void)ep;
    if (vec_n < 4) vec_order[vec_n++] = 'b';
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Steps over a breakpoint and continues - what a debugger does. */
static LONG skip_int3(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode == STATUS_BREAKPOINT_ &&
        ep->ContextRecord->Rip == (QWORD)ep->ExceptionRecord->ExceptionAddress &&
        *(const unsigned char *)ep->ContextRecord->Rip == 0xCC) {
        ep->ContextRecord->Rip += 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* --- nesting, threads, the unhandled filter ------------------------------ */

static int nested(void) {
    __try {
        __try {
            *bad_ptr = 3;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 100 / zero;       /* a second exception, in the handler */
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return (int)GetExceptionCode();
    }
    return 0;
}

static volatile DWORD thread_result;

static DWORD WINAPI thread_body(LPVOID param) {
    (void)param;
    thread_result = (DWORD)write_fault() == 0xC0000005u &&
                    through_frames() == 12345;
    return 0;
}

static int unhandled_seen;

static LONG WINAPI top_filter(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode == MY_CODE + 1) {
        unhandled_seen++;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void start(void) {
    PVOID ha, hb, hs;
    HANDLE t;

    out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("seh: structured exception handling\r\n");

    /* --- faults ----------------------------------------------------------- */
    check((DWORD)write_fault() == 0xC0000005u,
          "a write to an unmapped address is caught: ACCESS_VIOLATION");
    check(caught_params[0] == 1 && caught_params[1] == 0x10,
          "with ExceptionInformation = { 1 (write), 0x10 (the address) }");
    check(caught_addr != 0 && caught_addr == caught_rip,
          "and ExceptionAddress is the faulting instruction, as is the "
          "CONTEXT's Rip");
    check(read_fault() == 7 && caught_params[0] == 0 &&
          caught_params[1] == 0,
          "a read of NULL: ExceptionInformation = { 0 (read), 0 }");
    check((DWORD)divide() == STATUS_DIV_ZERO_,
          "a division by zero: INTEGER_DIVIDE_BY_ZERO");

    /* --- frames and finally --------------------------------------------- */
    order_n = 0;
    order[0] = '\0';
    check(through_frames() == 12345,
          "a fault two calls down is caught by the caller's caller, and its "
          "locals survive the unwind");
    check(order[0] == 'F' && order[1] == '1' && order[2] == '2' &&
          order[3] == 'E' && order_n == 4,
          "in order: filter, the inner __finally, the outer __finally, "
          "then the __except block");
    if (!(order[0] == 'F' && order_n == 4)) {
        say("    (order was \"");
        say(order);
        say("\")\r\n");
    }

    /* --- RaiseException -------------------------------------------------- */
    check((DWORD)raise_caught() == MY_CODE,
          "RaiseException(code, 2 args) is caught with its code and "
          "arguments");
    continued = 0;
    check(raise_continued() == 1 && continued == 1,
          "a filter returning EXCEPTION_CONTINUE_EXECUTION resumes after "
          "RaiseException");

    /* --- vectored -------------------------------------------------------- */
    vec_n = 0;
    ha = AddVectoredExceptionHandler(0, vec_a);
    hb = AddVectoredExceptionHandler(1, vec_b);          /* first */
    check(ha != NULL_PTR && hb != NULL_PTR, "AddVectoredExceptionHandler x2");
    check((DWORD)divide() == STATUS_DIV_ZERO_ && vec_n == 2 &&
          vec_order[0] == 'b' && vec_order[1] == 'a',
          "vectored handlers run before frame handlers, the FIRST-added-"
          "with-First=1 first");
    check(RemoveVectoredExceptionHandler(ha) &&
          RemoveVectoredExceptionHandler(hb), "and can be removed");
    vec_n = 0;
    (void)divide();
    check(vec_n == 0, "after which they are not called");

    hs = AddVectoredExceptionHandler(1, skip_int3);
    __asm__ volatile ("int3");
    check(1, "a vectored handler steps over an int3 and continues - "
             "BREAKPOINT arrived with Rip AT the int3");
    RemoveVectoredExceptionHandler(hs);

    /* --- nesting, threads ------------------------------------------------ */
    check((DWORD)nested() == STATUS_DIV_ZERO_,
          "an exception inside an __except block is caught by the outer "
          "__try");
    thread_result = 0;
    t = CreateThread(NULL_PTR, 0, thread_body, NULL_PTR, 0, NULL_PTR);
    check(t != NULL_PTR && WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 &&
          thread_result == 1,
          "all of it on a second thread's own stack");

    /* --- nothing on the stack wants it ----------------------------------- */
    SetUnhandledExceptionFilter(top_filter);
    unhandled_seen = 0;
    RaiseException(MY_CODE + 1, 0, 0, NULL_PTR);
    check(unhandled_seen == 1,
          "an exception nothing handles reaches SetUnhandledExceptionFilter's "
          "filter, which may continue it");
    SetUnhandledExceptionFilter(NULL_PTR);

    say("\r\nseh: ");
    say_u((DWORD)passes);
    say(" passed, ");
    say_u((DWORD)failures);
    say(" failed\r\n");
    (void)say_x;
    ExitProcess((DWORD)failures);
}
