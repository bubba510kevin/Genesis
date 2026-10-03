#include "device.h"
#include "timer.h"
#include "fileobj.h"
#include "ns.h"
#include "dispatch.h"
#include "nt.h"
#include "nt_context.h"
#include "ntmix.h"
#include "ntsec.h"
#include "ntsync.h"
#include "ntvm.h"
#include "section.h"
#include "ntspawn.h"
#include "pipe.h"
#include "kheap.h"
#include "registry.h"
#include "acl.h"
#include "fileobj.h"
#include "object.h"
#include "process.h"
#include "sched.h"
#include "teb.h"
#include "screen.h"
#include "syscall.h"
#include "tty.h"
#include "typesk.h"

/* See nt.h for the number space and why these two calls exist this early. */

/* --- layout assertions ---------------------------------------------------
 *
 * These are ABI structures: their field offsets are fixed by what a compiler
 * building a Windows program would produce, not by anything this kernel is
 * free to choose. A mismatch does not fail to build and does not crash - it
 * reads one field from the wrong place, and the symptom is a call that
 * returns a plausible error and prints nothing.
 *
 * That is not hypothetical. nt_unicode_string_t was declared packed, which
 * removed the four bytes of padding before Buffer and moved it from offset 8
 * to offset 4; every NtDisplayString and NtOpenFile then read a pointer made
 * of padding and half an address, failed the user-pointer check, and returned
 * STATUS_ACCESS_VIOLATION silently. A test image that happened to have been
 * generated against the same wrong layout passed.
 *
 * A negative array size is the check that cannot be ignored: it fails the
 * build rather than the run. */
typedef char nt_unicode_string_layout[
    (sizeof(nt_unicode_string_t) == 16 &&
     __builtin_offsetof(nt_unicode_string_t, length) == 0 &&
     __builtin_offsetof(nt_unicode_string_t, maximum_length) == 2 &&
     __builtin_offsetof(nt_unicode_string_t, buffer) == 8) ? 1 : -1];

typedef char nt_object_attributes_layout[
    (sizeof(nt_object_attributes_t) == 48 &&
     __builtin_offsetof(nt_object_attributes_t, root_directory) == 8 &&
     __builtin_offsetof(nt_object_attributes_t, object_name) == 16 &&
     __builtin_offsetof(nt_object_attributes_t, attributes) == 24 &&
     __builtin_offsetof(nt_object_attributes_t, security_descriptor) == 32) ? 1 : -1];

typedef char nt_io_status_block_layout[
    (sizeof(nt_io_status_block_t) == 16 &&
     __builtin_offsetof(nt_io_status_block_t, information) == 8) ? 1 : -1];

/* NtDisplayString(PUNICODE_STRING).
 *
 * Written through the CONSOLE OBJECT rather than through print_string, and
 * that is the whole point of doing it this way: \Device\Console is one object
 * with one piece of state, and a PE binary writing to it has to reach the
 * same object ash does. Calling the screen driver directly here would work
 * today and would be a second console the moment the first one grows a
 * setting - which is exactly the drift the object namespace exists to
 * prevent. */
static uint64 nt_display_string(uint64 str_ptr) {
    nt_unicode_string_t us;
    object_t *console;
    uint64 pos = 0;
    uint64 chars, i;

    if (!user_ptr_ok(str_ptr)) {
        return STATUS_ACCESS_VIOLATION;
    }
    us = *(const nt_unicode_string_t *)str_ptr;
    if (us.length == 0) {
        return STATUS_SUCCESS;
    }
    if (!user_ptr_ok(us.buffer)) {
        return STATUS_ACCESS_VIOLATION;
    }

    console = tty_console();
    if (console == NULL || console->type == NULL ||
        console->type->write == NULL) {
        return STATUS_NOT_IMPLEMENTED;
    }

    /* Length is in bytes. Two per UTF-16 code unit. */
    chars = us.length / 2;

    /* Down-converted a chunk at a time through a bounded stack buffer. Not a
     * real UTF-16 decoder: anything outside Latin-1 becomes '?' rather than
     * being silently dropped, so a string that did not survive the trip looks
     * wrong instead of looking short. A proper conversion is owed once
     * something writes text this cannot carry. */
    while (chars > 0) {
        char chunk[64];
        uint64 take = chars < sizeof(chunk) ? chars : sizeof(chunk);

        for (i = 0; i < take; i++) {
            uint16 wc = *(const uint16 *)(us.buffer + (i * 2));

            chunk[i] = (wc != 0 && wc < 0x100) ? (char)wc : '?';
        }
        console->type->write(console, chunk, take, &pos);
        us.buffer += take * 2;
        chars     -= take;
    }
    return STATUS_SUCCESS;
}

/* Terminate the process whose leader is `pid` from outside: every thread
 * retired wherever it is, the leader last (it owns the tables the others'
 * retirement reads), and the full 32-bit code recorded for its process
 * object. TerminateProcess(handle) on another process. */
static uint64 nt_kill_process(int pid, uint32 code) {
    process_t *leader = proc_find(pid);
    int i;

    if (leader == NULL || leader->tgid != pid || leader->group_finished) {
        return STATUS_SUCCESS;            /* already gone: nothing to do */
    }
    if (!leader->nt_exit_code_set) {
        leader->nt_exit_code = code;
        leader->nt_exit_code_set = 1;
    }
    for (i = 0; i < proc_slots_used(); i++) {
        process_t *t = proc_at(i);

        if (t != NULL && t != leader && !t->is_kthread && t->tgid == pid &&
            t->state != PROC_ZOMBIE) {
            proc_retire(t, (int)(code & 0xFF));
        }
    }
    if (leader->state != PROC_ZOMBIE) {
        proc_retire(leader, (int)(code & 0xFF));
    }
    proc_group_finished(leader);
    return STATUS_SUCCESS;
}

/* NtTerminateProcess(HANDLE, NTSTATUS).
 *
 * The current process (the pseudo-handle -1, or a handle that names it)
 * goes down the ordinary group exit. A handle to ANOTHER process - from
 * NtCreateUserProcess - kills that one from outside. The status is the full
 * 32-bit NT exit code: recorded for the process object, which is what
 * GetExitCodeProcess and a waiter read, while the POSIX side keeps the low
 * eight bits it can carry. */
static uint64 nt_terminate_process(uint64 handle, uint64 status,
                                   struct syscall_frame *frame) {
    process_t *me = proc_current();
    process_t *leader;

    if (handle != NT_CURRENT_PROCESS && handle != 0) {
        object_t *obj = nt_object_of(handle);
        int pid = 0;

        if (obj == NULL || process_object_query(obj, &pid, NULL, NULL) < 0) {
            return STATUS_INVALID_HANDLE;
        }
        if (pid != me->tgid) {
            return nt_kill_process(pid, (uint32)status);
        }
    }
    leader = proc_find(me->tgid);
    if (leader != NULL && !leader->nt_exit_code_set) {
        leader->nt_exit_code = (uint32)status;
        leader->nt_exit_code_set = 1;
    }
    /* Straight into the shared exit path. Ending a process is mechanism, not
     * ABI - the reaping, the SIGCHLD to a Linux parent, the vfork resume are
     * all the same regardless of which number asked.
     *
     * The GROUP path, not the single-thread one. With threads, "terminate
     * the process" and "end this thread" came apart: ExitProcess called
     * with a worker still running used to end only the caller, leaving the
     * process a zombie leader with a live thread in it - on NT, and here
     * now, every thread goes. */
    return syscall_exit_group(status & 0xFF, frame);
}



/* --- names ---------------------------------------------------------------
 *
 * This is where the namespace stops being theoretical. An NT caller names an
 * object with an OBJECT_ATTRIBUTES holding a UNICODE_STRING, and that string
 * is a native path - \??\CON, \Device\Console, \??\C:\etc\motd - which is
 * exactly what ns_lookup already resolves, unparsed remainder and all. No
 * second name table, no per-personality device list: a PE opening \??\CON and
 * ash opening /dev/console reach the same object because they resolve through
 * the same tree.
 */

/* UTF-16 down to the namespace's own 8-bit form. Not a real decoder for the
 * same reason nt_display_string's is not: a character this cannot carry
 * becomes '?' so a name that did not survive fails to resolve loudly, rather
 * than silently resolving to a different object. Device and drive names are
 * ASCII in practice; the day one is not, the failure is a -ENOENT with a
 * visibly mangled name and not a mystery. */
static int unicode_to_path(uint64 str_ptr, char *out, uint64 cap) {
    nt_unicode_string_t us;
    uint64 chars, i;

    if (!user_ptr_ok(str_ptr)) {
        return 0;
    }
    us = *(const nt_unicode_string_t *)str_ptr;
    if (us.length == 0 || !user_ptr_ok(us.buffer)) {
        return 0;
    }
    chars = us.length / 2;               /* Length is BYTES, not characters */
    if (chars + 1 > cap) {
        return 0;
    }
    for (i = 0; i < chars; i++) {
        uint16 wc = *(const uint16 *)(us.buffer + i * 2);

        out[i] = (wc != 0 && wc < 0x100) ? (char)wc : '?';
    }
    out[chars] = '\0';
    return 1;
}

/* Argument `n` (one-based, n >= 5) off the caller's stack. Bounds-checked
 * like any other user pointer: the RSP it is computed from came out of a
 * register the caller controls.
 *
 * Returns the whole 64-bit slot, ALWAYS - it cannot know the declared width
 * of the argument it is fetching. Every stack argument occupies eight bytes,
 * but the Win64 ABI only requires the caller to write as many of them as the
 * type needs, so for anything narrower than 64 bits the high half is stale
 * stack. A caller of this function fetching a ULONG, a DWORD or a BOOL must
 * mask; see nt_rw_file's Length, which is where that was learned. */
int nt_stack_arg(uint64 rsp, int n, uint64 *out) {
    uint64 at = rsp + NT_STACK_ARG_OFFSET(n);

    if (!user_ptr_ok(at) || !user_ptr_ok(at + 7)) {
        return 0;
    }
    *out = *(const uint64 *)at;
    return 1;
}

static int nt_handle_index(uint64 handle) {
    uint64 raw;

    if (handle == 0 || (handle & 3) != 0) {
        return -1;                       /* NULL is never a valid handle */
    }
    raw = handle >> 2;
    if (raw == 0 || raw > MAX_HANDLES) {
        return -1;
    }
    return (int)(raw - 1);
}

static uint64 status_from_ns(int rc) {
    switch (rc) {
        case -2:  return STATUS_OBJECT_NAME_NOT_FOUND;
        case -20: return STATUS_NOT_A_DIRECTORY;
        case -21: return STATUS_OBJECT_PATH_NOT_FOUND;   /* named a directory */
        case -36: return STATUS_NAME_TOO_LONG;
        default:  return STATUS_OBJECT_PATH_NOT_FOUND;
    }
}

/* NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
 *            ULONG ShareAccess, ULONG OpenOptions)
 *
 * Six parameters, so all of them are in registers. */
static uint64 nt_open_file(uint64 handle_out, uint64 access_mask,
                           uint64 attrs_ptr, uint64 iosb_ptr) {
    process_t *p = proc_current();
    nt_object_attributes_t oa;
    nt_io_status_block_t *iosb = (nt_io_status_block_t *)iosb_ptr;
    char path[NS_PATH_MAX];
    char remainder[NS_PATH_MAX];
    object_t *obj = NULL;
    open_file_t *of;
    uint32 access = 0;
    int rc, index;

    if (!user_ptr_ok(handle_out) || !user_ptr_ok(attrs_ptr)) {
        return STATUS_ACCESS_VIOLATION;
    }
    oa = *(const nt_object_attributes_t *)attrs_ptr;
    if (oa.length < sizeof(nt_object_attributes_t)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (oa.root_directory != 0) {
        /* Relative opens need the path of the directory a handle names, and
         * an open file object stores an object rather than a path. Worth
         * doing when something asks; the same gap sys_openat has for a dirfd
         * other than AT_FDCWD, and it should be closed once for both. */
        return STATUS_NOT_IMPLEMENTED;
    }
    if (!unicode_to_path(oa.object_name, path, sizeof(path))) {
        return STATUS_INVALID_PARAMETER;
    }

    if (access_mask & (GENERIC_READ | FILE_READ_DATA)) {
        access |= ACCESS_READ;
    }
    if (access_mask & (GENERIC_WRITE | FILE_WRITE_DATA)) {
        access |= ACCESS_WRITE;
    }
    if (access == 0) {
        access = ACCESS_READ;
    }

    rc = ns_lookup(path, &obj, remainder, sizeof(remainder));
    if (rc != 0) {
        return status_from_ns(rc);
    }

    /* The unparsed remainder, handed to the device that owns it.
     *
     * \??\C: resolves to the volume object and hands back \etc\motd, which
     * belongs to the VOLUME and not to the namespace. This used to convert it
     * to "/etc/motd" and call fileobj_open - correct while there was one
     * volume and it was the root, and wrong the moment there are two:
     * fileobj_open routes through the mount table, so \??\D:\notes.txt
     * resolved to volume D:, threw it away, and read /notes.txt off whatever
     * was mounted at "/". A path that names one disk and reads another.
     *
     * dev_open_object asks the device instead, which is what the remainder
     * was designed for and what device.h's parse op exists to do. A PE
     * opening \??\C:\etc\motd and ash opening /etc/motd still get the same
     * bytes - now because both reach the same volume, rather than because
     * there was only ever one. */
    {
        object_t *opened = NULL;

        rc = dev_open_object(obj, remainder, access, &opened);
        if (rc != 0) {
            return rc == -30 ? STATUS_ACCESS_DENIED
                             : STATUS_OBJECT_NAME_NOT_FOUND;
        }
        obj = opened;
    }

    of = of_open(obj, access);
    ob_deref(obj);
    if (of == NULL) {
        return STATUS_TOO_MANY_OPENED_FILES;
    }
    /* Not inheritable by default. That is the NT rule and the opposite of the
     * POSIX one, which is exactly why HANDLE_INHERITABLE exists as a separate
     * bit from HANDLE_CLOEXEC rather than as its inverse. */
    index = handle_alloc(p->handles, of, 0);
    if (index < 0) {
        return STATUS_TOO_MANY_OPENED_FILES;
    }

    *(uint64 *)handle_out = NT_HANDLE_FROM_INDEX(index);
    if (user_ptr_ok(iosb_ptr)) {
        iosb->status      = STATUS_SUCCESS;
        iosb->information = 1;           /* FILE_OPENED */
    }
    return STATUS_SUCCESS;
}

static uint64 nt_close(uint64 handle) {
    int index = nt_handle_index(handle);

    if (index < 0) {
        return STATUS_INVALID_HANDLE;
    }
    return handle_close(proc_current()->handles, index) == 0
               ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

/* NtReadFile / NtWriteFile.
 *
 * Nine documented parameters. Six arrive in registers and Length, ByteOffset
 * and Key come off the user stack - see the convention in nt.h. Event, the
 * APC routine and its context are accepted and ignored: they select
 * asynchronous completion, and nothing here completes asynchronously yet.
 * Refusing them outright would be the wrong call, because a caller that
 * passes NULL for all three - which is what synchronous I/O looks like - is
 * asking for exactly what this does.
 *
 * The byte count goes in IoStatusBlock->Information and NOT in the return
 * value. That is the shape difference from read(2) that catches people: a
 * caller reading the NTSTATUS as a length sees 0 and concludes end of file on
 * every successful read. */
static uint64 nt_rw_file(uint64 handle, int writing) {
    process_t   *p = proc_current();
    open_file_t *f;
    nt_io_status_block_t *iosb;
    uint64 user_rsp = syscall_get_user_rsp();
    uint64 iosb_ptr, buffer, length;
    int64  done;
    int    index = nt_handle_index(handle);

    if (index < 0) {
        return STATUS_INVALID_HANDLE;
    }
    /* Arguments five, six and seven: IoStatusBlock, Buffer, Length. Event,
     * the APC routine and its context are arguments two to four and are
     * accepted and ignored - they select asynchronous completion, and a
     * caller passing NULL for all three is asking for exactly what this
     * does. */
    if (!nt_stack_arg(user_rsp, 5, &iosb_ptr) ||
        !nt_stack_arg(user_rsp, 6, &buffer) ||
        !nt_stack_arg(user_rsp, 7, &length)) {
        return STATUS_ACCESS_VIOLATION;
    }
    /* Length is a ULONG - THIRTY-TWO bits - and nt_stack_arg hands back the
     * whole 64-bit slot it sits in. The Win64 ABI only requires a caller to
     * write the low four bytes of the eight-byte home for a 32-bit argument,
     * so the top half is whatever that stack slot last held.
     *
     * Without this mask, k32.exe's very first WriteFile of 47 bytes arrived
     * as 0x901175080000002f. The low half is right, so the correct 47 bytes
     * came out and the write then kept going for the rest of the count -
     * printing whatever followed in the image until it walked off the last
     * mapped page and took a fault in console_write. That is why the symptom
     * looked like string corruption rather than a bad length: the first line
     * was perfect.
     *
     * Masking here rather than in the object's write op, because this is
     * where the NT ABI is being decoded and the width is a fact about the NT
     * prototype, not about the console. */
    length &= 0xFFFFFFFFULL;

    if (!user_ptr_ok(iosb_ptr) || !user_ptr_ok(buffer)) {
        return STATUS_ACCESS_VIOLATION;
    }
    iosb = (nt_io_status_block_t *)iosb_ptr;

    f = handle_get(p->handles, index);
    if (f == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (f->obj == NULL || f->obj->type == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (writing) {
        if (!(f->access & ACCESS_WRITE) || f->obj->type->write == NULL) {
            return STATUS_ACCESS_DENIED;
        }
        done = f->obj->type->write(f->obj, (const void *)buffer, length,
                                   &f->offset);
    } else {
        if (!(f->access & ACCESS_READ) || f->obj->type->read == NULL) {
            return STATUS_ACCESS_DENIED;
        }
        done = f->obj->type->read(f->obj, (void *)buffer, length, &f->offset);
    }

    if (done < 0) {
        iosb->status      = STATUS_ACCESS_DENIED;
        iosb->information = 0;
        return STATUS_ACCESS_DENIED;
    }
    iosb->information = (uint64)done;

    /* End of file is a STATUS, not a zero-length success. A caller that loops
     * until Information is zero works either way; one that tests NT_SUCCESS
     * loops forever without this. */
    if (!writing && done == 0 && length > 0) {
        /* A pipe with no writer left is BROKEN on NT, not at its end -
         * ReadFile's ERROR_BROKEN_PIPE, which is what a loop draining a
         * child's output stops on. */
        uint64 eof = (f->obj->type != NULL &&
                      f->obj->type->klass == OBJ_PIPE)
                         ? STATUS_PIPE_BROKEN : STATUS_END_OF_FILE;

        iosb->status = eof;
        return eof;
    }
    iosb->status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

/* NtAllocateVirtualMemory(ProcessHandle, PBASE, ZeroBits, PSIZE, Type, Protect)
 *
 * BaseAddress and RegionSize are IN-OUT: a caller passes *BaseAddress = NULL
 * to mean "anywhere" and reads back where it landed, and RegionSize is
 * rounded up to a page and written back. Ignoring the write-back is the
 * classic way to make a caller free the wrong range later. */
/* --- virtual memory (ROADMAP 16(l)/14(d)) ------------------------------------
 *
 * The page-state model is kernel/mm/ntvm.c; these copy the in/out
 * arguments across. Only the calling process: a handle to another one does
 * not exist yet (16(m)). */
static int nt_self(uint64 process) {
    return process == NT_CURRENT_PROCESS || process == 0;
}

/* NtAllocateVirtualMemory(HANDLE, PVOID *Base, ULONG_PTR ZeroBits,
 *                         PSIZE_T Size, ULONG Type, ULONG Protect) */
static uint64 nt_allocate_virtual(uint64 process, uint64 base_ptr,
                                  uint64 size_ptr, uint64 type,
                                  uint64 protect) {
    uint64 base, size;
    uint32 st;

    if (!nt_self(process)) {
        return STATUS_INVALID_HANDLE;
    }
    if (!user_range_ok(base_ptr, 8) || !user_range_ok(size_ptr, 8)) {
        return STATUS_ACCESS_VIOLATION;
    }
    base = *(const uint64 *)base_ptr;
    size = *(const uint64 *)size_ptr;
    st = ntvm_allocate(proc_current()->space, &base, &size, (uint32)type,
                       (uint32)protect);
    if (st == STATUS_SUCCESS) {
        *(uint64 *)base_ptr = base;
        *(uint64 *)size_ptr = size;
    }
    return st;
}

/* NtFreeVirtualMemory(HANDLE, PVOID *Base, PSIZE_T Size, ULONG FreeType) */
static uint64 nt_free_virtual(uint64 process, uint64 base_ptr,
                              uint64 size_ptr, uint64 type) {
    uint64 base, size;
    uint32 st;

    if (!nt_self(process)) {
        return STATUS_INVALID_HANDLE;
    }
    if (!user_range_ok(base_ptr, 8) || !user_range_ok(size_ptr, 8)) {
        return STATUS_ACCESS_VIOLATION;
    }
    base = *(const uint64 *)base_ptr;
    size = *(const uint64 *)size_ptr;
    st = ntvm_free(proc_current()->space, &base, &size, (uint32)type);
    if (st == STATUS_SUCCESS) {
        *(uint64 *)base_ptr = base;
        *(uint64 *)size_ptr = size;
    }
    return st;
}

/* NtProtectVirtualMemory(HANDLE, PVOID *Base, PSIZE_T Size, ULONG New,
 *                        PULONG Old) */
static uint64 nt_protect_virtual(uint64 process, uint64 base_ptr,
                                 uint64 size_ptr, uint64 new_protect,
                                 uint64 old_ptr) {
    uint64 base, size;
    uint32 st, old = 0;

    if (!nt_self(process)) {
        return STATUS_INVALID_HANDLE;
    }
    if (!user_range_ok(base_ptr, 8) || !user_range_ok(size_ptr, 8) ||
        !user_range_ok(old_ptr, 4)) {
        return STATUS_ACCESS_VIOLATION;
    }
    base = *(const uint64 *)base_ptr;
    size = *(const uint64 *)size_ptr;
    st = ntvm_protect(proc_current()->space, &base, &size,
                      (uint32)new_protect, &old);
    if (st == STATUS_SUCCESS) {
        *(uint64 *)base_ptr = base;
        *(uint64 *)size_ptr = size;
        *(uint32 *)old_ptr = old;
    }
    return st;
}

/* NtQueryVirtualMemory(HANDLE, PVOID Base, MEMORY_INFORMATION_CLASS,
 *                      PVOID Buffer, SIZE_T Length, PSIZE_T ReturnLength)
 * Class 0, MemoryBasicInformation, only. */
static uint64 nt_query_virtual(uint64 process, uint64 addr, uint64 klass,
                               uint64 buf, uint64 len, uint64 retlen_ptr) {
    process_t *p = proc_current();
    nt_module_table_t mt;
    uint64 images[2 * NT_MAX_MODULES];
    ntvm_mbi_t mbi;
    int n = 0, k;
    uint32 st;

    if (!nt_self(process)) {
        return STATUS_INVALID_HANDLE;
    }
    if ((uint32)klass != 0) {
        return STATUS_INVALID_INFO_CLASS;
    }
    if (len < sizeof(mbi)) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!user_range_ok(buf, sizeof(mbi)) ||
        (retlen_ptr != 0 && !user_range_ok(retlen_ptr, 8))) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (nt_modules_read(p->space, &mt) == 0) {
        for (k = 0; k < (int)mt.count && k < NT_MAX_MODULES; k++) {
            images[2 * n]     = mt.mod[k].base;
            images[2 * n + 1] = mt.mod[k].size;
            n++;
        }
    }
    st = ntvm_query(p->space, addr, &mbi, images, n);
    if (st != STATUS_SUCCESS) {
        return st;
    }
    *(ntvm_mbi_t *)buf = mbi;
    if (retlen_ptr != 0) {
        *(uint64 *)retlen_ptr = sizeof(mbi);
    }
    return STATUS_SUCCESS;
}

/* --- failure tracing -----------------------------------------------------
 *
 * An NT call reports failure by returning a status. A caller that does not
 * check it - or checks it and silently gives up, which is what a hand-written
 * test image tends to do - produces a process that runs, prints nothing and
 * exits cleanly. That is indistinguishable from "the image did not load",
 * from "the wrong binary is staged", and from a real bug in here, and it has
 * now cost three round trips to tell apart.
 *
 * So a failing NT call says so. The number and the status are enough to
 * separate all three: a status at all means the image ran and reached the
 * kernel, STATUS_ACCESS_VIOLATION on argument one means the image and this
 * table disagree about where arguments live, and no line at all means
 * execution never got here.
 *
 * Success stays silent - tracing every write would drown the thing being
 * traced. */
#define NT_TRACE_FAILURES 1

/* --- the dispatcher objects ----------------------------------------------
 *
 * NtCreateEvent and its relatives. The objects are kernel/obj/dispatch.c;
 * this is the ring-3 door onto them, and it is deliberately thin - every one
 * of these is "parse the arguments, call one dispatch.c function, turn the
 * errno into an NTSTATUS".
 *
 * --- naming, and why it is the same code path as a file -------------------
 * An OBJECT_ATTRIBUTES carrying \BaseNamedObjects\Foo goes through the same
 * unicode_to_path and the same namespace as \??\CON does. That is the whole
 * argument ns.h makes for having one namespace: two unrelated processes agree
 * on a mutex by name using the machinery that already lets them agree on a
 * console by name. A NULL ObjectName means an unnamed object, which is legal
 * and common - CreateEvent(NULL, ...) - and gets a handle and no entry.
 */

/* Wrap an object in a handle for the calling process, consuming the caller's
 * reference. Shared by all four creates and by the open, because the sequence
 * - of_open, deref, handle_alloc, write the HANDLE out - has four steps and
 * three of them are undo-on-failure. */
static uint64 nt_handle_out(object_t *obj, uint64 handle_out, uint32 access) {
    process_t *p = proc_current();
    open_file_t *of;
    int index;

    of = of_open(obj, access);
    ob_deref(obj);                    /* the open instance holds it now */
    if (of == NULL) {
        return STATUS_TOO_MANY_OPENED_FILES;
    }
    index = handle_alloc(p->handles, of, 0);
    if (index < 0) {
        return STATUS_TOO_MANY_OPENED_FILES;
    }
    *(uint64 *)handle_out = NT_HANDLE_FROM_INDEX(index);
    return STATUS_SUCCESS;
}

/* A new handle to `obj` (which keeps its other references). */
uint64 nt_handle_for(object_t *obj, uint64 handle_out) {
    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    ob_ref(obj);
    return nt_handle_out(obj, handle_out, ACCESS_READ | ACCESS_WRITE);
}

/* The object a HANDLE names, or NULL. */
object_t *nt_object_of(uint64 handle) {
    process_t *p = proc_current();
    open_file_t *f;
    int index = nt_handle_index(handle);

    if (index < 0) {
        return NULL;
    }
    f = handle_get(p->handles, index);
    return (f != NULL) ? f->obj : NULL;
}

/* Pull the optional name out of an OBJECT_ATTRIBUTES.
 *
 * Returns 1 with *named set to whether there was one at all. A missing
 * ObjectName is not an error - it is how an unnamed object is asked for -
 * which is why "no name" and "a name that would not convert" have to be
 * distinguishable here rather than both being falsey. */
static int nt_attrs_name(uint64 attrs_ptr, char *path, uint64 cap, int *named,
                         uint32 *attributes) {
    nt_object_attributes_t oa;

    *named = 0;
    *attributes = 0;
    if (attrs_ptr == 0) {
        return 1;                     /* no attributes at all: unnamed */
    }
    if (!user_ptr_ok(attrs_ptr)) {
        return 0;
    }
    oa = *(const nt_object_attributes_t *)attrs_ptr;
    if (oa.length < sizeof(nt_object_attributes_t)) {
        return 0;
    }
    *attributes = oa.attributes;
    if (oa.root_directory != 0) {
        return 0;                     /* relative names: same gap as opens */
    }
    if (oa.object_name == 0) {
        return 1;                     /* attributes, but unnamed */
    }
    if (!unicode_to_path(oa.object_name, path, cap)) {
        return 0;
    }
    *named = 1;
    return 1;
}

/* Name a freshly created object, if the caller asked for one.
 *
 * On collision the object is DESTROYED rather than returned: NtCreateEvent
 * with a taken name fails, and handing back the existing object instead would
 * be NtOpenEvent's job. Silently opening someone else's mutex when you asked
 * to create one is how two programs end up believing they each own it. */
static uint64 nt_name_new(object_t *obj, const char *path, int named) {
    int rc;

    if (!named) {
        return STATUS_SUCCESS;
    }
    rc = ns_insert(path, obj);
    if (rc == -17) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    if (rc != 0) {
        return status_from_ns(rc);
    }
    /* NT's default for a name a program gives an object: it lasts as long as
     * a handle to the object does (object.h, OB_FLAG_TEMPORARY). */
    ob_make_temporary(obj);
    return STATUS_SUCCESS;
}

/* Hand the creator its handle to a just-created (and maybe just-named)
 * object. If that fails after the name went in, the name is taken back out:
 * nobody holds a handle, so nothing would ever close one and remove it, and
 * the namespace's reference would keep a dead object and its name forever. */
static uint64 nt_handle_out_new(object_t *obj, uint64 handle_out, int named) {
    uint64 st;

    if (named) {
        ob_ref(obj);                  /* survive nt_handle_out's deref */
    }
    st = nt_handle_out(obj, handle_out, ACCESS_READ | ACCESS_WRITE);
    if (named) {
        if (st != STATUS_SUCCESS) {
            (void)ns_remove_object(obj);
        }
        ob_deref(obj);
    }
    return st;
}

/* Open the object a name already names, if it is of class `klass`.
 * NtOpenEvent, NtOpenMutant and NtOpenSemaphore, and the open half of an
 * OBJ_OPENIF create.
 *
 * The TYPE is checked. Opening a semaphore as an event would otherwise hand
 * back a handle whose SetEvent silently means ReleaseSemaphore - which is the
 * failure the type registry exists to make impossible to write by accident. */
static uint64 nt_open_by_name(const char *path, obj_class_t klass,
                              uint64 handle_out) {
    ns_entry_t *e = ns_lookup_entry(path);

    if (e == NULL || e->kind != NS_OBJECT || e->object == NULL) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (e->object->type == NULL || e->object->type->klass != klass) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    ob_ref(e->object);
    return nt_handle_out(e->object, handle_out, ACCESS_READ | ACCESS_WRITE);
}

/* The open half of open-or-create. Returns STATUS_OBJECT_NAME_NOT_FOUND
 * when the caller should go on and create; anything else is the answer.
 *
 * Looking before creating rather than creating and catching the collision
 * matters for one type: a mutant created with InitialOwner is owned the
 * moment it exists, and tearing down an owned mutant is an abandonment.
 * There is no window between the look and the insert - NT syscalls run under
 * the big kernel lock and nothing between them blocks. When the name exists,
 * the create's other arguments (InitialOwner, InitialState, the counts) are
 * IGNORED, as on NT: CreateMutexW(..., TRUE, name) on an existing mutex does
 * not take it, which is why ERROR_ALREADY_EXISTS has to be checked. */
static uint64 nt_open_if(const char *path, int named, uint32 attributes,
                         obj_class_t klass, uint64 handle_out) {
    uint64 st;

    if (!named || !(attributes & OBJ_OPENIF)) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    st = nt_open_by_name(path, klass, handle_out);
    if (st == STATUS_SUCCESS) {
        return STATUS_OBJECT_NAME_EXISTS;
    }
    return st;
}

/* NtQuerySecurityObject(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR,
 *                       ULONG Length, PULONG LengthNeeded)
 *
 * The Windows view of exactly the ACL the POSIX side evaluates - not a
 * parallel permission system, and not a translation either. See
 * kernel/include/ntsec.h: ZFS stores an NFSv4 ACL, NFSv4's model came from
 * NT's, and the access mask is the same field in both.
 *
 * STATUS_BUFFER_TOO_SMALL with LengthNeeded filled in is the documented way
 * to ask how big the descriptor is, and callers rely on it: the standard
 * idiom is to call once with a zero length, allocate, and call again. That
 * makes the too-small path an ordinary success case rather than an error,
 * which is why LengthNeeded is written before the length is checked. */
static uint64 nt_query_security(uint64 handle, uint64 info, uint64 sd_ptr,
                                uint64 length, uint64 needed_ptr) {
    process_t   *p = proc_current();
    open_file_t *of;
    const fs_node_t *node;
    acl_t  a;
    uint8  sd[NTSEC_MAX_SD];
    int64  n;
    int    index = nt_handle_index(handle);

    if (index < 0) {
        return STATUS_INVALID_HANDLE;
    }
    of = handle_get(p->handles, index);
    if (of == NULL || of->obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    /* A SACL is an audit list, and this kernel has no auditing. Refusing is
     * better than returning an empty one: an empty SACL means "audit
     * nothing", which is a claim, whereas the honest answer is that the
     * question cannot be answered here. */
    if (info & SACL_SECURITY_INFORMATION) {
        return STATUS_NOT_IMPLEMENTED;
    }

    node = fileobj_node(of->obj);
    if (node == NULL) {
        /* A console, a pipe, a device. They have no ACL because they are not
         * on a filesystem, and inventing one would mean inventing an owner. */
        return STATUS_INVALID_HANDLE;
    }
    if (fs_getacl(node, (struct acl *)&a) != 0) {
        return STATUS_ACCESS_DENIED;
    }

    n = ntsec_build(&a, sd, sizeof(sd));
    if (n < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (user_ptr_ok(needed_ptr)) {
        *(uint32 *)needed_ptr = (uint32)n;
    }
    if (length < (uint64)n) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (!user_range_ok(sd_ptr, (uint64)n)) {
        return STATUS_ACCESS_VIOLATION;
    }
    {
        uint8 *dst = (uint8 *)sd_ptr;
        int64 i;

        for (i = 0; i < n; i++) {
            dst[i] = sd[i];
        }
    }
    return STATUS_SUCCESS;
}

/* NtCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, EVENT_TYPE,
 *               BOOLEAN InitialState) */
static uint64 nt_create_event(uint64 handle_out, uint64 attrs_ptr,
                              uint64 type, uint64 initial) {
    char path[NS_PATH_MAX];
    object_t *obj;
    uint64 st;
    uint32 attributes = 0;
    int named = 0;

    if (!user_ptr_ok(handle_out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!nt_attrs_name(attrs_ptr, path, sizeof(path), &named, &attributes)) {
        return STATUS_INVALID_PARAMETER;
    }
    st = nt_open_if(path, named, attributes, OBJ_EVENT, handle_out);
    if (st != STATUS_OBJECT_NAME_NOT_FOUND) {
        return st;
    }
    /* NotificationEvent is manual-reset. The argument is a 32-bit enum
     * arriving in a 64-bit register, so it is masked - see nt_stack_arg on
     * why the high half of a narrow argument is stale. */
    obj = event_create(((uint32)type == NotificationEvent),
                       ((uint32)initial != 0));
    if (obj == NULL) {
        return STATUS_NO_MEMORY;
    }
    st = nt_name_new(obj, path, named);
    if (st != STATUS_SUCCESS) {
        ob_deref(obj);
        return st;
    }
    return nt_handle_out_new(obj, handle_out, named);
}

/* NtOpenEvent / NtOpenMutant / NtOpenSemaphore
 * (PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES) */
static uint64 nt_open_named(uint64 handle_out, uint64 attrs_ptr,
                            obj_class_t klass) {
    char path[NS_PATH_MAX];
    uint32 attributes = 0;
    int named = 0;

    if (!user_ptr_ok(handle_out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!nt_attrs_name(attrs_ptr, path, sizeof(path), &named, &attributes) ||
        !named) {
        return STATUS_INVALID_PARAMETER;
    }
    return nt_open_by_name(path, klass, handle_out);
}

/* NtSetEvent / NtResetEvent (HANDLE, PLONG PreviousState) */
static uint64 nt_event_signal(uint64 handle, uint64 prev_ptr, int op) {
    object_t *obj = nt_object_of(handle);
    int64 prev = 0;
    int rc;

    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (prev_ptr != 0 && !user_ptr_ok(prev_ptr)) {
        return STATUS_ACCESS_VIOLATION;
    }
    rc = ob_signal(obj, op, 1, &prev);
    if (rc != 0) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (prev_ptr != 0) {
        *(int32 *)prev_ptr = (int32)prev;
    }
    return STATUS_SUCCESS;
}

/* NtWaitForSingleObject(HANDLE, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
 *
 * The timeout is NT's, and its sign is its meaning: NULL waits forever, a
 * NEGATIVE value is a relative interval in 100-nanosecond units, a positive
 * one is an absolute time since 1601. Only the first two are honoured -
 * absolute returns STATUS_NOT_IMPLEMENTED rather than being read as a
 * relative interval of astronomical length, which is what dropping the sign
 * would do. */
/* An NT timeout pointer to an absolute deadline in ticks, 0 for none. */
static uint64 nt_wait_deadline(uint64 timeout_ptr, uint64 *out) {
    uint64 deadline = 0;

    if (timeout_ptr != 0) {
        int64 t;

        if (!user_ptr_ok(timeout_ptr) || !user_ptr_ok(timeout_ptr + 7)) {
            return STATUS_ACCESS_VIOLATION;
        }
        t = *(const int64 *)timeout_ptr;
        if (t > 0) {
            return STATUS_NOT_IMPLEMENTED;
        }
        if (t == 0) {
            /* Zero is "do not wait" - a poll, spelled as a timeout. One tick
             * in the past rather than zero, because deadline 0 means "no
             * deadline" to ob_wait and would block forever: the two spellings
             * of nothing mean opposite things and this is the seam. */
            deadline = timer_ticks_now();
            if (deadline == 0) {
                deadline = 1;
            }
        } else {
            /* 100ns units to ticks, rounding UP so a sub-tick timeout waits
             * at least one tick rather than none. */
            uint64 hundred_ns = (uint64)(-t);
            uint64 ticks_wanted = (hundred_ns * (uint64)timer_hz())
                                / 10000000ULL;

            if (ticks_wanted == 0) {
                ticks_wanted = 1;
            }
            deadline = timer_ticks_now() + ticks_wanted;
        }
    }
    *out = deadline;
    return STATUS_SUCCESS;
}

/* The kernel half of every multi-object (and every alertable) wait: each
 * object is held by a reference for the length of the wait - a handle
 * another thread closes meanwhile must not let the object's pool slot be
 * reused under a waiter still testing it - and the dispatcher's answer
 * becomes an NTSTATUS. */
static uint64 nt_wait_objects(object_t **objs, int n, int wait_all,
                              int alertable, uint64 deadline) {
    int i, rc;

    for (i = 0; i < n; i++) {
        ob_ref(objs[i]);
    }
    rc = dispatch_wait_multiple(objs, n, wait_all, alertable, deadline);
    for (i = 0; i < n; i++) {
        ob_deref(objs[i]);
    }
    if (rc >= DISPATCH_WAIT_ABANDONED) {
        return STATUS_ABANDONED_WAIT_0 + (uint64)(rc - DISPATCH_WAIT_ABANDONED);
    }
    if (rc >= 0) {
        return STATUS_WAIT_0 + (uint64)rc;
    }
    switch (rc) {
    case -110:
        return STATUS_TIMEOUT;
    case -4:
        return STATUS_ALERTED;
    case DISPATCH_WAIT_APC:
        return STATUS_USER_APC;     /* delivered on the way out */
    case DISPATCH_WAIT_DUPLICATE:
        return STATUS_INVALID_PARAMETER_MIX;
    default:
        return STATUS_OBJECT_TYPE_MISMATCH;     /* something not waitable */
    }
}

static uint64 nt_wait_single(uint64 handle, uint64 alertable,
                             uint64 timeout_ptr) {
    object_t *obj = nt_object_of(handle);
    uint64 deadline = 0, st;
    int rc;

    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    st = nt_wait_deadline(timeout_ptr, &deadline);
    if (st != STATUS_SUCCESS) {
        return st;
    }
    if ((uint8)alertable != 0) {
        /* An alertable wait is a wait on one object that an APC may also
         * end - which is what the multi-object wait already knows how to
         * do. Its answers are this call's answers for a count of one. */
        return nt_wait_objects(&obj, 1, 0, 1, deadline);
    }

    rc = ob_wait(obj, deadline);
    if (rc == 0) {
        return STATUS_SUCCESS;
    }
    if (rc == 1) {
        /* Acquired, from an owner that died holding it. Success-shaped
         * (NT_SUCCESS is true) but not STATUS_SUCCESS, so a caller that
         * checks for exactly WAIT_OBJECT_0 notices. */
        return STATUS_ABANDONED_WAIT_0;
    }
    if (rc == -110) {
        /* A SUCCESS-shaped code. NT_SUCCESS(STATUS_TIMEOUT) is true, so a
         * caller that only tests NT_SUCCESS and then uses the object has a
         * bug - which is exactly why the two are different values rather
         * than one. */
        return STATUS_TIMEOUT;
    }
    if (rc == -4) {
        return STATUS_ALERTED;
    }
    if (rc == -22) {
        /* ob_wait's "this type has no wait slot" - a console, a file, a pipe.
         * The HANDLE is perfectly good; the object behind it is not something
         * that can be waited on, which is a type mismatch and not a bad
         * handle. Reporting INVALID_HANDLE here sends a caller looking at its
         * own bookkeeping for a handle it closed, which is the wrong place
         * entirely. */
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    return STATUS_INVALID_HANDLE;
}

/* NtWaitForMultipleObjects(ULONG Count, PHANDLE Handles, WAIT_TYPE,
 *                          BOOLEAN Alertable, PLARGE_INTEGER Timeout)
 *
 * WAIT_TYPE is NT's: WaitAll 0, WaitAny 1. The result is STATUS_WAIT_0 + i
 * (the index taken; 0 for wait-all), STATUS_ABANDONED_WAIT_0 + i, or
 * STATUS_TIMEOUT; STATUS_USER_APC when Alertable and an APC ran instead. */
static uint64 nt_wait_multiple(uint64 count, uint64 handles_ptr,
                               uint64 wait_type, uint64 alertable,
                               uint64 timeout_ptr) {
    object_t *objs[DISPATCH_WAIT_MAX];
    uint64 deadline = 0, st;
    int n = (int)(uint32)count, i;

    if (n < 1 || n > DISPATCH_WAIT_MAX) {
        return STATUS_INVALID_PARAMETER_1;
    }
    if ((uint32)wait_type > 1) {
        return STATUS_INVALID_PARAMETER_3;
    }
    if (!user_ptr_ok(handles_ptr) ||
        !user_ptr_ok(handles_ptr + (uint64)n * 8 - 1)) {
        return STATUS_ACCESS_VIOLATION;
    }
    st = nt_wait_deadline(timeout_ptr, &deadline);
    if (st != STATUS_SUCCESS) {
        return st;
    }
    for (i = 0; i < n; i++) {
        objs[i] = nt_object_of(((const uint64 *)handles_ptr)[i]);
        if (objs[i] == NULL) {
            return STATUS_INVALID_HANDLE;
        }
    }
    return nt_wait_objects(objs, n, (uint32)wait_type == 0,
                           (uint8)alertable != 0, deadline);
}

/* NtCreateSemaphore(PHANDLE, ACCESS_MASK, POA, LONG Initial, LONG Maximum) */
static uint64 nt_create_semaphore(uint64 handle_out, uint64 attrs_ptr,
                                  uint64 initial, uint64 maximum) {
    char path[NS_PATH_MAX];
    object_t *obj;
    uint64 st;
    uint32 attributes = 0;
    int named = 0;

    if (!user_ptr_ok(handle_out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!nt_attrs_name(attrs_ptr, path, sizeof(path), &named, &attributes)) {
        return STATUS_INVALID_PARAMETER;
    }
    st = nt_open_if(path, named, attributes, OBJ_SEMAPHORE, handle_out);
    if (st != STATUS_OBJECT_NAME_NOT_FOUND) {
        return st;
    }
    obj = semaphore_create((int32)initial, (int32)maximum);
    if (obj == NULL) {
        /* semaphore_create refuses an initial count above the maximum rather
         * than adjusting it, so this covers a bad argument as well as a full
         * pool. INVALID_PARAMETER is the more useful of the two answers and
         * the more likely cause. */
        return STATUS_INVALID_PARAMETER;
    }
    st = nt_name_new(obj, path, named);
    if (st != STATUS_SUCCESS) {
        ob_deref(obj);
        return st;
    }
    return nt_handle_out_new(obj, handle_out, named);
}

/* NtReleaseSemaphore(HANDLE, LONG ReleaseCount, PLONG PreviousCount) */
static uint64 nt_release_semaphore(uint64 handle, uint64 count,
                                   uint64 prev_ptr) {
    object_t *obj = nt_object_of(handle);
    int64 prev = 0;
    int rc;

    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (prev_ptr != 0 && !user_ptr_ok(prev_ptr)) {
        return STATUS_ACCESS_VIOLATION;
    }
    rc = ob_signal(obj, OB_SIG_SET, (int32)count, &prev);
    if (rc != 0) {
        /* Over-release lands here. NT returns STATUS_SEMAPHORE_LIMIT_EXCEEDED
         * for it; INVALID_PARAMETER is what this kernel can distinguish, and
         * the important half is that it is an ERROR at all - see
         * dispatch.c on why the count is not clamped. */
        return STATUS_INVALID_PARAMETER;
    }
    if (prev_ptr != 0) {
        *(int32 *)prev_ptr = (int32)prev;
    }
    return STATUS_SUCCESS;
}

/* NtCreateMutant(PHANDLE, ACCESS_MASK, POA, BOOLEAN InitialOwner) */
static uint64 nt_create_mutant(uint64 handle_out, uint64 attrs_ptr,
                               uint64 owned) {
    char path[NS_PATH_MAX];
    object_t *obj;
    uint64 st;
    uint32 attributes = 0;
    int named = 0;

    if (!user_ptr_ok(handle_out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!nt_attrs_name(attrs_ptr, path, sizeof(path), &named, &attributes)) {
        return STATUS_INVALID_PARAMETER;
    }
    st = nt_open_if(path, named, attributes, OBJ_MUTANT, handle_out);
    if (st != STATUS_OBJECT_NAME_NOT_FOUND) {
        return st;
    }
    obj = mutant_create((uint32)owned != 0);
    if (obj == NULL) {
        return STATUS_NO_MEMORY;
    }
    st = nt_name_new(obj, path, named);
    if (st != STATUS_SUCCESS) {
        ob_deref(obj);
        return st;
    }
    return nt_handle_out_new(obj, handle_out, named);
}

/* NtReleaseMutant(HANDLE, PLONG PreviousCount) */
static uint64 nt_release_mutant(uint64 handle, uint64 prev_ptr) {
    object_t *obj = nt_object_of(handle);
    int64 prev = 0;
    int rc;

    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (prev_ptr != 0 && !user_ptr_ok(prev_ptr)) {
        return STATUS_ACCESS_VIOLATION;
    }
    rc = ob_signal(obj, OB_SIG_SET, 1, &prev);
    if (rc == -1) {
        /* Released by somebody who does not hold it. NT has a status for
         * exactly this, and it is worth using rather than collapsing into
         * INVALID_PARAMETER: it is the error a lock bug produces. */
        return STATUS_MUTANT_NOT_OWNED;
    }
    if (rc != 0) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (prev_ptr != 0) {
        *(int32 *)prev_ptr = (int32)prev;
    }
    return STATUS_SUCCESS;
}

/* --- keyed events and I/O completion ports (ROADMAP 16(k)) ----------------
 *
 * The objects are kernel/obj/ntsync.c; this is the marshalling. Both kinds
 * are named, opened, open-or-created and given temporary names exactly as
 * the dispatcher objects above are. */

/* The create half shared by both: open-or-create, name, handle. `obj` is the
 * new object or NULL (no memory); it is destroyed unused when OBJ_OPENIF
 * found an existing one. */
static uint64 nt_create_simple(uint64 handle_out, uint64 attrs_ptr,
                               obj_class_t klass,
                               object_t *(*make)(uint32), uint32 arg) {
    char path[NS_PATH_MAX];
    object_t *obj;
    uint64 st;
    uint32 attributes = 0;
    int named = 0;

    if (!user_ptr_ok(handle_out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!nt_attrs_name(attrs_ptr, path, sizeof(path), &named, &attributes)) {
        return STATUS_INVALID_PARAMETER;
    }
    st = nt_open_if(path, named, attributes, klass, handle_out);
    if (st != STATUS_OBJECT_NAME_NOT_FOUND) {
        return st;
    }
    obj = make(arg);
    if (obj == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    st = nt_name_new(obj, path, named);
    if (st != STATUS_SUCCESS) {
        ob_deref(obj);
        return st;
    }
    return nt_handle_out_new(obj, handle_out, named);
}

/* --- NT timers -------------------------------------------------------------
 *
 * The object is a dispatcher (kernel/obj/dispatch.c, timer_create); this is
 * the marshalling. Times: a NEGATIVE DueTime is relative, in 100ns units; a
 * positive one is an absolute system time (100ns since 1601), converted to
 * the monotonic clock when the timer is set. */

static object_t *make_timer(uint32 type) {
    return timer_create(type == 0);      /* NotificationTimer is manual */
}

static uint64 nt_create_timer(uint64 handle_out, uint64 attrs_ptr,
                              uint64 type) {
    if (type > 1) {
        return STATUS_INVALID_PARAMETER_4;
    }
    return nt_create_simple(handle_out, attrs_ptr, OBJ_TIMER, make_timer,
                            (uint32)type);
}

static object_t *nt_timer_of(uint64 handle, uint64 *st) {
    object_t *obj = nt_object_of(handle);

    if (obj == NULL) {
        *st = STATUS_INVALID_HANDLE;
        return NULL;
    }
    if (obj->type == NULL || obj->type->klass != OBJ_TIMER) {
        *st = STATUS_OBJECT_TYPE_MISMATCH;
        return NULL;
    }
    return obj;
}

static uint64 nt_set_timer(uint64 handle, uint64 due_ptr, uint64 apc,
                           uint64 apc_ctx, uint64 period_ms,
                           uint64 prev_ptr) {
    uint64 st = STATUS_SUCCESS, now = timer_ns(), due;
    object_t *obj = nt_timer_of(handle, &st);
    int64 v;
    int was = 0;

    if (obj == NULL) {
        return st;
    }
    if (!user_range_ok(due_ptr, 8)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if ((int32)period_ms < 0) {
        return STATUS_INVALID_PARAMETER_6;
    }
    if (prev_ptr != 0 && !user_range_ok(prev_ptr, 1)) {
        return STATUS_ACCESS_VIOLATION;
    }
    v = *(const int64 *)due_ptr;
    if (v < 0) {
        due = now + (uint64)(-v) * 100ULL;
    } else {
        /* FILETIME -> ns since 1970 -> monotonic. Anything before boot is
         * already due. */
        uint64 epoch = 116444736000000000ULL;
        uint64 boot = timer_realtime_ns() - now;      /* wall clock at boot */
        uint64 wall = ((uint64)v > epoch) ? ((uint64)v - epoch) * 100ULL : 0;

        due = (wall > boot) ? wall - boot : 1;
    }
    if (timer_set(obj, due, (uint64)(uint32)period_ms * 1000000ULL, apc,
                  apc_ctx, &was) != 0) {
        return STATUS_INVALID_HANDLE;
    }
    if (prev_ptr != 0) {
        *(uint8 *)prev_ptr = (uint8)(was != 0);
    }
    return STATUS_SUCCESS;
}

static uint64 nt_cancel_timer(uint64 handle, uint64 state_ptr) {
    uint64 st = STATUS_SUCCESS;
    object_t *obj = nt_timer_of(handle, &st);
    int was = 0;

    if (obj == NULL) {
        return st;
    }
    if (state_ptr != 0 && !user_range_ok(state_ptr, 1)) {
        return STATUS_ACCESS_VIOLATION;
    }
    (void)timer_cancel(obj, &was);
    if (state_ptr != 0) {
        *(uint8 *)state_ptr = (uint8)(was != 0);
    }
    return STATUS_SUCCESS;
}

/* NtQueryTimer, TimerBasicInformation (class 0): { LARGE_INTEGER
 * RemainingTime (100ns); BOOLEAN TimerState } - 16 bytes with padding. */
static uint64 nt_query_timer(uint64 handle, uint64 klass, uint64 buf,
                             uint64 len, uint64 ret_ptr) {
    uint64 st = STATUS_SUCCESS, rem = 0;
    object_t *obj = nt_timer_of(handle, &st);
    int sig = 0;

    if (obj == NULL) {
        return st;
    }
    if (klass != 0) {
        return STATUS_INVALID_INFO_CLASS;
    }
    if (len != 16) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!user_range_ok(buf, 16) ||
        (ret_ptr != 0 && !user_range_ok(ret_ptr, 4))) {
        return STATUS_ACCESS_VIOLATION;
    }
    (void)timer_query(obj, &rem, &sig);
    *(int64 *)buf = (int64)(rem / 100ULL);
    *(uint64 *)(buf + 8) = (uint64)(sig != 0);
    if (ret_ptr != 0) {
        *(uint32 *)ret_ptr = 16;
    }
    return STATUS_SUCCESS;
}

static object_t *make_keyed_event(uint32 unused) {
    (void)unused;
    return keyed_event_create();
}

/* NtReleaseKeyedEvent / NtWaitForKeyedEvent */
static uint64 nt_keyed_event(uint64 handle, uint64 key, uint64 timeout_ptr,
                             int kind) {
    object_t *obj;
    uint64 deadline = 0, st;
    int rc;

    if (handle == 0) {
        obj = keyed_event_global();
    } else {
        obj = nt_object_of(handle);
        if (obj == NULL) {
            return STATUS_INVALID_HANDLE;
        }
    }
    if (obj == NULL || obj->type == NULL ||
        obj->type->klass != OBJ_KEYED_EVENT) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (key & 1) {
        return STATUS_INVALID_PARAMETER_1;
    }
    st = nt_wait_deadline(timeout_ptr, &deadline);
    if (st != STATUS_SUCCESS) {
        return st;
    }
    rc = keyed_event_rendezvous(obj, key, kind, deadline);
    if (rc == 0) {
        return STATUS_SUCCESS;
    }
    if (rc == -110) {
        return STATUS_TIMEOUT;
    }
    return rc == -4 ? STATUS_ALERTED : STATUS_OBJECT_TYPE_MISMATCH;
}

static object_t *make_io_completion(uint32 concurrency) {
    return io_completion_create(concurrency);
}

/* NtSetIoCompletion(HANDLE, KeyContext, ApcContext, IoStatus, Information) */
static uint64 nt_set_io_completion(uint64 handle, uint64 key, uint64 apc_ctx,
                                   uint64 status, uint64 information) {
    object_t *obj = nt_object_of(handle);
    io_packet_t pk;
    int rc;

    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    pk.key         = key;
    pk.apc_context = apc_ctx;
    pk.status      = (uint32)status;    /* an NTSTATUS: 32 bits */
    pk.information = information;
    rc = io_completion_post(obj, &pk);
    if (rc == -12) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return rc == 0 ? STATUS_SUCCESS : STATUS_OBJECT_TYPE_MISMATCH;
}

static uint64 nt_remove_status(int rc) {
    if (rc == -110) {
        return STATUS_TIMEOUT;
    }
    if (rc == IO_REMOVE_APC) {
        return STATUS_USER_APC;
    }
    if (rc == -4) {
        return STATUS_ALERTED;
    }
    return STATUS_OBJECT_TYPE_MISMATCH;
}

/* NtRemoveIoCompletion(HANDLE, PVOID *Key, PVOID *ApcContext,
 *                      PIO_STATUS_BLOCK, PLARGE_INTEGER Timeout) */
static uint64 nt_remove_io_completion(uint64 handle, uint64 key_ptr,
                                      uint64 apc_ptr, uint64 iosb_ptr,
                                      uint64 timeout_ptr) {
    object_t *obj = nt_object_of(handle);
    io_packet_t pk;
    uint64 deadline = 0, st;
    int rc;

    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (!user_range_ok(key_ptr, 8) || !user_range_ok(apc_ptr, 8) ||
        !user_range_ok(iosb_ptr, sizeof(nt_io_status_block_t))) {
        return STATUS_ACCESS_VIOLATION;
    }
    st = nt_wait_deadline(timeout_ptr, &deadline);
    if (st != STATUS_SUCCESS) {
        return st;
    }
    ob_ref(obj);                      /* a close meanwhile must not free it */
    rc = io_completion_remove(obj, &pk, 1, deadline, 0);
    ob_deref(obj);
    if (rc < 1) {
        return nt_remove_status(rc);
    }
    *(uint64 *)key_ptr = pk.key;
    *(uint64 *)apc_ptr = pk.apc_context;
    ((nt_io_status_block_t *)iosb_ptr)->status      = pk.status;
    ((nt_io_status_block_t *)iosb_ptr)->information = pk.information;
    return STATUS_SUCCESS;
}

/* NtRemoveIoCompletionEx(HANDLE, PFILE_IO_COMPLETION_INFORMATION, ULONG Count,
 *                        PULONG Removed, PLARGE_INTEGER Timeout, BOOLEAN) */
#define NT_IO_REMOVE_MAX 64
static uint64 nt_remove_io_completion_ex(uint64 handle, uint64 info_ptr,
                                         uint64 count, uint64 removed_ptr,
                                         uint64 timeout_ptr,
                                         uint64 alertable) {
    object_t *obj = nt_object_of(handle);
    io_packet_t pk[NT_IO_REMOVE_MAX];
    uint64 deadline = 0, st;
    uint32 n = (uint32)count;
    int rc, i;

    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (n == 0) {
        return STATUS_INVALID_PARAMETER_3;
    }
    /* More than this many per call is legal on NT; it is clipped rather
     * than refused, and the caller simply calls again for the rest. */
    if (n > NT_IO_REMOVE_MAX) {
        n = NT_IO_REMOVE_MAX;
    }
    if (!user_range_ok(info_ptr, (uint64)n * 32) ||
        !user_range_ok(removed_ptr, 4)) {
        return STATUS_ACCESS_VIOLATION;
    }
    st = nt_wait_deadline(timeout_ptr, &deadline);
    if (st != STATUS_SUCCESS) {
        return st;
    }
    ob_ref(obj);
    rc = io_completion_remove(obj, pk, (int)n, deadline,
                              (uint8)alertable != 0);
    ob_deref(obj);
    if (rc < 1) {
        *(uint32 *)removed_ptr = 0;
        return nt_remove_status(rc);
    }
    for (i = 0; i < rc; i++) {
        nt_file_io_completion_info_t *e =
            &((nt_file_io_completion_info_t *)info_ptr)[i];

        e->key_context = pk[i].key;
        e->apc_context = pk[i].apc_context;
        e->status      = pk[i].status;
        e->information = pk[i].information;
    }
    *(uint32 *)removed_ptr = (uint32)rc;
    return STATUS_SUCCESS;
}

/* NtQueryIoCompletion(HANDLE, Class, PVOID, ULONG Length, PULONG RetLen) */
static uint64 nt_query_io_completion(uint64 handle, uint64 klass,
                                     uint64 buf, uint64 length,
                                     uint64 retlen_ptr) {
    object_t *obj = nt_object_of(handle);
    int depth;

    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if ((uint32)klass != 0) {
        return STATUS_INVALID_INFO_CLASS;
    }
    if ((uint32)length < 4) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!user_range_ok(buf, 4) ||
        (retlen_ptr != 0 && !user_range_ok(retlen_ptr, 4))) {
        return STATUS_ACCESS_VIOLATION;
    }
    depth = io_completion_depth(obj);
    if (depth < 0) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *(int32 *)buf = depth;
    if (retlen_ptr != 0) {
        *(uint32 *)retlen_ptr = 4;
    }
    return STATUS_SUCCESS;
}

/* --- sections and views (ROADMAP 16(l)) --------------------------------------
 *
 * The object is kernel/mm/section.c, the views kernel/mm/ntvm.c. */

/* NtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
 *                 PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection,
 *                 ULONG AllocationAttributes, HANDLE FileHandle) */
static uint64 nt_create_section(uint64 handle_out, uint64 attrs_ptr,
                                uint64 size_ptr, uint64 protect,
                                uint64 attributes, uint64 file_handle) {
    process_t *p = proc_current();
    char path[NS_PATH_MAX];
    object_t *file = NULL, *obj;
    uint64 size = 0, st;
    uint32 oattr = 0, status = 0;
    int named = 0, writable = 0;

    if (!user_ptr_ok(handle_out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!nt_attrs_name(attrs_ptr, path, sizeof(path), &named, &oattr)) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((uint32)attributes & SEC_IMAGE) {
        return STATUS_NOT_SUPPORTED;    /* the PE loader maps images itself */
    }
    if (size_ptr != 0) {
        if (!user_range_ok(size_ptr, 8)) {
            return STATUS_ACCESS_VIOLATION;
        }
        size = *(const uint64 *)size_ptr;
    }
    if (file_handle != 0) {
        int index = nt_handle_index(file_handle);
        open_file_t *of = index >= 0 ? handle_get(p->handles, index) : NULL;

        if (of == NULL || of->obj == NULL) {
            return STATUS_INVALID_HANDLE;
        }
        file = of->obj;
        writable = (of->access & ACCESS_WRITE) != 0;
    } else if (size == 0) {
        return STATUS_INVALID_PARAMETER_4;   /* a pagefile section needs one */
    }
    st = nt_open_if(path, named, oattr, OBJ_SECTION, handle_out);
    if (st != STATUS_OBJECT_NAME_NOT_FOUND) {
        return st;
    }
    obj = section_create(size, (uint32)protect, file, writable, &status);
    if (obj == NULL) {
        return status;
    }
    st = nt_name_new(obj, path, named);
    if (st != STATUS_SUCCESS) {
        ob_deref(obj);
        return st;
    }
    return nt_handle_out_new(obj, handle_out, named);
}

/* NtMapViewOfSection(HANDLE Section, HANDLE Process, PVOID *Base,
 *                    ULONG_PTR ZeroBits, SIZE_T CommitSize,
 *                    PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize,
 *                    SECTION_INHERIT, ULONG AllocationType, ULONG Protect) */
static uint64 nt_map_view(uint64 section, uint64 process, uint64 base_ptr,
                          uint64 offset_ptr, uint64 size_ptr, uint64 type,
                          uint64 protect) {
    object_t *obj = nt_object_of(section);
    uint64 base, offset = 0, size;
    uint32 st;

    if (!nt_self(process)) {
        return STATUS_INVALID_HANDLE;
    }
    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (obj->type == NULL || obj->type->klass != OBJ_SECTION) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (!user_range_ok(base_ptr, 8) || !user_range_ok(size_ptr, 8) ||
        (offset_ptr != 0 && !user_range_ok(offset_ptr, 8))) {
        return STATUS_ACCESS_VIOLATION;
    }
    base = *(const uint64 *)base_ptr;
    size = *(const uint64 *)size_ptr;
    if (offset_ptr != 0) {
        offset = *(const uint64 *)offset_ptr;
    }
    st = ntvm_map_view(proc_current()->space, obj, &base, offset, &size,
                       (uint32)protect, (uint32)type);
    if (st == STATUS_SUCCESS) {
        *(uint64 *)base_ptr = base;
        *(uint64 *)size_ptr = size;
    }
    return st;
}

static uint64 nt_unmap_view(uint64 process, uint64 base) {
    if (!nt_self(process)) {
        return STATUS_INVALID_HANDLE;
    }
    return ntvm_unmap_view(proc_current()->space, base);
}

/* NtFlushVirtualMemory(HANDLE, PVOID *Base, PSIZE_T Size, PIO_STATUS_BLOCK) */
static uint64 nt_flush_virtual(uint64 process, uint64 base_ptr,
                               uint64 size_ptr, uint64 iosb_ptr) {
    uint64 base, size;
    uint32 st;

    if (!nt_self(process)) {
        return STATUS_INVALID_HANDLE;
    }
    if (!user_range_ok(base_ptr, 8) || !user_range_ok(size_ptr, 8) ||
        !user_range_ok(iosb_ptr, sizeof(nt_io_status_block_t))) {
        return STATUS_ACCESS_VIOLATION;
    }
    base = *(const uint64 *)base_ptr;
    size = *(const uint64 *)size_ptr;
    st = ntvm_flush(proc_current()->space, &base, &size);
    ((nt_io_status_block_t *)iosb_ptr)->status = st;
    ((nt_io_status_block_t *)iosb_ptr)->information = 0;
    if (st == STATUS_SUCCESS) {
        *(uint64 *)base_ptr = base;
        *(uint64 *)size_ptr = size;
    }
    return st;
}

/* --- handles across processes (ROADMAP 16(m)) ------------------------------
 *
 * A process's handle table is its leader's; another process's is reached
 * through a process handle. Holding that handle is the permission - access
 * masks are not checked anywhere yet. */

/* The live process a process handle (or NtCurrentProcess()) names. */
static process_t *nt_process_of(uint64 handle) {
    process_t *me = proc_current(), *leader;
    object_t *obj;
    int pid = 0;

    if (handle == NT_CURRENT_PROCESS) {
        leader = proc_find(me->tgid);
        return leader != NULL ? leader : me;
    }
    obj = nt_object_of(handle);
    if (obj == NULL || process_object_query(obj, &pid, NULL, NULL) != 0) {
        return NULL;                  /* not a process, or it has ended */
    }
    leader = proc_find(pid);
    if (leader == NULL || leader->tgid != pid || leader->state == PROC_ZOMBIE) {
        return NULL;
    }
    return leader;
}

/* What a pseudo-handle stands for, as an object a real handle can name:
 * GetCurrentProcess() and GetCurrentThread() duplicated are the classic way
 * to get a handle another thread or process can use. The main thread has
 * no Thread object until it is asked for one. */
static object_t *pseudo_object(uint64 h) {
    process_t *me = proc_current();

    if (h == NT_CURRENT_PROCESS) {
        process_t *leader = proc_find(me->tgid);

        return proc_process_object(leader != NULL ? leader : me);
    }
    if (h == NT_CURRENT_THREAD) {
        if (me->nt_thread_obj == NULL) {
            me->nt_thread_obj = thread_object_create(me->pid);
        }
        return me->nt_thread_obj;
    }
    return NULL;
}

#define DUPLICATE_CLOSE_SOURCE     0x1u
#define DUPLICATE_SAME_ACCESS      0x2u
#define DUPLICATE_SAME_ATTRIBUTES  0x4u
#define OBJ_INHERIT                0x2u

/* NtDuplicateObject(HANDLE SourceProcess, HANDLE SourceHandle,
 *                   HANDLE TargetProcess, PHANDLE TargetHandle,
 *                   ACCESS_MASK, ULONG HandleAttributes, ULONG Options)
 * The new handle shares the source's open instance (and file position), as
 * a duplicated handle does on NT. */
static uint64 nt_duplicate_object(uint64 src_proc, uint64 src_handle,
                                  uint64 dst_proc, uint64 dst_ptr,
                                  uint64 attributes, uint64 options) {
    process_t *src = nt_process_of(src_proc), *dst = NULL;
    open_file_t *of = NULL;
    uint32 flags = 0;
    int sindex = -1, dindex;

    if (src == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (dst_ptr != 0) {
        dst = nt_process_of(dst_proc);
        if (dst == NULL) {
            return STATUS_INVALID_HANDLE;
        }
        if (!user_range_ok(dst_ptr, 8)) {
            return STATUS_ACCESS_VIOLATION;
        }
    }
    if ((src_handle == NT_CURRENT_PROCESS || src_handle == NT_CURRENT_THREAD) &&
        src->tgid == proc_current()->tgid) {
        object_t *obj = pseudo_object(src_handle);

        if (obj == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        of = of_open(obj, ACCESS_READ | ACCESS_WRITE);
        if (of == NULL) {
            return STATUS_TOO_MANY_OPENED_FILES;
        }
    } else {
        sindex = nt_handle_index(src_handle);
        of = sindex >= 0 ? handle_get(src->handles, sindex) : NULL;
        if (of == NULL) {
            return STATUS_INVALID_HANDLE;
        }
        if ((uint32)options & DUPLICATE_SAME_ATTRIBUTES) {
            flags = (uint32)handle_flags(src->handles, sindex) &
                    HANDLE_INHERITABLE;
        }
        of_ref(of);
    }
    if ((uint32)attributes & OBJ_INHERIT) {
        flags |= HANDLE_INHERITABLE;
    }
    if (dst != NULL) {
        dindex = handle_alloc(dst->handles, of, flags);   /* consumes of */
        if (dindex < 0) {
            return STATUS_TOO_MANY_OPENED_FILES;
        }
        *(uint64 *)dst_ptr = NT_HANDLE_FROM_INDEX(dindex);
    } else {
        of_deref(of);
    }
    if (((uint32)options & DUPLICATE_CLOSE_SOURCE) && sindex >= 0) {
        (void)handle_close(src->handles, sindex);
    }
    return STATUS_SUCCESS;
}

/* NtOpenProcess(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PCLIENT_ID): by
 * process id. Any process - Linux ones too: one table serves both. */
static uint64 nt_open_process(uint64 handle_out, uint64 cid_ptr) {
    process_t *leader;
    object_t *obj;
    int pid;

    if (!user_range_ok(handle_out, 8) || !user_range_ok(cid_ptr, 16)) {
        return STATUS_ACCESS_VIOLATION;
    }
    pid = (int)((const uint64 *)cid_ptr)[0];
    leader = proc_find(pid);
    if (pid <= 0 || leader == NULL || leader->tgid != pid ||
        leader->is_kthread || leader->state == PROC_ZOMBIE) {
        return STATUS_INVALID_CID;
    }
    obj = proc_process_object(leader);
    if (obj == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return nt_handle_for(obj, handle_out);
}

/* NtSetInformationObject / NtQueryObject, class 4 only:
 * OBJECT_HANDLE_FLAG_INFORMATION { BOOLEAN Inherit; BOOLEAN ProtectFromClose; }
 * - SetHandleInformation / GetHandleInformation. ProtectFromClose is
 * remembered by nothing and reported FALSE. */
#define ObjectHandleFlagInformation 4

static uint64 nt_set_information_object(uint64 handle, uint64 klass,
                                        uint64 buf, uint64 len) {
    process_t *p = proc_current();
    int index = nt_handle_index(handle), flags;

    if ((uint32)klass != ObjectHandleFlagInformation) {
        return STATUS_INVALID_INFO_CLASS;
    }
    if ((uint32)len < 2) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!user_range_ok(buf, 2)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (index < 0 || handle_get(p->handles, index) == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    flags = handle_flags(p->handles, index);
    if (((const uint8 *)buf)[0]) {
        flags |= HANDLE_INHERITABLE;
    } else {
        flags &= ~HANDLE_INHERITABLE;
    }
    (void)handle_set_flags(p->handles, index, (uint32)flags);
    return STATUS_SUCCESS;
}

static uint64 nt_query_object(uint64 handle, uint64 klass, uint64 buf,
                              uint64 len, uint64 retlen_ptr) {
    process_t *p = proc_current();
    int index = nt_handle_index(handle);

    if ((uint32)klass != ObjectHandleFlagInformation) {
        return STATUS_INVALID_INFO_CLASS;
    }
    if ((uint32)len < 2) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!user_range_ok(buf, 2) ||
        (retlen_ptr != 0 && !user_range_ok(retlen_ptr, 4))) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (index < 0 || handle_get(p->handles, index) == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    ((uint8 *)buf)[0] = (handle_flags(p->handles, index) &
                         HANDLE_INHERITABLE) != 0;
    ((uint8 *)buf)[1] = 0;
    if (retlen_ptr != 0) {
        *(uint32 *)retlen_ptr = 2;
    }
    return STATUS_SUCCESS;
}

/* NtGenesisCreatePipe(PHANDLE Read, PHANDLE Write, ULONG Attributes,
 *                     ULONG BufferSize) - Genesis's own, like
 * NtGenesisLoadImage: an anonymous pipe as two handles, OBJ_INHERIT making
 * both inheritable. NT builds CreatePipe out of a uniquely named pipe in
 * \Device\NamedPipe; there is no named-pipe file system yet (16(q)), and
 * an anonymous pipe needs nothing a name would add. The same pipe object
 * Linux's pipe2 makes, so a Windows parent and a Linux child (or the
 * reverse) can be joined by one. */
static uint64 nt_genesis_create_pipe(uint64 rd_ptr, uint64 wr_ptr,
                                     uint64 attributes) {
    process_t *p = proc_current();
    object_t *rd = NULL, *wr = NULL;
    open_file_t *rf, *wf;
    uint32 flags = ((uint32)attributes & OBJ_INHERIT) ? HANDLE_INHERITABLE : 0;
    int ri, wi;

    if (!user_range_ok(rd_ptr, 8) || !user_range_ok(wr_ptr, 8)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (pipe_create(&rd, &wr) != 0) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    rf = of_open(rd, ACCESS_READ);
    wf = of_open(wr, ACCESS_WRITE);
    ob_deref(rd);
    ob_deref(wr);
    if (rf == NULL || wf == NULL) {
        of_deref(rf);
        of_deref(wf);
        return STATUS_TOO_MANY_OPENED_FILES;
    }
    ri = handle_alloc(p->handles, rf, flags);
    if (ri < 0) {
        of_deref(wf);
        return STATUS_TOO_MANY_OPENED_FILES;
    }
    wi = handle_alloc(p->handles, wf, flags);
    if (wi < 0) {
        (void)handle_close(p->handles, ri);
        return STATUS_TOO_MANY_OPENED_FILES;
    }
    *(uint64 *)rd_ptr = NT_HANDLE_FROM_INDEX(ri);
    *(uint64 *)wr_ptr = NT_HANDLE_FROM_INDEX(wi);
    return STATUS_SUCCESS;
}

/* --- the registry (ROADMAP 16(p)) ---------------------------------------------
 *
 * The Configuration Manager is kernel/obj/registry.c; this copies names and
 * buffers across. Key names come as OBJECT_ATTRIBUTES: absolute
 * ("\Registry\Machine\...") with no RootDirectory, or relative to the key a
 * RootDirectory handle names - which is how RegOpenKeyEx(HKLM, "Software")
 * reaches the kernel. */

#define REG_PATH_MAX 4096                /* characters in one name */

/* A UNICODE_STRING from user memory into a kmalloc'd copy. NULL pointer or
 * zero length is the empty name (the default value). 0 on a bad string. */
static int reg_ustr(uint64 us_ptr, uint16 **out, uint32 *chars) {
    nt_unicode_string_t us;
    uint32 n, i;

    *out = NULL;
    *chars = 0;
    if (us_ptr == 0) {
        return 1;
    }
    if (!user_range_ok(us_ptr, sizeof(us))) {
        return 0;
    }
    us = *(const nt_unicode_string_t *)us_ptr;
    n = us.length / 2;
    if (n == 0) {
        return 1;
    }
    if (n > REG_PATH_MAX || !user_range_ok(us.buffer, (uint64)n * 2)) {
        return 0;
    }
    *out = kmalloc((uint64)n * 2 + 2);
    if (*out == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        (*out)[i] = ((const uint16 *)us.buffer)[i];
    }
    *chars = n;
    return 1;
}

static struct reg_key *reg_key_of_handle(uint64 handle) {
    return registry_key_of(nt_object_of(handle));
}

/* Open or create the key OBJECT_ATTRIBUTES name; a handle on success. */
static uint64 reg_open(uint64 handle_out, uint64 attrs_ptr, int create,
                       uint64 class_ptr, uint64 disp_ptr) {
    nt_object_attributes_t oa;
    struct reg_key *base = NULL, *k = NULL;
    uint16 *name = NULL, *klass = NULL;
    uint32 chars = 0, class_chars = 0, st;
    object_t *obj;
    int created = 0;

    if (!user_range_ok(handle_out, 8) ||
        (disp_ptr != 0 && !user_range_ok(disp_ptr, 4))) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (attrs_ptr == 0 || !user_range_ok(attrs_ptr, sizeof(oa))) {
        return STATUS_INVALID_PARAMETER;
    }
    oa = *(const nt_object_attributes_t *)attrs_ptr;
    if (oa.root_directory != 0) {
        base = reg_key_of_handle(oa.root_directory);
        if (base == NULL) {
            return STATUS_INVALID_HANDLE;
        }
    }
    if (!reg_ustr(oa.object_name, &name, &chars) ||
        (create && !reg_ustr(class_ptr, &klass, &class_chars))) {
        if (name != NULL) {
            kfree(name);
        }
        return STATUS_INVALID_PARAMETER;
    }
    if (base == NULL && chars == 0) {
        st = STATUS_OBJECT_PATH_SYNTAX_BAD;
    } else {
        st = registry_lookup(base, name, chars, create, klass, class_chars,
                             &k, &created);
    }
    if (name != NULL) {
        kfree(name);
    }
    if (klass != NULL) {
        kfree(klass);
    }
    if (st != STATUS_SUCCESS) {
        return st;
    }
    obj = registry_key_object(k);               /* takes k's reference */
    if (obj == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (disp_ptr != 0) {
        *(uint32 *)disp_ptr = created ? REG_CREATED_NEW_KEY
                                      : REG_OPENED_EXISTING_KEY;
    }
    return nt_handle_out(obj, handle_out, ACCESS_READ | ACCESS_WRITE);
}

static uint64 reg_set_value(uint64 handle, uint64 name_ptr, uint64 type,
                            uint64 data, uint64 size) {
    struct reg_key *k = reg_key_of_handle(handle);
    uint16 *name;
    uint32 chars, st;

    if (k == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if ((uint32)size != 0 && !user_range_ok(data, (uint32)size)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!reg_ustr(name_ptr, &name, &chars)) {
        return STATUS_INVALID_PARAMETER;
    }
    st = registry_set_value(k, name, chars, (uint32)type, (const void *)data,
                            (uint32)size);
    if (name != NULL) {
        kfree(name);
    }
    return st;
}

/* The query/enumerate calls share a shape: a user buffer the kernel side
 * fills within its length, and the full length written back. */
static uint64 reg_out(uint32 st, uint32 result, uint64 retlen_ptr) {
    if (retlen_ptr != 0 &&
        (st == STATUS_SUCCESS || st == 0x80000005u || st == STATUS_BUFFER_TOO_SMALL)) {
        *(uint32 *)retlen_ptr = result;
    }
    return st;
}

static int reg_bufs_ok(uint64 buf, uint64 len, uint64 retlen_ptr) {
    return ((uint32)len == 0 || user_range_ok(buf, (uint32)len)) &&
           (retlen_ptr == 0 || user_range_ok(retlen_ptr, 4));
}

static uint64 reg_query_value(uint64 handle, uint64 name_ptr, uint64 klass,
                              uint64 buf, uint64 len, uint64 retlen_ptr) {
    struct reg_key *k = reg_key_of_handle(handle);
    uint16 *name;
    uint32 chars, st, result = 0;

    if (k == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (!reg_bufs_ok(buf, len, retlen_ptr)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!reg_ustr(name_ptr, &name, &chars)) {
        return STATUS_INVALID_PARAMETER;
    }
    st = registry_query_value(k, name, chars, (uint32)klass, (void *)buf,
                              (uint32)len, &result);
    if (name != NULL) {
        kfree(name);
    }
    return reg_out(st, result, retlen_ptr);
}

static uint64 reg_delete_value(uint64 handle, uint64 name_ptr) {
    struct reg_key *k = reg_key_of_handle(handle);
    uint16 *name;
    uint32 chars, st;

    if (k == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (!reg_ustr(name_ptr, &name, &chars)) {
        return STATUS_INVALID_PARAMETER;
    }
    st = registry_delete_value(k, name, chars);
    if (name != NULL) {
        kfree(name);
    }
    return st;
}

static uint64 reg_enumerate(uint64 handle, uint64 index, uint64 klass,
                            uint64 buf, uint64 len, uint64 retlen_ptr,
                            int values) {
    struct reg_key *k = reg_key_of_handle(handle);
    uint32 st, result = 0;

    if (k == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (!reg_bufs_ok(buf, len, retlen_ptr)) {
        return STATUS_ACCESS_VIOLATION;
    }
    st = values ? registry_enumerate_value(k, (uint32)index, (uint32)klass,
                                           (void *)buf, (uint32)len, &result)
                : registry_enumerate_key(k, (uint32)index, (uint32)klass,
                                         (void *)buf, (uint32)len, &result);
    return reg_out(st, result, retlen_ptr);
}

static uint64 reg_query_key(uint64 handle, uint64 klass, uint64 buf,
                            uint64 len, uint64 retlen_ptr) {
    struct reg_key *k = reg_key_of_handle(handle);
    uint32 st, result = 0;

    if (k == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (!reg_bufs_ok(buf, len, retlen_ptr)) {
        return STATUS_ACCESS_VIOLATION;
    }
    st = registry_query_key(k, (uint32)klass, (void *)buf, (uint32)len,
                            &result);
    return reg_out(st, result, retlen_ptr);
}

/* --- threads (ROADMAP item 14(a)) -------------------------------------------
 *
 * See NT_SYS_CREATE_THREAD in nt.h for the contract and the one register
 * difference from NT. The thread itself is an ordinary task of the process's
 * thread group - the same kind clone() makes, through the same constructor
 * (proc_spawn_thread) - plus the three things only an NT thread has: its own
 * TEB, so GS:0 and everything read through it is per thread; its own stack
 * in the fixed thread region; and a Thread object for its handle. */

static uint64 nt_create_thread(struct syscall_frame *frame) {
    process_t *p = proc_current();
    uint64 handle_out = frame->r10;
    uint64 process    = frame->r9;
    uint64 rsp        = syscall_get_user_rsp();
    uint64 start = 0, arg = 0, flags = 0, stack_size = 0;
    uint64 pages, stack_top, stack_lo, teb_va, i;
    struct syscall_frame f;
    object_t *tobj;
    process_t *t;
    int slot = -1;

    if (!nt_stack_arg(rsp, 5, &start) || !nt_stack_arg(rsp, 6, &arg) ||
        !nt_stack_arg(rsp, 7, &flags) || !nt_stack_arg(rsp, 9, &stack_size)) {
        return STATUS_ACCESS_VIOLATION;
    }
    flags = (uint32)flags;           /* a ULONG: see nt_stack_arg */
    if (!user_ptr_ok(handle_out) || !user_ptr_ok(handle_out + 7)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (process != NT_CURRENT_PROCESS) {
        return STATUS_NOT_IMPLEMENTED;   /* another process: no such thing */
    }
    if (start == 0 || !user_ptr_ok(start)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (p->nt_thread_start == 0 || p->space == NULL ||
        p->space == vmm_kernel_space()) {
        return STATUS_NOT_SUPPORTED;     /* no ntdll to start it in */
    }

    /* The stack: what was asked for, or the main thread's 64KB, rounded to
     * pages and capped one page short of the slot so the guard page below
     * it always stays unmapped. Committed up front - there is no demand
     * paging to grow it later, and a stack that faults at its second page
     * is worse than a smaller one that is honest about its size. */
    if (stack_size == 0) {
        stack_size = NT_THREAD_STACK_DEFAULT;
    }
    if (stack_size > NT_THREAD_STACK_STRIDE - 0x1000ULL) {
        stack_size = NT_THREAD_STACK_STRIDE - 0x1000ULL;
    }
    pages = (stack_size + 0xFFFULL) / 0x1000ULL;
    if (pages < 2) {
        pages = 2;
    }

    for (i = 0; i < NT_THREAD_SLOTS; i++) {
        if (vmm_get_phys_in(p->space, NT_THREAD_TEB_BASE +
                                      i * NT_THREAD_TEB_STRIDE) == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    teb_va    = NT_THREAD_TEB_BASE + (uint64)slot * NT_THREAD_TEB_STRIDE;
    stack_top = NT_THREAD_STACK_BASE +
                ((uint64)slot + 1) * NT_THREAD_STACK_STRIDE;
    stack_lo  = stack_top - pages * 0x1000ULL;

    if (nt_map_zeroed(p->space, stack_lo, pages) != 0) {
        for (i = 0; i < pages; i++) {
            vmm_unmap_page_in(p->space, stack_lo + i * 0x1000ULL,
                              VMM_FREE_FRAME);
        }
        return STATUS_NO_MEMORY;
    }

    /* How it starts. RtlUserThreadStart(StartRoutine in RDX, Argument in
     * R8) - see nt.h. Every other register is zeroed rather than inherited
     * from the creator's syscall, so nothing of the creating thread's
     * state leaks into the new one's first instruction. RSP is 40 below
     * the 16-aligned top: a zero return address (a start routine may not
     * return through it - RtlUserThreadStart never does) with the 32-byte
     * home area above it, which is exactly the stack a Win64 function
     * expects to be entered with. */
    f        = *frame;
    f.rax    = 0;
    f.rdi    = 0;
    f.rsi    = 0;
    f.rdx    = start;
    f.r10    = 0;
    f.r8     = arg;
    f.r9     = 0;
    f.rip    = p->nt_thread_start;
    f.rbx    = 0;
    f.rbp    = 0;
    f.r12    = 0;
    f.r13    = 0;
    f.r14    = 0;
    f.r15    = 0;

    t = proc_spawn_thread(p, &f, stack_top - 40, p->thread.fs_base, teb_va);
    if (t == NULL) {
        for (i = 0; i < pages; i++) {
            vmm_unmap_page_in(p->space, stack_lo + i * 0x1000ULL,
                              VMM_FREE_FRAME);
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* Recorded before anything else can fail, so every failure below is
     * undone by proc_free alone - it runs proc_nt_thread_exit, which unmaps
     * exactly what these name. */
    t->nt_stack_lo    = stack_lo;
    t->nt_stack_pages = pages;
    if (nt_thread_teb_init(p->space, teb_va, stack_top, pages * 0x1000ULL,
                           p->tgid, t->pid) != 0) {
        t->nt_teb_va = teb_va;           /* partially mapped: unmap it too */
        proc_free(t);
        return STATUS_NO_MEMORY;
    }
    t->nt_teb_va = teb_va;

    /* Its own copy of every module's implicit TLS, before it can run. */
    {
        uint64 tls_va = 0, tls_pages = 0;

        if (nt_thread_tls_init(p->space, slot, teb_va, &tls_va, &tls_pages) != 0) {
            t->nt_tls_va = tls_va;
            t->nt_tls_pages = tls_pages;
            proc_free(t);
            return STATUS_NO_MEMORY;
        }
        t->nt_tls_va = tls_va;
        t->nt_tls_pages = tls_pages;
    }

    /* Two references: the thread's own, dropped when it exits (after it
     * has signalled the object), and the handle's, dropped by NtClose. */
    tobj = thread_object_create(t->pid);
    if (tobj == NULL) {
        proc_free(t);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    t->nt_thread_obj = tobj;
    ob_ref(tobj);
    {
        uint64 st = nt_handle_out(tobj, handle_out,
                                  ACCESS_READ | ACCESS_WRITE);

        if (st != STATUS_SUCCESS) {
            proc_free(t);
            return st;
        }
    }

    if (flags & THREAD_CREATE_FLAGS_CREATE_SUSPENDED) {
        /* Complete - TEB, stack, TLS, handle - but on no run queue, so it
         * cannot run a single instruction until NtResumeThread. Parked,
         * which also keeps a signal from waking it (signal_send). */
        t->nt_suspend_count = 1;
        t->nt_parked        = 1;
        t->state            = PROC_BLOCKED;
        return STATUS_SUCCESS;
    }
    t->state = PROC_READY;
    sched_enqueue(t);
    return STATUS_SUCCESS;
}

/* Another thread of this process, from outside: see nt.h. */
static uint64 nt_terminate_other(object_t *obj, uint32 status) {
    process_t *p = proc_current();
    process_t *t;
    uint32 code = 0;
    int tid = 0;

    if (thread_object_query(obj, &tid, &code) != 0) {
        return STATUS_THREAD_IS_TERMINATING;   /* already exited */
    }
    t = proc_find(tid);
    if (t == NULL || t->tgid != p->tgid || t->is_kthread ||
        t->state == PROC_ZOMBIE) {
        return STATUS_THREAD_IS_TERMINATING;
    }
    /* The full 32-bit code first, as the self path does - proc_retire's
     * status is a POSIX one and keeps eight bits. Then the one retirement
     * path every outside kill uses (exit_group, a fatal signal): off its
     * wait queue, its Thread object signalled, its mutants abandoned, and
     * - if it is running in ring 3 on another CPU - that CPU kicked so it
     * switches away. Blocked, ready or suspended, it simply never runs
     * again. */
    thread_object_record_cpu(obj, t->cpu_ticks);
    thread_object_exited(obj, status);
    proc_retire(t, (int)(status & 0xFF));
    return STATUS_SUCCESS;
}

/* NtTerminateThread(HANDLE, NTSTATUS). */
static uint64 nt_terminate_thread(uint64 handle, uint64 status,
                                  struct syscall_frame *frame) {
    process_t *p = proc_current();

    if (handle != NT_CURRENT_THREAD && handle != 0) {
        object_t *obj = nt_object_of(handle);

        if (obj == NULL) {
            return STATUS_INVALID_HANDLE;
        }
        if (obj->type->klass != OBJ_THREAD) {
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
        if (obj != p->nt_thread_obj) {
            return nt_terminate_other(obj, (uint32)status);
        }
    }
    /* The full 32-bit code, first - GetExitCodeThread reports it, and the
     * exit path below only has room for a POSIX status's eight bits. */
    if (p->nt_thread_obj != NULL) {
        thread_object_record_cpu(p->nt_thread_obj, p->cpu_ticks);
        thread_object_exited(p->nt_thread_obj, (uint32)status);
    }
    /* One thread's exit, not the process's: the rest of the group runs on.
     * (When this is the last thread, the process ends with it - there is
     * nothing left to run in it - and its parent is told the ordinary way.) */
    return syscall_exit_process(status & 0xFF, frame);
}

/* NtQueryInformationThread(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG),
 * ThreadBasicInformation only. */
typedef struct {
    uint32 exit_status;               /* 0x00 STATUS_PENDING while running */
    uint32 pad0;
    uint64 teb_base;                  /* 0x08 */
    uint64 unique_process;            /* 0x10 CLIENT_ID */
    uint64 unique_thread;             /* 0x18 */
    uint64 affinity_mask;             /* 0x20 */
    int32  priority;                  /* 0x28 */
    int32  base_priority;             /* 0x2C */
} nt_thread_basic_info_t;

typedef char nt_tbi_layout[(sizeof(nt_thread_basic_info_t) == 0x30 &&
    __builtin_offsetof(nt_thread_basic_info_t, unique_thread) == 0x18)
    ? 1 : -1];

uint64 nt_query_thread_more(uint64 handle, uint64 cls, uint64 buf, uint64 len,
                            uint64 retlen);
uint64 nt_thread_affinity(uint64 handle);
int32  nt_thread_priority(uint64 handle);

static uint64 nt_query_thread(uint64 handle, uint64 info_class,
                              uint64 buf, uint64 len, uint64 retlen_ptr) {
    process_t *p = proc_current();
    nt_thread_basic_info_t tbi;

    if ((uint32)info_class != ThreadBasicInformation) {
        /* ThreadTimes, the priorities, group affinity and the ideal
         * processor - kernel/exec/nt_sys.c. */
        return nt_query_thread_more(handle, info_class, buf, len, retlen_ptr);
    }
    if ((uint32)len < sizeof(tbi)) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!user_ptr_ok(buf) || !user_ptr_ok(buf + sizeof(tbi) - 1)) {
        return STATUS_ACCESS_VIOLATION;
    }

    tbi.pad0           = 0;
    tbi.unique_process = (uint64)p->tgid;
    /* The thread's real mask and the priority its last SetThreadPriority
     * gave it - every CPU runs user code now. */
    tbi.affinity_mask  = nt_thread_affinity(handle);
    tbi.priority       = nt_thread_priority(handle);
    tbi.base_priority  = tbi.priority - 8;

    if (handle == NT_CURRENT_THREAD) {
        tbi.exit_status   = STATUS_PENDING;
        tbi.teb_base      = p->thread.gs_base;
        tbi.unique_thread = (uint64)p->pid;
    } else {
        object_t *obj = nt_object_of(handle);
        uint32 code = 0;
        int tid = 0, exited;
        process_t *t;

        if (obj == NULL) {
            return STATUS_INVALID_HANDLE;
        }
        exited = thread_object_query(obj, &tid, &code);
        if (exited < 0) {
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
        tbi.exit_status   = exited ? code : STATUS_PENDING;
        tbi.unique_thread = (uint64)tid;
        t = exited ? NULL : proc_find(tid);
        tbi.teb_base      = (t != NULL && t->tgid == p->tgid)
                                ? t->thread.gs_base : 0;
    }

    *(nt_thread_basic_info_t *)buf = tbi;
    if (retlen_ptr != 0 && user_ptr_ok(retlen_ptr) &&
        user_ptr_ok(retlen_ptr + 3)) {
        *(uint32 *)retlen_ptr = (uint32)sizeof(tbi);
    }
    return STATUS_SUCCESS;
}

static uint64 nt_trace(uint64 number, uint64 status) {
#if NT_TRACE_FAILURES
    /* ERROR severity only (top two bits set). Success and informational
     * codes are not failures - STATUS_TIMEOUT, STATUS_NO_YIELD_PERFORMED,
     * and the calls whose "status" is a value (the processor number,
     * SetThreadIdealProcessor's previous ideal) all have them clear, and
     * printing those buried the real failures under noise. */
    if ((status & 0xC0000000u) == 0xC0000000u &&
        (uint32)status != STATUS_INFO_LENGTH_MISMATCH &&
        (uint32)status != STATUS_BUFFER_TOO_SMALL) {
        print_string("  NT call ", 0x0E);
        print_hex((uint32)number, 0x0E);
        print_string(" -> status ", 0x0E);
        print_hex((uint32)status, 0x0E);
        print_string("\n", 0x0E);
    }
#else
    (void)number;
#endif
    return status;
}

static uint64 nt_syscall_dispatch_one(struct syscall_frame *frame);

uint64 nt_syscall_dispatch(struct syscall_frame *frame) {
    uint64 st = nt_syscall_dispatch_one(frame);

    /* An alertable wait ended by a queued APC: run it now, on the way out.
     * The CONTEXT handed to the dispatcher returns STATUS_USER_APC as this
     * call's result once the APC (and any queued behind it) has run. */
    if ((uint32)st == STATUS_USER_APC) {
        st = nt_apc_deliver(frame, st);
    }
    return st;
}

static uint64 nt_syscall_dispatch_one(struct syscall_frame *frame) {
    /* Win64: argument one in R10 (the stub copied it out of RCX, which
     * SYSCALL destroys), two in RDX, three in R8, four in R9, the rest on the
     * caller's stack from [rsp+0x28]. See nt.h. */
    switch (frame->rax) {
        case NT_SYS_DISPLAY_STRING:
            return nt_trace(frame->rax, nt_display_string(frame->r10));

        case NT_SYS_TERMINATE_PROCESS:
            return nt_terminate_process(frame->r10, frame->rdx, frame);

        case NT_SYS_OPEN_FILE:
            /* FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock.
             * ShareAccess and OpenOptions are arguments five and six, on the
             * stack, and are ignored: there is no sharing enforcement to
             * apply them to yet, and refusing them would refuse every
             * ordinary open. */
            return nt_trace(frame->rax,
                            nt_open_file(frame->r10, frame->rdx,
                                         frame->r8, frame->r9));

        case NT_SYS_CLOSE:
            return nt_trace(frame->rax, nt_close(frame->r10));

        case NT_SYS_READ_FILE:
            return nt_trace(frame->rax, nt_rw_file(frame->r10, 0));

        case NT_SYS_WRITE_FILE:
            return nt_trace(frame->rax, nt_rw_file(frame->r10, 1));

        case NT_SYS_ALLOCATE_VIRTUAL: {
            uint64 type = 0, protect = 0;

            /* AllocationType and Protect: arguments five and six. */
            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &type) ||
                !nt_stack_arg(syscall_get_user_rsp(), 6, &protect)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            nt_allocate_virtual(frame->r10, frame->rdx,
                                                frame->r9, type, protect));
        }

        case NT_SYS_FREE_VIRTUAL:
            return nt_trace(frame->rax,
                            nt_free_virtual(frame->r10, frame->rdx, frame->r8,
                                            frame->r9));

        case NT_SYS_PROTECT_VIRTUAL: {
            uint64 old = 0;

            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &old)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            nt_protect_virtual(frame->r10, frame->rdx,
                                               frame->r8, frame->r9, old));
        }

        case NT_SYS_CREATE_SECTION: {
            uint64 protect = 0, attributes = 0, file = 0;

            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &protect) ||
                !nt_stack_arg(syscall_get_user_rsp(), 6, &attributes) ||
                !nt_stack_arg(syscall_get_user_rsp(), 7, &file)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            nt_create_section(frame->r10, frame->r8, frame->r9,
                                              protect, attributes, file));
        }

        case NT_SYS_OPEN_SECTION:
            return nt_trace(frame->rax,
                            nt_open_named(frame->r10, frame->r8, OBJ_SECTION));

        case NT_SYS_MAP_VIEW: {
            uint64 offset = 0, size = 0, type = 0, protect = 0;

            /* Section, Process, *Base, ZeroBits in registers; CommitSize (5)
             * is not needed - every view is committed - and Inherit (8) has
             * no meaning without child processes. */
            if (!nt_stack_arg(syscall_get_user_rsp(), 6, &offset) ||
                !nt_stack_arg(syscall_get_user_rsp(), 7, &size) ||
                !nt_stack_arg(syscall_get_user_rsp(), 9, &type) ||
                !nt_stack_arg(syscall_get_user_rsp(), 10, &protect)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            nt_map_view(frame->r10, frame->rdx, frame->r8,
                                        offset, size, type, protect));
        }

        case NT_SYS_UNMAP_VIEW:
            return nt_trace(frame->rax,
                            nt_unmap_view(frame->r10, frame->rdx));

        case NT_SYS_CREATE_USER_PROCESS:
            return nt_trace(frame->rax, nt_create_user_process(frame));

        case NT_SYS_DUPLICATE_OBJECT: {
            uint64 attributes = 0, options = 0;

            /* DesiredAccess (5) is not checked: no access masks yet. */
            if (!nt_stack_arg(syscall_get_user_rsp(), 6, &attributes) ||
                !nt_stack_arg(syscall_get_user_rsp(), 7, &options)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            nt_duplicate_object(frame->r10, frame->rdx,
                                                frame->r8, frame->r9,
                                                attributes, options));
        }

        case NT_SYS_OPEN_PROCESS:
            return nt_trace(frame->rax,
                            nt_open_process(frame->r10, frame->r9));

        case NT_SYS_SET_INFORMATION_OBJECT:
            return nt_trace(frame->rax,
                            nt_set_information_object(frame->r10, frame->rdx,
                                                      frame->r8, frame->r9));

        case NT_SYS_QUERY_OBJECT: {
            uint64 retlen = 0;

            (void)nt_stack_arg(syscall_get_user_rsp(), 5, &retlen);
            return nt_trace(frame->rax,
                            nt_query_object(frame->r10, frame->rdx, frame->r8,
                                            frame->r9, retlen));
        }

        case NT_SYS_CREATE_KEY: {
            uint64 klass = 0, disp = 0;

            /* TitleIndex (4) and CreateOptions (6) are not used: every key
             * is volatile until hives are persistent. */
            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &klass) ||
                !nt_stack_arg(syscall_get_user_rsp(), 7, &disp)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            reg_open(frame->r10, frame->r8, 1, klass, disp));
        }

        case NT_SYS_OPEN_KEY:
            return nt_trace(frame->rax,
                            reg_open(frame->r10, frame->r8, 0, 0, 0));

        case NT_SYS_DELETE_KEY: {
            struct reg_key *k = reg_key_of_handle(frame->r10);

            return nt_trace(frame->rax, k == NULL ? STATUS_INVALID_HANDLE
                                                  : registry_delete_key(k));
        }

        case NT_SYS_SET_VALUE_KEY: {
            uint64 data = 0, size = 0;

            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &data) ||
                !nt_stack_arg(syscall_get_user_rsp(), 6, &size)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            reg_set_value(frame->r10, frame->rdx, frame->r9,
                                          data, size));
        }

        case NT_SYS_QUERY_VALUE_KEY: {
            uint64 len = 0, ret = 0;

            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &len)) {
                return STATUS_ACCESS_VIOLATION;
            }
            (void)nt_stack_arg(syscall_get_user_rsp(), 6, &ret);
            return nt_trace(frame->rax,
                            reg_query_value(frame->r10, frame->rdx, frame->r8,
                                            frame->r9, len, ret));
        }

        case NT_SYS_DELETE_VALUE_KEY:
            return nt_trace(frame->rax,
                            reg_delete_value(frame->r10, frame->rdx));

        case NT_SYS_ENUMERATE_KEY:
        case NT_SYS_ENUMERATE_VALUE_KEY: {
            uint64 len = 0, ret = 0;

            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &len)) {
                return STATUS_ACCESS_VIOLATION;
            }
            (void)nt_stack_arg(syscall_get_user_rsp(), 6, &ret);
            return nt_trace(frame->rax,
                            reg_enumerate(frame->r10, frame->rdx, frame->r8,
                                          frame->r9, len, ret,
                                          (frame->rax & NT_SYSCALL_NR_MASK) ==
                                              NT_SYS_ENUMERATE_VALUE_KEY));
        }

        case NT_SYS_QUERY_KEY: {
            uint64 ret = 0;

            (void)nt_stack_arg(syscall_get_user_rsp(), 5, &ret);
            return nt_trace(frame->rax,
                            reg_query_key(frame->r10, frame->rdx, frame->r8,
                                          frame->r9, ret));
        }

        case NT_SYS_CREATE_TIMER:
            return nt_trace(frame->rax,
                            nt_create_timer(frame->r10, frame->r8, frame->r9));

        case NT_SYS_OPEN_TIMER:
            return nt_trace(frame->rax,
                            nt_open_named(frame->r10, frame->r8, OBJ_TIMER));

        case NT_SYS_SET_TIMER: {
            /* ResumeTimer (5) is ignored: there is no power management to
             * wake from. Period (6) and PreviousState (7) are on the
             * stack. */
            uint64 period = 0, prev = 0;

            (void)nt_stack_arg(syscall_get_user_rsp(), 6, &period);
            (void)nt_stack_arg(syscall_get_user_rsp(), 7, &prev);
            return nt_trace(frame->rax,
                            nt_set_timer(frame->r10, frame->rdx, frame->r8,
                                         frame->r9, period, prev));
        }

        case NT_SYS_CANCEL_TIMER:
            return nt_trace(frame->rax,
                            nt_cancel_timer(frame->r10, frame->rdx));

        case NT_SYS_QUERY_TIMER: {
            uint64 ret = 0;

            (void)nt_stack_arg(syscall_get_user_rsp(), 5, &ret);
            return nt_trace(frame->rax,
                            nt_query_timer(frame->r10, frame->rdx, frame->r8,
                                           frame->r9, ret));
        }

        case NT_SYS_FLUSH_KEY:
            /* Nothing is persistent yet, so everything is already "on disk"
             * as much as it ever will be. */
            return nt_trace(frame->rax,
                            reg_key_of_handle(frame->r10) == NULL
                                ? STATUS_INVALID_HANDLE : STATUS_SUCCESS);

        case NT_SYS_GENESIS_CREATE_PIPE:
            return nt_trace(frame->rax,
                            nt_genesis_create_pipe(frame->r10, frame->rdx,
                                                   frame->r8));

        case NT_SYS_FLUSH_VIRTUAL:
            return nt_trace(frame->rax,
                            nt_flush_virtual(frame->r10, frame->rdx,
                                             frame->r8, frame->r9));

        case NT_SYS_QUERY_VIRTUAL: {
            uint64 buf = 0, len = 0, retlen = 0;

            /* Base, Class: two and three; Buffer, Length, ReturnLength: four
             * to six. */
            buf = frame->r9;
            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &len)) {
                return STATUS_ACCESS_VIOLATION;
            }
            (void)nt_stack_arg(syscall_get_user_rsp(), 6, &retlen);
            return nt_trace(frame->rax,
                            nt_query_virtual(frame->r10, frame->rdx,
                                             frame->r8, buf, len, retlen));
        }

        /* --- the dispatcher objects ---------------------------------------
         * Win64 puts arguments one to four in R10 (RCX before the stub), RDX,
         * R8 and R9; five onward are on the caller's stack. */
        case NT_SYS_CREATE_EVENT: {
            uint64 initial = 0;

            /* InitialState is argument five. A failure to fetch it is treated
             * as FALSE rather than as an error: it is a BOOLEAN and the
             * not-signalled state is the safe reading of a missing one. */
            (void)nt_stack_arg(syscall_get_user_rsp(), 5, &initial);
            return nt_trace(frame->rax,
                            nt_create_event(frame->r10, frame->r8,
                                            frame->r9, initial));
        }

        case NT_SYS_OPEN_EVENT:
            return nt_trace(frame->rax,
                            nt_open_named(frame->r10, frame->r8, OBJ_EVENT));

        case NT_SYS_OPEN_MUTANT:
            return nt_trace(frame->rax,
                            nt_open_named(frame->r10, frame->r8, OBJ_MUTANT));

        case NT_SYS_OPEN_SEMAPHORE:
            return nt_trace(frame->rax,
                            nt_open_named(frame->r10, frame->r8,
                                          OBJ_SEMAPHORE));

        case NT_SYS_CREATE_KEYED_EVENT:
            return nt_trace(frame->rax,
                            nt_create_simple(frame->r10, frame->r8,
                                             OBJ_KEYED_EVENT,
                                             make_keyed_event, 0));

        case NT_SYS_OPEN_KEYED_EVENT:
            return nt_trace(frame->rax,
                            nt_open_named(frame->r10, frame->r8,
                                          OBJ_KEYED_EVENT));

        case NT_SYS_RELEASE_KEYED_EVENT:
            return nt_trace(frame->rax,
                            nt_keyed_event(frame->r10, frame->rdx, frame->r9,
                                           KEYED_RELEASE));

        case NT_SYS_WAIT_KEYED_EVENT:
            return nt_trace(frame->rax,
                            nt_keyed_event(frame->r10, frame->rdx, frame->r9,
                                           KEYED_WAIT));

        case NT_SYS_CREATE_IO_COMPLETION:
            return nt_trace(frame->rax,
                            nt_create_simple(frame->r10, frame->r8,
                                             OBJ_IO_COMPLETION,
                                             make_io_completion,
                                             (uint32)frame->r9));

        case NT_SYS_OPEN_IO_COMPLETION:
            return nt_trace(frame->rax,
                            nt_open_named(frame->r10, frame->r8,
                                          OBJ_IO_COMPLETION));

        case NT_SYS_SET_IO_COMPLETION: {
            uint64 info = 0;

            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &info)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            nt_set_io_completion(frame->r10, frame->rdx,
                                                 frame->r8, frame->r9, info));
        }

        case NT_SYS_REMOVE_IO_COMPLETION: {
            uint64 timeout = 0;

            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &timeout)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            nt_remove_io_completion(frame->r10, frame->rdx,
                                                    frame->r8, frame->r9,
                                                    timeout));
        }

        case NT_SYS_REMOVE_IO_COMPLETION_EX: {
            uint64 timeout = 0, alertable = 0;

            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &timeout) ||
                !nt_stack_arg(syscall_get_user_rsp(), 6, &alertable)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            nt_remove_io_completion_ex(frame->r10, frame->rdx,
                                                       frame->r8, frame->r9,
                                                       timeout, alertable));
        }

        case NT_SYS_QUERY_IO_COMPLETION: {
            uint64 retlen = 0;

            (void)nt_stack_arg(syscall_get_user_rsp(), 5, &retlen);
            return nt_trace(frame->rax,
                            nt_query_io_completion(frame->r10, frame->rdx,
                                                   frame->r8, frame->r9,
                                                   retlen));
        }

        case NT_SYS_SET_EVENT:
            return nt_trace(frame->rax,
                            nt_event_signal(frame->r10, frame->rdx,
                                            OB_SIG_SET));

        case NT_SYS_RESET_EVENT:
            return nt_trace(frame->rax,
                            nt_event_signal(frame->r10, frame->rdx,
                                            OB_SIG_RESET));

        case NT_SYS_WAIT_SINGLE:
            return nt_trace(frame->rax,
                            nt_wait_single(frame->r10, frame->rdx, frame->r8));

        case NT_SYS_CONTINUE:
            /* Returns only on failure - success leaves by iretq. */
            return nt_trace(frame->rax,
                            nt_continue(frame, frame->r10, frame->rdx));

        case NT_SYS_TEST_ALERT:
            return nt_test_alert(frame);

        case NT_SYS_GENESIS_LOAD_IMAGE:
            return nt_trace(frame->rax,
                            nt_genesis_load_image(frame->r10, frame->rdx));

        case NT_SYS_RAISE_EXCEPTION:
            return nt_trace(frame->rax,
                            nt_raise_exception(frame, frame->r10, frame->rdx,
                                               frame->r8));

        case NT_SYS_WAIT_MULTIPLE: {
            uint64 timeout = 0;

            if (!nt_stack_arg(syscall_get_user_rsp(), 5, &timeout)) {
                return STATUS_ACCESS_VIOLATION;
            }
            return nt_trace(frame->rax,
                            nt_wait_multiple(frame->r10, frame->rdx, frame->r8,
                                             frame->r9, timeout));
        }

        case NT_SYS_CREATE_SEMAPHORE: {
            uint64 maximum = 0;

            (void)nt_stack_arg(syscall_get_user_rsp(), 5, &maximum);
            return nt_trace(frame->rax,
                            nt_create_semaphore(frame->r10, frame->r8,
                                                frame->r9, maximum));
        }

        case NT_SYS_RELEASE_SEMAPHORE:
            return nt_trace(frame->rax,
                            nt_release_semaphore(frame->r10, frame->rdx,
                                                 frame->r8));

        case NT_SYS_CREATE_MUTANT:
            return nt_trace(frame->rax,
                            nt_create_mutant(frame->r10, frame->r8,
                                             frame->r9));

        case NT_SYS_RELEASE_MUTANT:
            return nt_trace(frame->rax,
                            nt_release_mutant(frame->r10, frame->rdx));

        case NT_SYS_QUERY_SECURITY: {
            uint64 needed = 0;

            /* LengthNeeded is argument five, on the stack. Unlike
             * NtCreateEvent's InitialState, a failure to fetch it is NOT
             * treated as a default: it is an out-parameter, and writing to a
             * pointer this code could not read is how a wild store happens.
             * Zero means "do not report the length", which nt_query_security
             * handles through user_ptr_ok. */
            (void)nt_stack_arg(syscall_get_user_rsp(), 5, &needed);
            return nt_trace(frame->rax,
                            nt_query_security(frame->r10, frame->rdx,
                                              frame->r8, frame->r9, needed));
        }

        case NT_SYS_CREATE_THREAD:
            return nt_trace(frame->rax, nt_create_thread(frame));

        case NT_SYS_TERMINATE_THREAD:
            /* Not traced: on success it does not return to this caller. */
            return nt_terminate_thread(frame->r10, frame->rdx, frame);

        case NT_SYS_QUERY_THREAD: {
            uint64 retlen = 0;

            (void)nt_stack_arg(syscall_get_user_rsp(), 5, &retlen);
            return nt_trace(frame->rax,
                            nt_query_thread(frame->r10, frame->rdx,
                                            frame->r8, frame->r9, retlen));
        }

        default: {
            /* The machine / process / scheduling calls live in nt_sys.c. */
            int handled = 0;
            uint64 st = nt_sys_dispatch(frame, &handled);

            if (handled) {
                return nt_trace(frame->rax, st);
            }
        }
            /* Printing the number is the same loop that got the Linux side to
             * a shell: run it, read the number, implement it, repeat. The
             * difference is that here the number is one we chose, so an
             * unexpected one means our own ntdll and this table have drifted -
             * worth saying out loud rather than failing silently. */
            print_string("NT syscall ", 0x0C);
            print_hex((uint32)frame->rax, 0x0C);
            print_string(" - not implemented\n", 0x0C);
            return (uint64)STATUS_NOT_IMPLEMENTED;
    }
}

int nt_is_sigreturn(uint64 nr) {
    (void)nr;
    return 0;
}
