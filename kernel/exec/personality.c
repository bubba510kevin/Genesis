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

const syscall_personality_t *personality_current(void) {
    process_t *p = proc_current();

    return personality_for(p != NULL ? p->personality : PERSONALITY_LINUX);
}
