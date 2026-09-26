#ifndef IO_H
#define IO_H

#include "typesk.h"

/* Port I/O, CPUID and MSR access.
 *
 * These were previously duplicated as file-static inlines inside pic.c.
 * Everything that talks to hardware needs them: the ATA driver needs the
 * 16-bit and string forms, and the long-mode entry path needs cpuid() to ask
 * whether the CPU supports it and wrmsr() to switch it on.
 *
 * All inline, all in the header, deliberately: these compile to one or two
 * instructions and a function call would cost more than the work.
 */

/* --- 8/16/32-bit port access -------------------------------------------- */

static inline void outb(uint16 port, uint8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8 inb(uint16 port) {
    uint8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16 port, uint16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16 inw(uint16 port) {
    uint16 value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16 port, uint32 value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32 inl(uint16 port) {
    uint32 value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Burn one I/O cycle on an unused port. Some legacy controllers - the 8259
 * among them - need a moment between back-to-back writes. */
static inline void io_wait(void) {
    outb(0x80, 0);
}

/* --- string I/O ---------------------------------------------------------
 * ATA PIO moves a 512-byte sector as 256 words through a single data port.
 * `rep insw` does that in one instruction; a loop of inw() works but is
 * markedly slower for something on the critical path of every disk read. */

static inline void insw(uint16 port, void *addr, uint32 word_count) {
    __asm__ volatile ("rep insw"
                      : "+D"(addr), "+c"(word_count)
                      : "d"(port)
                      : "memory");
}

static inline void outsw(uint16 port, const void *addr, uint32 word_count) {
    __asm__ volatile ("rep outsw"
                      : "+S"(addr), "+c"(word_count)
                      : "d"(port));
}

/* --- CPUID --------------------------------------------------------------
 * Note this clobbers EBX. That is fine here because the kernel builds with
 * -fno-pie; under PIC, EBX is the GOT pointer and GCC will refuse the "=b"
 * constraint, which is why you often see cpuid wrappers save and restore it
 * by hand. If you ever add -fPIC, this is the code that breaks. */

static inline void cpuid_count(uint32 leaf, uint32 subleaf,
                               uint32 *eax, uint32 *ebx,
                               uint32 *ecx, uint32 *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(subleaf));
}

static inline void cpuid(uint32 leaf, uint32 *eax, uint32 *ebx,
                         uint32 *ecx, uint32 *edx) {
    cpuid_count(leaf, 0, eax, ebx, ecx, edx);
}

/* --- Model-specific registers -------------------------------------------
 * MSRs are 64-bit and split across EDX:EAX. EFER (0xC0000080) is the one the
 * long-mode transition needs: setting bit 8 (LME) is what arms it, though it
 * only takes effect once CR0.PG is turned on with PAE already enabled. */

#define MSR_EFER          0xC0000080u
#define EFER_SCE          (1u << 0)   /* syscall/sysret enable */
#define EFER_LME          (1u << 8)   /* long mode enable      */
#define EFER_LMA          (1u << 10)  /* long mode active (read-only) */
#define EFER_NXE          (1u << 11)  /* no-execute enable     */

static inline uint64 rdmsr(uint32 msr) {
    uint32 low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64)high << 32) | (uint64)low;
}

static inline void wrmsr(uint32 msr, uint64 value) {
    __asm__ volatile ("wrmsr"
                      : :
                      "c"(msr),
                      "a"((uint32)(value & 0xFFFFFFFFu)),
                      "d"((uint32)(value >> 32)));
}

#endif
