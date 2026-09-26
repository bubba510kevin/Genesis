#include "device.h"
#include "timer.h"
#include "fileobj.h"
#include "ns.h"
#include "dispatch.h"
#include "nt.h"
#include "ntsec.h"
#include "acl.h"
#include "fileobj.h"
#include "object.h"
#include "process.h"
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

/* NtTerminateProcess(HANDLE, NTSTATUS).
 *
 * Only the current process, named by the pseudo-handle -1, which is what a
 * process exiting itself passes. Terminating another one needs a handle to
 * it, and there is no handle table entry that names a process yet. */
static uint64 nt_terminate_process(uint64 handle, uint64 status,
                                   struct syscall_frame *frame) {
    if (handle != NT_CURRENT_PROCESS && handle != 0) {
        return STATUS_INVALID_HANDLE;
    }
    /* Straight into the shared exit path. Ending a process is mechanism, not
     * ABI - the reaping, the SIGCHLD to a Linux parent, the vfork resume are
     * all the same regardless of which number asked. */
    return syscall_exit_process(status & 0xFF, frame);
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
static int nt_stack_arg(uint64 rsp, int n, uint64 *out) {
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
        iosb->status = STATUS_END_OF_FILE;
        return STATUS_END_OF_FILE;
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
static uint64 nt_allocate_virtual(uint64 process, uint64 base_ptr,
                                  uint64 size_ptr) {
    uint64 want, length, got;

    if (process != NT_CURRENT_PROCESS && process != 0) {
        /* Allocating in another process needs a handle that names one, and
         * nothing in the handle table does yet. */
        return STATUS_INVALID_HANDLE;
    }
    if (!user_ptr_ok(base_ptr) || !user_ptr_ok(size_ptr)) {
        return STATUS_ACCESS_VIOLATION;
    }
    want   = *(const uint64 *)base_ptr;
    length = *(const uint64 *)size_ptr;
    if (length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    got = syscall_map_anonymous(want, length, want != 0);
    if ((int64)got < 0) {
        return STATUS_NO_MEMORY;
    }

    *(uint64 *)base_ptr = got;
    *(uint64 *)size_ptr = (length + 0xFFFULL) & ~0xFFFULL;
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

/* The object a HANDLE names, or NULL. */
static object_t *nt_object_of(uint64 handle) {
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
static int nt_attrs_name(uint64 attrs_ptr, char *path, uint64 cap, int *named) {
    nt_object_attributes_t oa;

    *named = 0;
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
    return STATUS_SUCCESS;
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
    int named = 0;

    if (!user_ptr_ok(handle_out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!nt_attrs_name(attrs_ptr, path, sizeof(path), &named)) {
        return STATUS_INVALID_PARAMETER;
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
    return nt_handle_out(obj, handle_out, ACCESS_READ | ACCESS_WRITE);
}

/* NtOpenEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES) */
static uint64 nt_open_event(uint64 handle_out, uint64 attrs_ptr) {
    char path[NS_PATH_MAX];
    ns_entry_t *e;
    int named = 0;

    if (!user_ptr_ok(handle_out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!nt_attrs_name(attrs_ptr, path, sizeof(path), &named) || !named) {
        return STATUS_INVALID_PARAMETER;
    }
    e = ns_lookup_entry(path);
    if (e == NULL || e->kind != NS_OBJECT || e->object == NULL) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    /* The TYPE is checked. Opening a semaphore as an event would otherwise
     * hand back a handle whose SetEvent silently means ReleaseSemaphore -
     * which is the failure the type registry exists to make impossible to
     * write by accident. */
    if (e->object->type == NULL || e->object->type->klass != OBJ_EVENT) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    ob_ref(e->object);
    return nt_handle_out(e->object, handle_out, ACCESS_READ | ACCESS_WRITE);
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
static uint64 nt_wait_single(uint64 handle, uint64 alertable,
                             uint64 timeout_ptr) {
    object_t *obj = nt_object_of(handle);
    uint64 deadline = 0;
    int rc;

    (void)alertable;                  /* no APCs, so nothing to be alerted by */
    if (obj == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (timeout_ptr != 0) {
        int64 t;

        if (!user_ptr_ok(timeout_ptr)) {
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

    rc = ob_wait(obj, deadline);
    if (rc == 0) {
        return STATUS_SUCCESS;
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

/* NtCreateSemaphore(PHANDLE, ACCESS_MASK, POA, LONG Initial, LONG Maximum) */
static uint64 nt_create_semaphore(uint64 handle_out, uint64 attrs_ptr,
                                  uint64 initial, uint64 maximum) {
    char path[NS_PATH_MAX];
    object_t *obj;
    uint64 st;
    int named = 0;

    if (!user_ptr_ok(handle_out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!nt_attrs_name(attrs_ptr, path, sizeof(path), &named)) {
        return STATUS_INVALID_PARAMETER;
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
    return nt_handle_out(obj, handle_out, ACCESS_READ | ACCESS_WRITE);
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
    int named = 0;

    if (!user_ptr_ok(handle_out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!nt_attrs_name(attrs_ptr, path, sizeof(path), &named)) {
        return STATUS_INVALID_PARAMETER;
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
    return nt_handle_out(obj, handle_out, ACCESS_READ | ACCESS_WRITE);
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

static uint64 nt_trace(uint64 number, uint64 status) {
#if NT_TRACE_FAILURES
    if (status != STATUS_SUCCESS) {
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

uint64 nt_syscall_dispatch(struct syscall_frame *frame) {
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

        case NT_SYS_ALLOCATE_VIRTUAL:
            /* AllocationType and Protect are arguments five and six, on the
             * stack, and are not read - see the note on MEM_COMMIT in nt.h. */
            return nt_trace(frame->rax,
                            nt_allocate_virtual(frame->r10, frame->rdx,
                                                frame->r9));

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
                            nt_open_event(frame->r10, frame->r8));

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

        default:
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
