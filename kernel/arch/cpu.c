#include "cpu.h"
#include "io.h"
#include "screen.h"
#include "typesk.h"

#define CPUID_LEAF_VENDOR      0x00000000u
#define CPUID_LEAF_EXT_MAX     0x80000000u
#define CPUID_LEAF_EXT_FEATS   0x80000001u
#define CPUID_EXT_EDX_NX       (1u << 20)   /* no-execute page protection */
#define CPUID_EXT_EDX_LM       (1u << 29)   /* long mode supported */

int cpu_has_cpuid(void) {
    /* In long mode the question is settled: CPUID is architecturally present
     * on every CPU that can reach this code. The 32-bit build probes the
     * EFLAGS.ID bit with pushfl/popfl, which does not exist here - and would
     * be pointless if it did. */
    return 1;
}

void cpu_vendor(char *out) {
    uint32 eax, ebx, ecx, edx;
    int i;

    for (i = 0; i < 13; i++) {
        out[i] = '\0';
    }
    if (!cpu_has_cpuid()) {
        out[0] = '?';
        return;
    }

    cpuid(CPUID_LEAF_VENDOR, &eax, &ebx, &ecx, &edx);

    /* The 12 characters arrive in EBX, then EDX, then ECX. Not alphabetical,
     * not EAX-first - it is simply the order Intel chose in 1993. */
    for (i = 0; i < 4; i++) { out[i]     = (char)((ebx >> (i * 8)) & 0xFF); }
    for (i = 0; i < 4; i++) { out[4 + i] = (char)((edx >> (i * 8)) & 0xFF); }
    for (i = 0; i < 4; i++) { out[8 + i] = (char)((ecx >> (i * 8)) & 0xFF); }
    out[12] = '\0';
}

int cpu_has_long_mode(void) {
    uint32 eax, ebx, ecx, edx;

    if (!cpu_has_cpuid()) {
        return 0;
    }

    /* Check the highest supported extended leaf BEFORE asking for 0x80000001.
     * On a CPU with no extended leaves, CPUID does not fault on an
     * out-of-range leaf - it returns the result of the highest basic leaf
     * instead. Skip this and you will happily read a garbage EDX and decide a
     * Pentium supports long mode. */
    cpuid(CPUID_LEAF_EXT_MAX, &eax, &ebx, &ecx, &edx);
    if (eax < CPUID_LEAF_EXT_FEATS) {
        return 0;
    }

    cpuid(CPUID_LEAF_EXT_FEATS, &eax, &ebx, &ecx, &edx);
    return (edx & CPUID_EXT_EDX_LM) != 0;
}

int cpu_has_nx(void) {
    uint32 eax, ebx, ecx, edx;

    if (!cpu_has_cpuid()) {
        return 0;
    }

    /* Same maximum-extended-leaf check as cpu_has_long_mode, and for the same
     * reason: an out-of-range leaf does not fault, it answers with the
     * highest basic leaf's registers. Reading NX out of a leaf that was
     * never asked is how a feature gets enabled on a CPU that does not have
     * it - and this one is enabled once, at boot, with nothing after that
     * point in a position to notice it was wrong. */
    cpuid(CPUID_LEAF_EXT_MAX, &eax, &ebx, &ecx, &edx);
    if (eax < CPUID_LEAF_EXT_FEATS) {
        return 0;
    }

    cpuid(CPUID_LEAF_EXT_FEATS, &eax, &ebx, &ecx, &edx);
    return (edx & CPUID_EXT_EDX_NX) != 0;
}

int cpu_in_long_mode(void) {
    if (!cpu_has_long_mode()) {
        return 0;   /* EFER may not exist; do not read it */
    }
    return (rdmsr(MSR_EFER) & EFER_LMA) != 0;
}

void cpu_report(uint8 color) {
    char vendor[13];

    cpu_vendor(vendor);

    print_string("CPU: ", color);
    print_string(vendor, color);
    print_string("  long mode: ", color);
    print_string(cpu_has_long_mode() ? "yes" : "no", color);
    print_string("  currently: ", color);
    print_string(cpu_in_long_mode() ? "64-bit" : "32-bit", color);
    print_string("\n", color);
}

/* A known-good FXSAVE image, captured from the CPU once it is configured.
 * Every new thread starts from a copy of this. */
static uint8 fpu_template[FXSAVE_SIZE] __attribute__((aligned(16)));

void fpu_thread_init(void *area) {
    uint8 *dst = (uint8 *)area;
    int i;

    for (i = 0; i < FXSAVE_SIZE; i++) {
        dst[i] = fpu_template[i];
    }
}

void fpu_save(void *area) {
    __asm__ volatile ("fxsave (%0)" : : "r"(area) : "memory");
}

void fpu_restore(const void *area) {
    __asm__ volatile ("fxrstor (%0)" : : "r"(area) : "memory");
}

/* The per-CPU half of fpu_init, for an application processor: CR0 and CR4
 * are that CPU's own registers, and a user thread scheduled onto a CPU
 * without OSFXSR takes #UD on its first SSE instruction. The template is
 * NOT recaptured - it is the BSP's, and every thread starts from it on any
 * CPU. */
void fpu_init_ap(void) {
    uint64 cr0, cr4;
    uint32 mxcsr = 0x1F80;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |=  (1ULL << 1);
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    __asm__ volatile ("fninit");
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
}

void fpu_init(void) {
    uint64 cr0, cr4;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);          /* EM = 0: real FPU, do not trap to emulate */
    cr0 |=  (1ULL << 1);          /* MP = 1: monitor coprocessor              */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);           /* OSFXSR: fxsave/fxrstor available, SSE ok */
    cr4 |= (1ULL << 10);          /* OSXMMEXCPT: SSE exceptions -> #XF not #UD */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    /* Put the x87 stack into a known state. Without it the tag word is
     * whatever the firmware left, and the first FPU instruction can raise a
     * spurious exception. */
    __asm__ volatile ("fninit");

    /* MXCSR explicitly, rather than trusting the reset value. Everything in
     * it is masked at 0x1F80; leaving it at whatever firmware left means a
     * process can take an #XF for an ordinary rounded result. */
    {
        uint32 mxcsr = 0x1F80;
        __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
    }

    /* Capture the clean state now, while it is known good, and hand a copy to
     * every thread created afterwards. Building the image by hand instead
     * would mean getting FCW, MXCSR and MXCSR_MASK right from the manual and
     * being wrong on some CPU; asking the CPU what its own initial state
     * looks like cannot be. */
    __asm__ volatile ("fxsave (%0)" : : "r"(fpu_template) : "memory");

    /* The kernel itself still never touches XMM (-mno-sse), which is what
     * lets the switch below save and restore around switch_context without
     * worrying about what the scheduler does in between. */
}
