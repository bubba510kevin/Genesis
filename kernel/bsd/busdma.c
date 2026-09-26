/* bus_space and bus_dma - see kernel/bsd/compat/machine/bus.h for the
 * design, and in particular for the honest account of what bus_dma does and
 * does not do here.
 *
 * The short version: bus_space is a real dispatch onto memory or ports, and
 * bus_dma is real for the case x86-64 without an IOMMU actually has (a
 * mapping IS the physical address) and REFUSES the cases it cannot serve
 * rather than pretending.
 */

#include <sys/param.h>
/* Before anything that reaches net/if.h: struct ifreq embeds a sockaddr by
 * value, and the error when this is missing names net/if.h rather than the
 * file that forgot the include. */
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <machine/bus.h>

#include "kheap.h"
#include "kprintf.h"
#include "paging.h"
#include "pmm.h"

/* Genesis's own port I/O, declared here rather than by including io.h - that
 * header spells outb(port, value) and several other headers in a driver's
 * include set spell it outb(value, port). Only these six are wanted. */
static __inline unsigned char gio_inb(unsigned short p) {
    unsigned char v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static __inline unsigned short gio_inw(unsigned short p) {
    unsigned short v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static __inline unsigned int gio_inl(unsigned short p) {
    unsigned int v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static __inline void gio_outb(unsigned short p, unsigned char v) {
    __asm__ __volatile__("outb %0, %1" :: "a"(v), "Nd"(p));
}
static __inline void gio_outw(unsigned short p, unsigned short v) {
    __asm__ __volatile__("outw %0, %1" :: "a"(v), "Nd"(p));
}
static __inline void gio_outl(unsigned short p, unsigned int v) {
    __asm__ __volatile__("outl %0, %1" :: "a"(v), "Nd"(p));
}

/* --- bus_space ----------------------------------------------------------
 *
 * The tag says which address space; the handle is the base within it. See
 * machine/bus.h. A MEMORY handle is already a mapped kernel virtual address
 * (rman_get_bushandle returns the mapping, not the physical address), so a
 * read is a dereference.
 */

unsigned char bus_space_read_1(bus_space_tag_t t, bus_space_handle_t h,
                               bus_size_t o) {
    if (t == BUS_SPACE_TAG_IO) {
        return gio_inb((unsigned short)(h + o));
    }
    return *(volatile unsigned char *)(h + o);
}

unsigned short bus_space_read_2(bus_space_tag_t t, bus_space_handle_t h,
                                bus_size_t o) {
    if (t == BUS_SPACE_TAG_IO) {
        return gio_inw((unsigned short)(h + o));
    }
    return *(volatile unsigned short *)(h + o);
}

unsigned int bus_space_read_4(bus_space_tag_t t, bus_space_handle_t h,
                              bus_size_t o) {
    if (t == BUS_SPACE_TAG_IO) {
        return gio_inl((unsigned short)(h + o));
    }
    return *(volatile unsigned int *)(h + o);
}

void bus_space_write_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
                       unsigned char v) {
    if (t == BUS_SPACE_TAG_IO) {
        gio_outb((unsigned short)(h + o), v);
        return;
    }
    *(volatile unsigned char *)(h + o) = v;
}

void bus_space_write_2(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
                       unsigned short v) {
    if (t == BUS_SPACE_TAG_IO) {
        gio_outw((unsigned short)(h + o), v);
        return;
    }
    *(volatile unsigned short *)(h + o) = v;
}

void bus_space_write_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
                       unsigned int v) {
    if (t == BUS_SPACE_TAG_IO) {
        gio_outl((unsigned short)(h + o), v);
        return;
    }
    *(volatile unsigned int *)(h + o) = v;
}

/* --- bus_dma ------------------------------------------------------------- */

struct bus_dma_tag {
    bus_addr_t lowaddr;      /* the highest address the device can reach   */
    bus_size_t alignment;
    bus_size_t maxsize;
    bus_size_t maxsegsz;
    int        nsegments;
    int        in_use;
};

struct bus_dmamap {
    void      *vaddr;        /* what was loaded, or allocated              */
    bus_addr_t paddr;
    bus_size_t len;
    int        loaded;
    int        in_use;
};

#define DMA_TAG_MAX 16

/* One map per DMA'd buffer, and a NIC creates one per DESCRIPTOR - re(4)
 * alone wants RL_TX_DESC_CNT + RL_RX_DESC_CNT of them, which is 64 + 256.
 * 32 was sized for "a driver or two doing a handful of transfers" and the
 * first real NIC blew through it during attach, reporting "could not create
 * DMA map for TX" - the honest error path working, against a wrong number.
 *
 * 1024 costs about 40KB of .bss, which is NOBITS and therefore free in the
 * boot image; only address space in the kernel window, which has room. */
#define DMA_MAP_MAX 1024

static struct bus_dma_tag dma_tags[DMA_TAG_MAX];
static struct bus_dmamap  dma_maps[DMA_MAP_MAX];

int bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment,
                       bus_size_t boundary, bus_addr_t lowaddr,
                       bus_addr_t highaddr, bus_dma_filter_t filter,
                       void *filterarg, bus_size_t maxsize, int nsegments,
                       bus_size_t maxsegsz, int flags, bus_dma_lock_t lockfunc,
                       void *lockfuncarg, bus_dma_tag_t *dmat) {
    int i;

    (void)parent; (void)boundary; (void)highaddr; (void)filter;
    (void)filterarg; (void)flags; (void)lockfunc; (void)lockfuncarg;

    for (i = 0; i < DMA_TAG_MAX; i++) {
        if (!dma_tags[i].in_use) {
            dma_tags[i].in_use    = 1;
            dma_tags[i].lowaddr   = lowaddr;
            dma_tags[i].alignment = alignment;
            dma_tags[i].maxsize   = maxsize;
            dma_tags[i].maxsegsz  = maxsegsz;
            dma_tags[i].nsegments = nsegments;
            *dmat = &dma_tags[i];
            return 0;
        }
    }
    kprintf_c(0x0C, "busdma: tag pool exhausted (%d)\n", DMA_TAG_MAX);
    return ENOMEM;
}

int bus_dma_tag_destroy(bus_dma_tag_t dmat) {
    if (dmat != 0) {
        dmat->in_use = 0;
    }
    return 0;
}

static struct bus_dmamap *map_alloc(void) {
    int i;

    for (i = 0; i < DMA_MAP_MAX; i++) {
        if (!dma_maps[i].in_use) {
            dma_maps[i].in_use = 1;
            dma_maps[i].loaded = 0;
            return &dma_maps[i];
        }
    }
    return 0;
}

int bus_dmamap_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t *mapp) {
    struct bus_dmamap *m = map_alloc();

    (void)dmat; (void)flags;
    if (m == 0) {
        kprintf_c(0x0C, "busdma: map pool exhausted (%d)\n", DMA_MAP_MAX);
        return ENOMEM;
    }
    *mapp = m;
    return 0;
}

int bus_dmamap_destroy(bus_dma_tag_t dmat, bus_dmamap_t map) {
    (void)dmat;
    if (map != 0) {
        map->in_use = 0;
    }
    return 0;
}

int bus_dmamem_alloc(bus_dma_tag_t dmat, void **vaddr, int flags,
                     bus_dmamap_t *mapp) {
    struct bus_dmamap *m;
    void *p;

    if (dmat == 0) {
        return EINVAL;
    }
    m = map_alloc();
    if (m == 0) {
        return ENOMEM;
    }

    /* Page-aligned, which covers every alignment a NIC descriptor ring asks
     * for in practice and is what kmalloc_a gives. An alignment request
     * larger than a page is REFUSED rather than quietly under-aligned - a
     * ring the hardware requires to be 256-byte aligned and which is not is
     * a device that DMAs to the wrong place. */
    if (dmat->alignment > PMM_PAGE_SIZE) {
        kprintf_c(0x0C, "busdma: alignment %lx exceeds a page\n",
                  (uint64)dmat->alignment);
        m->in_use = 0;
        return ENOMEM;
    }
    p = kmalloc_a((kh_size)dmat->maxsize);
    if (p == 0) {
        m->in_use = 0;
        return ENOMEM;
    }
    if (flags & BUS_DMA_ZERO) {
        bus_size_t i;
        for (i = 0; i < dmat->maxsize; i++) {
            ((unsigned char *)p)[i] = 0;
        }
    }
    m->vaddr  = p;
    m->paddr  = (bus_addr_t)vmm_get_phys((virt_addr_t)(uintptr)p);
    m->len    = dmat->maxsize;
    m->loaded = 1;
    *vaddr = p;
    *mapp  = m;
    return 0;
}

void bus_dmamem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map) {
    (void)dmat;
    if (vaddr != 0) {
        kfree(vaddr);
    }
    if (map != 0) {
        map->in_use = 0;
    }
}

int bus_dmamap_load(bus_dma_tag_t dmat, bus_dmamap_t map, void *buf,
                    bus_size_t buflen, bus_dmamap_callback_t *callback,
                    void *callback_arg, int flags) {
    bus_dma_segment_t seg;
    bus_addr_t phys;

    (void)flags;
    if (dmat == 0 || map == 0) {
        return EINVAL;
    }
    phys = (bus_addr_t)vmm_get_phys((virt_addr_t)(uintptr)buf);
    if (phys == 0) {
        return EINVAL;
    }

    /* THE CHECK THAT MATTERS. A device whose tag says it can only address
     * the low 4GB must not be handed a buffer above it: the DMA silently
     * goes to the truncated address, corrupting whatever is there, and
     * nothing anywhere reports an error. Upstream solves this by bounce
     * buffering; there is no bounce buffer here, so it is refused. */
    if (phys + buflen - 1 > dmat->lowaddr) {
        kprintf_c(0x0C, "busdma: buffer at %lx is above the device's %lx "
                        "limit and there is no bounce buffer\n",
                  (uint64)phys, (uint64)dmat->lowaddr);
        return ENOMEM;
    }

    map->vaddr  = buf;
    map->paddr  = phys;
    map->len    = buflen;
    map->loaded = 1;

    seg.ds_addr = phys;
    seg.ds_len  = buflen;
    if (callback != 0) {
        callback(callback_arg, &seg, 1, 0);
    }
    return 0;
}

/* Load an mbuf CHAIN as scatter-gather segments.
 *
 * One segment per mbuf, and it fails if the chain needs more than the tag
 * allows - which is exactly the case m_defrag exists to fix, and a driver
 * that gets EFBIG here is expected to defrag and retry. if_rl.c does. */
int bus_dmamap_load_mbuf_sg(bus_dma_tag_t dmat, bus_dmamap_t map,
                            struct mbuf *m, bus_dma_segment_t *segs,
                            int *nsegs, int flags) {
    int n = 0;
    struct mbuf *p;

    (void)flags;
    if (dmat == 0 || map == 0 || m == 0) {
        return EINVAL;
    }
    for (p = m; p != 0; p = p->m_next) {
        bus_addr_t phys;

        if (p->m_len == 0) {
            continue;
        }
        if (n >= dmat->nsegments) {
            return EFBIG;
        }
        phys = (bus_addr_t)vmm_get_phys((virt_addr_t)(uintptr)p->m_data);
        if (phys == 0 || phys + p->m_len - 1 > dmat->lowaddr) {
            return ENOMEM;
        }
        segs[n].ds_addr = phys;
        segs[n].ds_len  = (bus_size_t)p->m_len;
        n++;
    }
    *nsegs = n;
    map->loaded = 1;
    return 0;
}

void bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map) {
    (void)dmat;
    if (map != 0) {
        map->loaded = 0;
    }
}

/* A compiler barrier, and that is CORRECT on x86 rather than a stub.
 *
 * bus_dmamap_sync exists for architectures where the CPU's cache is not
 * coherent with DMA and lines have to be flushed or invalidated by hand. x86
 * is coherent, so there is nothing to flush. What there IS to do is stop the
 * COMPILER reordering a descriptor read across the sync that was meant to
 * order it - which the barrier does. Omitting it entirely would be a real
 * bug, just not a cache one. */
void bus_dmamap_sync(bus_dma_tag_t dmat, bus_dmamap_t map, int op) {
    (void)dmat; (void)map; (void)op;
    __asm__ __volatile__("" ::: "memory");
}

/* --- odds and ends a bigger driver reaches for --------------------------- */

/* Upstream's "chatty boot" flag, set from the loader. Genesis has no loader
 * variables; zero, so a driver's verbose branches stay quiet. */
int bootverbose = 0;

/* pause(9) moved to kernel/bsd/kern_synch.c when the socket layer arrived.
 * It was a udelay() busy-wait here, which was the honest thing while the only
 * caller was a driver waiting microseconds for a chip; the socket layer
 * pauses for whole ticks, and spinning through those keeps the CPU from
 * taking the very interrupt the caller is waiting on. */
