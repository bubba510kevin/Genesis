#ifndef PIPE_H
#define PIPE_H

#include "object.h"
#include "typesk.h"

/* An anonymous pipe: a ring buffer with an object at each end.
 *
 * --- Why two objects and not one ------------------------------------------
 * The read end and the write end are separate object_t's over one shared
 * body, and that is not decoration. Everything a pipe has to get right is a
 * question about WHICH END was closed:
 *
 *   the last write end closes -> readers see end of file
 *   the last read end closes  -> writers get SIGPIPE and -EPIPE
 *
 * One object cannot answer that. Its destructor fires when the last reference
 * of either kind goes, by which time both ends are gone and there is nobody
 * left to tell. Two objects means ob_deref already does the counting: the
 * write end's destroy runs exactly when the last process holding it lets go,
 * which is the definition of "the last writer closed".
 *
 * fork() falls out of this rather than needing anything. handle_table_clone
 * takes a reference on the same open instance, so a forked child holds the
 * SAME end object; the end stays open until every process holding it has
 * closed. That is what makes `cmd1 | cmd2` terminate: the shell closes both
 * ends after forking, cmd1 exits and drops the last write reference, and
 * cmd2's read returns 0.
 *
 * --- Storage --------------------------------------------------------------
 * A static pool, for the reasons object.c gives: teardown runs where the heap
 * is not necessarily safe, and exhaustion arrives as -ENFILE at a specific
 * limit rather than as heap pressure somewhere unrelated. */

/* Create a pipe. Both out-parameters receive a REFERENCED object which the
 * caller owns and must ob_deref (or hand to of_open, which takes its own).
 *
 * Returns 0, or -ENFILE if the pipe pool or the object pool is exhausted. On
 * failure neither pointer is written. */
int pipe_create(object_t **read_end, object_t **write_end);

/* Non-zero if this object is one end of a pipe. lseek and fstat both need to
 * know - a pipe has no position, and a libc uses -ESPIPE and S_IFIFO to work
 * out what it is holding. */
int pipe_is_pipe(const object_t *obj);

/* Bytes buffered in a pipe, readable without blocking, through its READ end
 * (0 for anything else). PeekNamedPipe's ReadDataAvailable. */
uint32 pipe_available(const object_t *read_end);

/* Copy up to n buffered bytes WITHOUT consuming them; how many were. */
uint32 pipe_peek(const object_t *read_end, void *buf, uint32 n);

#endif
