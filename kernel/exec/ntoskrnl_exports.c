#include "ntoskrnl_exports.h"
#include "typesk.h"
#include "wdm.h"

/* See ntoskrnl_exports.h. Every address below is a real, already-working
 * function from kernel/wdm.c (previous pass) - this table adds zero new
 * implementation, only a name a loaded .sys's import table can find. */

typedef struct {
    const char *name;
    void       *addr;
} nt_export_t;

static const nt_export_t ntoskrnl_exports[] = {
    { "IoCreateDevice",              (void *)IoCreateDevice },
    { "IoDeleteDevice",              (void *)IoDeleteDevice },
    { "IoCompleteRequest",           (void *)IoCompleteRequest },
    { "IoGetCurrentIrpStackLocation",(void *)IoGetCurrentIrpStackLocation },
    { "ExAllocatePool2",             (void *)ExAllocatePool2 },
    { "ExFreePool",                  (void *)ExFreePool },
    { "KeAcquireSpinLock",           (void *)KeAcquireSpinLock },
    { "KeReleaseSpinLock",           (void *)KeReleaseSpinLock },
    { "KeRaiseIrql",                 (void *)KeRaiseIrql },
    { "KeLowerIrql",                 (void *)KeLowerIrql },
    { "KeGetCurrentIrql",            (void *)KeGetCurrentIrql },
    { "DbgPrint",                    (void *)DbgPrint },

    /* The multiprocessor interface - kernel/driver/wdm_smp.c. */
    { "KeNumberProcessors",          (void *)&KeNumberProcessors },  /* DATA */
    { "KeGetCurrentProcessorNumber", (void *)KeGetCurrentProcessorNumber },
    { "KeGetCurrentProcessorNumberEx", (void *)KeGetCurrentProcessorNumberEx },
    { "KeQueryActiveProcessorCount", (void *)KeQueryActiveProcessorCount },
    { "KeQueryActiveProcessorCountEx", (void *)KeQueryActiveProcessorCountEx },
    { "KeQueryActiveProcessors",     (void *)KeQueryActiveProcessors },
    { "KeQueryMaximumProcessorCount", (void *)KeQueryMaximumProcessorCount },
    { "KeQueryMaximumProcessorCountEx", (void *)KeQueryMaximumProcessorCountEx },
    { "KeQueryActiveGroupCount",     (void *)KeQueryActiveGroupCount },
    { "KeQueryMaximumGroupCount",    (void *)KeQueryMaximumGroupCount },
    { "KeGetProcessorIndexFromNumber", (void *)KeGetProcessorIndexFromNumber },
    { "KeGetProcessorNumberFromIndex", (void *)KeGetProcessorNumberFromIndex },
    { "KeIpiGenericCall",            (void *)KeIpiGenericCall },
    { "KeGenericCallDpc",            (void *)KeGenericCallDpc },
    { "KeSignalCallDpcSynchronize",  (void *)KeSignalCallDpcSynchronize },
    { "KeSignalCallDpcDone",         (void *)KeSignalCallDpcDone },
    { "KeInitializeDpc",             (void *)KeInitializeDpc },
    { "KeInitializeThreadedDpc",     (void *)KeInitializeThreadedDpc },
    { "KeSetTargetProcessorDpc",     (void *)KeSetTargetProcessorDpc },
    { "KeSetTargetProcessorDpcEx",   (void *)KeSetTargetProcessorDpcEx },
    { "KeSetImportanceDpc",          (void *)KeSetImportanceDpc },
    { "KeInsertQueueDpc",            (void *)KeInsertQueueDpc },
    { "KeRemoveQueueDpc",            (void *)KeRemoveQueueDpc },
    { "KeFlushQueuedDpcs",           (void *)KeFlushQueuedDpcs },
    { "KeSetSystemAffinityThread",   (void *)KeSetSystemAffinityThread },
    { "KeSetSystemAffinityThreadEx", (void *)KeSetSystemAffinityThreadEx },
    { "KeRevertToUserAffinityThread", (void *)KeRevertToUserAffinityThread },
    { "KeRevertToUserAffinityThreadEx", (void *)KeRevertToUserAffinityThreadEx },
    { "KeSetSystemGroupAffinityThread", (void *)KeSetSystemGroupAffinityThread },
    { "KeRevertToUserGroupAffinityThread", (void *)KeRevertToUserGroupAffinityThread },
    { "KeInitializeSpinLock",        (void *)KeInitializeSpinLock },
    { "KeAcquireSpinLockAtDpcLevel", (void *)KeAcquireSpinLockAtDpcLevel },
    { "KeReleaseSpinLockFromDpcLevel", (void *)KeReleaseSpinLockFromDpcLevel },
    { "KeTryToAcquireSpinLockAtDpcLevel", (void *)KeTryToAcquireSpinLockAtDpcLevel },
    { "KeAcquireSpinLockRaiseToDpc", (void *)KeAcquireSpinLockRaiseToDpc },
    { "KeTestSpinLock",              (void *)KeTestSpinLock },
    { "KeAcquireInStackQueuedSpinLock", (void *)KeAcquireInStackQueuedSpinLock },
    { "KeReleaseInStackQueuedSpinLock", (void *)KeReleaseInStackQueuedSpinLock },
    { "KeAcquireInStackQueuedSpinLockAtDpcLevel",
                                     (void *)KeAcquireInStackQueuedSpinLockAtDpcLevel },
    { "KeReleaseInStackQueuedSpinLockFromDpcLevel",
                                     (void *)KeReleaseInStackQueuedSpinLockFromDpcLevel },
    { "ExInterlockedInsertHeadList", (void *)ExInterlockedInsertHeadList },
    { "ExInterlockedInsertTailList", (void *)ExInterlockedInsertTailList },
    { "ExInterlockedRemoveHeadList", (void *)ExInterlockedRemoveHeadList },
    { "KeStallExecutionProcessor",   (void *)KeStallExecutionProcessor },
    { "KeQueryPerformanceCounter",   (void *)KeQueryPerformanceCounter },
    { "KeQueryTickCount",            (void *)KeQueryTickCount },
    { "KeQueryTimeIncrement",        (void *)KeQueryTimeIncrement },
    { NULL, NULL },
};

static int str_eq_ci(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;

        if (x != y) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int str_eq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int nt_is_synthetic_dll(const char *dll_name) {
    return str_eq_ci(dll_name, "ntoskrnl.exe") || str_eq_ci(dll_name, "hal.dll");
}

int nt_resolve_import(const char *dll_name, const char *symbol_name,
                      uint64 *addr) {
    int i;

    (void)dll_name; /* one shared table for both names - see the header */
    for (i = 0; ntoskrnl_exports[i].name != NULL; i++) {
        /* Real PE export names are case-sensitive; only the DLL name
         * itself (nt_is_synthetic_dll above) is matched case-insensitively,
         * matching how Windows treats the two. */
        if (str_eq(ntoskrnl_exports[i].name, symbol_name)) {
            *addr = (uint64)ntoskrnl_exports[i].addr;
            return 1;
        }
    }
    return 0;
}
