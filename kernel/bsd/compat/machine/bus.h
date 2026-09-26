#ifndef _MACHINE_BUS_H_
#define _MACHINE_BUS_H_

#include <sys/types.h>

/* <machine/bus.h> - bus_space and bus_dma.
 *
 * ===== bus_space =========================================================
 *
 * The tag/handle pair is FreeBSD's machine-dependent abstraction over "which
 * address space, and where in it". On x86 there are exactly two address
 * spaces - memory and I/O ports - so the TAG carries which one, and the
 * HANDLE carries the base address within it.
 *
 * That is genuinely all it is here, and it is worth saying because the pair
 * looks like it should be opaque cookies from a bus driver. A driver gets
 * them off a resource with rman_get_bustag/rman_get_bushandle and passes
 * them back to every read and write, which is why they are plain integers:
 * anything richer would need a lookup per register access.
 *
 * Genesis's own bus_read_N (kernel/include/sys/bus.h) already dispatches on
 * memory-vs-port and is what nb_rtl.c uses. These are the same operation
 * reached the older way, and both exist because both spellings appear in
 * real driver source.
 */

#define BUS_SPACE_TAG_IO   0
#define BUS_SPACE_TAG_MEM  1

typedef int    bus_space_tag_t;
typedef uintptr bus_space_handle_t;
typedef uintptr bus_addr_t;
typedef uintptr bus_size_t;

unsigned char  bus_space_read_1(bus_space_tag_t t, bus_space_handle_t h,
                                bus_size_t o);
unsigned short bus_space_read_2(bus_space_tag_t t, bus_space_handle_t h,
                                bus_size_t o);
unsigned int   bus_space_read_4(bus_space_tag_t t, bus_space_handle_t h,
                                bus_size_t o);
void bus_space_write_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
                       unsigned char v);
void bus_space_write_2(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
                       unsigned short v);
void bus_space_write_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
                       unsigned int v);

/* The _stream_ variants do NOT byte-swap; the ordinary ones are defined to
 * be host-endian and swap on a big-endian machine. x86-64 is little-endian
 * and PCI is little-endian, so the two are identical here - which is why
 * these are aliases and not a second implementation. On a big-endian port
 * they would have to diverge, and a driver using the wrong one reads a
 * register with its bytes reversed. */
#define bus_space_read_stream_1(t, h, o)      bus_space_read_1((t), (h), (o))
#define bus_space_read_stream_2(t, h, o)      bus_space_read_2((t), (h), (o))
#define bus_space_read_stream_4(t, h, o)      bus_space_read_4((t), (h), (o))
#define bus_space_write_stream_1(t, h, o, v)  bus_space_write_1((t), (h), (o), (v))
#define bus_space_write_stream_2(t, h, o, v)  bus_space_write_2((t), (h), (o), (v))
#define bus_space_write_stream_4(t, h, o, v)  bus_space_write_4((t), (h), (o), (v))

/* A barrier between accesses. Real, not a no-op: x86's store buffer means a
 * write to a command register can still be in flight when the following read
 * of a status register is issued, and a device that was supposed to see them
 * in order does not. */
#define BUS_SPACE_BARRIER_READ  0x01
#define BUS_SPACE_BARRIER_WRITE 0x02
static __inline void bus_space_barrier(bus_space_tag_t t, bus_space_handle_t h,
                                       bus_size_t o, bus_size_t l, int flags) {
    (void)t; (void)h; (void)o; (void)l; (void)flags;
    __asm__ __volatile__("mfence" ::: "memory");
}

/* ===== bus_dma ===========================================================
 *
 * THE HONEST PART OF THIS FILE, and it deserves to be read before anything
 * is built on it.
 *
 * bus_dma exists to solve problems Genesis does not have and cannot yet
 * solve: bounce buffering when a device cannot reach the physical address a
 * buffer landed at, scatter-gather segment splitting, and cache coherency on
 * architectures that need explicit sync. On x86-64 with a device that can
 * address all of RAM, a "DMA mapping" is just the physical address of the
 * buffer, and bus_dmamap_sync is genuinely nothing.
 *
 * So this implementation is real for the case it covers and REFUSES the
 * cases it does not:
 *
 *   - A tag whose maxaddr excludes memory the buffer is actually in causes
 *     bus_dmamap_load to fail rather than silently hand the device an
 *     address it cannot reach. That is the failure mode that would otherwise
 *     present as a device that DMAs into nothing, with no error anywhere.
 *   - nsegments > 1 is accepted but a load that would need more than one
 *     segment fails. Genesis's kernel heap is physically contiguous, so a
 *     single buffer is a single segment; a caller passing a chain of mbufs
 *     would need real segment splitting and does not get a pretend version.
 *
 * bus_dmamap_sync is a compiler barrier and nothing else, which is CORRECT
 * on x86 for coherent DMA - not a stub. The barrier matters: without it the
 * compiler may hoist a descriptor read above the sync that was supposed to
 * order it.
 */

typedef struct bus_dma_tag  *bus_dma_tag_t;
typedef struct bus_dmamap   *bus_dmamap_t;
typedef void                *bus_dma_filter_t;
typedef struct bus_dma_lock *bus_dma_lock_t;

typedef struct bus_dma_segment {
    bus_addr_t ds_addr;
    bus_size_t ds_len;
} bus_dma_segment_t;

typedef void bus_dmamap_callback_t(void *, bus_dma_segment_t *, int, int);
typedef void bus_dmamap_callback2_t(void *, bus_dma_segment_t *, int,
                                    bus_size_t, int);

#define BUS_SPACE_MAXADDR      ((bus_addr_t)0xFFFFFFFFFFFFFFFFULL)
#define BUS_SPACE_MAXADDR_32BIT ((bus_addr_t)0xFFFFFFFFULL)
#define BUS_SPACE_MAXSIZE      ((bus_size_t)0xFFFFFFFFFFFFFFFFULL)
#define BUS_SPACE_MAXSIZE_32BIT ((bus_size_t)0xFFFFFFFFULL)
#define BUS_SPACE_UNRESTRICTED  0

#define BUS_DMA_WAITOK      0x00
#define BUS_DMA_NOWAIT      0x01
#define BUS_DMA_ALLOCNOW    0x02
#define BUS_DMA_COHERENT    0x04
#define BUS_DMA_ZERO        0x08
#define BUS_DMA_NOCACHE     0x10

#define BUS_DMASYNC_PREREAD   0x01
#define BUS_DMASYNC_POSTREAD  0x02
#define BUS_DMASYNC_PREWRITE  0x04
#define BUS_DMASYNC_POSTWRITE 0x08

int  bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment,
                        bus_size_t boundary, bus_addr_t lowaddr,
                        bus_addr_t highaddr, bus_dma_filter_t filter,
                        void *filterarg, bus_size_t maxsize, int nsegments,
                        bus_size_t maxsegsz, int flags, bus_dma_lock_t lockfunc,
                        void *lockfuncarg, bus_dma_tag_t *dmat);
int  bus_dma_tag_destroy(bus_dma_tag_t dmat);
int  bus_dmamem_alloc(bus_dma_tag_t dmat, void **vaddr, int flags,
                      bus_dmamap_t *mapp);
void bus_dmamem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map);
int  bus_dmamap_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t *mapp);
int  bus_dmamap_destroy(bus_dma_tag_t dmat, bus_dmamap_t map);
int  bus_dmamap_load(bus_dma_tag_t dmat, bus_dmamap_t map, void *buf,
                     bus_size_t buflen, bus_dmamap_callback_t *callback,
                     void *callback_arg, int flags);
int  bus_dmamap_load_mbuf_sg(bus_dma_tag_t dmat, bus_dmamap_t map,
                             struct mbuf *m, bus_dma_segment_t *segs,
                             int *nsegs, int flags);
void bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map);
void bus_dmamap_sync(bus_dma_tag_t dmat, bus_dmamap_t map, int op);

/* The tag a device's DMA is relative to. On x86 there is no IOMMU here and
 * no per-bus translation, so this is the NULL parent tag - which is what
 * bus_dma_tag_create already accepts. */
#define bus_get_dma_tag(dev) ((bus_dma_tag_t)0)

#endif
