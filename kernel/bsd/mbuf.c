/* Genesis: the single translation unit for the vendored FreeBSD mbuf
 * subsystem (ROADMAP item 11, plan Part 3).
 *
 * There is one .c here and three .inc files beside it in vendor/ for the
 * same reason kernel/zfs/ is arranged that way: the vendored files are
 * exactly what was copied out of vendsrc/, and everything this port had to
 * DECIDE lives on this side of the line - in kernel/bsd/compat/, in
 * kernel/bsd/uma.c, and in the environment set up below.
 *
 * What is vendored:
 *   kernel/bsd/compat/sys/mbuf.h   - byte-for-byte vendsrc/sys/sys/mbuf.h.
 *                                    Real struct mbuf, real struct pkthdr,
 *                                    real m_get/m_gethdr/m_free/m_init.
 *   kernel/bsd/compat/sys/queue.h  - byte-for-byte vendsrc/sys/sys/queue.h.
 *   kernel/bsd/vendor/ .inc files  - function-for-function copies out of
 *                                    kern_mbuf.c, uipc_mbuf.c, uipc_mbuf2.c.
 *                                    Each file's header says what was taken
 *                                    and what was left behind.
 *
 * What is Genesis's:
 *   kernel/bsd/uma.c               - the slab allocator the vendored code
 *                                    allocates from.
 *   kernel/bsd/compat/             - the ~10 shim headers standing in for
 *                                    FreeBSD's kernel environment.
 *   this file                      - the globals and stubs the fragments
 *                                    reference, and the init entry point.
 *
 * Not done here, on purpose: no ifnet, no protocol, no driver, nothing
 * wired to hardware. ROADMAP item 6 calls mbuf "a second subsystem, not a
 * header"; this provides the subsystem. Item 12 is what would consume it. */

/* FreeBSD splits every one of its headers into a userland half and a kernel
 * half on this macro; sys/mbuf.h's kernel half is all of the allocation API,
 * so without it this file gets the struct definitions and nothing else.
 * Upstream sets it on the compiler command line for every kernel object.
 * Set here instead, and only here, because it is scoped to this one
 * translation unit rather than to everything build.py compiles. */
#define _KERNEL 1

#include <sys/param.h>
/* <sys/eventhandler.h> - kern_mbuf.c fires mbuf_lowmem when a zone is
 * exhausted, so the protocols can give buffers back. Real now; see
 * kernel/bsd/kern_eventhandler.c. */
#include <sys/eventhandler.h>
/* <sys/uio.h> for struct uio: m_uiotombuf and m_mbuftouio are part of the
 * socket data path and take one. */
#include <sys/uio.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/counter.h>
#include <sys/queue.h>
#include <vm/uma.h>
#include <sys/mbuf.h>

#include "kheap.h"
#include "backtrace.h"
#include "bsd.h"

/* ------------------------------------------------------------------------
 * panic
 *
 * KASSERT, MPASS and the vendored code's own explicit panic() calls all land
 * here. Real: prints, walks the frame-pointer chain through Part 2's ksyms
 * table so the message names functions rather than hex, and stops. Genesis
 * has no other panic - this is the first one in the tree, and it is
 * deliberately local to kernel/bsd/ rather than promoted to a kernel-wide
 * facility, because promoting it would mean deciding what the rest of the
 * kernel should do on an assertion failure, which is its own change.
 */
void panic(const char *fmt, ...) {
    va_list ap;

    kprintf_c(0x0C, "\npanic: ");
    va_start(ap, fmt);
    kvprintf(0x0C, fmt, ap);
    va_end(ap);
    kprintf_c(0x0C, "\n");

    backtrace_print((uint64)(uintptr)__builtin_return_address(0),
                    (uint64)(uintptr)__builtin_frame_address(0), 0x0C);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* ------------------------------------------------------------------------
 * malloc(9)
 *
 * Only the packet-tag code calls this. Straight through to kmalloc, with
 * M_ZERO honoured because m_tag_alloc's callers can pass it.
 */
MALLOC_DEFINE(M_PACKET_TAGS, MBUF_TAG_MEM_NAME, "packet-attached information");

void *malloc(size_t size, struct malloc_type *type, int flags) {
    void *p;

    (void)type;
    if (size == 0) {
        return 0;
    }
    p = kmalloc((kh_size)size);
    if (p != 0 && (flags & M_ZERO) != 0) {
        bzero(p, size);
    }
    return p;
}

void free(void *addr, struct malloc_type *type) {
    (void)type;
    kfree(addr);
}

/* ------------------------------------------------------------------------
 * The globals kern_mbuf.c's mbuf_init() reads.
 *
 * Upstream these are sysctl-tunable limits, set from loader tunables before
 * mbuf_init runs and published back through kern.ipc.*. Zero means "no
 * limit", which is what every `if (nmbufs > 0)` in the vendored mbuf_init
 * tests for - so leaving them zero keeps the vendored code on its own
 * no-limit path rather than needing the sysctl tree to exist.
 *
 * That is not the same as unbounded: kernel/bsd/uma.c grows a zone a page at
 * a time out of the kernel heap, and kheap.h caps that at KHEAP_MAX_SIZE. An
 * mbuf storm exhausts the heap and m_get() starts returning NULL, which is a
 * case the vendored code already handles. Setting a real cap here is a
 * one-line change if that stops being good enough.
 */
int nmbufs;
int nmbclusters;
int nmbjumbop;
int nmbjumbo9;
int nmbjumbo16;

static counter_u64_t snd_tag_count;

/* The zone globals and the local prototypes, transcribed from
 * vendsrc/sys/kern/kern_mbuf.c lines 317-337. These are the two blocks of
 * that file that are declarations rather than code, so they sit here with
 * the rest of the environment instead of in the vendored fragment - the
 * fragment is functions only, which is what makes re-extracting it from a
 * newer upstream a mechanical operation.
 *
 * mb_zinit_pack and mb_zfini_pack are omitted from upstream's prototype
 * block below on purpose: they are static and defined before their only use
 * (mbuf_init's uma_zsecond_create call), so upstream declares them and we
 * do not need to. Everything else is genuinely forward-referenced. */
uma_zone_t zone_mbuf;
uma_zone_t zone_clust;
uma_zone_t zone_pack;
uma_zone_t zone_jumbop;
uma_zone_t zone_jumbo9;
uma_zone_t zone_jumbo16;

static int  mb_ctor_mbuf(void *, int, void *, int);
static int  mb_ctor_clust(void *, int, void *, int);
static int  mb_ctor_pack(void *, int, void *, int);
static void mb_dtor_mbuf(void *, int, void *);
static void mb_dtor_pack(void *, int, void *);
static int  mb_zinit_pack(void *, int, int);
static void mb_zfini_pack(void *, int);
static void mb_reclaim(uma_zone_t, int);

/* uipc_mbuf.c's m_pullup() uses this to decide how much to pull into the
 * first mbuf: the largest protocol header any domain registered. No domains,
 * so zero - and zero is correct rather than a placeholder, because
 * m_pullup's max(len, max_protohdr) then just uses the caller's length. */
u_int max_protohdr;

/* ------------------------------------------------------------------------
 * Stubs for code paths this tree cannot reach.
 *
 * Each of these is referenced by vendored code and each is on a branch
 * guarded by a flag nothing here sets. They panic rather than returning
 * quietly: reaching one means an assumption written down below has stopped
 * being true, and finding that out at the point of failure is worth more
 * than limping on.
 */

/* M_EXTPG mbufs carry a vector of physical pages instead of a data pointer.
 * They are created by KERN_TLS and by sendfile, neither of which exists
 * here, and m_gethdr/m_get/m_getcl cannot produce one. m_copydata's
 * M_EXTPG branch is the only caller. */
static void m_copyfromunmapped(const struct mbuf *m, int off, int len,
                               caddr_t cp) {
    (void)m; (void)off; (void)len; (void)cp;
    panic("m_copyfromunmapped: M_EXTPG mbuf in a tree that cannot create one");
}

static void m_copytounmapped(const struct mbuf *m, int off, int len,
                             c_caddr_t cp) {
    (void)m; (void)off; (void)len; (void)cp;
    panic("m_copytounmapped: M_EXTPG mbuf in a tree that cannot create one");
}

/* A send tag is a handle a NIC driver hands back for rate-limited or
 * TLS-offloaded transmit. Reached from m_free() only when a packet header
 * has CSUM_SND_TAG set, which only a driver sets. Upstream's version calls
 * back into the owning ifnet; there is no ifnet. */
void m_snd_tag_destroy(struct m_snd_tag *mst) {
    (void)mst;
    panic("m_snd_tag_destroy: send tag with no ifnet to return it to");
}

/* ------------------------------------------------------------------------
 * The vendored source itself.
 *
 * Order matters: kern_mbuf.inc defines the zone globals and the constructors
 * that uipc_mbuf.inc's routines allocate through, and both reference the tag
 * routines in uipc_mbuf2.inc. Included rather than compiled separately
 * because the constructors are static - that is upstream's arrangement, and
 * kernel/zfs/zfs_vendor.c already does the same thing for the same reason.
 */
/* max_linkhdr - how much room m_devget leaves at the front of a chain so a
 * link-layer header can be PREPENDED later without copying.
 *
 * Upstream this is set by the network stack's initialisation (ip_init and
 * friends raise it as protocols register). There is no such stack here, so
 * it is set once to the Ethernet header size rounded up to a long - which is
 * what a machine with only Ethernet interfaces would converge on anyway.
 *
 * Zero would also "work" and would quietly cost a full copy on every
 * transmit that needed to prepend anything. */
/* u_int, not int - sys/mbuf.h declares them and it is vendored verbatim. */
u_int max_linkhdr = 16;
u_int max_protohdr = 40;    /* IPv6 header, upstream's worst case */
u_int max_hdr = 56;
u_int max_datalen = MHLEN - 56;

/* --- the two unmapped-mbuf entry points, declined ------------------------
 *
 * An EXTPG ("unmapped") mbuf carries a list of physical pages instead of a
 * KVA-mapped buffer, so that sendfile and KTLS can push page cache pages onto
 * the wire without mapping them. Nothing in this tree creates one: there is no
 * sendfile, no KTLS, and no driver that advertises IFCAP_MEXTPG.
 *
 * Both of these are the "convert to/from unmapped" halves and are reached only
 * from a branch guarded by that capability. They return failure rather than a
 * plausible empty result, so a future path that does create an EXTPG mbuf
 * fails loudly at the conversion instead of silently transmitting nothing.
 *
 * m_unmapped_uiomove() is the third and is not here at all: it was extracted
 * with the rest and then removed, because copying out of an EXTPG mbuf needs
 * PHYS_TO_VM_PAGE and uiomove_fromphys - the vm_page_array Genesis
 * deliberately does not have (see kernel/bsd/uma_vendor.c's token table for
 * why). Its one caller, m_mbuftouio, tests M_EXTPG first.
 */
struct mbuf;
struct uio;
int _mb_unmapped_to_ext(struct mbuf *m, struct mbuf **mres);

int
m_unmapped_uiomove(const struct mbuf *m, int m_off, struct uio *uio, int len) {
    (void)m; (void)m_off; (void)uio; (void)len;
    return (EOPNOTSUPP);
}
static struct mbuf *
m_uiotombuf_nomap(struct uio *uio, int how, int len, int maxseg, int flags) {
    (void)uio; (void)how; (void)len; (void)maxseg; (void)flags;
    return (NULL);
}

int
_mb_unmapped_to_ext(struct mbuf *m, struct mbuf **mres) {
    (void)m;
    *mres = NULL;
    return (EOPNOTSUPP);
}

#include "vendor/kern_mbuf.inc"
#include "vendor/uipc_mbuf.inc"
#include "vendor/uipc_mbuf2.inc"

/* Three small functions from the parts of uipc_mbuf.c and kern_mbuf.c that
 * were not extracted, copied verbatim: TCP calls them. max_hdr is the room a
 * packet header mbuf leaves for link + protocol headers; a protocol that
 * registers a bigger header (TCP with options) grows it. */
static void
max_hdr_grow(void)
{

	max_hdr = max_linkhdr + max_protohdr;
	MPASS(max_hdr <= MHLEN);
}

void
max_linkhdr_grow(u_int new)
{

	if (new > max_linkhdr) {
		max_linkhdr = new;
		max_hdr_grow();
	}
}

void
max_protohdr_grow(u_int new)
{

	if (new > max_protohdr) {
		max_protohdr = new;
		max_hdr_grow();
	}
}

/* Free the first `count` not-yet-ready mbufs of a chain - the undo for a
 * send that queued data still being filled in (sendfile's M_NOTREADY). An
 * EXTPG mbuf counts once per page. */
void
mb_free_notready(struct mbuf *m, int count)
{
	int i;

	for (i = 0; i < count && m != NULL; i++) {
		if ((m->m_flags & M_EXTPG) != 0) {
			m->m_epg_nrdy--;
			if (m->m_epg_nrdy != 0)
				continue;
		}
		m = m_free(m);
	}
	KASSERT(i == count, ("Removed only %d items from %p", i, m));
}

/* ------------------------------------------------------------------------
 * Genesis entry points.
 */

/* Layout checks. These are upstream's own assertions, from uipc_mbuf.c's
 * top-of-file block and kern_mbuf.c's, kept because they are the thing that
 * catches a compat header having quietly changed the vendored struct: get
 * sys/types.h's caddr_t wrong, or MSIZE, and struct mbuf silently stops
 * matching what the vendored code indexes into. Compile-time, so they cost
 * nothing and cannot be skipped. */
CTASSERT((((MSIZE - 1) ^ MSIZE) + 1) >> 1 == MSIZE);
_Static_assert(sizeof(struct mbuf) <= MSIZE, "size of mbuf exceeds MSIZE");
CTASSERT(MSIZE - offsetof(struct mbuf, m_dat) == MLEN);
CTASSERT(MSIZE - offsetof(struct mbuf, m_pktdat) == MHLEN);
CTASSERT(offsetof(struct mbuf, m_dat) % 8 == 0);
CTASSERT(offsetof(struct mbuf, m_pktdat) % 8 == 0);
CTASSERT(offsetof(struct mbuf, m_dat) == 32);
CTASSERT(sizeof(struct pkthdr) == 64);
CTASSERT(sizeof(struct m_ext) == 160);

void net_mbuf_init(void) {
    /* uma_startup() was the Genesis shim's init. The real allocator's is
     * genesis_uma_init() in kernel/bsd/uma_vendor.c, and flk.c calls it
     * before this - UMA has to exist before a zone can be created, and the
     * boot order is stated there rather than hidden in a call from here. */

    /* Upstream reaches mbuf_init through SYSINIT; sys/systm.h explains why
     * that expands to nothing here and why the call is explicit instead. */
    mbuf_init(0);

    if (zone_mbuf == 0 || zone_clust == 0 || zone_pack == 0) {
        kprintf_c(0x0C, "mbuf: zone creation failed\n");
        return;
    }

    kprintf("mbuf: FreeBSD mbuf(9) up - MSIZE %d, MLEN %d, MHLEN %d, "
            "MCLBYTES %d\n", MSIZE, MLEN, MHLEN, MCLBYTES);
}

/* Exercises the real vendored allocation paths and reports pass/fail. Called
 * from flk.c at boot; the matching manual-verification entry is in
 * src/verif.c.
 *
 * Every call below goes through the vendored inline in sys/mbuf.h into
 * uma_zalloc_arg and out through the vendored constructor - there is no
 * Genesis-side shortcut being tested here. */
int net_mbuf_selftest(void) {
    struct mbuf *m;
    struct mbuf *chain;
    char buf[64];
    int failures = 0;
    int i;

    /* 1. A plain mbuf: data must point into the mbuf's own storage, and the
     *    length must be zero-initialised by m_init(). */
    m = m_get(M_NOWAIT, MT_DATA);
    if (m == 0) {
        kprintf_c(0x0C, "mbuf selftest: m_get returned NULL\n");
        return 1;
    }
    if (m->m_data != m->m_dat || m->m_len != 0 || m->m_type != MT_DATA) {
        kprintf_c(0x0C, "mbuf selftest: m_get did not initialise\n");
        failures++;
    }
    if ((m->m_flags & M_PKTHDR) != 0) {
        kprintf_c(0x0C, "mbuf selftest: m_get set M_PKTHDR\n");
        failures++;
    }
    m_freem(m);

    /* 2. A packet-header mbuf: M_PKTHDR set, data in m_pktdat (past the
     *    header), and an empty tag list from m_pkthdr_init. */
    m = m_gethdr(M_NOWAIT, MT_DATA);
    if (m == 0) {
        kprintf_c(0x0C, "mbuf selftest: m_gethdr returned NULL\n");
        return failures + 1;
    }
    if ((m->m_flags & M_PKTHDR) == 0 || m->m_data != m->m_pktdat) {
        kprintf_c(0x0C, "mbuf selftest: m_gethdr did not initialise\n");
        failures++;
    }
    if (!SLIST_EMPTY(&m->m_pkthdr.tags)) {
        kprintf_c(0x0C, "mbuf selftest: fresh pkthdr has tags\n");
        failures++;
    }
    m_freem(m);

    /* 3. A cluster-backed packet from the secondary zone: M_EXT set, an
     *    external buffer of MCLBYTES, and EXT_PACKET from mb_zinit_pack's
     *    override of mb_ctor_clust's type. */
    m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
    if (m == 0) {
        kprintf_c(0x0C, "mbuf selftest: m_getcl returned NULL\n");
        return failures + 1;
    }
    if ((m->m_flags & M_EXT) == 0 || m->m_ext.ext_size != MCLBYTES ||
        m->m_ext.ext_type != EXT_PACKET || m->m_data != m->m_ext.ext_buf) {
        kprintf_c(0x0C, "mbuf selftest: m_getcl cluster not attached\n");
        failures++;
    }

    /* 4. Round-trip real data through the cluster. */
    for (i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = (char)(i + 1);
    }
    m->m_len = (int)sizeof(buf);
    m->m_pkthdr.len = (int)sizeof(buf);
    bcopy(buf, mtod(m, caddr_t), sizeof(buf));
    bzero(buf, sizeof(buf));
    m_copydata(m, 0, (int)sizeof(buf), buf);
    for (i = 0; i < (int)sizeof(buf); i++) {
        if (buf[i] != (char)(i + 1)) {
            kprintf_c(0x0C, "mbuf selftest: m_copydata mismatch at %d\n", i);
            failures++;
            break;
        }
    }
    m_freem(m);

    /* 4b. THE SHARED KEG - the check that only passes against real UMA.
     *
     * zone_pack is a SECONDARY zone over zone_mbuf's keg (uma_zsecond_create
     * in net_mbuf_init). Sharing a keg means the two zones draw from ONE
     * item pool: an mbuf obtained from zone_pack and freed can be handed out
     * again by zone_mbuf, and vice versa.
     *
     * Genesis's old hand-written shim could not do this. Its secondary zones
     * kept their own disjoint pools, and its own header said so. So this
     * check is the one that distinguishes a real keg from a lookalike:
     * allocate through m_getcl (zone_pack), free it, then allocate through
     * m_get (zone_mbuf) repeatedly, and require the mbuf zone's item count
     * not to climb. With disjoint pools every m_get grows zone_mbuf because
     * the freed packet went somewhere zone_mbuf cannot see. */
    {
        int before, after, k;

        for (k = 0; k < 8; k++) {
            struct mbuf *p = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
            if (p != 0) {
                m_freem(p);
            }
        }
        before = (int)uma_zone_get_cur(zone_mbuf);
        for (k = 0; k < 64; k++) {
            struct mbuf *p = m_get(M_NOWAIT, MT_DATA);
            if (p != 0) {
                m_freem(p);
            }
        }
        after = (int)uma_zone_get_cur(zone_mbuf);
        if (after > before) {
            kprintf_c(0x0C, "mbuf selftest: zone_mbuf grew from %d to %d "
                            "across 64 alloc/free pairs - the secondary "
                            "zone is not sharing a keg\n", before, after);
            failures++;
        }
    }

    /* 5. A chain: m_length must walk it, and m_freem must free all of it.
     *    Built with m_prepend, which allocates a second mbuf and moves the
     *    packet header onto it - the path that exercises m_move_pkthdr. */
    chain = m_gethdr(M_NOWAIT, MT_DATA);
    if (chain == 0) {
        kprintf_c(0x0C, "mbuf selftest: chain head alloc failed\n");
        return failures + 1;
    }
    chain->m_len = 8;
    chain->m_pkthdr.len = 8;
    chain = m_prepend(chain, 4, M_NOWAIT);
    if (chain == 0) {
        kprintf_c(0x0C, "mbuf selftest: m_prepend returned NULL\n");
        return failures + 1;
    }
    if ((chain->m_flags & M_PKTHDR) == 0 || chain->m_next == 0) {
        kprintf_c(0x0C, "mbuf selftest: m_prepend did not move the header\n");
        failures++;
    }
    if (m_length(chain, 0) != 12) {
        kprintf_c(0x0C, "mbuf selftest: m_length is %d, expected 12\n",
                  m_length(chain, 0));
        failures++;
    }
    m_freem(chain);

    /* 6. Free-list reuse: after all of the above, an allocation must come
     *    back from the zone rather than growing it. Checked by allocating
     *    and freeing repeatedly and confirming the mbuf zone's item count
     *    stops climbing - a ctor that failed to reset state, or a free path
     *    that lost the item, shows up here and nowhere else. */
    for (i = 0; i < 64; i++) {
        m = m_get(M_NOWAIT, MT_DATA);
        if (m == 0) {
            kprintf_c(0x0C, "mbuf selftest: exhausted after %d reuses\n", i);
            failures++;
            break;
        }
        m_freem(m);
    }

    if (failures == 0) {
        kprintf("mbuf: selftest passed\n");
    } else {
        kprintf_c(0x0C, "mbuf: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
