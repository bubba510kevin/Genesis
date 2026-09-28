#include "elfso.h"
#include "fs.h"
#include "nt.h"
#include "ntmix.h"
#include "paging.h"
#include "pe.h"
#include "personality.h"
#include "process.h"
#include "sched.h"
#include "screen.h"
#include "syscall.h"
#include "teb.h"
#include "typesk.h"

/* A Linux process that loads Windows DLLs (ROADMAP item 19, stage 2).
 *
 * Two things make a DLL runnable, and a process started from an ELF image
 * has neither:
 *
 *   1. AN NT ENVIRONMENT. Compiled Windows code reads its TEB through GS
 *      (GetLastError, the thread id, TLS, the stack bounds SEH walks
 *      between) and reaches the PEB from it (the process parameters and so
 *      GetStdHandle, the module table). nt_attach builds the same blocks the
 *      PE exec path builds, at the same fixed addresses, into the running
 *      process - and gives every thread in it a TEB of its own. From then on
 *      the process "has NT" (personality_has_nt): its tagged NT calls are
 *      served (stage 1 refused them), and every thread it clones later gets
 *      a TEB too.
 *
 *   2. THE IMAGE, LINKED. pe_load_library, the exec path's linker seeded
 *      with what the process already has (the PEB's module table), so a
 *      second DLL binds to the ntdll and kernel32 the first one brought.
 *
 * What stays in ring 3 (src/libgnt): calling each new module's entry point,
 * dependencies first, and finding exports - the headers are mapped, so that
 * is a walk of the export directory in the caller's own memory.
 *
 * The process stays PERSONALITY_LINUX. Personality now answers only "what
 * was exec'd" - how a fault is reported (a signal, here), what the entry
 * stack looks like - and the call table is chosen per call anyway. */

static int attach_err(const char *what, int rc) {
    print_string("pe: attach: ", 0x0C);
    print_string(what, 0x0C);
    print_string("\n", 0x0C);
    return rc;
}

int nt_thread_teb_attach(process_t *t, uint64 stack_top, uint64 stack_size) {
    uint64 teb_va = 0, i;
    int slot = -1;

    if (t == NULL || t->space == NULL) {
        return -22;
    }
    for (i = 0; i < NT_THREAD_SLOTS; i++) {
        if (vmm_get_phys_in(t->space, NT_THREAD_TEB_BASE +
                                      i * NT_THREAD_TEB_STRIDE) == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return -11;                      /* -EAGAIN: no TEB slot free */
    }
    teb_va = NT_THREAD_TEB_BASE + (uint64)slot * NT_THREAD_TEB_STRIDE;
    if (nt_thread_teb_init(t->space, teb_va, stack_top, stack_size,
                           t->tgid, t->pid) != 0) {
        t->nt_teb_va = teb_va;           /* partly mapped: exit unmaps it */
        return -12;
    }
    /* Recorded on the thread so its exit unmaps it (proc_nt_thread_exit),
     * which is also what frees the slot. */
    t->nt_teb_va = teb_va;
    t->thread.gs_base = teb_va;

    /* Its copy of any implicit TLS the process's images declared. None in a
     * process that was ELF to begin with, but a Linux clone() inside a PE
     * process lands here as well. */
    {
        uint64 tls_va = 0, tls_pages = 0;

        if (nt_thread_tls_init(t->space, slot, teb_va, &tls_va, &tls_pages) != 0) {
            t->nt_tls_va = tls_va;
            t->nt_tls_pages = tls_pages;
            return -12;
        }
        t->nt_tls_va = tls_va;
        t->nt_tls_pages = tls_pages;
    }
    return 0;
}

static int nt_attach(process_t *p) {
    static const char *const argv[] = { "", NULL };
    nt_params_desc_t pd;
    nt_module_table_t mt;
    uint64 stack_top, stack_size;
    int i, rc;

    if (personality_has_nt(p)) {
        return 0;
    }
    /* The fixed blocks must be free - a Linux process is entitled to have
     * mapped anything anywhere - and no thread may already be using GS for
     * something of its own (arch_prctl(ARCH_SET_GS)): its base is about to
     * become the TEB. */
    if (vmm_get_phys_in(p->space, NT_TEB_BASE) != 0 ||
        vmm_get_phys_in(p->space, NT_PEB_BASE) != 0 ||
        vmm_get_phys_in(p->space, NT_PARAMS_BASE) != 0) {
        return attach_err("the TEB/PEB addresses are already in use", -17);
    }
    for (i = 0; i < MAX_PROCESSES; i++) {
        process_t *t = proc_at(i);

        if (t != NULL && t->tgid == p->tgid && t->state != PROC_ZOMBIE &&
            t->thread.gs_base != 0) {
            return attach_err("a thread already uses GS", -16);
        }
    }

    /* The calling thread's stack bounds, as SEH's walk reads them: the main
     * thread's are known, another thread's are not (its stack was mmap'd by
     * whatever created it), so it gets the page it is standing on. */
    if (p->pid == p->tgid) {
        stack_top  = USER_STACK_TOP;
        stack_size = USER_STACK_SIZE;
    } else {
        stack_top  = (syscall_get_user_rsp() + 0xFFFULL) & ~0xFFFULL;
        stack_size = 0x1000;
    }

    rc = nt_process_init(p->space, 0, stack_top, stack_size, p->tgid, p->pid);
    if (rc != 0) {
        return attach_err("cannot build the TEB and PEB", rc);
    }

    /* What GetStdHandle reads: the descriptors this process already has, as
     * the PE exec path hands them over (see sys_execve). */
    pd.image_path = "";
    pd.cwd        = p->cwd;
    pd.argv       = argv;
    pd.envp       = NULL;
    pd.std_input  = handle_get(p->handles, 0) != NULL ? NT_HANDLE_FROM_INDEX(0) : 0;
    pd.std_output = handle_get(p->handles, 1) != NULL ? NT_HANDLE_FROM_INDEX(1) : 0;
    pd.std_error  = handle_get(p->handles, 2) != NULL ? NT_HANDLE_FROM_INDEX(2) : 0;
    rc = nt_process_params_init(p->space, &pd);
    if (rc != 0) {
        return attach_err("cannot build the process parameters", rc);
    }

    mt.magic = NT_MODULES_MAGIC;
    mt.count = 0;
    for (i = 0; i < NT_MAX_MODULES; i++) {
        mt.mod[i].base = mt.mod[i].size = 0;
    }
    rc = nt_modules_publish(p->space, &mt);
    if (rc != 0) {
        return attach_err("cannot publish the module table", rc);
    }

    /* Every thread of the process gets a TEB: this one the main block, the
     * others a slot each. A thread running on another CPU picks its new GS
     * base up on its way back to ring 3 (gs_reload), which the poke makes
     * immediate. */
    for (i = 0; i < MAX_PROCESSES; i++) {
        process_t *t = proc_at(i);

        if (t == NULL || t->tgid != p->tgid || t->state == PROC_ZOMBIE) {
            continue;
        }
        t->nt_attached = 1;
        if (t == p) {
            t->thread.gs_base = NT_TEB_BASE;
            syscall_set_user_gs_base(NT_TEB_BASE);
            continue;
        }
        rc = nt_thread_teb_attach(t, 0, 0);
        if (rc != 0) {
            return attach_err("no TEB for another thread", rc);
        }
        t->gs_reload = 1;
        if (t->oncpu) {
            sched_poke(t);
        }
    }
    return 0;
}

static int read_file(const char *path, uint8 **out, uint32 *size) {
    return fs_read_whole(path, out, size) == 0 ? 0 : -1;
}

static void release_file(uint8 *buf) {
    fs_free_file(buf);
}

/* Link the DLL at `resolved` into p's space, add the new modules to the
 * PEB's table, and tell every thread where ntdll's dispatchers are if ntdll
 * came with it. 0, or a negated errno (the reason already printed). */
static int load_pe(process_t *p, const char *resolved, pe_runtime_load_t *r_out) {
    nt_module_table_t mt;
    pe_loaded_module_t loaded[PE_MAX_MODULES];
    pe_runtime_load_t r;
    int i, n, rc;

    if (nt_modules_read(p->space, &mt) != 0 || mt.magic != NT_MODULES_MAGIC) {
        return -22;
    }
    n = 0;
    for (i = 0; i < (int)mt.count && i < NT_MAX_MODULES && n < PE_MAX_MODULES; i++) {
        loaded[n].base = mt.mod[i].base;
        loaded[n].size = mt.mod[i].size;
        n++;
    }

    rc = pe_load_library(p->space, resolved, loaded, n, read_file,
                         release_file, &r);
    if (rc != PE_OK) {
        print_string("pe: ", 0x0C);
        print_string(resolved, 0x0C);
        print_string(": ", 0x0C);
        print_string(pe_strerror(rc), 0x0C);
        print_string("\n", 0x0C);
        return rc == PE_ERR_NOMEM ? -12 : -8;              /* ENOMEM/ENOEXEC */
    }

    /* The new modules join the table - which is what the NEXT load is
     * seeded with, and what ntdll's unwinder searches. */
    for (i = 0; i < r.count; i++) {
        if (mt.count >= NT_MAX_MODULES) {
            return -12;
        }
        mt.mod[mt.count].base = r.mods[i].base;
        mt.mod[mt.count].size = r.mods[i].size;
        mt.count++;
    }
    if (nt_modules_publish(p->space, &mt) != 0) {
        return -14;
    }

    /* ntdll arrived: every thread learns where its dispatchers are. */
    if (r.thread_start != 0 || r.apc_dispatcher != 0 || r.exception_dispatcher != 0) {
        for (i = 0; i < MAX_PROCESSES; i++) {
            process_t *t = proc_at(i);

            if (t == NULL || t->tgid != p->tgid) {
                continue;
            }
            if (r.thread_start != 0) {
                t->nt_thread_start = r.thread_start;
            }
            if (r.apc_dispatcher != 0) {
                t->nt_apc_dispatcher = r.apc_dispatcher;
            }
            if (r.exception_dispatcher != 0) {
                t->nt_exc_dispatcher = r.exception_dispatcher;
            }
        }
    }

    *r_out = r;
    return 0;
}

uint64 nt_genesis_pe_load(uint64 path_ptr, uint64 out_ptr) {
    process_t *p = proc_current();
    char resolved[PATH_MAX_LEN];
    pe_runtime_load_t r;
    gnt_pe_load_out_t *out = (gnt_pe_load_out_t *)out_ptr;
    int i, rc;

    if (!user_ptr_ok(path_ptr) || !user_ptr_ok(out_ptr) ||
        !user_ptr_ok(out_ptr + sizeof(*out) - 1)) {
        return (uint64)-14;
    }
    if (path_normalize(p->cwd, (const char *)path_ptr, resolved,
                       sizeof(resolved)) != PATH_OK) {
        return (uint64)-36;
    }
    if (p->space == NULL || p->space == vmm_kernel_space()) {
        return (uint64)-22;
    }

    rc = nt_attach(p);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }
    rc = load_pe(p, resolved, &r);
    if (rc != 0) {
        return (uint64)(int64)rc;
    }

    out->base  = r.base;
    out->count = (uint32)r.count;
    out->reserved = 0;
    for (i = 0; i < GNT_PE_LOAD_MAX; i++) {
        out->mods[i].base  = (i < r.count) ? r.mods[i].base : 0;
        out->mods[i].size  = (i < r.count) ? r.mods[i].size : 0;
        out->mods[i].entry = (i < r.count) ? r.mods[i].entry : 0;
    }
    return 0;
}

/* Link the shared object at `resolved` and add the new objects to the PEB's
 * ELF table. 0 or a negated errno. */
static int load_elf(process_t *p, const char *resolved, elfso_result_t *r) {
    nt_elf_table_t et;
    elfso_loaded_t loaded[NT_MAX_ELF];
    int i, n = 0;

    if (nt_elf_read(p->space, &et) != 0) {
        return -22;
    }
    if (et.magic != NT_ELF_MAGIC) {          /* the first .so in the process */
        et.magic = NT_ELF_MAGIC;
        et.count = 0;
    }
    for (i = 0; i < (int)et.count && i < NT_MAX_ELF; i++) {
        loaded[n].base = et.mod[i].base;
        loaded[n].size = et.mod[i].size;
        n++;
    }
    if (elfso_load_library(p->space, resolved, loaded, n, read_file,
                           release_file, r) != ELFSO_OK) {
        return -8;
    }
    for (i = 0; i < r->count; i++) {
        if (et.count >= NT_MAX_ELF) {
            return -12;
        }
        et.mod[et.count].base = r->mods[i].base;
        et.mod[et.count].size = r->mods[i].size;
        et.count++;
    }
    return nt_elf_publish(p->space, &et) == 0 ? 0 : -14;
}

uint64 nt_genesis_load_image(uint64 path_ptr, uint64 out_ptr) {
    process_t *p = proc_current();
    char resolved[PATH_MAX_LEN];
    gnt_load_out_t *out = (gnt_load_out_t *)out_ptr;
    uint8 *file = NULL;
    uint32 size = 0;
    uint8 magic[4] = { 0, 0, 0, 0 };
    int i, rc;

    if (!user_ptr_ok(path_ptr) || !user_ptr_ok(out_ptr) ||
        !user_ptr_ok(out_ptr + sizeof(*out) - 1)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (path_normalize(p->cwd, (const char *)path_ptr, resolved,
                       sizeof(resolved)) != PATH_OK) {
        return STATUS_NAME_TOO_LONG;
    }
    if (!personality_has_nt(p)) {
        return STATUS_INVALID_SYSTEM_SERVICE;   /* routing refuses it first */
    }
    /* Which linker: the file says. Read once for the magic and released -
     * each loader reads what it needs itself, dependencies included. */
    if (fs_read_whole(resolved, &file, &size) != 0) {
        return STATUS_DLL_NOT_FOUND;
    }
    for (i = 0; i < 4 && (uint32)i < size; i++) {
        magic[i] = file[i];
    }
    fs_free_file(file);

    out->reserved = 0;
    if (magic[0] == 'M' && magic[1] == 'Z') {
        pe_runtime_load_t r;

        rc = load_pe(p, resolved, &r);
        if (rc != 0) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        out->base = r.base;
        out->count = (uint32)r.count;
        for (i = 0; i < GNT_PE_LOAD_MAX; i++) {
            out->mods[i].base       = (i < r.count) ? r.mods[i].base : 0;
            out->mods[i].size       = (i < r.count) ? r.mods[i].size : 0;
            out->mods[i].entry      = (i < r.count) ? r.mods[i].entry : 0;
            out->mods[i].init_array = 0;
            out->mods[i].init_count = 0;
            out->mods[i].kind       = GNT_KIND_PE;
        }
        return STATUS_SUCCESS;
    }
    if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        elfso_result_t r;

        rc = load_elf(p, resolved, &r);
        if (rc != 0) {
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        out->base = r.base;
        out->count = (uint32)r.count;
        for (i = 0; i < GNT_PE_LOAD_MAX; i++) {
            int in = i < r.count;

            out->mods[i].base       = in ? r.mods[i].base : 0;
            out->mods[i].size       = in ? r.mods[i].size : 0;
            out->mods[i].entry      = in ? r.mods[i].init : 0;
            out->mods[i].init_array = in ? r.mods[i].init_array : 0;
            out->mods[i].init_count = in ? r.mods[i].init_count : 0;
            out->mods[i].kind       = GNT_KIND_ELF;
        }
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_IMAGE_FORMAT;
}
