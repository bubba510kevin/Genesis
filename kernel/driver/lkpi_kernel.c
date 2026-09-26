/* The LinuxKPI implementation: everything kernel/include/linux/ declares,
 * backed by Genesis's own primitives.
 *
 * Separate from lkpi.c, which is the PCI driver-model ADAPTER (turning a
 * struct pci_driver into a driver_t for bus.c). This file is the KERNEL API
 * a Linux driver calls once it is running - printk, kmalloc, ioremap, locks,
 * delays. Two different jobs that happened to share a file while the surface
 * was three functions; they do not share one now.
 *
 * The tension running through this file is that Genesis's own headers and
 * Linux's disagree about names - both have a kmalloc, both have an inb with
 * the arguments in the OPPOSITE order, both have a spinlock. So this file
 * includes Genesis's own headers, and not the linux ones, wherever the two
 * collide - declaring the Linux-side prototype by hand instead. Where
 * that happens it is called out, because a silent disagreement here is a
 * driver writing a port number into a device register.
 */

#include "klock.h"
#include "kheap.h"
#include "kprintf.h"
#include <stdarg.h>
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "timer.h"
#include "typesk.h"
#include "vmalloc.h"
#include "backtrace.h"

/* Linux's fixed-width names, spelled here rather than by including
 * linux/types.h, so this file is not committed to the whole Linux header
 * set just to name a u32. */
typedef uint8  u8;
typedef uint16 u16;
typedef uint32 u32;
typedef uint64 u64;

/* --- printk --------------------------------------------------------------
 *
 * Linux encodes the log level as a "<N>" prefix ON the format string. Parsed
 * off here and turned into a Genesis console colour, rather than being
 * stripped and discarded - an error and an info line looking identical on the
 * console is a real loss when the whole point of a driver log is to be
 * skimmed.
 *
 * NOT handled: Linux's extended pointer formats, %pM (a MAC address), %pI4
 * (an IPv4 address), %pS (a symbol). kprintf does not implement them, so a
 * driver printing a MAC address gets the raw pointer in hex. Worth knowing
 * before it is discovered from a boot log; adding them means teaching
 * kprintf, which is a change to a file every part of this kernel uses.
 */
int printk(const char *fmt, ...) {
    uint8 color = 0x0F;
    va_list ap;

    if (fmt[0] == '<' && fmt[1] >= '0' && fmt[1] <= '7' && fmt[2] == '>') {
        switch (fmt[1]) {
            case '0': case '1': case '2': case '3':
                color = 0x0C;   /* emerg..err   - red     */
                break;
            case '4':
                color = 0x0E;   /* warning      - yellow  */
                break;
            case '7':
                color = 0x08;   /* debug        - grey    */
                break;
            default:
                color = 0x0F;   /* notice, info - white   */
                break;
        }
        fmt += 3;
    }

    va_start(ap, fmt);
    kvprintf(color, fmt, ap);
    va_end(ap);
    return 0;
}

void linux_bug(const char *file, int line, const char *cond) {
    kprintf_c(0x4F, "BUG at %s:%d: %s\n", file, line, cond);
    /* A backtrace, because that is the entire reason stopping here is more
     * useful than continuing: Part 2 of the item 11 pass built ksyms and the
     * frame walker so a kernel-side assertion could say where it came from.
     * The RIP and RBP are this frame's - __builtin_return_address gives the
     * caller's RIP, which is the frame worth starting from. */
    backtrace_print((uint64)(uintptr)__builtin_return_address(0),
                    (uint64)(uintptr)__builtin_frame_address(0), 0x0C);
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

int linux_warn(const char *file, int line, const char *cond) {
    kprintf_c(0x0E, "WARNING at %s:%d: %s\n", file, line, cond);
    return 1;
}

/* --- kmalloc -------------------------------------------------------------
 *
 * Genesis's kmalloc/kfree are declared in kheap.h with DIFFERENT signatures
 * from Linux's (no gfp_t, and kfree takes a non-const void *). Both cannot be
 * visible under the same name, so the Linux ones are defined here with
 * Genesis-internal names for the underlying calls. linux/slab.h is not
 * included by this file for exactly that reason.
 */
#define __GFP_ZERO 0x8000u

static void zero_bytes(void *p, unsigned long n) {
    unsigned char *b = (unsigned char *)p;
    unsigned long i;

    for (i = 0; i < n; i++) {
        b[i] = 0;
    }
}

void *linux_kmalloc(unsigned long size, unsigned int flags) {
    void *p;

    /* Zero is a legal request in Linux and returns a non-NULL pointer that
     * may not be dereferenced. Genesis's kmalloc(0) behaviour is not
     * something to depend on, so it is normalised here. */
    if (size == 0) {
        size = 1;
    }
    p = kmalloc_a((kh_size)size);
    if (p != NULL && (flags & __GFP_ZERO)) {
        zero_bytes(p, size);
    }
    return p;
}

void *linux_kzalloc(unsigned long size, unsigned int flags) {
    return linux_kmalloc(size, flags | __GFP_ZERO);
}

void *linux_kmalloc_array(unsigned long n, unsigned long size, unsigned int flags) {
    /* The overflow check is the reason this is not a plain multiply. n * size
     * wrapping produces a SMALL allocation for a LARGE request, which the
     * caller then writes past the end of - a heap overflow driven by an
     * integer the caller may not control. */
    if (n != 0 && size > (unsigned long)-1 / n) {
        return NULL;
    }
    return linux_kmalloc(n * size, flags);
}

void *linux_kcalloc(unsigned long n, unsigned long size, unsigned int flags) {
    return linux_kmalloc_array(n, size, flags | __GFP_ZERO);
}

void linux_kfree(const void *p) {
    /* const on the parameter is Linux's signature, not a claim that the
     * memory is unmodified. Cast away rather than changing the prototype,
     * which driver source depends on. */
    if (p != NULL) {
        kfree((void *)(uintptr)p);
    }
}

void *linux_krealloc(void *p, unsigned long size, unsigned int flags) {
    (void)flags;
    return krealloc(p, (kh_size)size);
}

/* --- MMIO ----------------------------------------------------------------
 *
 * The accessors are real functions with real barriers rather than macros
 * over a volatile dereference. On x86 the compiler may reorder a volatile
 * access against a non-volatile one, so a driver that writes a command
 * register and then reads a status register can have those two reach the
 * device in the wrong order. Linux's own readl/writel carry these barriers.
 */
u8  readb(const volatile void *a) {
    u8 v = *(const volatile u8 *)a;
    __asm__ __volatile__("" ::: "memory");
    return v;
}
u16 readw(const volatile void *a) {
    u16 v = *(const volatile u16 *)a;
    __asm__ __volatile__("" ::: "memory");
    return v;
}
u32 readl(const volatile void *a) {
    u32 v = *(const volatile u32 *)a;
    __asm__ __volatile__("" ::: "memory");
    return v;
}
u64 readq(const volatile void *a) {
    u64 v = *(const volatile u64 *)a;
    __asm__ __volatile__("" ::: "memory");
    return v;
}

void writeb(u8 v, volatile void *a) {
    __asm__ __volatile__("" ::: "memory");
    *(volatile u8 *)a = v;
}
void writew(u16 v, volatile void *a) {
    __asm__ __volatile__("" ::: "memory");
    *(volatile u16 *)a = v;
}
void writel(u32 v, volatile void *a) {
    __asm__ __volatile__("" ::: "memory");
    *(volatile u32 *)a = v;
}
void writeq(u64 v, volatile void *a) {
    __asm__ __volatile__("" ::: "memory");
    *(volatile u64 *)a = v;
}

/* Page-table bits for MMIO. The same two, and the same reasoning, as
 * kernel/arch/lapic.c and kernel/arch/ioapic.c: a cached mapping of a device
 * register means a read served from a stale cache line and a write sitting in
 * a buffer past the point the device should have seen it. */
#define PAGE_PWT 0x8
#define PAGE_PCD 0x10

/* How many ioremap mappings can be outstanding. A fixed pool, matching this
 * tree's convention - and it has to exist because iounmap is given only the
 * VIRTUAL address and has to recover the range length to unmap and free. */
#define IOREMAP_MAX 16

static struct {
    uint64 va;        /* page-aligned base handed to vmm_map_page */
    uint64 pages;
} ioremaps[IOREMAP_MAX];

void *ioremap(unsigned long phys_addr, unsigned long size) {
    uint64 first = (uint64)phys_addr & ~0xFFFULL;
    uint64 offset = (uint64)phys_addr & 0xFFFULL;
    uint64 pages = ((uint64)size + offset + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64 va, p;
    int slot;

    if (size == 0) {
        return NULL;
    }
    for (slot = 0; slot < IOREMAP_MAX; slot++) {
        if (ioremaps[slot].pages == 0) {
            break;
        }
    }
    if (slot == IOREMAP_MAX) {
        kprintf_c(0x0C, "lkpi: ioremap table full (%d)\n", IOREMAP_MAX);
        return NULL;
    }

    va = kvm_alloc_range(pages * PMM_PAGE_SIZE, PMM_PAGE_SIZE);
    if (va == 0) {
        return NULL;
    }
    for (p = 0; p < pages; p++) {
        if (!vmm_map_page((virt_addr_t)(va + p * PMM_PAGE_SIZE),
                          (phys_addr_t)(first + p * PMM_PAGE_SIZE),
                          PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT)) {
            /* Unwind the pages already mapped. A partial mapping left behind
             * is worse than a failed one: the caller gets NULL and moves on,
             * and the VA range stays permanently half-populated. */
            while (p-- > 0) {
                vmm_unmap_page((virt_addr_t)(va + p * PMM_PAGE_SIZE));
            }
            kvm_free_range(va);
            return NULL;
        }
    }

    ioremaps[slot].va    = va;
    ioremaps[slot].pages = pages;
    return (void *)(va + offset);
}

void iounmap(void *addr) {
    uint64 va = (uint64)(uintptr)addr & ~0xFFFULL;
    int slot;
    uint64 p;

    for (slot = 0; slot < IOREMAP_MAX; slot++) {
        if (ioremaps[slot].pages != 0 && ioremaps[slot].va == va) {
            break;
        }
    }
    if (slot == IOREMAP_MAX) {
        /* Not ours. Reported rather than ignored: an iounmap of something
         * that was never ioremapped is a driver bug, and silently returning
         * turns it into a leak nobody sees. */
        kprintf_c(0x0E, "lkpi: iounmap(%lx) - not an ioremap mapping\n", va);
        return;
    }
    for (p = 0; p < ioremaps[slot].pages; p++) {
        vmm_unmap_page((virt_addr_t)(va + p * PMM_PAGE_SIZE));
    }
    kvm_free_range(va);
    ioremaps[slot].pages = 0;
}

/* --- port I/O ------------------------------------------------------------
 *
 * THE ARGUMENT ORDER IS REVERSED from Genesis's own io.h. Linux writes
 * outb(value, port); Genesis writes outb(port, value). They are the same two
 * integers and the compiler cannot tell them apart, so a mix-up writes a port
 * number into a device register and reads back garbage.
 *
 * The inline assembly is written out here rather than calling Genesis's io.h
 * for exactly that reason: including io.h would put a conflicting outb
 * declaration in scope, and there would be no way to define this one.
 */
u8 inb(u16 port) {
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
u16 inw(u16 port) {
    u16 v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
u32 inl(u16 port) {
    u32 v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
void outb(u8 v, u16 port) {
    __asm__ __volatile__("outb %0, %1" :: "a"(v), "Nd"(port));
}
void outw(u16 v, u16 port) {
    __asm__ __volatile__("outw %0, %1" :: "a"(v), "Nd"(port));
}
void outl(u32 v, u16 port) {
    __asm__ __volatile__("outl %0, %1" :: "a"(v), "Nd"(port));
}

/* --- locks ---------------------------------------------------------------
 *
 * spinlock_t and struct mutex are declared in linux/spinlock.h and
 * linux/mutex.h as opaque byte arrays, so driver source can embed one by
 * value without dragging klock.h into every driver. The size has to be big
 * enough for a real mtx_t, and "has to be" is checked here rather than
 * assumed - a mismatch would scribble on whatever field follows the lock in
 * a driver's softc, which is the kind of corruption that presents nowhere
 * near its cause.
 */
struct linux_lock_storage {
    unsigned long opaque[8];
    int           initialised;
};

typedef char lkpi_assert_lock_fits[
    (sizeof(mtx_t) <= sizeof(((struct linux_lock_storage *)0)->opaque)) ? 1 : -1];

/* Lazy initialisation, because DEFINE_SPINLOCK and DEFINE_MUTEX produce a
 * zeroed struct with no constructor having run - Linux's static initialisers
 * are compile-time-complete and Genesis's kmtx_init is not. A driver that
 * does call spin_lock_init still works; this only covers the one that does
 * not, which is legal Linux. */
static mtx_t *lock_of(struct linux_lock_storage *s, const char *name) {
    mtx_t *m = (mtx_t *)s->opaque;

    if (!s->initialised) {
        kmtx_init(m, name);
        s->initialised = 1;
    }
    return m;
}

void spin_lock_init(struct linux_lock_storage *s) {
    s->initialised = 0;
    (void)lock_of(s, "linux spinlock");
}
void spin_lock(struct linux_lock_storage *s) {
    kmtx_lock(lock_of(s, "linux spinlock"));
}
void spin_unlock(struct linux_lock_storage *s) {
    kmtx_unlock(lock_of(s, "linux spinlock"));
}
int spin_trylock(struct linux_lock_storage *s) {
    return kmtx_trylock(lock_of(s, "linux spinlock"));
}

unsigned long linux_spin_lock_irqsave(struct linux_lock_storage *s) {
    unsigned long flags;

    /* Read RFLAGS and disable, in that order. Reading after the cli would
     * always report interrupts disabled and _irqrestore would then leave
     * them off forever - the classic version of this bug. */
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    kmtx_lock(lock_of(s, "linux spinlock"));
    return flags;
}

void linux_spin_unlock_irqrestore(struct linux_lock_storage *s,
                                  unsigned long flags) {
    kmtx_unlock(lock_of(s, "linux spinlock"));
    /* Restore rather than unconditionally sti: the caller may have been
     * called with interrupts already off, and turning them on here would
     * re-enable them inside somebody else's critical section. */
    __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

void mutex_init(struct linux_lock_storage *s) {
    s->initialised = 0;
    (void)lock_of(s, "linux mutex");
}
void mutex_lock(struct linux_lock_storage *s) {
    kmtx_lock(lock_of(s, "linux mutex"));
}
void mutex_unlock(struct linux_lock_storage *s) {
    kmtx_unlock(lock_of(s, "linux mutex"));
}
int mutex_trylock(struct linux_lock_storage *s) {
    return kmtx_trylock(lock_of(s, "linux mutex"));
}
int mutex_is_locked(struct linux_lock_storage *s) {
    return kmtx_owned(lock_of(s, "linux mutex"));
}

/* --- time ----------------------------------------------------------------- */

unsigned long linux_jiffies(void) {
    return (unsigned long)timer_ticks_now();
}

/* udelay - spin for at least `usecs` microseconds.
 *
 * ON THE TSC, NOT ON THE TIMER TICK, and that is not an optimisation - it is
 * the difference between working and hanging. The first version spun waiting
 * for timer_ticks_now() to advance, which is correct only once interrupts
 * are enabled. Drivers run before that: kld_load_directories loads modules
 * from disk at flk.c well ahead of sti, so a driver calling DELAY() during
 * attach waited for a tick that could not arrive. The symptom was a boot
 * that stopped dead after the driver's last printf with no fault and no
 * message.
 *
 * The TSC runs regardless of interrupt state, which is exactly the property
 * needed here.
 *
 * --- about the frequency ---------------------------------------------------
 * There is no TSC calibration in this kernel, so the rate is assumed rather
 * than measured, and the assumption is deliberately HIGH - 5GHz. That errs
 * toward waiting TOO LONG on any real machine: at 5000 assumed cycles per
 * microsecond, a 2.5GHz CPU waits two microseconds for every one asked for.
 *
 * The direction matters and the other one is unsafe. A hardware delay is a
 * MINIMUM - "wait 50us after reset before touching this register" - so
 * over-waiting costs time and under-waiting reads a register the device has
 * not finished updating, which presents as intermittent nonsense far from
 * here. Calibrating against the PIT would make this exact; assuming fast
 * makes it correct, which is the property that cannot be traded away.
 */
#define UDELAY_ASSUMED_TSC_PER_US 5000ULL

static __inline uint64 rdtsc_now(void) {
    uint32 lo, hi;

    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64)hi << 32) | lo;
}

void udelay(unsigned long usecs) {
    uint64 start = rdtsc_now();
    uint64 want  = (uint64)usecs * UDELAY_ASSUMED_TSC_PER_US;

    while (rdtsc_now() - start < want) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}

void mdelay(unsigned long msecs) {
    udelay(msecs * 1000);
}

void usleep_range(unsigned long min_us, unsigned long max_us) {
    (void)max_us;
    udelay(min_us);
}

/* msleep BLOCKS in Linux, yielding the CPU. Here it busy-waits, and that is
 * a deliberate choice rather than a missing piece.
 *
 * Genesis can block a process - sched_sleep_until, which sys_nanosleep uses -
 * but a driver is not a process. The dominant caller of msleep is a probe or
 * attach routine running during boot enumeration, before the first context
 * switch, where there is nothing to yield TO and a block would wait for a
 * reschedule that cannot come. Getting that wrong is a hang at boot with no
 * output, which is the worst failure this file could have.
 *
 * So: correct, and wasteful in the case where yielding would have been
 * possible. The cost is CPU time during attach, which is bounded and happens
 * once. Making this yield when a process context genuinely exists is a real
 * improvement and needs a driver that actually sleeps for long enough to
 * care - none does yet. */
void msleep(unsigned int msecs) {
    udelay((unsigned long)msecs * 1000);
}

void ssleep(unsigned int secs) {
    msleep(secs * 1000);
}

/* --- strings -------------------------------------------------------------
 *
 * memcpy/memset/memcmp already exist as global symbols (kernel/zfs/
 * zfs_shim.c, where the vendored ZFS reader needed them first) and are NOT
 * redefined here - a second definition would be a duplicate symbol at link
 * time. Only the ones nothing else in the tree provides are below.
 */
/* memmove is NOT here: kernel/zfs/zfs_shim.c already defines it globally,
 * alongside memcpy/memset/memcmp, because GCC emits calls to those four for
 * ordinary struct assignments and they had to exist before the vendored ZFS
 * reader would link. Defining a second one here would be a duplicate symbol.
 * linux/string.h declares it, which is all a driver needs. */

unsigned long strlen(const char *s) {
    unsigned long n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    unsigned long i = 0;

    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return (int)((unsigned char)a[i]) - (int)((unsigned char)b[i]);
}

int strncmp(const char *a, const char *b, unsigned long n) {
    unsigned long i = 0;

    while (i < n && a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    if (i == n) {
        return 0;
    }
    return (int)((unsigned char)a[i]) - (int)((unsigned char)b[i]);
}

char *strcpy(char *dst, const char *src) {
    unsigned long i = 0;

    while ((dst[i] = src[i]) != '\0') {
        i++;
    }
    return dst;
}

char *strncpy(char *dst, const char *src, unsigned long n) {
    unsigned long i = 0;

    while (i < n && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    while (i < n) {
        dst[i++] = '\0';
    }
    return dst;
}

char *strchr(const char *s, int c) {
    unsigned long i = 0;

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == (char)c) {
            return (char *)(uintptr)&s[i];
        }
    }
    return c == '\0' ? (char *)(uintptr)&s[i] : (char *)0;
}

unsigned long strlcpy(char *dst, const char *src, unsigned long size) {
    unsigned long srclen = strlen(src);
    unsigned long copy = srclen;

    if (size == 0) {
        return srclen;
    }
    if (copy >= size) {
        copy = size - 1;
    }
    {
        unsigned long i;
        for (i = 0; i < copy; i++) {
            dst[i] = src[i];
        }
    }
    dst[copy] = '\0';
    /* The SOURCE length, not the copied length. This is the only way the
     * caller can detect truncation, and returning the copied length instead
     * is the standard way to get strlcpy wrong. */
    return srclen;
}
