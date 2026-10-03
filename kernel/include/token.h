#ifndef TOKEN_H
#define TOKEN_H

#include "typesk.h"

struct object;
struct process;

/* Access tokens (ROADMAP 16(n), the first slice) - kernel/obj/token.c.
 *
 * A process's token is built from its credentials the first time anything
 * opens it and then kept on the process (the leader), so a privilege
 * enabled through one handle stays enabled for the process, as on NT:
 *   user   S-1-22-1-<euid>, the SID the file security descriptors already
 *          use for that uid (ntsec.c);
 *   groups S-1-22-2-<gid> for the primary and supplementary groups,
 *          Everyone, LOCAL, Authenticated Users, INTERACTIVE, and - for
 *          root - BUILTIN\Administrators; plus the integrity label, High
 *          for root and Medium otherwise;
 *   privileges an administrator's set for root, a standard user's
 *          otherwise, with NT's enabled-by-default ones enabled.
 * Not yet: impersonation (thread tokens), NtDuplicateToken, restricted and
 * filtered tokens, and access checks that consult any of this. */

struct object *token_of_process(struct process *leader);   /* referenced */
int token_is_token(const struct object *obj);

/* NtQueryInformationToken's classes, NT's layouts. `user_base` is the
 * user address `buf` will be copied to - the structures hold pointers to
 * their own SIDs. Returns 0 or an NTSTATUS; *needed is always set. */
uint32 token_query(struct object *tok, uint32 klass, void *buf, uint32 len,
                   uint64 user_base, uint32 *needed);

/* NtAdjustPrivilegesToken over a kernel copy of the TOKEN_PRIVILEGES.
 * prev (may be NULL) receives the previous state of what changed. Returns
 * STATUS_SUCCESS, STATUS_NOT_ALL_ASSIGNED, or an error. */
uint32 token_adjust(struct object *tok, int disable_all, const void *new_state,
                    uint32 new_len, void *prev, uint32 prev_len,
                    uint32 *prev_needed);

#endif
