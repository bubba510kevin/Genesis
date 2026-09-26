/* A PE built by MinGW-w64 that imports from ntdll.dll.
 *
 * The difference from src/mkpe.py's hand-assembled image is the whole point:
 * nothing here knows a syscall number, a register convention or a structure
 * offset. It calls NtOpenFile the way any Windows program does, the linker
 * emits an import table naming ntdll.dll, and the kernel's loader is what
 * makes the call land somewhere. If this runs, the import table, the export
 * directory, the IAT and the DLL all agree.
 *
 * -nostdlib: there is no C runtime on the volume and none is wanted. The
 * entry point below is the whole of the startup code.
 */

#include "../ntdll/ntdll.h"

static const WCHAR con_path[]  = L"\\??\\CON";
static const WCHAR dos_path[]  = L"C:\\etc\\motd";

static const char banner[] =
    "hello from a MinGW-built PE, linked against ntdll.dll\n";
static const char heap_ok[] =
    "RtlAllocateHeap returned usable memory\n";
static const char path_note[] =
    "RtlDosPathNameToNtPathName_U: C:\\etc\\motd -> ";
static const char read_note[] =
    "and reading it through that name gives: ";

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

/* UTF-16 down to ASCII, for printing a native path back out. Anything this
 * cannot carry becomes '?' rather than being dropped, so a mangled name looks
 * wrong instead of looking short. */
static void say_unicode(HANDLE h, PUNICODE_STRING u) {
    char out[128];
    SIZE_T chars = u->Length / 2;
    SIZE_T i;

    if (chars > sizeof(out) - 2) {
        chars = sizeof(out) - 2;
    }
    for (i = 0; i < chars; i++) {
        WCHAR c = u->Buffer[i];

        out[i] = (c != 0 && c < 0x100) ? (char)c : '?';
    }
    out[chars] = '\n';
    write_all(h, out, (DWORD)(chars + 1));
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

void start(void) {
    UNICODE_STRING    name;
    UNICODE_STRING    nt_name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK   iosb;
    HANDLE            con = 0;
    HANDLE            file = 0;
    char             *buf;
    NTSTATUS          st;

    /* The kernel already built the TEB and PEB; this claims the process heap
     * out of the PEB so RtlAllocateHeap has somewhere to hang it. On Windows
     * the kernel enters here rather than at the image entry - Genesis jumps
     * straight to the image, so the image calls it. */
    LdrInitializeThunk();

    RtlInitUnicodeString(&name, con_path);
    init_attributes(&oa, &name);
    st = NtOpenFile(&con, GENERIC_WRITE, &oa, &iosb, 0, 0);
    if (!NT_SUCCESS(st)) {
        /* No console handle, so the one call that needs no handle. */
        UNICODE_STRING oops;
        static const WCHAR msg[] = L"NtOpenFile on the console failed\n";

        RtlInitUnicodeString(&oops, msg);
        NtDisplayString(&oops);
        NtTerminateProcess(NtCurrentProcess(), st);
        return;
    }

    say(con, banner);

    /* The heap, which is RtlAllocateHeap over NtAllocateVirtualMemory. */
    buf = (char *)RtlAllocateHeap(0, HEAP_ZERO_MEMORY, 512);
    if (buf != 0) {
        buf[0] = 'x';
        say(con, heap_ok);
    }

    /* The path conversion, which is what every Win32 filename call will go
     * through: a DOS path in, a native path out, resolved by the same
     * namespace ash resolves /dev through. */
    if (RtlDosPathNameToNtPathName_U(dos_path, &nt_name, 0, 0)) {
        say(con, path_note);
        say_unicode(con, &nt_name);

        init_attributes(&oa, &nt_name);
        st = NtOpenFile(&file, GENERIC_READ, &oa, &iosb, 0, 0);
        if (NT_SUCCESS(st) && buf != 0) {
            iosb.Information = 0;
            st = NtReadFile(file, 0, 0, 0, &iosb, buf, 256, 0, 0);
            if (NT_SUCCESS(st) && iosb.Information > 0) {
                say(con, read_note);
                write_all(con, buf, (DWORD)iosb.Information);
            }
            NtClose(file);
        }
        RtlFreeHeap(0, 0, nt_name.Buffer);
    }

    if (buf != 0) {
        RtlFreeHeap(0, 0, buf);
    }
    NtClose(con);
    NtTerminateProcess(NtCurrentProcess(), STATUS_SUCCESS);
}
