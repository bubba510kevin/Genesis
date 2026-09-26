/* A PE that exercises the NT dispatcher objects through ntdll.dll.
 *
 * The ring-3 half of ROADMAP item 14. The objects themselves are checked at
 * boot by dispatch_selftest(), which can reach them directly; this checks the
 * part that boot test cannot see - the SYSCALL SURFACE. Argument marshalling
 * across the Win64 boundary, OBJECT_ATTRIBUTES parsing, handle allocation,
 * and the NTSTATUS values, none of which exist below ring 3.
 *
 * Like src/winhello, nothing here knows a syscall number or a register
 * convention: it calls NtCreateEvent the way any Windows program does and the
 * linker emits an import naming ntdll.dll.
 *
 * --- what it deliberately does NOT claim ----------------------------------
 * Two PROCESSES. Item 14's written-down check is a named event created by one
 * process and opened by name from another, and that still cannot be done:
 * there is no NtCreateProcess, so a second NT process cannot be started. What
 * is exercised here is the naming path in one process - create by name, open
 * by name, collide on a second create - which is every step except the second
 * process. The blocking half is covered at boot, where a kernel thread really
 * does wait for another context to signal.
 */

#include "../ntdll/ntdll.h"

static const WCHAR con_path[]   = L"\\??\\CON";
static const WCHAR ev_name[]    = L"\\BaseNamedObjects\\WinSyncEvent";

static int failures;

static SIZE_T ascii_len(const char *s) {
    SIZE_T n = 0;

    while (s[n] != 0) {
        n++;
    }
    return n;
}

static void write_all(HANDLE h, const void *data, DWORD len) {
    IO_STATUS_BLOCK iosb;

    NtWriteFile(h, 0, 0, 0, &iosb, (PVOID)data, len, 0, 0);
}

static void say(HANDLE h, const char *s) {
    write_all(h, s, (DWORD)ascii_len(s));
}

static void say_hex(HANDLE h, DWORD v) {
    static const char digits[] = "0123456789ABCDEF";
    char out[11];
    int i;

    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 8; i++) {
        out[2 + i] = digits[(v >> ((7 - i) * 4)) & 0xF];
    }
    out[10] = 0;
    say(h, out);
}

/* One check, one line, in the shape the rest of this tree prints them. The
 * status is printed on failure because "it returned the wrong thing" is not
 * useful without knowing what it returned. */
static void check(HANDLE h, int cond, const char *what, DWORD got) {
    say(h, cond ? "  ok    " : "  FAIL  ");
    say(h, what);
    if (!cond) {
        failures++;
        say(h, "  (status ");
        say_hex(h, got);
        say(h, ")");
    }
    say(h, "\n");
}

static void init_attributes(POBJECT_ATTRIBUTES oa, PUNICODE_STRING name) {
    oa->Length                   = sizeof(*oa);
    oa->Reserved                 = 0;
    oa->RootDirectory            = 0;
    oa->ObjectName               = name;
    oa->Attributes               = OBJ_CASE_INSENSITIVE;
    oa->Reserved2                = 0;
    oa->SecurityDescriptor       = 0;
    oa->SecurityQualityOfService = 0;
}

/* A relative timeout in 100ns units, which is what NT's negative convention
 * means. Zero would be "do not wait at all"; these tests want both. */
static LARGE_INTEGER relative_ms(int ms) {
    LARGE_INTEGER t;

    t.QuadPart = -((long long)ms * 10000LL);
    return t;
}

void start(void) {
    UNICODE_STRING    uname;
    OBJECT_ATTRIBUTES oa;
    OBJECT_ATTRIBUTES noname;
    IO_STATUS_BLOCK   iosb;
    LARGE_INTEGER     zero;
    LARGE_INTEGER     brief;
    HANDLE            con = 0;
    HANDLE            ev = 0, ev2 = 0, sem = 0, mut = 0;
    NTSTATUS          st;
    LONG              prev;

    LdrInitializeThunk();

    RtlInitUnicodeString(&uname, con_path);
    init_attributes(&oa, &uname);
    st = NtOpenFile(&con, GENERIC_WRITE, &oa, &iosb, 0, 0);
    if (!NT_SUCCESS(st)) {
        UNICODE_STRING oops;
        static const WCHAR msg[] = L"winsync: no console\n";

        RtlInitUnicodeString(&oops, msg);
        NtDisplayString(&oops);
        NtTerminateProcess(NtCurrentProcess(), st);
        return;
    }

    say(con, "\nwinsync: NT dispatcher objects through ntdll.dll\n");

    zero.QuadPart = 0;
    brief = relative_ms(30);
    init_attributes(&noname, 0);         /* unnamed: ObjectName is NULL */

    /* --- events ---------------------------------------------------------- */
    st = NtCreateEvent(&ev, 0, &noname, SynchronizationEvent, 0);
    check(con, NT_SUCCESS(st) && ev != 0, "NtCreateEvent, unnamed", st);

    /* Not signalled, and a zero timeout must come back rather than block.
     * STATUS_TIMEOUT is a SUCCESS code, so testing NT_SUCCESS here would pass
     * on a call that did not acquire anything - which is the mistake the
     * separate value exists to catch, and why this compares it exactly. */
    st = NtWaitForSingleObject(ev, 0, &zero);
    check(con, st == STATUS_TIMEOUT,
          "waiting on an unsignalled event with a zero timeout is "
          "STATUS_TIMEOUT", st);

    prev = 0xBAD;
    st = NtSetEvent(ev, &prev);
    check(con, st == STATUS_SUCCESS, "NtSetEvent", st);
    check(con, prev == 0, "and reports the previous state", (DWORD)prev);

    st = NtWaitForSingleObject(ev, 0, &zero);
    check(con, st == STATUS_SUCCESS, "after which the wait succeeds", st);

    /* A synchronisation event auto-resets, so the SECOND wait must time out.
     * One wait cannot tell the two kinds of event apart. */
    st = NtWaitForSingleObject(ev, 0, &zero);
    check(con, st == STATUS_TIMEOUT,
          "and it auto-reset - a second wait times out", st);

    /* A real relative timeout, not zero: this one actually waits. */
    st = NtWaitForSingleObject(ev, 0, &brief);
    check(con, st == STATUS_TIMEOUT,
          "a 30ms relative timeout expires rather than hanging", st);

    /* An absolute timeout is refused rather than misread. A positive value
     * read as a relative interval would be a wait of astronomical length,
     * which is a hang wearing a plausible number. */
    {
        LARGE_INTEGER absolute;

        absolute.QuadPart = 1;
        st = NtWaitForSingleObject(ev, 0, &absolute);
        check(con, st == STATUS_NOT_IMPLEMENTED,
              "an ABSOLUTE timeout is refused, not read as a relative one",
              st);
    }

    /* --- naming ---------------------------------------------------------- */
    RtlInitUnicodeString(&uname, ev_name);
    init_attributes(&oa, &uname);
    st = NtCreateEvent(&ev2, 0, &oa, NotificationEvent, 1);
    check(con, NT_SUCCESS(st) && ev2 != 0,
          "NtCreateEvent under \\BaseNamedObjects", st);

    /* A second create of the same name FAILS. It does not quietly hand back
     * the existing object - that is NtOpenEvent's job, and conflating them is
     * how two programs each believe they created the mutex. */
    {
        HANDLE dup = 0;

        st = NtCreateEvent(&dup, 0, &oa, NotificationEvent, 0);
        check(con, st == STATUS_OBJECT_NAME_COLLISION,
              "and creating that name again is a COLLISION, not a silent open",
              st);
    }

    {
        HANDLE opened = 0;

        st = NtOpenEvent(&opened, 0, &oa);
        check(con, NT_SUCCESS(st) && opened != 0,
              "NtOpenEvent finds it BY NAME", st);
        if (opened != 0) {
            /* It was created signalled and is a NOTIFICATION event, so it
             * stays set - which is what makes this the same object rather
             * than a fresh one that happens to answer. */
            st = NtWaitForSingleObject(opened, 0, &zero);
            check(con, st == STATUS_SUCCESS,
                  "and it is the SAME object - still signalled", st);
            st = NtWaitForSingleObject(opened, 0, &zero);
            check(con, st == STATUS_SUCCESS,
                  "and being a notification event, stays signalled", st);
            NtClose(opened);
        }
    }

    {
        HANDLE missing = 0;
        UNICODE_STRING nope;
        OBJECT_ATTRIBUTES noa;
        static const WCHAR gone[] = L"\\BaseNamedObjects\\NoSuchThing";

        RtlInitUnicodeString(&nope, gone);
        init_attributes(&noa, &nope);
        st = NtOpenEvent(&missing, 0, &noa);
        check(con, st == STATUS_OBJECT_NAME_NOT_FOUND,
              "opening a name that is not there is NAME_NOT_FOUND", st);
    }

    /* --- semaphore -------------------------------------------------------- */
    st = NtCreateSemaphore(&sem, 0, &noname, 1, 2);
    check(con, NT_SUCCESS(st) && sem != 0, "NtCreateSemaphore(1, 2)", st);

    st = NtWaitForSingleObject(sem, 0, &zero);
    check(con, st == STATUS_SUCCESS, "takes its one permit", st);
    st = NtWaitForSingleObject(sem, 0, &zero);
    check(con, st == STATUS_TIMEOUT, "and then it is empty", st);

    prev = 0xBAD;
    st = NtReleaseSemaphore(sem, 1, &prev);
    check(con, st == STATUS_SUCCESS && prev == 0,
          "NtReleaseSemaphore returns the previous count", st);

    /* Over-release is REFUSED. The pair below is what makes this a test: a
     * clamp would pass the refusal on its own, so the count is then shown not
     * to have moved. */
    st = NtReleaseSemaphore(sem, 5, 0);
    check(con, st == STATUS_INVALID_PARAMETER,
          "releasing past the maximum is refused, not clamped", st);
    st = NtWaitForSingleObject(sem, 0, &zero);
    check(con, st == STATUS_SUCCESS, "and the count did not move - one left",
          st);
    st = NtWaitForSingleObject(sem, 0, &zero);
    check(con, st == STATUS_TIMEOUT, "and only one", st);

    /* --- mutant ----------------------------------------------------------- */
    st = NtCreateMutant(&mut, 0, &noname, 0);
    check(con, NT_SUCCESS(st) && mut != 0, "NtCreateMutant, unowned", st);

    st = NtWaitForSingleObject(mut, 0, &zero);
    check(con, st == STATUS_SUCCESS, "it can be taken", st);
    /* RECURSIVE. A semaphore of one would time out here, which is the whole
     * reason a mutant is not one. */
    st = NtWaitForSingleObject(mut, 0, &zero);
    check(con, st == STATUS_SUCCESS, "and taken AGAIN by its owner", st);

    st = NtReleaseMutant(mut, 0);
    check(con, st == STATUS_SUCCESS, "released once", st);
    st = NtReleaseMutant(mut, 0);
    check(con, st == STATUS_SUCCESS, "and once more, as the second take "
                                     "requires", st);
    st = NtReleaseMutant(mut, 0);
    check(con, st == STATUS_MUTANT_NOT_OWNED,
          "a third release is MUTANT_NOT_OWNED - it is not held", st);

    /* --- handles ----------------------------------------------------------- */
    st = NtWaitForSingleObject((HANDLE)0x1234, 0, &zero);
    check(con, st == STATUS_INVALID_HANDLE,
          "waiting on a bogus handle is INVALID_HANDLE, not a fault", st);
    /* The handle is GOOD and the object is not waitable, which is a type
     * mismatch rather than a bad handle. Asserting the exact status and not
     * merely "not success" is the difference between a check that would pass
     * on any error and one that says the error is the right one - a caller
     * told INVALID_HANDLE goes looking at its own bookkeeping for a handle it
     * closed, which is the wrong place entirely. */
    st = NtWaitForSingleObject(con, 0, &zero);
    check(con, st == STATUS_OBJECT_TYPE_MISMATCH,
          "and waiting on the CONSOLE is TYPE_MISMATCH - the handle is fine, "
          "the object is not waitable", st);

    NtClose(ev);
    NtClose(ev2);
    NtClose(sem);
    NtClose(mut);

    say(con, failures == 0 ? "winsync: all checks passed\n"
                           : "winsync: FAILURES\n");
    NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)failures);
}
