#ifndef OBJECT_H
#define OBJECT_H

#include "typesk.h"

/* The object manager, and the handle table over it.
 *
 * --- Why this is not simply an fd array ---------------------------------
 * A POSIX file descriptor and an NT HANDLE are the same thing wearing
 * different names, and both are the TOP of a three-level structure that is
 * easy to collapse into two and expensive to un-collapse later:
 *
 *   POSIX:  fd (int)  ->  open file description  ->  inode
 *   NT:     HANDLE    ->  FILE_OBJECT            ->  the object
 *
 * The middle level is the one that gets left out. POSIX requires that dup(2)
 * produce a descriptor SHARING a file offset with the original - `dup2(1,2)`
 * then writing to both advances one position, and shell redirection depends
 * on that. So the offset cannot live in the descriptor. NT reached the same
 * arrangement independently: FILE_OBJECT holds CurrentByteOffset and several
 * handles may reference one FILE_OBJECT.
 *
 * Two levels would work until the first dup and then be wrong in a way that
 * shows up as a shell writing over its own output.
 *
 * --- What is deliberately absent ---
 * The NT namespace: \Device\, \??\, symbolic links, lookup by name. That is
 * real work and speculative until there is a PE binary to exercise it. What
 * is here is the minimum every personality needs - create, reference,
 * dereference, close, look up by handle - and named lookup layers on top
 * without disturbing it.
 *
 * --- Allocation policy ---
 * Lowest free index, always. POSIX guarantees it and shell redirection
 * depends on it; NT promises nothing, so the stricter rule satisfies both. */

#define MAX_HANDLES      32
#define MAX_OBJECTS      64
#define MAX_OPEN_FILES   64

/* Access modes, matching POSIX O_RDONLY/O_WRONLY/O_RDWR in value so the
 * Linux personality can pass them through unchanged. */
#define ACCESS_READ      0x1
#define ACCESS_WRITE     0x2

/* Readiness bits, as the object manager spells them.
 *
 * Deliberately not #include'd from syscall.h. These are the object layer's
 * own vocabulary; POLLIN and POLLOUT are the LINUX ABI's, and NT's
 * IO_STATUS/alertable-wait arrangement is a third spelling that this layer
 * will have to answer in too. They have the same values today because
 * choosing different ones would mean a translation table for no gain - and
 * syscall.c carries a build-time assertion that they still match, so the day
 * one of them moves it is a compile error rather than a poll that reports
 * writability when asked about readability.
 *
 * The identical-values-by-choice arrangement is the same one HANDLE_CLOEXEC
 * and FD_CLOEXEC are already in. */
#define OB_POLLIN     0x0001
#define OB_POLLOUT    0x0004
#define OB_POLLERR    0x0008
#define OB_POLLHUP    0x0010
#define OB_POLLNVAL   0x0020
#define OB_POLL_ALWAYS (OB_POLLERR | OB_POLLHUP | OB_POLLNVAL)

/* Per-handle flags. Both personalities need both bits; only the DEFAULT
 * differs, and that comes from the process's personality tag. */
#define HANDLE_CLOEXEC       0x1   /* POSIX: cleared by default             */
#define HANDLE_INHERITABLE   0x2   /* NT: cleared by default                */

typedef enum {
    OBJ_NONE = 0,
    OBJ_CONSOLE,
    OBJ_FILE,
    OBJ_DIRECTORY,

    /* A pipe. Distinct from OBJ_CONSOLE even though both are unseekable,
     * because a libc asks fstat what it is holding and answers differently:
     * S_IFIFO makes it choose block buffering, S_IFCHR makes it choose line
     * buffering. Reporting a pipe as a terminal is what makes `cmd | cat`
     * flush on every line and `cmd > file` not flush at all. */
    OBJ_PIPE,

    /* A raw block device: a disk, addressed in bytes over sectors.
     *
     * Distinct from OBJ_CONSOLE even though both are devices, because stat
     * reports S_IFBLK for one and S_IFCHR for the other and programs draw
     * real conclusions from the difference - dd chooses a transfer size from
     * it, and a partition tool refuses to touch a character device. It is
     * also seekable, which no other device here is. */
    OBJ_BLOCK,

    /* --- the dispatcher objects (ROADMAP item 14) -------------------------
     *
     * Three classes and not one, and the roadmap is explicit about why: the
     * differences are VISIBLE TO A WAITER, so collapsing them would change
     * what a wait means rather than just what it is called.
     *
     * An EVENT is a flag. A SEMAPHORE is a count with a limit. A MUTANT is
     * ownership, and it is RECURSIVE - the owner may take it again and must
     * release it as many times, which is exactly why it is not a semaphore
     * of one. */
    OBJ_EVENT,
    OBJ_SEMAPHORE,
    OBJ_MUTANT,

    /* A registered object TYPE, named under \ObjectTypes. What makes the
     * namespace self-describing: a tool walking it can ask whether it is
     * looking at a Device or a Section. */
    OBJ_TYPE,

    /* An eventfd counter. Its own class rather than OBJ_PIPE, even though
     * both are unseekable byte-ish things a poll loop waits on, because stat
     * has to describe them differently and because a program that fstat's one
     * is asking what it holds. Same argument OBJ_PIPE itself makes against
     * being folded into OBJ_CONSOLE. */
    OBJ_EVENTFD,

    /* A BSD socket behind a descriptor. Its own class because stat must
     * report S_IFSOCK - a program that fstat's a descriptor to find out what
     * it is holding gets the wrong answer from any of the others. */
    OBJ_SOCKET,

    /* An NT thread, as a handle sees it: a dispatcher object that becomes
     * signalled - permanently - when the thread exits, and that carries the
     * exit code. It is NOT the schedulable thing itself (that is a
     * process_t); it is what WaitForSingleObject on a thread handle waits on
     * and what GetExitCodeThread reads, and it outlives the thread for as
     * long as anyone holds a handle to it. */
    OBJ_THREAD
} obj_class_t;

/* What ob_signal is being asked to do. One slot with an op rather than three
 * slots, because the three types answer the same question differently rather
 * than answering different questions - and a type that grows a fourth
 * operation should not widen the vtable for everybody. */
typedef enum {
    /* SetEvent, ReleaseSemaphore(count), ReleaseMutant. */
    OB_SIG_SET = 0,
    /* ResetEvent. Meaningless for a semaphore or a mutant, which say so with
     * -EINVAL rather than accepting it and doing nothing. */
    OB_SIG_RESET
} ob_signal_op_t;

struct object;

/* What a type knows how to do. A type that cannot do something leaves the
 * pointer NULL and the caller gets -EBADF or -EINVAL rather than a crash. */
typedef struct object_type {
    const char *name;
    obj_class_t klass;

    /* Return bytes transferred, or a negative errno. `offset` is the open
     * instance's position, updated in place by types that have one; a
     * console ignores it, which is exactly what makes it unseekable. */
    int64 (*read)(struct object *obj, void *buf, uint64 n, uint64 *offset);
    int64 (*write)(struct object *obj, const void *buf, uint64 n, uint64 *offset);

    /* Fill `buf` with linux_dirent64 records, resuming from `pos` and
     * updating it. Optional: a type without one is not a directory, and
     * getdents64 answers -ENOTDIR for it.
     *
     * This is a vtable slot rather than a branch in the syscall because a
     * directory is not necessarily a directory on disk. The device directory
     * is a walk over the object namespace, and the syscall should not have to
     * know that any more than read(2) knows whether it is talking to a file
     * or a console. */
    int64 (*getdents)(struct object *obj, void *buf, uint64 max, uint64 *pos);

    /* Which of POLLIN / POLLOUT / POLLHUP hold RIGHT NOW - asked WITHOUT
     * performing the operation. Optional; see ob_poll for what absence means.
     *
     * This is the one question no object could answer before poll(2) existed.
     * Every type answers "block until it would not block"; nothing answered
     * "would it block". They are not the same question and the second cannot
     * be built out of the first - trying it is a read that has to be undone.
     *
     * Same shape as ob_is_directory and for the same reason: the type knows,
     * and the caller must not guess. A syscall that switched on klass to
     * decide readiness would be wrong for the first object whose class is
     * shared by two types with different blocking behaviour, which /dev/null
     * and the console already are.
     *
     * `events` is what the caller asked about. A type may use it to skip work
     * it was not asked for; it must not use it to suppress POLLHUP or
     * POLLERR, which are reported whether requested or not. */
    int   (*poll)(struct object *obj, int events);

    /* --- wait and signal: the dispatcher half -----------------------------
     *
     * WAIT IS NOT POLL, and the pairing is the thing to get right.
     *
     * poll asks "would this block" and must not change anything. wait
     * PERFORMS THE OPERATION AND CONSUMES: a synchronisation event resets, a
     * semaphore decrements, a mutant takes ownership. So wait cannot be built
     * out of poll - "poll says ready, therefore take it" has a window between
     * the two in which somebody else takes it - and poll must not be used as
     * a cheap wait, because it would report ready forever on an object nobody
     * ever consumes.
     *
     * They are the same relationship read(2) and poll(2) already have, which
     * is why poll's own comment above says the second question cannot be
     * built out of the first. This is that sentence again from the other end.
     *
     * `deadline` is in absolute ticks; 0 means no deadline. Returns 0 when
     * the object was acquired, -ETIMEDOUT, or -EINTR when a signal arrived.
     *
     * BOTH ARE OPTIONAL, and their absence is the answer to the negative half
     * of this item's check: waiting on a console must be -EINVAL rather than
     * a hang, and it is, because ob_wait finds no slot. A type that is not
     * waitable is not distinguishable from one nobody has got to yet unless
     * something asserts it. */
    int   (*wait)(struct object *obj, uint64 deadline);

    /* `count` is ReleaseSemaphore's release count and is 1 for the others.
     * `prev` receives the previous state where the caller can use it - the
     * semaphore's old count, which is what ReleaseSemaphore returns - or is
     * left alone when there is nothing meaningful to report. */
    int   (*signal)(struct object *obj, int op, int64 count, int64 *prev);

    /* Last reference dropped. Optional. */
    void  (*destroy)(struct object *obj);
} object_type_t;

typedef struct object {
    const object_type_t *type;
    uint32 refcount;
    void  *body;        /* type-specific state */
} object_t;

/* One open instance: position, access mode and status flags. Refcounted
 * because dup shares it and close must not tear it down while another
 * descriptor holds it.
 *
 * --- Three sets of flags, and which level each lives at -------------------
 * This is the distinction fcntl exists to expose, and getting it wrong is
 * invisible until a program depends on one of them:
 *
 *   access flags   O_RDONLY/O_WRONLY/O_RDWR. Fixed at open, never changeable.
 *                  Lives here, in `access`.
 *   status flags   O_APPEND, O_NONBLOCK. Shared by EVERY descriptor that
 *                  refers to this open instance, because they are a property
 *                  of the open, not of the name for it - F_SETFL through a
 *                  dup is visible through the original. Lives here, in
 *                  `status`.
 *   handle flags   FD_CLOEXEC. Private to ONE descriptor: dup deliberately
 *                  does not copy it. Lives in handle_t.flags, one level up.
 *
 * Putting FD_CLOEXEC here would make a dup inherit it, which is the exact
 * behaviour POSIX says dup must not have and the reason a shell's redirection
 * of a close-on-exec descriptor would silently close it in the child. */
typedef struct open_file {
    object_t *obj;
    uint64    offset;
    uint32    access;
    uint32    status;
    uint32    refcount;
} open_file_t;

/* One entry of a process's table. A POSIX fd is an index into this array. */
typedef struct handle {
    open_file_t *file;
    uint32       flags;
} handle_t;

/* --- objects ------------------------------------------------------------ */

/* Take an object from the pool. refcount starts at 1. NULL if exhausted. */
object_t *ob_create(const object_type_t *type, void *body);

void ob_ref(object_t *obj);

/* Drop a reference; destroys at zero. */
void ob_deref(object_t *obj);

/* Is this object a directory?
 *
 * Asking the TYPE is the only way to ask this that is correct for every
 * object. fileobj_is_dir() answers it by casting obj->body to a fat_entry_t
 * and reading the attribute byte, which is right for a file object and
 * nonsense for anything else - handed a device-directory object, whose body
 * is an ns_entry_t, it reads byte 24 of the entry's NAME. That is not a
 * hypothetical: it is why opening /dev with O_DIRECTORY answered -ENOTDIR.
 *
 * klass is already the field that carries this fact, and it is set by every
 * type that exists. */
int ob_is_directory(const object_t *obj);

/* What this object would report to poll(2) right now, restricted to `events`
 * plus the three bits that are always reported.
 *
 * A type with no poll method answers POLLIN|POLLOUT - "an operation on me
 * cannot block". That default is not laziness, it is the only safe one. The
 * alternative default, "not ready", turns a poll on any type nobody has got
 * to yet into a hang; this one turns it at worst into a read that blocks,
 * which is the behaviour the program had before it called poll. A regular
 * file is genuinely in this category and always will be: POSIX says a file is
 * always ready for both, because a disk read that takes a millisecond is not
 * "blocking" in the sense poll means.
 *
 * A NULL object is POLLNVAL, which is what a closed descriptor reports. */
int ob_poll(object_t *obj, int events);

/* --- waiting on a dispatcher object -------------------------------------
 *
 * Block until `obj` is signalled, and CONSUME - see object_type_t::wait for
 * why that is not what poll does and cannot be built out of it.
 *
 * -EINVAL for an object with no wait slot, which is every type except the
 * dispatcher objects. That is the answer, not a placeholder: "you cannot wait
 * on a console" is a true statement about a console, and a caller that gets
 * it can do something sensible. Blocking forever instead would be the same
 * answer delivered as a hang.
 *
 * `deadline` is absolute ticks; 0 waits indefinitely. */
int ob_wait(object_t *obj, uint64 deadline);

/* Signal `obj`. `op` is an ob_signal_op_t; `count` is a semaphore's release
 * count and 1 for everything else; `prev`, if non-NULL, receives the previous
 * state where the type has one to report.
 *
 * -EINVAL for an object with no signal slot, for the same reason as above. */
int ob_signal(object_t *obj, int op, int64 count, int64 *prev);

/* --- the type registry --------------------------------------------------
 *
 * Publish a type under \ObjectTypes, which is what makes the namespace
 * self-describing: real NT has one entry per registered type, and it is how a
 * tool walking the namespace knows whether it is looking at a Device or a
 * Section. Genesis had object_type_t.name and nothing published it.
 *
 * Safe to call twice with the same type; the second call is a no-op. Returns
 * 0, or a negative errno if the namespace could not take it. */
int ob_register_type(const object_type_t *type);

/* Publish every type registered so far. Called once at boot, after the
 * namespace exists - registration can happen before \ObjectTypes does, and a
 * type that registered early must not be lost. */
void ob_publish_types(void);


/* --- open instances ----------------------------------------------------- */

/* Wrap an object in an open instance, taking a reference on it. */
open_file_t *of_open(object_t *obj, uint32 access);

void of_ref(open_file_t *f);
void of_deref(open_file_t *f);

/* --- handle tables ------------------------------------------------------ */

/* Zero a table. Does not release anything - for a fresh process only. */
void handle_table_init(handle_t *table);

/* Install `file` at the lowest free index. Returns the index or -EMFILE.
 *
 * TAKES OWNERSHIP of the caller's reference, on success and on failure both.
 * `handle_alloc(t, of_open(obj, access), 0)` is therefore correct and
 * complete - which is the point, because the alternative convention (add a
 * reference, caller still holds its own) reads identically and silently
 * leaks one every time a caller forgets. A caller that needs to keep its own
 * reference calls of_ref() first; dup does exactly that. */
int handle_alloc(handle_t *table, open_file_t *file, uint32 flags);

/* handle_alloc with a floor: the lowest free index that is at least `min`.
 *
 * This is F_DUPFD, whose entire content is that floor - `fcntl(fd, F_DUPFD,
 * 10)` means "duplicate this, and put it at 10 or above". A shell uses it to
 * park a descriptor somewhere it will not collide with the 0/1/2 it is about
 * to rearrange, so answering with the lowest free index regardless would hand
 * back exactly the descriptor the caller was trying to avoid.
 *
 * handle_alloc is this with min 0. Ownership rules are identical. */
int handle_alloc_from(handle_t *table, int min, open_file_t *file, uint32 flags);

/* The per-descriptor flags at one index. Returns -EBADF if it is closed.
 *
 * A getter rather than reaching into table[i].flags at the call site, because
 * the caller that reaches in is one refactor away from reaching into
 * file->status instead and reintroducing exactly the confusion the comment on
 * open_file_t is about. */
int handle_flags(const handle_t *table, int index);
int handle_set_flags(handle_t *table, int index, uint32 flags);

/* Install at a specific index, closing whatever was there. This is dup2, and
 * the ordering matters: dup2(fd, fd) must be a no-op rather than closing and
 * reopening, which is why the identity case returns early.
 *
 * Takes ownership on the same terms as handle_alloc. */
int handle_install_at(handle_t *table, int index, open_file_t *file, uint32 flags);

/* The open instance behind an index, or NULL if the index is closed or out
 * of range. */
open_file_t *handle_get(const handle_t *table, int index);

/* Close one index. Returns 0, or -EBADF if it was not open. */
int handle_close(handle_t *table, int index);

/* Close every index. For process teardown. */
void handle_close_all(handle_t *table);

/* Close every index marked HANDLE_CLOEXEC. For execve, which is the only
 * reason that flag exists. */
void handle_close_on_exec(handle_t *table);

/* Copy a table, taking a reference on each open instance - so both processes
 * share offsets, which is what fork does. Handles not marked inheritable are
 * skipped when `inheritable_only` is set, which is what NT does. */
void handle_table_clone(handle_t *dst, const handle_t *src, int inheritable_only);

#endif
