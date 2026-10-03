#ifndef NTSPAWN_H
#define NTSPAWN_H

#include "paging.h"
#include "syscall.h"
#include "teb.h"
#include "typesk.h"

/* Windows process images and Windows process creation (ROADMAP 16(m)).
 *
 * pe_exec_build (syscall.c) puts everything a PE needs into an address
 * space before its first instruction - image and DLLs, TEB and PEB, the
 * shared page, the module table, implicit TLS, the parameters block. execve
 * calls it for the image replacing the caller; NtCreateUserProcess
 * (kernel/exec/ntspawn.c) for a process that does not exist yet, into a
 * space that is never loaded until that process first runs. */

typedef struct {
    uint64 entry;
    uint64 image_base;
    uint64 highest_vaddr;
    uint64 section_count;
    uint64 thread_start;            /* ntdll!RtlUserThreadStart, or 0     */
    uint64 apc_dispatcher;
    uint64 exc_dispatcher;
    uint64 tls_va, tls_pages;       /* the main thread's implicit TLS     */
    uint64 tls_entry_via_ntdll;     /* enter through ntdll for callbacks  */
} pe_exec_t;

int pe_exec_build(address_space_t *new_space, uint8 *image, uint32 size,
                  int pid, int tid, const nt_params_desc_t *pd,
                  pe_exec_t *out);

/* NtCreateUserProcess - the eleven-argument NT call, arguments five and
 * beyond on the user stack. Returns an NTSTATUS. */
uint64 nt_create_user_process(struct syscall_frame *frame);

#endif
