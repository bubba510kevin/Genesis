#ifndef LINUX_IO_H
#define LINUX_IO_H

#include "linux/types.h"

/* <linux/io.h> and <asm/io.h> - MMIO and port I/O as driver source spells it.
 *
 * --- why these are functions and not macros over a pointer ----------------
 * The obvious definition of readl(a) is *(volatile u32 *)(a), and it is
 * wrong in a way that does not show up until it does. A device register read
 * has to be ordered with respect to the writes around it, and on x86 the
 * compiler is free to reorder a plain volatile access against a non-volatile
 * one. Linux's real readl/writel carry barriers for exactly this reason, and
 * these do too - see kernel/driver/lkpi.c.
 *
 * They are also where a driver's MMIO can be traced from, which a macro
 * would not be.
 */

void __iomem_dummy(void);   /* keeps __iomem meaningful as a marker below */

/* Linux annotates device-memory pointers with __iomem and a static checker
 * enforces it. There is no such checker here, so it is defined away - but
 * defined rather than removed, because driver source says it and must
 * compile unmodified. */
#define __iomem
#define __force
#define __must_check

u8  readb(const volatile void *addr);
u16 readw(const volatile void *addr);
u32 readl(const volatile void *addr);
u64 readq(const volatile void *addr);

void writeb(u8 value, volatile void *addr);
void writew(u16 value, volatile void *addr);
void writel(u32 value, volatile void *addr);
void writeq(u64 value, volatile void *addr);

/* The _relaxed forms are the same access without the ordering guarantee.
 * Defined as the ordered ones rather than as bare dereferences: on x86 the
 * difference is a compiler barrier, which costs nothing measurable here, and
 * being stronger than asked for is the safe direction. */
#define readb_relaxed(a)     readb(a)
#define readw_relaxed(a)     readw(a)
#define readl_relaxed(a)     readl(a)
#define readq_relaxed(a)     readq(a)
#define writeb_relaxed(v, a) writeb(v, a)
#define writew_relaxed(v, a) writew(v, a)
#define writel_relaxed(v, a) writel(v, a)
#define writeq_relaxed(v, a) writeq(v, a)

/* Map device physical memory into the kernel address space, uncached.
 *
 * Uncached is not a detail: a cached mapping of a device register means a
 * read served from a cache line that predates the interrupt which changed
 * it, and a write sitting in a buffer past the point the device was supposed
 * to see it. The same reasoning kernel/arch/lapic.c gives for its own
 * mapping, applied to whatever a driver asks for.
 *
 * Returns NULL on failure, which driver code already checks. */
void __iomem *ioremap(unsigned long phys_addr, unsigned long size);
void iounmap(void __iomem *addr);

/* ioremap_wc / _nocache / _uc are all "uncached" here. Write-combining is a
 * real distinction on hardware with a framebuffer to stream into, and this
 * kernel has nothing that would benefit; mapping it uncached instead is
 * slower and correct, rather than faster and subtly wrong. */
#define ioremap_nocache(p, s) ioremap((p), (s))
#define ioremap_uc(p, s)      ioremap((p), (s))
#define ioremap_wc(p, s)      ioremap((p), (s))

/* Port I/O. Genesis has these already (kernel/include/io.h); these are the
 * Linux spellings, and the 16-bit port width is Linux's u16 rather than
 * Genesis's uint16 so that driver source passing an int does not warn. */
u8   inb(u16 port);
u16  inw(u16 port);
u32  inl(u16 port);
void outb(u8 value, u16 port);
void outw(u16 value, u16 port);
void outl(u32 value, u16 port);

/* NOTE the argument order. Linux's outb is outb(value, port); Genesis's own
 * io.h has outb(port, value). They are the same two integers in the opposite
 * order, the compiler cannot tell them apart, and a driver that gets it
 * backwards writes a port number into a device register. This header
 * deliberately does NOT include Genesis's io.h - the two spellings must never
 * be visible in one translation unit. */

/* Memory barriers. Real instructions, not empty macros: x86's store buffer
 * means a plain compiler barrier is not sufficient for mb(). */
#define barrier()  __asm__ __volatile__("" ::: "memory")
#define mb()       __asm__ __volatile__("mfence" ::: "memory")
#define rmb()      __asm__ __volatile__("lfence" ::: "memory")
#define wmb()      __asm__ __volatile__("sfence" ::: "memory")
#define smp_mb()   mb()
#define smp_rmb()  barrier()
#define smp_wmb()  barrier()

#endif
