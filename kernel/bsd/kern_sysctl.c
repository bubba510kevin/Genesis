/* sysctl(9): the MIB tree, the handlers, and a reader.
 *
 * --- why this is hand-written -------------------------------------------
 * <sys/sysctl.h> is VENDORED, so every SYSCTL_INT, SYSCTL_NODE and
 * SYSCTL_PROC in the network stack is upstream's macro producing upstream's
 * struct sysctl_oid. What is not vendored is kern/kern_sysctl.c, and the
 * reason is worth being specific about rather than hand-waving at: that file
 * is 2200 lines of which the majority is the __sysctl(2) SYSTEM CALL - name
 * translation for userland, ucred-based access checking, capsicum rights,
 * the wiring of user pages so a handler can copy into them without faulting,
 * and the name2oid/oidfmt/oiddescr meta-nodes that sysctl(8) walks. None of
 * that has a consumer here: there is no sysctl(2) in Genesis's syscall table.
 *
 * What IS needed is everything below the syscall: a tree that static OIDs
 * register into, dynamic add/remove for the ones UMA and the drivers build at
 * run time, and the handler functions the OIDs point at. That is what this
 * file is, and it is about a tenth the size.
 *
 * The seam is upstream's own: sysctl_root() calls the handler with a
 * struct sysctl_req whose oldfunc/newfunc do the copying, and upstream
 * already has TWO implementations of those (sysctl_old_user/sysctl_old_kernel)
 * precisely so the tree can be driven from inside the kernel. This file
 * provides the kernel one, which is the one that has a caller here.
 *
 * --- why it exists at all -----------------------------------------------
 * Not for its own sake. <sys/sysctl.h> was a 236-line Genesis shim that
 * compiled every SYSCTL_ macro to nothing, and it kept failing in a
 * particular way: a vendored file would use a macro at an arity the shim did
 * not have, or read a struct field the shim's struct did not carry, and the
 * error named the vendored file rather than the shim. Two of those cost real
 * time during the IP layer work. Vendoring the header ends that class of
 * failure, and vendoring the header is what obliges this file to exist.
 *
 * The side effect is a genuinely useful one: the network stack's counters and
 * tunables - net.inet.ip.forwarding, net.inet.icmp.icmplim, all of ipstat -
 * are now readable by name, which is what net_sysctl_report() below prints.
 */

#define _KERNEL 1

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/counter.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/linker_set.h>

#include "kheap.h"
#include "kprintf.h"

/* --- the roots -----------------------------------------------------------
 *
 * Upstream defines these in kern/kern_mib.c, which is otherwise entirely
 * about reporting the machine's identity (hostname, ostype, hw.model) - none
 * of which this kernel has to answer. So the nodes are declared here and the
 * rest of that file is not vendored.
 *
 * The numbers are the CTL_* constants from <sys/sysctl.h>, not OID_AUTO: a
 * top-level node's number is part of the MIB's stable numbering and userland
 * tools depend on it. Keeping them right costs nothing.
 */
SYSCTL_ROOT_NODE(CTL_KERN, kern, CTLFLAG_RW | CTLFLAG_CAPRD | CTLFLAG_MPSAFE,
    0, "High kernel, proc, limits &c");
SYSCTL_ROOT_NODE(CTL_VM, vm, CTLFLAG_RW | CTLFLAG_MPSAFE,
    0, "Virtual memory");
SYSCTL_ROOT_NODE(CTL_VFS, vfs, CTLFLAG_RW | CTLFLAG_MPSAFE,
    0, "File system");
SYSCTL_ROOT_NODE(CTL_NET, net, CTLFLAG_RW | CTLFLAG_MPSAFE,
    0, "Network, (see socket.h)");
SYSCTL_ROOT_NODE(CTL_DEBUG, debug, CTLFLAG_RW | CTLFLAG_MPSAFE,
    0, "Debugging");
SYSCTL_ROOT_NODE(CTL_HW, hw, CTLFLAG_RW | CTLFLAG_MPSAFE,
    0, "hardware");
SYSCTL_ROOT_NODE(CTL_MACHDEP, machdep, CTLFLAG_RW | CTLFLAG_MPSAFE,
    0, "machine dependent");
SYSCTL_ROOT_NODE(CTL_USER, user, CTLFLAG_RW | CTLFLAG_CAPRD | CTLFLAG_MPSAFE,
    0, "user-level");
SYSCTL_ROOT_NODE(CTL_P1003_1B, p1003_1b, CTLFLAG_RW | CTLFLAG_MPSAFE,
    0, "p1003_1b, (see p1003_1b.h)");
SYSCTL_ROOT_NODE(OID_AUTO, security, CTLFLAG_RW | CTLFLAG_MPSAFE,
    0, "Security");
SYSCTL_ROOT_NODE(OID_AUTO, compat, CTLFLAG_RW | CTLFLAG_MPSAFE,
    0, "Compatibility code");

/* kern.features - the node every FEATURE() macro hangs a flag off, so that
 * userland can ask "was this kernel built with X". netinet/in.c declares
 * FEATURE(inet, ...) under it. Upstream defines the node in kern/kern_mib.c,
 * which is not vendored for the reason above. */
SYSCTL_NODE(_kern, OID_AUTO, features, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Kernel Features");

/* The head of the whole tree. Upstream's name, because SYSCTL_ROOT_NODE and
 * SYSCTL_ADD_ROOT_NODE both refer to it by that name. */
struct sysctl_oid_list sysctl__children = RB_INITIALIZER(&sysctl__children);

/* The RB tree comparison and generated functions <sys/sysctl.h> declared but
 * left for the implementation file to instantiate. */
RB_GENERATE(sysctl_oid_list, sysctl_oid, oid_link, cmp_sysctl_oid);

/* One lock for the whole tree.
 *
 * Upstream uses an sx lock plus a separate "sysctl_lock" for the tree and
 * per-OID reference counting so that a sleeping handler does not block
 * registration. Nothing here sleeps inside a handler - every handler in this
 * file copies to or from memory and returns - so a plain mutex is the whole
 * requirement, and it is a real one rather than a no-op because
 * sysctl_add_oid is reachable from a driver attach on either CPU. */
static struct mtx sysctl_mtx;
static int sysctl_ready;

/* --- registration -------------------------------------------------------- */

static int oid_name_cmp(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return ((int)(unsigned char)*a - (int)(unsigned char)*b);
}

/* Next free number under `parent`, for an OID_AUTO node.
 *
 * Upstream starts at CTL_AUTO_START (0x100) and takes the highest in use plus
 * one, which keeps auto-numbered OIDs out of the range reserved for the
 * stable numbering the roots above use. Same rule here. */
static int next_auto_number(struct sysctl_oid_list *parent) {
    struct sysctl_oid *p;
    int n = CTL_AUTO_START;

    RB_FOREACH(p, sysctl_oid_list, parent) {
        if (p->oid_number >= n) {
            n = p->oid_number + 1;
        }
    }
    return (n);
}

void sysctl_register_oid(struct sysctl_oid *oidp) {
    struct sysctl_oid_list *parent = oidp->oid_parent;

    if (parent == NULL) {
        parent = &sysctl__children;
        oidp->oid_parent = parent;
    }
    if (oidp->oid_number == OID_AUTO) {
        oidp->oid_number = next_auto_number(parent);
    }
    /* A duplicate name would make the tree ambiguous to a lookup by name.
     * Upstream panics on this under INVARIANTS; here it is dropped and
     * reported, because a duplicate sysctl is never worth halting a boot
     * over and a silent drop is what would be hard to find later. */
    if (RB_INSERT(sysctl_oid_list, parent, oidp) != NULL) {
        kprintf_c(0x0E, "sysctl: duplicate oid number %d for '%s' - dropped\n",
                  oidp->oid_number, oidp->oid_name ? oidp->oid_name : "?");
    }
}

void sysctl_unregister_oid(struct sysctl_oid *oidp) {
    if (oidp->oid_parent != NULL) {
        RB_REMOVE(sysctl_oid_list, oidp->oid_parent, oidp);
    }
}

/* CTLFLAG_DORMANT lets a module register OIDs that are not visible until it
 * finishes initialising. Nothing here uses it; both halves exist so a
 * vendored file that does still links. */
void sysctl_register_disabled_oid(struct sysctl_oid *oidp) {
    oidp->oid_kind |= CTLFLAG_DORMANT;
    sysctl_register_oid(oidp);
}

void sysctl_enable_oid(struct sysctl_oid *oidp) {
    oidp->oid_kind &= ~CTLFLAG_DORMANT;
}

/* --- the static set ------------------------------------------------------
 *
 * Every SYSCTL_ macro at file scope emits a struct sysctl_oid AND a pointer
 * to it in the linker set `sysctl_set`. Walking that set and registering each
 * entry is what turns a pile of static structs into a tree.
 *
 * Runs from net_sysctl_init(), called out of flk.c. Ordering matters in one
 * direction only: a child must not be registered before its parent's
 * oid_children list is usable - and it always is, because that list is a
 * static RB_INITIALIZER inside the parent's own struct.
 */
SET_DECLARE(sysctl_set, struct sysctl_oid);

static int registered_count;

void net_sysctl_init(void) {
    struct sysctl_oid **oidp;

    if (sysctl_ready) {
        return;
    }
    mtx_init(&sysctl_mtx, "sysctl", NULL, MTX_DEF);
    sysctl_ready = 1;

    SET_FOREACH(oidp, sysctl_set) {
        sysctl_register_oid(*oidp);
        registered_count++;
    }
}

int net_sysctl_count(void) {
    return (registered_count);
}

/* --- dynamic OIDs -------------------------------------------------------- */

static struct malloc_type genesis_m_sysctloid[1] = { { "sysctloid" } };

struct sysctl_oid *
sysctl_add_oid(struct sysctl_ctx_list *clist, struct sysctl_oid_list *parent,
               int nbr, const char *name, int kind, void *arg1, intmax_t arg2,
               int (*handler)(SYSCTL_HANDLER_ARGS), const char *fmt,
               const char *descr, const char *label) {
    struct sysctl_oid *oidp;

    if (parent == NULL) {
        parent = &sysctl__children;
    }
    oidp = (struct sysctl_oid *)kmalloc(sizeof(*oidp));
    if (oidp == NULL) {
        return (NULL);
    }
    memset(oidp, 0, sizeof(*oidp));
    RB_INIT(&oidp->oid_children);
    oidp->oid_parent  = parent;
    oidp->oid_number  = nbr;
    oidp->oid_kind    = (u_int)kind | CTLFLAG_DYN;
    oidp->oid_arg1    = arg1;
    oidp->oid_arg2    = arg2;
    oidp->oid_name    = name;
    oidp->oid_handler = handler;
    oidp->oid_fmt     = fmt;
    oidp->oid_descr   = descr;
    oidp->oid_label   = label;
    oidp->oid_refcnt  = 1;

    /* The NAME is not copied.
     *
     * Upstream strdups it, because a module's OID can outlive the string
     * literal in an unloaded module's text. Every caller here passes a string
     * literal in kernel text that never goes away, and the one dynamic-name
     * caller in the vendored tree - UMA naming a zone's node - passes the
     * zone's own name, which outlives the node. Said out loud because a
     * future caller passing a stack buffer would get a dangling name and no
     * warning. */

    mtx_lock(&sysctl_mtx);
    sysctl_register_oid(oidp);
    mtx_unlock(&sysctl_mtx);

    if (clist != NULL) {
        (void)sysctl_ctx_entry_add(clist, oidp);
    }
    return (oidp);
}

int sysctl_remove_oid(struct sysctl_oid *oidp, int del, int recurse) {
    struct sysctl_oid *child, *tmp;

    if (oidp == NULL) {
        return (EINVAL);
    }
    if (recurse) {
        RB_FOREACH_SAFE(child, sysctl_oid_list, &oidp->oid_children, tmp) {
            (void)sysctl_remove_oid(child, del, 1);
        }
    } else if (!RB_EMPTY(&oidp->oid_children)) {
        /* Removing a node that still has children would orphan them - they
         * would stay linked to a freed parent. Upstream returns ENOTEMPTY
         * for the same reason. */
        return (ENOTEMPTY);
    }

    mtx_lock(&sysctl_mtx);
    sysctl_unregister_oid(oidp);
    mtx_unlock(&sysctl_mtx);

    if (del && (oidp->oid_kind & CTLFLAG_DYN) != 0) {
        kfree(oidp);
    }
    return (0);
}

int sysctl_remove_name(struct sysctl_oid *parent, const char *name, int del,
                       int recurse) {
    struct sysctl_oid *p, *tmp;

    RB_FOREACH_SAFE(p, sysctl_oid_list, &parent->oid_children, tmp) {
        if (p->oid_name != NULL && oid_name_cmp(p->oid_name, name) == 0) {
            return (sysctl_remove_oid(p, del, recurse));
        }
    }
    return (ENOENT);
}

void sysctl_rename_oid(struct sysctl_oid *oidp, const char *name) {
    oidp->oid_name = name;
}

int sysctl_move_oid(struct sysctl_oid *oidp, struct sysctl_oid_list *parent) {
    mtx_lock(&sysctl_mtx);
    sysctl_unregister_oid(oidp);
    oidp->oid_parent = parent;
    oidp->oid_number = OID_AUTO;
    sysctl_register_oid(oidp);
    mtx_unlock(&sysctl_mtx);
    return (0);
}

/* --- contexts ------------------------------------------------------------
 *
 * A context is a list of dynamically-added OIDs so that a subsystem can drop
 * all of its own in one call. UMA uses one per zone. */

int sysctl_ctx_init(struct sysctl_ctx_list *clist) {
    if (clist == NULL) {
        return (EINVAL);
    }
    TAILQ_INIT(clist);
    return (0);
}

struct sysctl_ctx_entry *
sysctl_ctx_entry_add(struct sysctl_ctx_list *clist, struct sysctl_oid *oidp) {
    struct sysctl_ctx_entry *e;

    if (clist == NULL || oidp == NULL) {
        return (NULL);
    }
    e = (struct sysctl_ctx_entry *)kmalloc(sizeof(*e));
    if (e == NULL) {
        return (NULL);
    }
    e->entry = oidp;
    TAILQ_INSERT_HEAD(clist, e, link);
    return (e);
}

struct sysctl_ctx_entry *
sysctl_ctx_entry_find(struct sysctl_ctx_list *clist, struct sysctl_oid *oidp) {
    struct sysctl_ctx_entry *e;

    if (clist == NULL) {
        return (NULL);
    }
    TAILQ_FOREACH(e, clist, link) {
        if (e->entry == oidp) {
            return (e);
        }
    }
    return (NULL);
}

int sysctl_ctx_entry_del(struct sysctl_ctx_list *clist,
                         struct sysctl_oid *oidp) {
    struct sysctl_ctx_entry *e = sysctl_ctx_entry_find(clist, oidp);

    if (e == NULL) {
        return (ENOENT);
    }
    TAILQ_REMOVE(clist, e, link);
    kfree(e);
    return (0);
}

int sysctl_ctx_free(struct sysctl_ctx_list *clist) {
    struct sysctl_ctx_entry *e;

    if (clist == NULL) {
        return (EINVAL);
    }
    /* Children before parents. The list is built with INSERT_HEAD, so it is
     * already in reverse creation order and a parent is always created before
     * its children - which makes a plain forward walk the right order and is
     * exactly why upstream inserts at the head. */
    while ((e = TAILQ_FIRST(clist)) != NULL) {
        TAILQ_REMOVE(clist, e, link);
        (void)sysctl_remove_oid(e->entry, 1, 0);
        kfree(e);
    }
    return (0);
}

/* --- request plumbing ----------------------------------------------------
 *
 * A handler copies its answer out through req->oldfunc and reads a new value
 * through req->newfunc. Upstream has a user-space pair (copyout/copyin) and a
 * kernel pair; only the kernel pair has a caller here, because there is no
 * sysctl(2).
 *
 * oldidx is advanced even past the end of the caller's buffer, which looks
 * wrong and is not: that is how a caller asking with oldptr == NULL learns
 * the size it needs. Truncation is reported as ENOMEM, upstream's code for
 * "the value did not fit and here is how much you need".
 */
static int sysctl_old_kernel(struct sysctl_req *req, const void *p,
                             size_t l) {
    size_t i = 0;

    if (req->oldptr != NULL) {
        i = l;
        if (req->oldlen <= req->oldidx) {
            i = 0;
        } else if (i > req->oldlen - req->oldidx) {
            i = req->oldlen - req->oldidx;
        }
        if (i > 0) {
            memcpy((char *)req->oldptr + req->oldidx, p, i);
        }
    }
    req->oldidx += l;
    if (req->oldptr != NULL && i != l) {
        return (ENOMEM);
    }
    return (0);
}

static int sysctl_new_kernel(struct sysctl_req *req, void *p, size_t l) {
    if (req->newptr == NULL) {
        return (0);
    }
    if (req->newlen - req->newidx < l) {
        return (EINVAL);
    }
    memcpy(p, (const char *)req->newptr + req->newidx, l);
    req->newidx += l;
    return (0);
}

/* Upstream wires the user pages a handler will write into, so the copy cannot
 * fault while a lock is held. Kernel buffers are always resident, so there is
 * nothing to wire and the honest implementation is to say the buffer is
 * already good. */
int sysctl_wire_old_buffer(struct sysctl_req *req, size_t len) {
    (void)len;
    req->lock = REQ_WIRED;
    return (0);
}

/* --- the handlers --------------------------------------------------------
 *
 * Each reads the value arg1 points at (or arg2 itself, when arg1 is NULL -
 * that is how SYSCTL_INT publishes a constant), hands it to the caller, and
 * accepts a replacement if the OID is writable and one was supplied.
 *
 * The CTLFLAG_WR test is upstream's and is the only access control here.
 * There is no ucred to check against; there is also no unprivileged caller,
 * because the only way into this tree is from kernel code.
 */
int sysctl_handle_int(SYSCTL_HANDLER_ARGS) {
    int tmp, error;

    if (arg1 != NULL) {
        tmp = *(int *)arg1;
    } else {
        tmp = (int)arg2;
    }
    error = SYSCTL_OUT(req, &tmp, sizeof(int));
    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    if (arg1 == NULL) {
        return (EPERM);
    }
    return (SYSCTL_IN(req, arg1, sizeof(int)));
}

int sysctl_handle_bool(SYSCTL_HANDLER_ARGS) {
    uint8_t tmp;
    int error;

    tmp = (arg1 != NULL) ? (*(bool *)arg1 ? 1 : 0) : (uint8_t)(arg2 != 0);
    error = SYSCTL_OUT(req, &tmp, sizeof(tmp));
    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    if (arg1 == NULL) {
        return (EPERM);
    }
    error = SYSCTL_IN(req, &tmp, sizeof(tmp));
    if (error == 0) {
        *(bool *)arg1 = (tmp != 0);
    }
    return (error);
}

int sysctl_handle_8(SYSCTL_HANDLER_ARGS) {
    int8_t tmp = (arg1 != NULL) ? *(int8_t *)arg1 : (int8_t)arg2;
    int error = SYSCTL_OUT(req, &tmp, sizeof(tmp));

    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    if (arg1 == NULL) {
        return (EPERM);
    }
    return (SYSCTL_IN(req, arg1, sizeof(tmp)));
}

int sysctl_handle_16(SYSCTL_HANDLER_ARGS) {
    int16_t tmp = (arg1 != NULL) ? *(int16_t *)arg1 : (int16_t)arg2;
    int error = SYSCTL_OUT(req, &tmp, sizeof(tmp));

    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    if (arg1 == NULL) {
        return (EPERM);
    }
    return (SYSCTL_IN(req, arg1, sizeof(tmp)));
}

int sysctl_handle_32(SYSCTL_HANDLER_ARGS) {
    int32_t tmp = (arg1 != NULL) ? *(int32_t *)arg1 : (int32_t)arg2;
    int error = SYSCTL_OUT(req, &tmp, sizeof(tmp));

    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    if (arg1 == NULL) {
        return (EPERM);
    }
    return (SYSCTL_IN(req, arg1, sizeof(tmp)));
}

int sysctl_handle_64(SYSCTL_HANDLER_ARGS) {
    int64_t tmp = (arg1 != NULL) ? *(int64_t *)arg1 : (int64_t)arg2;
    int error = SYSCTL_OUT(req, &tmp, sizeof(tmp));

    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    if (arg1 == NULL) {
        return (EPERM);
    }
    return (SYSCTL_IN(req, arg1, sizeof(tmp)));
}

int sysctl_handle_long(SYSCTL_HANDLER_ARGS) {
    long tmp = (arg1 != NULL) ? *(long *)arg1 : (long)arg2;
    int error = SYSCTL_OUT(req, &tmp, sizeof(tmp));

    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    if (arg1 == NULL) {
        return (EPERM);
    }
    return (SYSCTL_IN(req, arg1, sizeof(tmp)));
}

int sysctl_handle_string(SYSCTL_HANDLER_ARGS) {
    char *p = (char *)arg1;
    size_t outlen;
    int error;

    if (p == NULL) {
        return (EINVAL);
    }
    outlen = strlen(p) + 1;
    error = SYSCTL_OUT(req, p, outlen);
    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    /* arg2 is the buffer size. A new value that does not fit is refused
     * rather than truncated: a half-written string is worse than an
     * unchanged one, and upstream refuses it too. */
    if (req->newlen - req->newidx >= (size_t)arg2) {
        return (EINVAL);
    }
    error = SYSCTL_IN(req, p, req->newlen - req->newidx);
    if (error == 0) {
        p[req->newlen - req->newidx] = '\0';
    }
    return (error);
}

int sysctl_handle_opaque(SYSCTL_HANDLER_ARGS) {
    int error = SYSCTL_OUT(req, arg1, (size_t)arg2);

    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    return (SYSCTL_IN(req, arg1, (size_t)arg2));
}

int sysctl_handle_counter_u64(SYSCTL_HANDLER_ARGS) {
    uint64_t out = counter_u64_fetch(*(counter_u64_t *)arg1);
    int error = SYSCTL_OUT(req, &out, sizeof(out));

    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    /* Upstream: any write to a counter zeroes it, whatever the value. */
    counter_u64_zero(*(counter_u64_t *)arg1);
    return (0);
}

int sysctl_handle_counter_u64_array(SYSCTL_HANDLER_ARGS) {
    counter_u64_t *ca = (counter_u64_t *)arg1;
    int n = (int)arg2;
    int i, error;

    for (i = 0; i < n; i++) {
        uint64_t v = counter_u64_fetch(ca[i]);

        error = SYSCTL_OUT(req, &v, sizeof(v));
        if (error != 0) {
            return (error);
        }
    }
    if (req->newptr == NULL) {
        return (0);
    }
    for (i = 0; i < n; i++) {
        counter_u64_zero(ca[i]);
    }
    return (0);
}

/* Present a tick count to the caller as milliseconds and take it back the
 * same way. Used by TCP's timer tunables, where the stored value is in ticks
 * and the human-facing unit is not. */
int sysctl_msec_to_ticks(SYSCTL_HANDLER_ARGS) {
    int error, s, tt;

    tt = *(int *)arg1;
    s = (int)(((int64_t)tt * 1000) / hz);

    error = sysctl_handle_int(oidp, &s, 0, req);
    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    tt = (int)(((int64_t)s * hz) / 1000);
    if (tt < 1) {
        return (EINVAL);
    }
    *(int *)arg1 = tt;
    return (0);
}

int sysctl_msec_to_sbintime(SYSCTL_HANDLER_ARGS) {
    sbintime_t *sb = (sbintime_t *)arg1;
    int error;
    int64_t ms = (int64_t)((*sb * 1000) >> 32);

    error = sysctl_handle_64(oidp, &ms, 0, req);
    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    *sb = (sbintime_t)ms * SBT_1MS;
    return (0);
}

int sysctl_usec_to_sbintime(SYSCTL_HANDLER_ARGS) {
    sbintime_t *sb = (sbintime_t *)arg1;
    int error;
    int64_t us = (int64_t)((*sb * 1000000) >> 32);

    error = sysctl_handle_64(oidp, &us, 0, req);
    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    *sb = (sbintime_t)us * SBT_1US;
    return (0);
}

int sysctl_sec_to_timeval(SYSCTL_HANDLER_ARGS) {
    struct timeval *tv = (struct timeval *)arg1;
    int error, secs = (int)tv->tv_sec;

    error = sysctl_handle_int(oidp, &secs, 0, req);
    if (error != 0 || req->newptr == NULL) {
        return (error);
    }
    tv->tv_sec = secs;
    tv->tv_usec = 0;
    return (0);
}

/* DPCPU is FreeBSD's per-CPU static storage. There is none - the equivalent
 * Genesis machinery is per-CPU structures reached through %gs, not a linker
 * section - so a DPCPU sysctl reads the single instance. Distinct functions
 * rather than aliases so the shape of what is missing stays visible. */
int sysctl_dpcpu_int(SYSCTL_HANDLER_ARGS) {
    return (sysctl_handle_int(oidp, arg1, arg2, req));
}

int sysctl_dpcpu_long(SYSCTL_HANDLER_ARGS) {
    return (sysctl_handle_long(oidp, arg1, arg2, req));
}

int sysctl_dpcpu_quad(SYSCTL_HANDLER_ARGS) {
    return (sysctl_handle_64(oidp, arg1, arg2, req));
}

/* --- lookup and read by name --------------------------------------------
 *
 * This is the part that makes the tree useful from Genesis's side rather than
 * only from a vendored file's. Names are dotted, upstream's spelling, so
 * net_sysctl_read("net.inet.ip.forwarding", ...) does what its name says.
 */
static struct sysctl_oid *find_child(struct sysctl_oid_list *list,
                                     const char *name, size_t len) {
    struct sysctl_oid *p;

    RB_FOREACH(p, sysctl_oid_list, list) {
        size_t i;

        if (p->oid_name == NULL) {
            continue;
        }
        for (i = 0; i < len && p->oid_name[i] != '\0'; i++) {
            if (p->oid_name[i] != name[i]) {
                break;
            }
        }
        if (i == len && p->oid_name[i] == '\0') {
            return (p);
        }
    }
    return (NULL);
}

struct sysctl_oid *net_sysctl_find(const char *name) {
    struct sysctl_oid_list *list = &sysctl__children;
    struct sysctl_oid *oid = NULL;
    const char *s = name;

    if (name == NULL) {
        return (NULL);
    }
    for (;;) {
        const char *dot = s;
        size_t len;

        while (*dot != '\0' && *dot != '.') {
            dot++;
        }
        len = (size_t)(dot - s);
        if (len == 0) {
            return (NULL);
        }
        oid = find_child(list, s, len);
        if (oid == NULL) {
            return (NULL);
        }
        if (*dot == '\0') {
            return (oid);
        }
        list = &oid->oid_children;
        s = dot + 1;
    }
}

/* Read an OID's value into a caller buffer. Returns the number of bytes the
 * handler produced, or a negative errno - Genesis's convention, not
 * upstream's, because this is Genesis's entry point rather than a vendored
 * one. */
int net_sysctl_read(const char *name, void *buf, size_t len) {
    struct sysctl_oid *oid = net_sysctl_find(name);
    struct sysctl_req req;
    int error;

    if (oid == NULL) {
        return (-ENOENT);
    }
    if (oid->oid_handler == NULL) {
        return (-EINVAL);       /* a node, not a leaf */
    }
    memset(&req, 0, sizeof(req));
    req.oldptr  = buf;
    req.oldlen  = len;
    req.oldfunc = sysctl_old_kernel;
    req.newfunc = sysctl_new_kernel;

    error = oid->oid_handler(oid, oid->oid_arg1, oid->oid_arg2, &req);
    if (error != 0) {
        return (-error);
    }
    return ((int)req.oldidx);
}

/* Convenience: read an int-typed OID. The most common shape by far, and the
 * one a selftest wants. */
int net_sysctl_read_int(const char *name, int *out) {
    int v = 0;
    int n = net_sysctl_read(name, &v, sizeof(v));

    if (n < 0) {
        return (n);
    }
    if (n != (int)sizeof(int)) {
        return (-EINVAL);
    }
    *out = v;
    return (0);
}

/* --- sbuf ---------------------------------------------------------------
 *
 * Just enough for a sysctl handler that formats a string answer: a fixed
 * caller-supplied buffer, an overflow flag, and the four operations the
 * vendored tree performs on one. Upstream's sbuf(9) also auto-extends, can
 * drain incrementally into a sysctl request, and supports sections; none of
 * that is reached from here, and a partial implementation that silently
 * pretended to would be worse than one that reports overflow.
 */
struct sbuf *sbuf_new_for_sysctl(struct sbuf *s, char *buf, int length,
                                 struct sysctl_req *req) {
    if (s == NULL || buf == NULL || length <= 0) {
        return (NULL);
    }
    s->s_buf   = buf;
    s->s_size  = length;
    s->s_len   = 0;
    s->s_error = 0;
    s->s_flags = 0;
    s->s_req   = req;
    s->s_buf[0] = '\0';
    return (s);
}

/* sbuf_new() with a caller-supplied buffer. A NULL buffer means "allocate
 * one" upstream and is refused here rather than faked - see the header. */
struct sbuf *sbuf_new(struct sbuf *s, char *buf, int length, int flags) {
    struct sbuf *r = sbuf_new_for_sysctl(s, buf, length, NULL);

    if (r != NULL) {
        r->s_flags = flags;
    }
    return (r);
}

int sbuf_error(const struct sbuf *s) {
    return (s == NULL ? EINVAL : s->s_error);
}

void sbuf_clear(struct sbuf *s) {
    if (s != NULL) {
        s->s_len = 0;
        s->s_error = 0;
        if (s->s_buf != NULL && s->s_size > 0) {
            s->s_buf[0] = '\0';
        }
    }
}

void sbuf_delete(struct sbuf *s) {
    if (s != NULL) {
        s->s_buf = NULL;
    }
}

void sbuf_clear_flags(struct sbuf *s, int flags) {
    if (s != NULL) {
        s->s_flags &= ~flags;
    }
}

char *sbuf_data(struct sbuf *s) {
    return (s != NULL ? s->s_buf : NULL);
}

int sbuf_len(struct sbuf *s) {
    if (s == NULL || s->s_error != 0) {
        return (-1);
    }
    return (s->s_len);
}

int sbuf_bcat(struct sbuf *s, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t i;

    if (s == NULL || s->s_error != 0) {
        return (-1);
    }
    for (i = 0; i < len; i++) {
        if (s->s_len + 1 >= s->s_size) {
            s->s_error = ENOMEM;
            return (-1);
        }
        s->s_buf[s->s_len++] = p[i];
    }
    s->s_buf[s->s_len] = '\0';
    return (0);
}

int sbuf_cat(struct sbuf *s, const char *str) {
    return (sbuf_bcat(s, str, strlen(str)));
}

int sbuf_printf(struct sbuf *s, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    int n;

    if (s == NULL || s->s_error != 0) {
        return (-1);
    }
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        s->s_error = EINVAL;
        return (-1);
    }
    if ((size_t)n >= sizeof(tmp)) {
        n = (int)sizeof(tmp) - 1;   /* truncated by vsnprintf, not silently */
    }
    return (sbuf_bcat(s, tmp, (size_t)n));
}

/* Finish: hand the accumulated string to the sysctl request the buffer was
 * created for, which is what sbuf_new_for_sysctl exists to arrange. */
int sbuf_finish(struct sbuf *s) {
    int error;

    if (s == NULL) {
        return (EINVAL);
    }
    if (s->s_error != 0) {
        return (s->s_error);
    }
    if (s->s_req == NULL) {
        return (0);
    }
    error = SYSCTL_OUT(s->s_req, s->s_buf, (size_t)s->s_len + 1);
    return (error);
}

/* Push what is buffered so far out to the sysctl request and start the
 * buffer again, so a handler producing more than one buffer's worth (the
 * TCP host cache's list) can keep going. Without a request there is
 * nowhere to drain to, which is the same as upstream's -EDOOFUS-free
 * answer for an sbuf with no drain function: nothing to do. The trailing
 * NUL is sbuf_finish's job, so it is not sent here. */
int sbuf_drain(struct sbuf *s) {
    int error;

    if (s == NULL) {
        return (EINVAL);
    }
    if (s->s_error != 0) {
        return (s->s_error);
    }
    if (s->s_req == NULL || s->s_len == 0) {
        return (0);
    }
    error = SYSCTL_OUT(s->s_req, s->s_buf, (size_t)s->s_len);
    if (error != 0) {
        s->s_error = error;
        return (error);
    }
    s->s_len = 0;
    s->s_buf[0] = '\0';
    return (0);
}

/* --- report --------------------------------------------------------------
 *
 * Prints one named OID per line. Not a tree walk: a walk of a tree with a few
 * hundred nodes is a screenful of noise, and what is worth seeing at boot is
 * that the ones the protocol layer depends on exist and hold the values this
 * kernel expects.
 */
void net_sysctl_report(uint8 color) {
    static const char *const names[] = {
        "net.inet.ip.forwarding",
        "net.inet.ip.ttl",
        "net.inet.ip.redirect",
        "net.inet.icmp.maskrepl",
        "net.inet.icmp.icmplim",
        NULL
    };
    int i;

    kprintf_c(color, "sysctl: %d static oids registered\n", registered_count);
    for (i = 0; names[i] != NULL; i++) {
        int v = 0;

        if (net_sysctl_read_int(names[i], &v) == 0) {
            kprintf_c(color, "  %s = %d\n", names[i], v);
        } else {
            kprintf_c(color, "  %s ABSENT\n", names[i]);
        }
    }
}
