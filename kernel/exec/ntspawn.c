#include "dispatch.h"
#include "fs.h"
#include "kheap.h"
#include "nt.h"
#include "ntspawn.h"
#include "object.h"
#include "paging.h"
#include "pe.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "syscall.h"
#include "teb.h"
#include "typesk.h"

/* NtCreateUserProcess (ROADMAP 16(m)): a Windows process starting another.
 *
 * The real call's shape, eleven arguments:
 *
 *   NtCreateUserProcess(PHANDLE ProcessHandle, PHANDLE ThreadHandle,
 *       ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess,
 *       POBJECT_ATTRIBUTES ProcessObjectAttributes,
 *       POBJECT_ATTRIBUTES ThreadObjectAttributes,
 *       ULONG ProcessFlags, ULONG ThreadFlags,
 *       PRTL_USER_PROCESS_PARAMETERS ProcessParameters,
 *       PPS_CREATE_INFO CreateInfo, PPS_ATTRIBUTE_LIST AttributeList)
 *
 * The parent describes the child the way NT's kernelbase does: an
 * RTL_USER_PROCESS_PARAMETERS in its own memory (image path, command line,
 * current directory, environment, standard handles), and an attribute list
 * whose PS_ATTRIBUTE_IMAGE_NAME names the image as an NT path. Out come a
 * PROCESS handle (waitable, signalled with the 32-bit exit code when the
 * whole process has ended) and a THREAD handle for its first thread; a
 * PS_ATTRIBUTE_CLIENT_ID in the list receives the new pid and tid.
 *
 * How: the image is built into a fresh address space by pe_exec_build -
 * exactly what execve does for the image replacing the caller - and the
 * process is then started the way fork starts a child, from a bootstrap
 * frame on its own kernel stack that returns straight to the image's entry.
 * The parent's address space is never touched.
 *
 * Handles: with PROCESS_CREATE_FLAGS_INHERIT_HANDLES the child gets every
 * INHERITABLE handle of the parent at the same value, as on NT. The three
 * standard handles named in the parameters are given to the child at their
 * values either way - a console program's child shares its console - so
 * that GetStdHandle in the child finds them.
 *
 * Paths: drive C: is the root volume (there is one); other drives are
 * STATUS_OBJECT_PATH_NOT_FOUND until there is a second volume to name.
 *
 * A process created here is AUTOREAP: its parent is a Windows program that
 * will never call wait4, so the slot goes back as soon as the process has
 * ended. Its exit code lives on in the process object for as long as a
 * handle to it does. Not supported yet: another process's image section,
 * job objects, the thread attribute list beyond CLIENT_ID, a security
 * descriptor on either object. */

#define PROCESS_CREATE_FLAGS_INHERIT_HANDLES  0x00000004u
#define PS_ATTRIBUTE_CLIENT_ID                0x00010003u
#define PS_ATTRIBUTE_IMAGE_NAME               0x00020005u
#define RTL_USER_PROC_PARAMS_NORMALIZED       0x00000001u

#define SPAWN_MAX_CMDLINE  32767           /* characters, as on Windows */
#define SPAWN_MAX_ENV      32768

extern int nt_stack_arg(uint64 rsp, int n, uint64 *out);
uint64 nt_handle_for(object_t *obj, uint64 handle_out);

static int u_ok(uint64 p, uint64 len) {
    return user_range_ok(p, len);
}

/* A DOS or NT path in UTF-16 to a POSIX one: [\??\]C:\x\y -> /x/y. */
static int dos_to_posix(uint64 wbuf, uint64 chars, char *out, uint64 cap) {
    const uint16 *w = (const uint16 *)wbuf;
    uint64 i = 0, o = 0;

    if (chars >= 4 && w[0] == '\\' && (w[1] == '?' || w[1] == '\\') &&
        w[2] == '?' && w[3] == '\\') {
        i = 4;                                   /* \??\ or \\?\ */
    }
    if (chars < i + 2 || w[i + 1] != ':') {
        return 0;
    }
    if (w[i] != 'C' && w[i] != 'c') {
        return 0;                                /* one volume: C: */
    }
    i += 2;
    if (i >= chars) {
        out[o++] = '/';                          /* "C:" alone: the root */
    }
    for (; i < chars; i++) {
        uint16 c = w[i];

        if (o + 2 >= cap || c == 0) {
            return c == 0 && o > 0;
        }
        out[o++] = (c == '\\') ? '/' : (c < 0x80 ? (char)c : '?');
    }
    /* A trailing slash on a directory ("C:\dir\") is dropped, except for
     * the root itself. */
    while (o > 1 && out[o - 1] == '/') {
        o--;
    }
    out[o] = '\0';
    return out[0] == '/';
}

/* A UNICODE_STRING inside the parameters: its buffer is an address, or an
 * offset from the block when the block is not normalized. */
static uint64 ustr_buffer(const nt_unicode_string_t *u, uint64 params,
                          uint32 flags) {
    if (u->buffer == 0) {
        return 0;
    }
    return (flags & RTL_USER_PROC_PARAMS_NORMALIZED) ? u->buffer
                                                     : params + u->buffer;
}

/* The bootstrap: the image's entry, or ntdll's RtlUserThreadStart with the
 * entry in RDX when TLS callbacks must run first - execve's exit, built as
 * a frame for a thread that has not run yet. */
static void start_frame(struct syscall_frame *f, const pe_exec_t *pex) {
    uint8 *b = (uint8 *)f;
    uint64 i;

    for (i = 0; i < sizeof(*f); i++) {
        b[i] = 0;
    }
    f->rip = pex->tls_entry_via_ntdll != 0 ? pex->tls_entry_via_ntdll
                                           : pex->entry;
    if (pex->tls_entry_via_ntdll != 0) {
        f->rdx = pex->entry;
    }
    f->rflags = 0x202;
}

/* The user stack, mapped and zeroed in a space that is not loaded. The
 * value returned is a Win64 function-entry RSP: 8 mod 16, with a zero
 * return address and the home area above it - see execve. */
static uint64 map_stack(address_space_t *as) {
    uint64 base = (USER_STACK_TOP - USER_STACK_SIZE) & ~0xFFFULL, page, b;

    for (page = base; page < USER_STACK_TOP; page += PMM_PAGE_SIZE) {
        phys_addr_t f = vmm_alloc_page_in(as, page, PAGE_PRESENT | PAGE_RW |
                                                    PAGE_USER | PAGE_NX);
        uint8 *z;

        if (f == 0) {
            return 0;
        }
        z = (uint8 *)phys_to_virt(f & ~0xFFFULL);
        for (b = 0; b < PMM_PAGE_SIZE; b++) {
            z[b] = 0;
        }
    }
    return (USER_STACK_TOP & ~0xFULL) - 40;
}

/* Give the child the parent's handle `h` at the same value, unless it has
 * one there already (inherited). */
static void give_handle(process_t *parent, process_t *child, uint64 h) {
    uint64 raw = h >> 2;
    open_file_t *of;
    int index;

    if (h == 0 || (h & 3) != 0 || raw == 0 || raw > MAX_HANDLES) {
        return;
    }
    index = (int)(raw - 1);
    of = handle_get(parent->handles, index);
    if (of == NULL || handle_get(child->handles, index) != NULL) {
        return;
    }
    of_ref(of);
    (void)handle_install_at(child->handles, index, of, 0);
}

uint64 nt_create_user_process(struct syscall_frame *frame) {
    process_t *parent = proc_current(), *child;
    nt_rtl_user_process_params_t pp;
    uint64 hp_out = frame->r10, ht_out = frame->rdx;
    uint64 pflags = 0, tflags = 0, params = 0, attrs = 0, rsp;
    uint64 user_rsp = syscall_get_user_rsp();
    char *path = NULL, *cwd = NULL;
    uint16 *cmdline = NULL, *env = NULL;
    uint64 cmd_chars = 0, env_chars = 0, image_name = 0, image_chars = 0;
    uint64 client_id_ptr = 0, k;
    uint8 *image = NULL;
    uint32 size = 0;
    address_space_t *space = NULL;
    nt_params_desc_t pd;
    pe_exec_t pex;
    struct syscall_frame f;
    object_t *pobj, *tobj;
    uint64 st;
    int rc;

    if (!nt_stack_arg(user_rsp, 7, &pflags) ||
        !nt_stack_arg(user_rsp, 8, &tflags) ||
        !nt_stack_arg(user_rsp, 9, &params) ||
        !nt_stack_arg(user_rsp, 11, &attrs)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (!u_ok(hp_out, 8) || !u_ok(ht_out, 8)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (params == 0 || !u_ok(params, sizeof(pp))) {
        return STATUS_INVALID_PARAMETER;
    }
    pp = *(const nt_rtl_user_process_params_t *)params;

    /* The attribute list: the image name, and where the client id goes. */
    if (attrs != 0) {
        uint64 total, n;

        if (!u_ok(attrs, 8)) {
            return STATUS_ACCESS_VIOLATION;
        }
        total = *(const uint64 *)attrs;
        if (total < 8 || total > 8 + 32 * 32 || !u_ok(attrs, total)) {
            return STATUS_INVALID_PARAMETER;
        }
        for (n = 0; 8 + (n + 1) * 32 <= total; n++) {
            const uint64 *a = (const uint64 *)(attrs + 8 + n * 32);

            if ((uint32)a[0] == PS_ATTRIBUTE_IMAGE_NAME) {
                image_name = a[2];
                image_chars = a[1] / 2;
            } else if ((uint32)a[0] == PS_ATTRIBUTE_CLIENT_ID && a[1] >= 16) {
                client_id_ptr = a[2];
            }
        }
    }
    if (image_name == 0) {
        image_name = ustr_buffer(&pp.image_path_name, params, pp.flags);
        image_chars = pp.image_path_name.length / 2;
    }
    if (image_name == 0 || image_chars == 0 ||
        !u_ok(image_name, image_chars * 2)) {
        return STATUS_INVALID_PARAMETER;
    }

    path = kmalloc(PATH_MAX_LEN);
    cwd = kmalloc(PATH_MAX_LEN);
    if (path == NULL || cwd == NULL) {
        st = STATUS_NO_MEMORY;
        goto out;
    }
    if (!dos_to_posix(image_name, image_chars, path, PATH_MAX_LEN)) {
        st = STATUS_OBJECT_PATH_NOT_FOUND;
        goto out;
    }
    {
        uint64 cb = ustr_buffer(&pp.current_directory_path, params, pp.flags);
        uint64 cc = pp.current_directory_path.length / 2;

        if (cb == 0 || cc == 0 || !u_ok(cb, cc * 2) ||
            !dos_to_posix(cb, cc, cwd, PATH_MAX_LEN)) {
            for (k = 0; k < PATH_MAX_LEN; k++) {
                cwd[k] = parent->cwd[k];         /* the parent's */
                if (cwd[k] == '\0') {
                    break;
                }
            }
        }
    }

    /* The command line and environment, copied in: the parent's memory is
     * not the child's, and is not even this call's after it returns. */
    {
        uint64 cb = ustr_buffer(&pp.command_line, params, pp.flags);

        cmd_chars = pp.command_line.length / 2;
        if (cmd_chars > SPAWN_MAX_CMDLINE ||
            (cmd_chars != 0 && !u_ok(cb, cmd_chars * 2))) {
            st = STATUS_INVALID_PARAMETER;
            goto out;
        }
        cmdline = kmalloc((cmd_chars + 1) * 2);
        if (cmdline == NULL) {
            st = STATUS_NO_MEMORY;
            goto out;
        }
        for (k = 0; k < cmd_chars; k++) {
            cmdline[k] = ((const uint16 *)cb)[k];
        }
    }
    if (pp.environment != 0) {
        const uint16 *e = (const uint16 *)pp.environment;

        /* To the double null, bounded. */
        for (env_chars = 0; env_chars + 1 < SPAWN_MAX_ENV; env_chars++) {
            if (!u_ok((uint64)&e[env_chars], 4)) {
                st = STATUS_ACCESS_VIOLATION;
                goto out;
            }
            if (e[env_chars] == 0 && e[env_chars + 1] == 0) {
                env_chars += 2;
                break;
            }
        }
        env = kmalloc(env_chars * 2 + 2);
        if (env == NULL) {
            st = STATUS_NO_MEMORY;
            goto out;
        }
        for (k = 0; k < env_chars; k++) {
            env[k] = e[k];
        }
    }

    rc = fs_read_whole(path, &image, &size);
    if (rc != 0) {
        st = rc == -2 ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_ACCESS_DENIED;
        goto out;
    }
    if (!pe_is_pe(image, size)) {
        st = 0xC000012Fu;                       /* STATUS_INVALID_IMAGE_NOT_MZ */
        goto out;
    }

    child = proc_alloc(parent->pid);
    if (child == NULL) {
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    space = vmm_space_create();
    if (space == NULL) {
        proc_free(child);
        st = STATUS_NO_MEMORY;
        goto out;
    }

    /* Handles first: the parameters block names the standard ones. */
    if ((uint32)pflags & PROCESS_CREATE_FLAGS_INHERIT_HANDLES) {
        handle_table_clone(child->handles, parent->handles, 1);
    }
    give_handle(parent, child, pp.standard_input);
    give_handle(parent, child, pp.standard_output);
    give_handle(parent, child, pp.standard_error);

    pd.image_path = path;
    pd.cwd = cwd;
    pd.argv = NULL;
    pd.envp = NULL;
    pd.std_input  = pp.standard_input;
    pd.std_output = pp.standard_output;
    pd.std_error  = pp.standard_error;
    pd.command_line = cmdline;
    pd.command_line_chars = cmd_chars;
    pd.environment = env;
    pd.environment_chars = env_chars;
    rc = pe_exec_build(space, image, size, child->pid, child->thread.tid,
                       &pd, &pex);
    rsp = rc == 0 ? map_stack(space) : 0;
    if (rc != 0 || rsp == 0) {
        vmm_space_destroy(space);
        child->space = NULL;
        proc_free(child);
        st = rc == -12 || rc == 0 ? STATUS_NO_MEMORY
                                  : 0xC000007Bu;   /* INVALID_IMAGE_FORMAT */
        goto out;
    }

    /* The process, as fork would leave a child - but of nothing. */
    child->space        = space;
    child->personality  = PERSONALITY_WINDOWS;
    child->nt_attached  = 0;
    child->pgid         = parent->pgid;
    child->sid          = parent->sid;
    child->uid          = parent->uid;
    child->gid          = parent->gid;
    child->umask        = parent->umask;
    child->ngroups      = parent->ngroups;
    for (k = 0; k < CRED_NGROUPS; k++) {
        child->groups[k] = parent->groups[k];
    }
    rlimit_copy(child, parent);
    for (k = 0; k < PATH_MAX_LEN; k++) {
        child->cwd[k] = cwd[k];
        if (cwd[k] == '\0') {
            break;
        }
    }
    {
        const char *b = path;

        for (k = 0; path[k] != '\0'; k++) {
            if (path[k] == '/') {
                b = &path[k + 1];
            }
        }
        for (k = 0; b[k] != '\0' && k < sizeof(child->image_name) - 1; k++) {
            child->image_name[k] = b[k];
        }
        child->image_name[k] = '\0';
    }
    child->mmap_next         = USER_MMAP_BASE;
    child->brk_base          = (pex.highest_vaddr + 0xFFFULL) & ~0xFFFULL;
    child->brk_current       = child->brk_base;
    child->nt_thread_start   = pex.thread_start;
    child->nt_apc_dispatcher = pex.apc_dispatcher;
    child->nt_exc_dispatcher = pex.exc_dispatcher;
    child->nt_tls_va         = pex.tls_va;
    child->nt_tls_pages      = pex.tls_pages;
    child->thread.gs_base    = NT_TEB_BASE;
    child->thread.fs_base    = 0;
    child->autoreap          = 1;

    tobj = thread_object_create(child->pid);
    pobj = proc_process_object(child);
    if (tobj == NULL || pobj == NULL) {
        if (tobj != NULL) {
            ob_deref(tobj);
        }
        vmm_space_destroy(space);
        child->space = NULL;
        proc_free(child);
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    child->nt_thread_obj = tobj;            /* the thread's own reference */

    start_frame(&f, &pex);
    child->thread.saved_rsp = thread_bootstrap_stack(child->thread.kstack_top,
                                                     &f);
    child->saved_user_rsp = rsp;

    st = nt_handle_for(pobj, hp_out);
    if (st == STATUS_SUCCESS) {
        st = nt_handle_for(tobj, ht_out);
    }
    if (client_id_ptr != 0 && u_ok(client_id_ptr, 16)) {
        ((uint64 *)client_id_ptr)[0] = (uint64)child->pid;
        ((uint64 *)client_id_ptr)[1] = (uint64)child->thread.tid;
    }

    /* Last, so a failure above never left a half-made process running. A
     * handle that could not be returned is the parent's problem, not the
     * child's: it runs anyway, as it would on NT. */
    if ((uint32)tflags & THREAD_CREATE_FLAGS_CREATE_SUSPENDED) {
        child->nt_suspend_count = 1;
        child->nt_parked        = 1;
        child->state            = PROC_BLOCKED;
    } else {
        child->state = PROC_READY;
        sched_enqueue(child);
    }

out:
    if (image != NULL) {
        kfree(image);
    }
    if (path != NULL) {
        kfree(path);
    }
    if (cwd != NULL) {
        kfree(cwd);
    }
    if (cmdline != NULL) {
        kfree(cmdline);
    }
    if (env != NULL) {
        kfree(env);
    }
    return st;
}
