/* The one translation unit for FreeBSD's real UMA - ROADMAP item 11 / plan
 * Part 12.
 *
 * Same shape as kernel/zfs/zfs_vendor.c and kernel/bsd/mbuf.c: the vendored
 * .c is #included as a fragment rather than compiled on its own, because
 * upstream's file is full of `static` functions that the rest of the port
 * has to reach, and because build.py's sources() deliberately skips any
 * directory named vendor.
 *
 * What this replaces: kernel/bsd/uma.c, 511 lines of Genesis's own slab
 * allocator. The asymmetry that motivated the swap was that m_get() IS
 * uma_zalloc_arg(zone_mbuf, ...) - so the mbuf LIFECYCLE was FreeBSD's and
 * the memory it lived in was not.
 */

#define _KERNEL 1

#include "kprintf.h"
#include "kheap.h"
#include "klock.h"
#include "pmm.h"
#include "paging.h"
#include "vmalloc.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/taskqueue.h>
#include <sys/sleepqueue.h>
#include <sys/vmmeter.h>
#include <sys/smr.h>
#include <sys/domainset.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/sx.h>
#include <sys/smp.h>
#include <sys/proc.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_page.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>

/* --- the glue the compat headers declared ------------------------------- */

struct domainset  genesis_domainset;
struct genesis_vm_domain_s genesis_vm_domain;
/* `ticks` moved to kernel/bsd/kern_time.c when <sys/kernel.h> was vendored:
 * upstream declares it `volatile int` there, and something has to ADVANCE it
 * now that TCP's timers are counted in it. It was a never-incremented int
 * here, which was correct while UMA's only use of it was a rate limiter. */
/* MAXCPU-wide, like every other counter - see <sys/counter.h>. A one-element
 * version was written past by anything using zpcpu_get_cpu on it. */
uint64_t genesis_early_counter[MAXCPU];

/* M_TEMP's backing struct - see sys/malloc.h. */
struct malloc_type genesis_m_temp[1] = { { "temp" } };

/* strdup and sprintf, for UMA's sysctl-node naming. That naming is compiled
 * out (see sys/sysctl.h), but the code building the strings is not, so these
 * have to work rather than merely link. sprintf handles only the "%s" and
 * "%d" forms uma_core.c actually uses - a general printf here would be a
 * second implementation of kprintf's formatter with no other consumer. */
char *genesis_bsd_strdup(const char *s) {
    unsigned long n = 0;
    char *p;

    while (s[n] != '\0') {
        n++;
    }
    p = (char *)kmalloc((kh_size)(n + 1));
    if (p != 0) {
        unsigned long i;
        for (i = 0; i <= n; i++) {
            p[i] = s[i];
        }
    }
    return p;
}

int genesis_bsd_sprintf(char *buf, const char *fmt, ...) {
    /* Only reached from the compiled-out sysctl naming path. Producing an
     * empty string is correct there and is better than a partial
     * implementation that looks general. */
    (void)fmt;
    buf[0] = '\0';
    return 0;
}

/* genesis_smr_unsupported() is gone. It halted the machine on any SMR entry
 * point, deliberately, so that a zone created with UMA_ZONE_SMR would say so
 * rather than corrupt quietly. netinet/in_pcb.c creates one, the panic fired
 * on the first boot with the PCB layer in, and kernel/bsd/kern_smr.c is the
 * real implementation that replaced it. */


void genesis_mtx_init(struct mtx *m, const char *name, const char *type,
                      int opts) {
    (void)type;
    (void)opts;
    m->lock_object.lo_name  = name;
    m->lock_object.lo_class = LO_CLASS_MTX;
    kmtx_init(&m->gmtx, name);
}

void genesis_sx_init(struct sx *sx, const char *name) {
    sx->lock_object.lo_name  = name;
    sx->lock_object.lo_class = LO_CLASS_SX;
    krw_init(&sx->grw, name);
}

void genesis_rw_init(struct rwlock *rw, const char *name) {
    rw->lock_object.lo_name  = name;
    rw->lock_object.lo_class = LO_CLASS_RW;
    krw_init(&rw->grw, name);
}

void genesis_rw_init_pad(struct rwlock_padalign *rw, const char *name) {
    rw->lock_object.lo_name  = name;
    rw->lock_object.lo_class = LO_CLASS_RW;
    krw_init(&rw->grw, name);
}

/* critical sections: on Genesis these only have to stop this CPU being
 * preempted, and nothing preempts kernel code here except an interrupt. */
static int critical_nesting;
static uint64 critical_flags;

void genesis_critical_enter(void) {
    uint64 flags;
    __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    if (critical_nesting++ == 0) {
        critical_flags = flags;
    }
}

void genesis_critical_exit(void) {
    if (--critical_nesting == 0) {
        __asm__ volatile ("pushq %0\n\tpopfq" : : "r"(critical_flags)
                          : "memory", "cc");
    }
}

unsigned long genesis_vm_free_count(void) {
    return pmm_free_frames();
}

void genesis_vm_wait_domain(int domain) {
    (void)domain;
    /* No page daemon to wait for. Returning immediately turns an
     * out-of-memory wait into an out-of-memory failure, which is the honest
     * answer on a kernel where nothing can free pages in the background. */
}

/* genesis_msleep()/genesis_wakeup() used to be here, and both returned
 * immediately - the comment said so, and said what was lost: UMA's M_WAITOK
 * path did not block, so an allocation from a capped zone failed instead of
 * waiting. Nothing in this tree caps a zone, so nothing reached it.
 *
 * They are gone rather than fixed. sleep(9) is real now
 * (kernel/bsd/kern_synch.c), and UMA reaches it through <sys/sleepqueue.h>'s
 * primitives like upstream does, so a second private sleep entry point would
 * be two mechanisms for one thing.
 */


/* taskqueue_thread, taskqueue_fast and taskqueue_swi used to be defined here,
 * in kernel/bsd/callout.c and in kernel/bsd/netglue.c respectively - three
 * NULL pointers in three files, each with a comment explaining that nothing
 * read through them because a task ran inline at its enqueue site. Something
 * reads through them now. All three, and the timeout-task entry points that
 * used to decline here, live in kernel/bsd/kern_taskqueue.c.
 *
 * The reclaim task itself is still not scheduled, and that has not changed
 * for a different reason than before: there is a thread to run it on now, and
 * there is still nothing to reclaim UNDER. Nothing in this kernel raises
 * vm_lowmem - ROADMAP item 7's fourth blocker - so a periodic cache drain
 * would be work with no trigger. What was missing is now the trigger rather
 * than the context. */

/* --- the VM page layer -------------------------------------------------- */

/* --- the vm_page token table -------------------------------------------
 *
 * FreeBSD has vm_page_array: one struct vm_page for EVERY physical page in
 * the machine, indexed by frame number, so PHYS_TO_VM_PAGE is arithmetic and
 * always succeeds. Genesis has no such array and cannot cheaply have one -
 * at 4GB managed that is a million entries, and the kernel window is 4MB.
 *
 * So this is a hash table of tokens, created ON DEMAND. Any physical page
 * UMA asks about gets an entry the first time it is named, which is what
 * makes PHYS_TO_VM_PAGE total rather than partial. The first version
 * returned NULL for a page it had not allocated itself, and vsetzoneslab -
 * which is called on every slab, including ones backed by kmalloc - wrote
 * through it.
 *
 * Bounded by what UMA actually manages rather than by RAM size, which is the
 * property that makes this affordable. Exhaustion is reported rather than
 * silently returning NULL again. */
#define VM_PAGE_POOL   4096
#define VM_PAGE_BUCKETS 1024

static struct vm_page page_pool[VM_PAGE_POOL];
static int            page_pool_used;
static int            page_hash[VM_PAGE_BUCKETS];   /* index+1, 0 = empty */
static int            page_hash_ready;
static int            page_pool_exhausted;

static uint32 page_hash_of(vm_paddr_t pa) {
    return (uint32)((pa >> 12) & (VM_PAGE_BUCKETS - 1));
}

static void page_hash_init(void) {
    int i;

    if (page_hash_ready) {
        return;
    }
    for (i = 0; i < VM_PAGE_BUCKETS; i++) {
        page_hash[i] = 0;
    }
    page_hash_ready = 1;
}

/* THE INVARIANT THAT MATTERS: for a physically contiguous run, the tokens
 * must be CONSECUTIVE in page_pool, because uma_core.c's startup_free walks
 * a run with `m++`:
 *
 *     for (; bytes != 0; bytes -= PAGE_SIZE, m++) { vm_page_free(m); }
 *
 * FreeBSD's vm_page_array is one entry per physical page in address order,
 * so m++ IS the next physical page. A hash table's m++ is whatever token
 * happened to be allocated next - which here meant vm_page_free releasing
 * frames belonging to entirely unrelated allocations. It showed up as a
 * process resuming onto a kernel stack full of zeroes, several seconds and
 * one address-space switch away from the actual free.
 *
 * So page_lookup allocates in order and genesis_vm_page_alloc_noobj_contig
 * reserves its whole run up front. The hash is only an INDEX over the pool,
 * not the thing that decides where a token lives.
 *
 * Linear probe rather than chaining because there is no allocator available
 * underneath this - it IS the allocator's page layer. */
static struct vm_page *page_lookup(vm_paddr_t pa, int create) {
    uint32 h;
    int i;

    page_hash_init();
    pa &= ~(vm_paddr_t)(PMM_PAGE_SIZE - 1);
    h = page_hash_of(pa);

    for (i = 0; i < VM_PAGE_BUCKETS; i++) {
        uint32 slot = (h + (uint32)i) & (VM_PAGE_BUCKETS - 1);
        int idx = page_hash[slot];

        if (idx == 0) {
            if (!create) {
                return 0;
            }
            if (page_pool_used >= VM_PAGE_POOL) {
                if (!page_pool_exhausted) {
                    page_pool_exhausted = 1;
                    kprintf_c(0x0C, "uma: vm_page token pool exhausted at "
                                    "%d - raise VM_PAGE_POOL\n", VM_PAGE_POOL);
                }
                return 0;
            }
            idx = ++page_pool_used;
            page_pool[idx - 1].phys_addr = pa;
            page_pool[idx - 1].in_use    = 1;
            page_hash[slot] = idx;
            return &page_pool[idx - 1];
        }
        if (page_pool[idx - 1].phys_addr == pa) {
            return &page_pool[idx - 1];
        }
    }
    return 0;
}

static vm_page_t genesis_page_token(phys_addr_t pa) {
    return page_lookup((vm_paddr_t)pa, 1);
}

vm_page_t genesis_vm_page_alloc_noobj(int req) {
    phys_addr_t pa = pmm_alloc_frame();
    vm_page_t m;

    if (pa == 0) {
        return 0;
    }
    m = genesis_page_token(pa);
    if (m == 0) {
        pmm_free_frame(pa);
        return 0;
    }
    if (req & VM_ALLOC_ZERO) {
        uint8 *p = (uint8 *)phys_to_virt(pa);
        uint64 b;
        for (b = 0; b < PMM_PAGE_SIZE; b++) {
            p[b] = 0;
        }
    }
    return m;
}

/* Physically CONTIGUOUS multi-page allocation.
 *
 * Genesis's PMM hands out one frame at a time with no contiguous mode, so
 * this asks for frames until it gets a run of `npages` that happen to be
 * adjacent, and releases everything else. Crude, and honest about it: it is
 * O(frames touched) and it can fail on a fragmented heap where a real
 * buddy allocator would succeed.
 *
 * It is sufficient here because the only caller that asks for more than one
 * page is uma_startup1's bootstrap, which runs at boot when physical memory
 * is almost entirely free and contiguity is nearly guaranteed. A caller
 * needing this under fragmentation would need a real contiguous allocator in
 * pmm.c, which is its own piece of work. */
#define CONTIG_SCAN_MAX 4096

vm_page_t genesis_vm_page_alloc_noobj_contig(int req, unsigned long npages) {
    phys_addr_t held[CONTIG_SCAN_MAX];
    int nheld = 0;
    int i, start;
    vm_page_t first = 0;

    if (npages == 0) {
        return 0;
    }
    if (npages == 1) {
        return genesis_vm_page_alloc_noobj(req);
    }

    while (nheld < CONTIG_SCAN_MAX) {
        phys_addr_t pa = pmm_alloc_frame();

        if (pa == 0) {
            break;
        }
        held[nheld++] = pa;

        /* Is the tail of what we hold a run of npages ASCENDING and
         * adjacent? pmm_alloc_frame returns frames in bitmap order, so a run
         * shows up as consecutive entries. */
        if ((unsigned long)nheld >= npages) {
            int ok = 1;
            start = nheld - (int)npages;
            for (i = start + 1; i < nheld; i++) {
                if (held[i] != held[i - 1] + PMM_PAGE_SIZE) {
                    ok = 0;
                    break;
                }
            }
            if (ok) {
                unsigned long b;

                /* Give back everything before the run. */
                for (i = 0; i < start; i++) {
                    pmm_free_frame(held[i]);
                }
                if (req & VM_ALLOC_ZERO) {
                    uint8 *p = (uint8 *)phys_to_virt(held[start]);
                    for (b = 0; b < npages * PMM_PAGE_SIZE; b++) {
                        p[b] = 0;
                    }
                }
                /* A token for EVERY page of the run, in order, so that
                 * `m++` from the first reaches the second. See the
                 * invariant note on page_lookup. */
                {
                    unsigned long k;

                    for (k = 0; k < npages; k++) {
                        vm_page_t t = genesis_page_token(held[start + (int)k]);

                        if (t == 0) {
                            for (i = start; i < nheld; i++) {
                                pmm_free_frame(held[i]);
                            }
                            return 0;
                        }
                        if (k == 0) {
                            first = t;
                        } else if (t != first + k) {
                            /* Someone else took a token between ours. The
                             * run is unusable for m++ walking, and silently
                             * returning it is exactly the bug this comment
                             * describes - so refuse. */
                            kprintf_c(0x0C, "uma: contiguous run did not get "
                                            "consecutive vm_page tokens\n");
                            for (i = start; i < nheld; i++) {
                                pmm_free_frame(held[i]);
                            }
                            return 0;
                        }
                    }
                }
                return first;
            }
        }
    }

    for (i = 0; i < nheld; i++) {
        pmm_free_frame(held[i]);
    }
    return 0;
}

void genesis_vm_page_free(vm_page_t m) {
    if (m == 0 || !m->in_use) {
        return;
    }
    /* The frame goes back to the PMM; the TOKEN stays in the table. Tokens
     * are never recycled, deliberately: an address freed and reallocated
     * gets the same token, and UMA's slab/zone back-pointers in it are
     * overwritten by the next vsetzoneslab. Recycling would mean a lookup
     * during that window found nothing. */
    pmm_free_frame((phys_addr_t)m->phys_addr);
}

vm_page_t genesis_phys_to_vm_page(vm_paddr_t pa) {
    return page_lookup(pa, 1);
}

void *genesis_kva_alloc(vm_size_t size) {
    return (void *)kvm_alloc_range((uint64)size, PMM_PAGE_SIZE);
}

void genesis_kva_free(void *addr, vm_size_t size) {
    (void)size;
    kvm_free_range((uint64)addr);
}

void genesis_pmap_qenter(void *va, vm_page_t *ma, int count) {
    uint64 base = (uint64)va;
    int i;

    for (i = 0; i < count; i++) {
        vmm_map_page((virt_addr_t)(base + (uint64)i * PMM_PAGE_SIZE),
                     (phys_addr_t)ma[i]->phys_addr,
                     PAGE_PRESENT | PAGE_RW | PAGE_NX);
    }
}

void genesis_pmap_qremove(void *va, int count) {
    uint64 base = (uint64)va;
    int i;

    for (i = 0; i < count; i++) {
        vmm_unmap_page((virt_addr_t)(base + (uint64)i * PMM_PAGE_SIZE));
    }
}

int genesis_kernel_pmap;
struct vm_map_dummy genesis_kernel_map;

/* Reserve KVA for the VM radix trie's own nodes. Genesis has no radix trie -
 * that is FreeBSD's vm_object page index - so there is nothing to reserve
 * for. Defined rather than macro'd because uma_core.c declares it extern. */
void vm_radix_reserve_kva(void);
void vm_radix_reserve_kva(void) {
}

void genesis_pmap_remove(vm_offset_t start, vm_offset_t end) {
    vm_offset_t va;

    for (va = start; va < end; va += PMM_PAGE_SIZE) {
        vmm_unmap_page((virt_addr_t)va);
    }
}

vm_paddr_t genesis_pmap_kextract(vm_offset_t va) {
    /* NOT virt_to_phys. paging.h says so in as many words: virt_to_phys is
     * for KERNEL IMAGE SYMBOLS ONLY - things linked at KERNEL_VMA whose
     * VMA/LMA delta the linker script guarantees - and "is not the inverse
     * of phys_to_virt and never was".
     *
     * Every address this is asked about is one of the two things that header
     * warns about: a direct-map address (which is what genesis_pmap_map
     * returns), or a page mapped by vmm_map_page. Using virt_to_phys on
     * either produces a plausible-looking garbage frame number, and
     * startup_free then hands that frame to vm_page_free - releasing memory
     * belonging to something else entirely. It surfaced as a process
     * resuming onto a kernel stack full of zeroes.
     *
     * The direct-map case is arithmetic; everything else is a page-table
     * walk, which is what vmm_get_phys is for. */
    if (va >= PHYSMAP_BASE) {
        return (vm_paddr_t)(va - PHYSMAP_BASE);
    }
    return (vm_paddr_t)vmm_get_phys((virt_addr_t)va);
}

/* pmap_map: make a physical range addressable and return the VA.
 *
 * This is the single most important shim in the file and the first draft got
 * it wrong - it returned 0, uma_startup1 used that as the base of the zone
 * of zones, and the first zone_ctor wrote through a near-null pointer.
 *
 * Genesis's direct map already covers all physical memory, so the answer is
 * phys_to_virt(start) with nothing to map. amd64's own pmap_map does exactly
 * the same thing for the same reason - it returns a DMAP address - which is
 * also why it leaves *virt alone rather than advancing it. */
vm_offset_t genesis_pmap_map(vm_offset_t *virt, vm_paddr_t start,
                             vm_paddr_t end, int prot) {
    (void)virt;
    (void)end;
    (void)prot;
    return (vm_offset_t)phys_to_virt((phys_addr_t)start);
}

void *genesis_kmem_malloc(vm_size_t size, int flags) {
    /* Page-aligned, because every caller here is allocating a slab and UMA
     * derives item addresses by offset from the base. */
    void *p = kmalloc_a((kh_size)size);

    /* M_ZERO IS HONOURED, and the first draft discarded it.
     *
     * keg_alloc_slab sets M_ZERO in aflags for every keg that is not
     * UMA_ZONE_MALLOC - which is most of them, mbufs included - and then
     * relies on the slab arriving zeroed. uma_core.c's own comment at that
     * line says "Malloced items are zeroed in uma_zalloc", meaning the
     * OTHER case is zeroed here instead. Dropping the flag hands a zone
     * that asked for clean memory whatever the heap last had in it.
     *
     * That is a silent bug by construction: a freshly booted heap is mostly
     * zeroes anyway, so it would work for a long time and then not. */
    if (p != 0 && (flags & M_ZERO) != 0) {
        uint8 *b = (uint8 *)p;
        vm_size_t i;

        for (i = 0; i < size; i++) {
            b[i] = 0;
        }
    }
    return p;
}

void genesis_kmem_free(void *addr, vm_size_t size) {
    (void)size;
    kfree(addr);
}

/* --- and the vendored allocator itself ---------------------------------- */

#include "vendor/uma_core.inc"

/* --- the entry point flk.c calls ---------------------------------------
 *
 * Upstream reaches uma_startup1/uma_startup2 through SYSINIT, and
 * <sys/kernel.h> in this compat set expands SYSINIT to nothing (see the
 * comment there: Genesis's boot order is flk.c, written out by hand). So the
 * two are called explicitly, in the order the SYSINIT subsystem/order pairs
 * would have produced.
 *
 * uma_startup1 takes the start of the boot-time KVA reserve. Upstream hands
 * it `virtual_avail` - the first address not yet claimed by early boot - and
 * carves its own bootstrap memory out of it, because at that point in a
 * FreeBSD boot no allocator exists yet. Genesis does not have that
 * problem: kheap_init runs before any of this, so startup_alloc can be
 * backed by kmalloc outright. The plan called this out in advance as the
 * step a reader would expect to be hard and the one that is easy. */
void genesis_uma_init(void) {
    /* ZERO, not a real VA range, and that is deliberate.
     *
     * virtual_avail becomes bootstart and bootmem, and those two are read in
     * exactly two places: startup_free skips its pmap_remove unless the
     * address lies between them, and uma_startup2 skips its vm_map_insert
     * unless they differ. Both want to be skipped here - genesis_pmap_map
     * returns a DIRECT-MAP address and never advances bootmem, precisely
     * because the direct map is already there and needs no reservation.
     *
     * The first version allocated 256KB of kernel VA to pass in. It worked,
     * and it leaked the range permanently while making a reader think it was
     * being used for something. */
    uma_startup1((vm_offset_t)0);
    uma_startup2();
    uma_startup_pcpu(NULL);
    uma_startup3(NULL);
}

/* How deep this CPU is inside critical_enter(). Non-zero when nothing is
 * running that should be - critical_enter() disables interrupts, so an
 * unbalanced one stops the timer. */
int genesis_critical_depth(void) {
    return (critical_nesting);
}
