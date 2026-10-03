#include "io.h"
#include "ksmp.h"
#include "kusd.h"
#include "paging.h"
#include "pmm.h"
#include "teb.h"
#include "timer.h"
#include "typesk.h"

/* See kusd.h. */

#define NT_EPOCH_DELTA_100NS 116444736000000000ULL   /* 1601 -> 1970 */

static phys_addr_t kusd_phys;
static volatile uint8 *kusd;        /* the page, through the direct map */

#define U8(off)  (*(volatile uint8  *)(kusd + (off)))
#define U16(off) (*(volatile uint16 *)(kusd + (off)))
#define U32(off) (*(volatile uint32 *)(kusd + (off)))
#define U64(off) (*(volatile uint64 *)(kusd + (off)))

/* KSYSTEM_TIME { ULONG LowPart; LONG High1Time; LONG High2Time; }, written
 * in the order a lock-free reader relies on: High2, Low, High1. A reader
 * takes High1, Low, High2 and retries while High1 != High2. */
static void ksystem_time_set(uint32 off, uint64 v) {
    U32(off + 8) = (uint32)(v >> 32);
    __asm__ volatile ("" ::: "memory");
    U32(off + 0) = (uint32)v;
    __asm__ volatile ("" ::: "memory");
    U32(off + 4) = (uint32)(v >> 32);
}

static void features(void) {
    uint32 a, b, c, d, max, ext;

    cpuid(0, &max, &b, &c, &d);
    cpuid(1, &a, &b, &c, &d);
    U8(KUSD_PROCESSOR_FEATURES + PF_COMPARE_EXCHANGE_DOUBLE) = (d >> 8) & 1;
    U8(KUSD_PROCESSOR_FEATURES + PF_MMX_INSTRUCTIONS_AVAILABLE) = (d >> 23) & 1;
    U8(KUSD_PROCESSOR_FEATURES + PF_XMMI_INSTRUCTIONS_AVAILABLE) = (d >> 25) & 1;
    U8(KUSD_PROCESSOR_FEATURES + PF_XMMI64_INSTRUCTIONS_AVAILABLE) = (d >> 26) & 1;
    U8(KUSD_PROCESSOR_FEATURES + PF_RDTSC_INSTRUCTION_AVAILABLE) = (d >> 4) & 1;
    U8(KUSD_PROCESSOR_FEATURES + PF_PAE_ENABLED) = 1;     /* long mode is PAE */
    U8(KUSD_PROCESSOR_FEATURES + PF_SSE3_INSTRUCTIONS_AVAILABLE) = c & 1;
    U8(KUSD_PROCESSOR_FEATURES + PF_SSSE3_INSTRUCTIONS_AVAILABLE) = (c >> 9) & 1;
    U8(KUSD_PROCESSOR_FEATURES + PF_COMPARE_EXCHANGE128) = (c >> 13) & 1;
    U8(KUSD_PROCESSOR_FEATURES + PF_SSE4_1_INSTRUCTIONS_AVAILABLE) = (c >> 19) & 1;
    U8(KUSD_PROCESSOR_FEATURES + PF_SSE4_2_INSTRUCTIONS_AVAILABLE) = (c >> 20) & 1;
    U8(KUSD_PROCESSOR_FEATURES + PF_RDRAND_INSTRUCTION_AVAILABLE) = (c >> 30) & 1;
    /* AVX needs the OS to have enabled the YMM state (OSXSAVE + XCR0); the
     * kernel does not save YMM registers across a switch, so AVX is NOT
     * reported even on a CPU that has it - a program trusting the bit would
     * have its upper halves corrupted by the first context switch. */
    U8(KUSD_PROCESSOR_FEATURES + PF_AVX_INSTRUCTIONS_AVAILABLE) = 0;
    U8(KUSD_PROCESSOR_FEATURES + PF_NX_ENABLED) =
        (uint8)(paging_nx_enabled() != 0);
    /* __fastfail is int 0x29; the kernel turns any unexpected interrupt in
     * ring 3 into a fault the process cannot survive, which is the contract. */
    U8(KUSD_PROCESSOR_FEATURES + PF_FASTFAIL_AVAILABLE) = 1;

    cpuid(0x80000000u, &ext, &b, &c, &d);
    if (ext >= 0x80000001u) {
        cpuid(0x80000001u, &a, &b, &c, &d);
        U8(KUSD_PROCESSOR_FEATURES + PF_RDTSCP_INSTRUCTION_AVAILABLE) =
            (d >> 27) & 1;
    }
    (void)max;
}

void kusd_init(void) {
    static const char root[] = "C:\\Windows";
    uint64 hz = timer_tsc_hz();
    int i;

    if (kusd != NULL) {
        return;
    }
    kusd_phys = pmm_alloc_frame();
    if (kusd_phys == 0) {
        return;                     /* NT processes then get no page */
    }
    kusd = (volatile uint8 *)phys_to_virt(kusd_phys);
    for (i = 0; i < 0x1000; i++) {
        kusd[i] = 0;
    }

    /* TickCount counts timer TICKS; the multiplier turns one into ms the
     * way NT's does: ms = (ticks * multiplier) >> 24. */
    U32(KUSD_TICK_COUNT_MULTIPLIER) = (uint32)((1000ULL << 24) / timer_hz());
    U16(KUSD_IMAGE_NUMBER_LOW)  = 0x8664;
    U16(KUSD_IMAGE_NUMBER_HIGH) = 0x8664;
    for (i = 0; root[i] != '\0'; i++) {
        U16(KUSD_NT_SYSTEM_ROOT + 2 * i) = (uint16)root[i];
    }
    U32(KUSD_LARGE_PAGE_MINIMUM)     = 0x200000;
    U32(KUSD_NT_BUILD_NUMBER)        = KUSD_NT_BUILD;
    U32(KUSD_NT_PRODUCT_TYPE)        = 1;          /* NtProductWinNt */
    U8(KUSD_PRODUCT_TYPE_IS_VALID)   = 1;
    U16(KUSD_NATIVE_PROCESSOR_ARCH)  = 9;          /* PROCESSOR_ARCHITECTURE_AMD64 */
    U32(KUSD_NT_MAJOR_VERSION)       = KUSD_NT_MAJOR;
    U32(KUSD_NT_MINOR_VERSION)       = KUSD_NT_MINOR;
    U32(KUSD_NUMBER_OF_PHYSICAL_PAGES) = (uint32)pmm_total_frames();
    /* The frequency NtQueryPerformanceCounter reports (nt_sys.c): the TSC's
     * when it is calibrated, otherwise the tick. QpcBypassEnabled stays 0,
     * so a reader asks the kernel for the counter itself. */
    U64(KUSD_QPC_FREQUENCY) = hz != 0 ? hz : timer_hz();
    U32(KUSD_ACTIVE_PROCESSOR_COUNT) = (uint32)smp_cpu_count();
    U8(KUSD_ACTIVE_GROUP_COUNT)      = 1;
    features();
    kusd_tick();
}

void kusd_tick(void) {
    uint64 ticks, interrupt_time;

    if (kusd == NULL) {
        return;
    }
    ticks = timer_ticks_now();
    interrupt_time = ticks * (10000000ULL / timer_hz());
    ksystem_time_set(KUSD_INTERRUPT_TIME, interrupt_time);
    ksystem_time_set(KUSD_SYSTEM_TIME,
                     timer_realtime_ns() / 100 + NT_EPOCH_DELTA_100NS);
    ksystem_time_set(KUSD_TICK_COUNT, ticks);
    U32(KUSD_TICK_COUNT_LOW_DEPRECATED) = (uint32)ticks;
}

int kusd_map(address_space_t *as) {
    if (kusd == NULL || as == NULL) {
        return -12;
    }
    /* The space holds a reference like any other mapping of the frame, so
     * vmm_space_destroy's ordinary free just drops it again; the page itself
     * stays referenced by the kernel and is never freed. Read-only and
     * no-execute, and no PAGE_COW, so a write is an access violation in the
     * process rather than a private copy. */
    pmm_ref_frame(kusd_phys);
    if (!vmm_map_page_in(as, KUSD_USER_VA, kusd_phys,
                         PAGE_PRESENT | PAGE_USER | PAGE_NX)) {
        pmm_free_frame(kusd_phys);
        return -12;
    }
    return 0;
}

/* PEB offsets of the version fields (x64). */
#define PEB_NUMBER_OF_PROCESSORS  0x0B8
#define PEB_OS_MAJOR_VERSION      0x118
#define PEB_OS_MINOR_VERSION      0x11C
#define PEB_OS_BUILD_NUMBER       0x120
#define PEB_OS_PLATFORM_ID        0x124

void kusd_fill_peb(address_space_t *as) {
    phys_addr_t phys = vmm_get_phys_in(as, NT_PEB_BASE);
    uint8 *peb;

    if (phys == 0) {
        return;
    }
    peb = (uint8 *)phys_to_virt(phys & ~0xFFFULL);
    *(uint32 *)(peb + PEB_NUMBER_OF_PROCESSORS) = (uint32)smp_cpu_count();
    *(uint32 *)(peb + PEB_OS_MAJOR_VERSION)     = KUSD_NT_MAJOR;
    *(uint32 *)(peb + PEB_OS_MINOR_VERSION)     = KUSD_NT_MINOR;
    *(uint16 *)(peb + PEB_OS_BUILD_NUMBER)      = KUSD_NT_BUILD;
    *(uint32 *)(peb + PEB_OS_PLATFORM_ID)       = 2;    /* VER_PLATFORM_WIN32_NT */
}
