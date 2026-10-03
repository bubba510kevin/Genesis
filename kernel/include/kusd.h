#ifndef KUSD_H
#define KUSD_H

#include "paging.h"
#include "typesk.h"

/* KUSER_SHARED_DATA (ROADMAP 16(s)): one read-only page the kernel shares
 * with every Windows process at 0x7FFE0000, where user mode reads the time,
 * the tick count and the NT version WITHOUT a system call. Windows binaries
 * - Microsoft's own DLLs above all, which phase 4 runs precompiled - read it
 * directly: GetTickCount is a multiply of two fields there, and the version
 * checks behind GetVersionEx/IsProcessorFeaturePresent read it too.
 *
 * One physical page for the whole machine, written by the kernel through the
 * direct map and mapped user-readable (never writable, never executable)
 * into each NT address space. The clock fields are refreshed on every timer
 * tick with NT's KSYSTEM_TIME protocol: High2Time, then LowPart, then
 * High1Time, so a reader that sees High1Time == High2Time around its read of
 * LowPart has a consistent value.
 *
 * Genesis reports itself as NT 10.0 build 19045 (Windows 10 22H2), x64,
 * workstation - the parity target of phase 2. The same numbers go into the
 * PEB's OSMajorVersion fields (kusd_fill_peb). */

#define KUSD_USER_VA          0x000000007FFE0000ULL

#define KUSD_NT_MAJOR         10
#define KUSD_NT_MINOR         0
#define KUSD_NT_BUILD         19045

/* Field offsets - the Windows 10 x64 layout. Only the fields filled in are
 * named; everything else is zero, which for each is a legitimate value
 * ("not enabled", "none", "unknown"). */
#define KUSD_TICK_COUNT_LOW_DEPRECATED  0x000
#define KUSD_TICK_COUNT_MULTIPLIER      0x004
#define KUSD_INTERRUPT_TIME             0x008   /* KSYSTEM_TIME, 100ns since boot  */
#define KUSD_SYSTEM_TIME                0x014   /* KSYSTEM_TIME, 100ns since 1601  */
#define KUSD_TIME_ZONE_BIAS             0x020   /* KSYSTEM_TIME: UTC, so 0         */
#define KUSD_IMAGE_NUMBER_LOW           0x02C
#define KUSD_IMAGE_NUMBER_HIGH          0x02E
#define KUSD_NT_SYSTEM_ROOT             0x030   /* WCHAR[260]                      */
#define KUSD_LARGE_PAGE_MINIMUM         0x244
#define KUSD_NT_BUILD_NUMBER            0x260
#define KUSD_NT_PRODUCT_TYPE            0x264
#define KUSD_PRODUCT_TYPE_IS_VALID      0x268
#define KUSD_NATIVE_PROCESSOR_ARCH      0x26A
#define KUSD_NT_MAJOR_VERSION           0x26C
#define KUSD_NT_MINOR_VERSION           0x270
#define KUSD_PROCESSOR_FEATURES         0x274   /* BOOLEAN[64]                     */
#define KUSD_NUMBER_OF_PHYSICAL_PAGES   0x2E8
#define KUSD_QPC_FREQUENCY              0x300
#define KUSD_TICK_COUNT                 0x320   /* KSYSTEM_TIME, in ticks          */
#define KUSD_ACTIVE_PROCESSOR_COUNT     0x3C0
#define KUSD_ACTIVE_GROUP_COUNT         0x3C4

/* PF_* indices into ProcessorFeatures, as IsProcessorFeaturePresent takes. */
#define PF_COMPARE_EXCHANGE_DOUBLE      2
#define PF_MMX_INSTRUCTIONS_AVAILABLE   3
#define PF_XMMI_INSTRUCTIONS_AVAILABLE  6
#define PF_RDTSC_INSTRUCTION_AVAILABLE  8
#define PF_PAE_ENABLED                  9
#define PF_XMMI64_INSTRUCTIONS_AVAILABLE 10
#define PF_NX_ENABLED                   12
#define PF_SSE3_INSTRUCTIONS_AVAILABLE  13
#define PF_COMPARE_EXCHANGE128          14
#define PF_FASTFAIL_AVAILABLE           23
#define PF_RDRAND_INSTRUCTION_AVAILABLE 28
#define PF_RDTSCP_INSTRUCTION_AVAILABLE 32
#define PF_SSSE3_INSTRUCTIONS_AVAILABLE 36
#define PF_SSE4_1_INSTRUCTIONS_AVAILABLE 37
#define PF_SSE4_2_INSTRUCTIONS_AVAILABLE 38
#define PF_AVX_INSTRUCTIONS_AVAILABLE   39

/* Allocate and fill the page. Once, at boot, after the PMM, the timer and
 * SMP bring-up (it records the CPU count). */
void kusd_init(void);

/* Refresh the clock fields. Every tick, on the CPU that owns the clock. */
void kusd_tick(void);

/* Map the page read-only at KUSD_USER_VA in `as`. 0 or a negative errno. */
int kusd_map(address_space_t *as);

/* Write the version fields of the PEB at NT_PEB_BASE in `as`
 * (OSMajorVersion, OSMinorVersion, OSBuildNumber, OSPlatformId,
 * NumberOfProcessors) - the same numbers the shared page carries. */
void kusd_fill_peb(address_space_t *as);

#endif
