#include "nt.h"
#include "personality.h"
#include "process.h"
#include "syscall.h"
#include "typesk.h"

/* The two tables. Designated initialisers, for the same reason every
 * object_type_t uses them: a slot added to syscall_personality_t must not
 * silently rebind an existing one. */

static const syscall_personality_t linux_personality = {
    .name         = "linux",
    .id           = PERSONALITY_LINUX,
    .dispatch     = linux_syscall_dispatch,
    .is_sigreturn = linux_is_sigreturn
};

static const syscall_personality_t nt_personality = {
    .name         = "nt",
    .id           = PERSONALITY_WINDOWS,
    .dispatch     = nt_syscall_dispatch,
    .is_sigreturn = nt_is_sigreturn
};

const syscall_personality_t *personality_for(personality_t id) {
    switch (id) {
        case PERSONALITY_WINDOWS:
            return &nt_personality;
        case PERSONALITY_LINUX:
        default:
            /* Never NULL. This is called on the syscall path and the result
             * is called through immediately; a null here would be a triple
             * fault rather than a wrong answer, which is a much worse way to
             * find out a process's tag got corrupted. */
            return &linux_personality;
    }
}

int personality_has_nt(const process_t *p) {
    return p != NULL && (p->personality == PERSONALITY_WINDOWS || p->nt_attached);
}

const syscall_personality_t *personality_route(struct syscall_frame *frame,
                                               uint64 *refused) {
    process_t *p = proc_current();
    uint64 nr = frame->rax;

    if ((nr & ~(uint64)NT_SYSCALL_NR_MASK) == NT_SYSCALL_TAG) {
        if (!personality_has_nt(p)) {
            *refused = STATUS_INVALID_SYSTEM_SERVICE;
            return NULL;
        }
        frame->rax = nr & NT_SYSCALL_NR_MASK;
        return &nt_personality;
    }
    if (p != NULL && p->personality == PERSONALITY_WINDOWS &&
        nr >= NT_WIN32K_FIRST && nr <= NT_WIN32K_LAST) {
        *refused = STATUS_INVALID_SYSTEM_SERVICE;
        return NULL;
    }
    return &linux_personality;
}

const syscall_personality_t *personality_current(void) {
    process_t *p = proc_current();

    return personality_for(p != NULL ? p->personality : PERSONALITY_LINUX);
}
