/* Asynchronous I/O - see iomgr.h.
 *
 * Pending requests sit on one list, oldest first. The completion thread
 * parks on the readiness queue (waitq.h) - every readiness change in the
 * kernel wakes it - and completes whatever has become ready, in order. The
 * read itself goes into a kernel bounce buffer and is then copied out into
 * the issuing process's pages through the direct map, because the thread
 * doing it is not running in that address space.
 *
 * Everything here runs under the big kernel lock: the issuing syscall, the
 * completion thread (a kernel thread takes it to run), and the teardown
 * hooks. */

#include "iomgr.h"
#include "dispatch.h"
#include "kheap.h"
#include "kprintf.h"
#include "kthread.h"
#include "npfs.h"
#include "nt.h"
#include "nt_context.h"
#include "ntsync.h"
#include "object.h"
#include "paging.h"
#include "process.h"
#include "waitq.h"

typedef struct irp {
    struct irp      *next;
    int              kind;
    open_file_t     *of;          /* not referenced: iomgr_file_closed   */
    object_t        *obj;         /* referenced                          */
    address_space_t *space;
    int              tgid;
    int              tid;
    uint64           buf;
    uint32           len;
    uint64           offset;
    io_notify_t      n;           /* n.event referenced                  */
    object_t        *port;        /* referenced, or NULL                 */
    uint64           key;
} irp_t;

static irp_t *pending;
static process_t *worker;

/* --- copying into another address space -------------------------------- */

int iomgr_copy_out(address_space_t *as, uint64 uva, const void *src,
                   uint64 n) {
    const uint8 *s = (const uint8 *)src;

    while (n > 0) {
        uint64 page = uva & ~0xFFFULL, off = uva & 0xFFFULL;
        uint64 chunk = 0x1000 - off;
        uint64 flags = vmm_get_flags_in(as, page);
        phys_addr_t phys;
        uint8 *d;
        uint64 i;

        if (chunk > n) {
            chunk = n;
        }
        /* Present, user, writable - and not copy-on-write, which a write
         * through the direct map would make the other sharer's too. */
        if ((flags & (PAGE_PRESENT | PAGE_USER | PAGE_RW)) !=
                (PAGE_PRESENT | PAGE_USER | PAGE_RW) ||
            (flags & PAGE_COW)) {
            return -14;
        }
        phys = vmm_get_phys_in(as, page);
        if (phys == 0) {
            return -14;
        }
        d = (uint8 *)phys_to_virt(phys) + off;
        for (i = 0; i < chunk; i++) {
            d[i] = s[i];
        }
        s += chunk;
        uva += chunk;
        n -= chunk;
    }
    return 0;
}

/* --- reporting ------------------------------------------------------------ */

static void notify(address_space_t *space, int tid, const io_notify_t *n,
                   object_t *port, uint64 key, int skip_event,
                   uint32 status, uint64 info) {
    uint64 iosb[2];

    iosb[0] = status;
    iosb[1] = info;
    if (n->iosb != 0) {
        (void)iomgr_copy_out(space, n->iosb, iosb, sizeof(iosb));
    }
    if (n->event != NULL && !skip_event && n->event->type != NULL &&
        n->event->type->signal != NULL) {
        (void)n->event->type->signal(n->event, OB_SIG_SET, 0, NULL);
    }
    if (n->apc != 0) {
        process_t *t = proc_find(tid);

        /* PIO_APC_ROUTINE(ApcContext, IoStatusBlock, Reserved). */
        if (t != NULL && t->state != PROC_UNUSED && t->state != PROC_ZOMBIE) {
            (void)nt_apc_queue(t, n->apc, n->apc_ctx, n->iosb, 0);
        }
    } else if (port != NULL) {
        /* A file with a completion port posts its completions there, the
         * ApcContext (the OVERLAPPED) as the packet's context. */
        io_packet_t pk;

        pk.key = key;
        pk.apc_context = n->apc_ctx;
        pk.status = status;
        pk.information = info;
        (void)io_completion_post(port, &pk);
    }
}

void iomgr_complete_now(open_file_t *of, const io_notify_t *n, uint32 status,
                        uint64 information, int immediate) {
    process_t *me = proc_current();
    object_t *port = of->port;

    if (immediate && (of->nt_flags & OF_NT_SKIP_PORT_SUCCESS) &&
        (int32)status >= 0) {
        port = NULL;
    }
    notify(me->space, me->pid, n, port, of->port_key,
           (of->nt_flags & OF_NT_SKIP_SET_EVENT) && n->event == NULL,
           status, information);
}

/* --- the queue ------------------------------------------------------------ */

static void irp_free(irp_t *r) {
    if (r->obj != NULL) {
        ob_deref(r->obj);
    }
    if (r->n.event != NULL) {
        ob_deref(r->n.event);
    }
    if (r->port != NULL) {
        ob_deref(r->port);
    }
    kfree(r);
}

uint32 iomgr_queue(open_file_t *of, int kind, uint64 buf, uint32 len,
                   uint64 offset, const io_notify_t *n) {
    process_t *me = proc_current();
    irp_t *r, **at;

    if (worker == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    r = (irp_t *)kcalloc(1, sizeof(*r));
    if (r == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    r->kind = kind;
    r->of = of;
    r->obj = of->obj;
    ob_ref(r->obj);
    r->space = me->space;
    r->tgid = me->tgid;
    r->tid = me->pid;
    r->buf = buf;
    r->len = len;
    r->offset = offset;
    r->n = *n;
    if (r->n.event != NULL) {
        ob_ref(r->n.event);
    }
    r->port = of->port;
    if (r->port != NULL) {
        ob_ref(r->port);
    }
    r->key = of->port_key;
    for (at = &pending; *at != NULL; at = &(*at)->next) {
    }
    *at = r;
    if (n->iosb != 0) {
        uint64 iosb[2];

        iosb[0] = STATUS_PENDING;
        iosb[1] = 0;
        (void)iomgr_copy_out(me->space, n->iosb, iosb, sizeof(iosb));
    }
    waitq_wake_all(waitq_readiness());       /* the worker re-scans */
    return STATUS_PENDING;
}

static int irp_ready(irp_t *r) {
    if (r->kind == IRP_READ) {
        return (ob_poll(r->obj, OB_POLLIN) &
                (OB_POLLIN | OB_POLLHUP | OB_POLLERR)) != 0;
    }
    if (r->kind == IRP_LISTEN) {
        return npfs_listen_poll(r->obj) != -11;
    }
    return 1;
}

/* Do a ready request and report it. */
static void irp_run(irp_t *r) {
    uint32 status = STATUS_SUCCESS;
    uint64 info = 0;

    if (r->kind == IRP_READ) {
        uint8 *k = (uint8 *)kmalloc(r->len != 0 ? r->len : 1);
        int64 got;
        uint64 off = r->offset;

        if (k == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            got = r->obj->type->read(r->obj, k, r->len, &off);
            if (got > 0) {
                if (iomgr_copy_out(r->space, r->buf, k, (uint64)got) != 0) {
                    status = STATUS_ACCESS_VIOLATION;
                } else {
                    info = (uint64)got;
                }
            } else if (got == 0) {
                status = r->obj->type->klass == OBJ_PIPE ? STATUS_PIPE_BROKEN
                                                          : STATUS_END_OF_FILE;
            } else {
                status = got == -107 ? STATUS_PIPE_LISTENING
                       : got == -108 ? STATUS_PIPE_DISCONNECTED
                                     : STATUS_ACCESS_DENIED;
            }
            kfree(k);
        }
    } else if (r->kind == IRP_LISTEN) {
        int rc = npfs_listen_poll(r->obj);

        status = rc == 0   ? STATUS_SUCCESS
               : rc == -32 ? STATUS_PIPE_CLOSING
                           : STATUS_PIPE_BROKEN;
    }
    notify(r->space, r->tid, &r->n, r->port, r->key, 0, status, info);
}

static int any_ready(void *unused) {
    irp_t *r;

    (void)unused;
    for (r = pending; r != NULL; r = r->next) {
        if (irp_ready(r)) {
            return 1;
        }
    }
    return 0;
}

static void worker_main(void *unused) {
    (void)unused;
    for (;;) {
        irp_t **at;

        (void)waitq_wait(waitq_readiness(), any_ready, NULL);
        /* One ready request per pass from the head, then look again: a
         * completion can change what else is ready (and the list). */
        for (at = &pending; *at != NULL; at = &(*at)->next) {
            irp_t *r = *at;

            if (irp_ready(r)) {
                *at = r->next;
                irp_run(r);
                irp_free(r);
                break;
            }
        }
    }
}

void iomgr_init(void) {
    worker = kthread_create(worker_main, NULL, "iomgr");
    if (worker == NULL) {
        kprintf_c(0x0C, "iomgr: no kernel thread - overlapped I/O will "
                        "fail\n");
    }
}

/* --- cancellation ---------------------------------------------------------- */

static int cancel_where(open_file_t *of, address_space_t *as, int tgid,
                        uint64 iosb, int match_iosb, int report) {
    irp_t **at = &pending;
    int n = 0;

    while (*at != NULL) {
        irp_t *r = *at;
        int hit = (of == NULL || r->of == of) &&
                  (as == NULL || r->space == as) &&
                  (tgid == 0 || r->tgid == tgid) &&
                  (!match_iosb || r->n.iosb == iosb);

        if (!hit) {
            at = &r->next;
            continue;
        }
        *at = r->next;
        if (report) {
            notify(r->space, r->tid, &r->n, r->port, r->key, 0,
                   STATUS_CANCELLED, 0);
        }
        irp_free(r);
        n++;
    }
    return n;
}

int iomgr_cancel(open_file_t *of, uint64 iosb, int match_iosb) {
    return cancel_where(of, NULL, proc_current()->tgid, iosb, match_iosb, 1);
}

void iomgr_file_closed(open_file_t *of) {
    (void)cancel_where(of, NULL, 0, 0, 0, 1);
}

void iomgr_space_gone(address_space_t *as) {
    (void)cancel_where(NULL, as, 0, 0, 0, 0);
}
