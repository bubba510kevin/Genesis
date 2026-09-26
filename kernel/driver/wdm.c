#include "device.h"
#include "kheap.h"
#include "kprintf.h"
#include "lapic.h"
#include "pci.h"
#include <stdarg.h>
#include "typesk.h"
#include "wdm.h"

/* See wdm.h. Two adapters live here, both translating in the same
 * direction bus.h's own vocabulary already implies:
 *
 *   1. bus_dev_t/driver_t (Newbus's matching engine) -> DRIVER_OBJECT/
 *      AddDevice - built the same slot-trampoline way lkpi.c's Linux-shaped
 *      adapter is, and for the identical reason: driver_t's probe/attach
 *      are bare function pointers with no user-data slot, so telling two
 *      registered drivers apart needs one distinct trampoline per driver,
 *      not one shared function with an argument C has nowhere to pass.
 *
 *   2. device_ops_t (device.h) -> MajorFunction[] - one synthesized IRP per
 *      call, the direct payoff of device.h's own promise that its five
 *      slots are IRP major functions in disguise.
 */

#define WDM_MAX_DRIVERS 4

static DRIVER_OBJECT wdm_driver_pool[WDM_MAX_DRIVERS];
static int           wdm_driver_count;

static int wdm_probe_common(int slot, bus_dev_t *dev) {
    PDRIVER_OBJECT drv = &wdm_driver_pool[slot];
    const pci_ivars_t *f = (const pci_ivars_t *)bus_get_ivars(dev);
    const WDM_HARDWARE_ID *id;

    if (drv->HardwareIds == NULL) {
        /* No table set - match everything, the same BUS_PROBE_GENERIC
         * catch-all role pci_generic.c plays for Newbus. */
        return BUS_PROBE_GENERIC;
    }
    for (id = drv->HardwareIds; !(id->VendorId == 0 && id->DeviceId == 0); id++) {
        if (id->VendorId == f->vendor_id && id->DeviceId == f->device_id) {
            return BUS_PROBE_DEFAULT;
        }
    }
    return 1; /* not mine */
}

/* One PDO per attached device, from a fixed pool.
 *
 * This used to be a DEVICE_OBJECT local in wdm_attach_common - a throwaway
 * synthesized on the stack and gone the moment AddDevice returned. That was
 * survivable only because nothing could attach to it; now that
 * IoAttachDeviceToDeviceStack writes AttachedDevice into the target, the PDO
 * has to outlive the call that created it, or the FDO above holds a pointer
 * into a dead stack frame. */
static DEVICE_OBJECT wdm_pdo_pool[WDM_MAX_DRIVERS * 4];
static int           wdm_pdo_count;

static int wdm_attach_common(int slot, bus_dev_t *dev) {
    PDRIVER_OBJECT drv = &wdm_driver_pool[slot];
    PDEVICE_OBJECT pdo;
    NTSTATUS st;

    if (drv->DriverExtension == NULL || drv->DriverExtension->AddDevice == NULL) {
        return -1;
    }
    if (wdm_pdo_count >= (int)(sizeof(wdm_pdo_pool) / sizeof(wdm_pdo_pool[0]))) {
        return -1;
    }

    /* The physical device object - the bottom of the stack. BusDev is what
     * AddDevice needs to find out which PCI function it was just handed.
     * DriverObject stays NULL: a PDO enumerated by the bus is not owned by
     * a function driver, and IoCallDriver returning STATUS_NO_SUCH_DEVICE
     * for it is the correct answer rather than an oversight - a real bus
     * driver would own it, and this tree has no WDM bus driver. */
    pdo = &wdm_pdo_pool[wdm_pdo_count++];
    pdo->DriverObject    = NULL;
    pdo->DeviceExtension = NULL;
    pdo->BusDev          = dev;
    pdo->AttachedDevice  = NULL;
    pdo->StackSize       = 1;

    st = drv->DriverExtension->AddDevice(drv, pdo);
    return NT_SUCCESS(st) ? 0 : -1;
}

/* One pair per slot - see the file comment. WDM_MAX_DRIVERS above is the
 * only place the count needs to change. */
static int wdm_probe_0(bus_dev_t *dev)  { return wdm_probe_common(0, dev); }
static int wdm_attach_0(bus_dev_t *dev) { return wdm_attach_common(0, dev); }
static int wdm_probe_1(bus_dev_t *dev)  { return wdm_probe_common(1, dev); }
static int wdm_attach_1(bus_dev_t *dev) { return wdm_attach_common(1, dev); }
static int wdm_probe_2(bus_dev_t *dev)  { return wdm_probe_common(2, dev); }
static int wdm_attach_2(bus_dev_t *dev) { return wdm_attach_common(2, dev); }
static int wdm_probe_3(bus_dev_t *dev)  { return wdm_probe_common(3, dev); }
static int wdm_attach_3(bus_dev_t *dev) { return wdm_attach_common(3, dev); }

static int (*const wdm_probe_fns[WDM_MAX_DRIVERS])(bus_dev_t *) = {
    wdm_probe_0, wdm_probe_1, wdm_probe_2, wdm_probe_3,
};
static int (*const wdm_attach_fns[WDM_MAX_DRIVERS])(bus_dev_t *) = {
    wdm_attach_0, wdm_attach_1, wdm_attach_2, wdm_attach_3,
};

static driver_t wdm_driver_t_pool[WDM_MAX_DRIVERS];

NTSTATUS IoCreateDriver(const char *DriverName,
                        PDRIVER_INITIALIZE InitializationFunction) {
    int slot = wdm_driver_count;
    PDRIVER_OBJECT drv;
    driver_t *bus_drv;
    NTSTATUS st;
    int i;

    if (slot >= WDM_MAX_DRIVERS) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    drv = &wdm_driver_pool[slot];
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        drv->MajorFunction[i] = NULL;
    }
    drv->ExtensionStorage.AddDevice = NULL;
    drv->DriverExtension = &drv->ExtensionStorage;
    drv->HardwareIds     = NULL;
    drv->DriverName      = DriverName;

    st = InitializationFunction(drv, NULL);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    bus_drv = &wdm_driver_t_pool[slot];
    bus_drv->name   = drv->DriverName;
    bus_drv->probe  = wdm_probe_fns[slot];
    bus_drv->attach = wdm_attach_fns[slot];
    /* remove()/PnP-surprise-removal wiring is deferred with the rest of
     * hot-remove - see bus.h/device.h's identical posture. */
    bus_drv->detach = NULL;
    /* Nothing here for bus.c to allocate: a real driver's own AddDevice
     * calls IoCreateDevice for whatever per-device state it needs, the
     * same way disk_register calls dev_alloc separately rather than
     * having bus.c do it. */
    bus_drv->softc_size = 0;

    wdm_driver_count++;
    if (devclass_add_driver(devclass_find("pci"), bus_drv) != 0) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

NTSTATUS WDM_ABI IoCreateDevice(PDRIVER_OBJECT DriverObject, uint32 DeviceExtensionSize,
                        PDEVICE_OBJECT *DeviceObject) {
    PDEVICE_OBJECT dobj = (PDEVICE_OBJECT)kcalloc(1, sizeof(DEVICE_OBJECT));

    if (dobj == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    dobj->DriverObject = DriverObject;
    dobj->BusDev        = NULL;
    dobj->DeviceExtension = NULL;
    dobj->AttachedDevice  = NULL;
    /* One location, until something attaches on top. A driver allocating an
     * IRP for its own device asks for exactly this many. */
    dobj->StackSize       = 1;

    if (DeviceExtensionSize > 0) {
        dobj->DeviceExtension = kcalloc(1, DeviceExtensionSize);
        if (dobj->DeviceExtension == NULL) {
            kfree(dobj);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    *DeviceObject = dobj;
    return STATUS_SUCCESS;
}

void WDM_ABI IoDeleteDevice(PDEVICE_OBJECT DeviceObject) {
    if (DeviceObject == NULL) {
        return;
    }
    if (DeviceObject->DeviceExtension != NULL) {
        kfree(DeviceObject->DeviceExtension);
    }
    kfree(DeviceObject);
}

/* --- IRP pool ------------------------------------------------------------
 *
 * A fixed pool rather than kmalloc, matching this tree's convention and for
 * a reason specific to IRPs: they are allocated on the I/O path, and an
 * allocator that can fail under memory pressure exactly when the system is
 * trying to write memory out is the classic deadlock this design avoids.
 * Sixteen is generous for a kernel whose dispatch is synchronous end to end
 * - an IRP is freed before the call that made it returns - so the only way
 * to hold more than one at once is a driver deliberately keeping one. */
#define IRP_POOL_SIZE 16

static IRP irp_pool[IRP_POOL_SIZE];
static int irp_inuse_count;
static int irp_peak_count;

PIRP WDM_ABI IoAllocateIrp(int8 StackSize, uint8 ChargeQuota) {
    int i, k;

    (void)ChargeQuota;   /* no quota accounting - there are no processes
                          * charged for kernel-mode I/O here */

    if (StackSize < 1 || StackSize > IRP_MAX_STACK_LOCATIONS) {
        return NULL;
    }
    for (i = 0; i < IRP_POOL_SIZE; i++) {
        if (irp_pool[i].InUse) {
            continue;
        }
        /* Zeroed on allocation, not on free. Both work; doing it here means
         * a use-after-free reads the stale request rather than zeroes,
         * which is far easier to recognise as a use-after-free. */
        {
            uint8 *p = (uint8 *)&irp_pool[i];
            uint64 b;
            for (b = 0; b < sizeof(IRP); b++) {
                p[b] = 0;
            }
        }
        irp_pool[i].InUse           = 1;
        irp_pool[i].StackCount      = StackSize;
        /* One ABOVE the top: the first IoCallDriver steps down onto
         * StackSize. This is why a caller fills in the NEXT location before
         * that first call and not the current one - there is no current
         * one yet, and IoGetCurrentIrpStackLocation would hand back
         * Stack[StackSize + 1], which is off the end. */
        irp_pool[i].CurrentLocation = (int8)(StackSize + 1);
        irp_pool[i].IoStatus.Status = STATUS_SUCCESS;

        irp_inuse_count++;
        if (irp_inuse_count > irp_peak_count) {
            irp_peak_count = irp_inuse_count;
        }
        for (k = 0; k <= StackSize; k++) {
            irp_pool[i].Stack[k].DeviceObject = NULL;
        }
        return &irp_pool[i];
    }
    return NULL;
}

void WDM_ABI IoFreeIrp(PIRP Irp) {
    if (Irp == NULL || !Irp->InUse) {
        return;
    }
    Irp->InUse = 0;
    irp_inuse_count--;
}

PIO_STACK_LOCATION WDM_ABI IoGetCurrentIrpStackLocation(PIRP Irp) {
    if (Irp->CurrentLocation < 1 || Irp->CurrentLocation > Irp->StackCount) {
        /* Off either end. Returning Stack[0] - which is deliberately unused
         * - keeps a misbehaving driver reading a real, zeroed structure
         * instead of walking off the array. */
        return &Irp->Stack[0];
    }
    return &Irp->Stack[Irp->CurrentLocation];
}

PIO_STACK_LOCATION WDM_ABI IoGetNextIrpStackLocation(PIRP Irp) {
    int next = Irp->CurrentLocation - 1;

    if (next < 1 || next > Irp->StackCount) {
        return &Irp->Stack[0];
    }
    return &Irp->Stack[next];
}

void WDM_ABI IoSkipCurrentIrpStackLocation(PIRP Irp) {
    if (Irp->CurrentLocation <= Irp->StackCount) {
        Irp->CurrentLocation++;
    }
}

void WDM_ABI IoCopyCurrentIrpStackLocationToNext(PIRP Irp) {
    PIO_STACK_LOCATION cur  = IoGetCurrentIrpStackLocation(Irp);
    PIO_STACK_LOCATION next = IoGetNextIrpStackLocation(Irp);

    if (cur == next) {
        return;
    }
    *next = *cur;
    /* The copy must NOT carry this driver's completion routine down. If it
     * did, the routine would be registered on two locations and run twice
     * for one request - and the second run would be on an IRP the driver
     * has already released. */
    next->CompletionRoutine = NULL;
    next->Context           = NULL;
    next->InvokeOnSuccess   = 0;
    next->InvokeOnError     = 0;
    next->InvokeOnCancel    = 0;
}

void WDM_ABI IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE Routine,
                                    void *Context, uint8 InvokeOnSuccess,
                                    uint8 InvokeOnError, uint8 InvokeOnCancel) {
    PIO_STACK_LOCATION next = IoGetNextIrpStackLocation(Irp);

    next->CompletionRoutine = Routine;
    next->Context           = Context;
    next->InvokeOnSuccess   = InvokeOnSuccess;
    next->InvokeOnError     = InvokeOnError;
    next->InvokeOnCancel    = InvokeOnCancel;
}

NTSTATUS WDM_ABI IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION loc;
    uint8 major;

    if (DeviceObject == NULL || Irp == NULL) {
        return STATUS_NO_SUCH_DEVICE;
    }
    if (Irp->CurrentLocation <= 1) {
        /* Nowhere left to go. A driver forwarding past the bottom of the
         * stack is a bug in that driver, and it is worth failing loudly
         * here rather than indexing Stack[0] and calling whatever the
         * zeroed MajorFunction 0 resolves to. */
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp->CurrentLocation--;
    loc = &Irp->Stack[Irp->CurrentLocation];
    loc->DeviceObject = DeviceObject;

    major = loc->MajorFunction;
    if (DeviceObject->DriverObject == NULL ||
        major > IRP_MJ_MAXIMUM_FUNCTION ||
        DeviceObject->DriverObject->MajorFunction[major] == NULL) {
        return STATUS_NO_SUCH_DEVICE;
    }
    return DeviceObject->DriverObject->MajorFunction[major](DeviceObject, Irp);
}

void WDM_ABI IoCompleteRequest(PIRP Irp, int8 PriorityBoost) {
    (void)PriorityBoost;   /* no scheduler boost - nothing blocks on an IRP
                            * here, the dispatch is synchronous */

    if (Irp == NULL) {
        return;
    }

    /* Walk back UP, running each level's completion routine. The routine
     * stored at location N belongs to the driver at N+1 - the one that
     * passed the request down - which is why the DeviceObject handed to it
     * is the one from the location ABOVE, not the one that just completed.
     * Getting that wrong gives a filter its own device pointer instead of
     * the caller's and looks correct until two filters are stacked. */
    while (Irp->CurrentLocation <= Irp->StackCount) {
        PIO_STACK_LOCATION loc = &Irp->Stack[Irp->CurrentLocation];
        PIO_COMPLETION_ROUTINE routine = loc->CompletionRoutine;
        void          *ctx    = loc->Context;
        uint8          on_ok  = loc->InvokeOnSuccess;
        uint8          on_err = loc->InvokeOnError;
        PDEVICE_OBJECT owner  = NULL;
        int            succeeded = NT_SUCCESS(Irp->IoStatus.Status);

        if (Irp->CurrentLocation + 1 <= Irp->StackCount) {
            owner = Irp->Stack[Irp->CurrentLocation + 1].DeviceObject;
        }

        Irp->CurrentLocation++;

        if (routine != NULL &&
            ((succeeded && on_ok) || (!succeeded && on_err))) {
            NTSTATUS st = routine(owner, Irp, ctx);

            if (st == STATUS_MORE_PROCESSING_REQUIRED) {
                /* The driver has taken the IRP back. Stop here and do not
                 * touch it again - it may already have been freed or
                 * re-sent by the routine that just ran. */
                return;
            }
        }
    }
}

PDEVICE_OBJECT WDM_ABI IoAttachDeviceToDeviceStack(PDEVICE_OBJECT SourceDevice,
                                                   PDEVICE_OBJECT TargetDevice) {
    PDEVICE_OBJECT top;
    int guard;

    if (SourceDevice == NULL || TargetDevice == NULL) {
        return NULL;
    }

    /* Walk to the CURRENT top of the target's stack, which may not be the
     * target itself - another filter may already have attached. Bounded,
     * because a corrupted AttachedDevice chain would otherwise spin here
     * forever. */
    top = TargetDevice;
    for (guard = 0; top->AttachedDevice != NULL; guard++) {
        if (guard > IRP_MAX_STACK_LOCATIONS) {
            return NULL;
        }
        top = top->AttachedDevice;
    }
    if (top->StackSize >= IRP_MAX_STACK_LOCATIONS) {
        return NULL;
    }

    top->AttachedDevice   = SourceDevice;
    SourceDevice->StackSize = (int8)(top->StackSize + 1);
    return top;
}

void WDM_ABI IoDetachDevice(PDEVICE_OBJECT TargetDevice) {
    if (TargetDevice == NULL) {
        return;
    }
    TargetDevice->AttachedDevice = NULL;
}

/* --- IRQL / spinlocks: REAL now ----------------------------------------
 *
 * These were "honest uniprocessor stubs": KeAcquireSpinLock set a byte that
 * nothing could contend, and the IRQL was a variable that masked nothing.
 * Both were true statements about a kernel with one CPU and no LAPIC. Parts
 * 10 and 11 removed both premises.
 *
 * --- IRQL onto the TPR ---------------------------------------------------
 *
 * NT's IRQL and the LAPIC's task-priority register are the same idea: a
 * threshold below which interrupts do not get through. The LAPIC blocks any
 * vector whose priority class (vector >> 4) is at or below TPR >> 4, so
 * IRQL maps onto it by shifting into the top nibble.
 *
 * That mapping is exact for the levels this kernel uses and deliberately
 * NOT a general one. Real NT has 32 levels with specific hardware meanings
 * (DIRQL is per-device, derived from the vector the device was assigned);
 * modelling that faithfully would mean a device-to-IRQL table with no
 * consumer. What is real here is that raising IRQL now actually stops
 * interrupts arriving, which is the property a driver depends on.
 *
 * The legacy PIC is NOT affected by the TPR - it delivers through LINT0 in
 * virtual-wire mode. So raising IRQL masks dynamically allocated vectors
 * (48+, which is MSI) and does not mask the timer or the keyboard. Said out
 * loud because a driver expecting DISPATCH_LEVEL to hold off the clock will
 * be disappointed, and the honest answer is that Genesis's clock is not on
 * the LAPIC yet.
 */

static KIRQL irql_from_tpr(uint8 tpr) {
    return (KIRQL)(tpr >> 4);
}

static uint8 tpr_from_irql(KIRQL irql) {
    return (uint8)(irql << 4);
}

void WDM_ABI KeRaiseIrql(KIRQL NewIrql, KIRQL *OldIrql) {
    uint8 old = lapic_get_tpr();

    *OldIrql = irql_from_tpr(old);

    /* Raise only. NT's KeRaiseIrql is documented to fault if asked to
     * LOWER, and honouring that is worth more than being permissive: a
     * driver that passes the wrong level gets a visible complaint instead of
     * silently unmasking interrupts it believed it had blocked. */
    if (tpr_from_irql(NewIrql) < old) {
        kprintf_c(0x0C, "wdm: KeRaiseIrql asked to LOWER %d -> %d\n",
                  irql_from_tpr(old), NewIrql);
        return;
    }
    lapic_set_tpr(tpr_from_irql(NewIrql));
}

void WDM_ABI KeLowerIrql(KIRQL NewIrql) {
    lapic_set_tpr(tpr_from_irql(NewIrql));
}

KIRQL WDM_ABI KeGetCurrentIrql(void) {
    return irql_from_tpr(lapic_get_tpr());
}

/* A real spin lock now. KSPIN_LOCK's `locked` field is the lock word itself,
 * so a driver's KSPIN_LOCK is genuinely the thing being contended rather
 * than a byte set beside one.
 *
 * Raising to DISPATCH_LEVEL is part of the contract and not decoration: a
 * spin lock taken by both a driver and its own ISR deadlocks on ONE CPU
 * without the raise, because the ISR interrupts the holder and spins for a
 * lock that cannot be released until the ISR returns. */
void WDM_ABI KeAcquireSpinLock(PKSPIN_LOCK SpinLock, KIRQL *OldIrql) {
    KIRQL old;

    KeRaiseIrql(DISPATCH_LEVEL, &old);
    *OldIrql = old;

    for (;;) {
        uint32 prev;

        __asm__ volatile ("xchgl %0, %1"
                          : "=r"(prev), "+m"(SpinLock->locked)
                          : "0"(1u)
                          : "memory");
        if (prev == 0) {
            return;
        }
        while (SpinLock->locked) {
            __asm__ volatile ("pause");
        }
    }
}

void WDM_ABI KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql) {
    __asm__ volatile ("" : : : "memory");
    SpinLock->locked = 0;
    KeLowerIrql(NewIrql);
}

void *WDM_ABI ExAllocatePool2(uint32 Flags, uint64 NumberOfBytes, uint32 Tag) {
    (void)Flags;
    (void)Tag;
    return kcalloc(1, NumberOfBytes);
}

void WDM_ABI ExFreePool(void *P) {
    kfree(P);
}

/* --- device_ops_t -> MajorFunction[] ------------------------------------- */

/* Every one of these four used to build a STACK-ALLOCATED IRP with one
 * embedded location and call MajorFunction[] directly, bypassing the device
 * stack entirely. They now allocate a real IRP sized to the whole stack and
 * enter it at the TOP through IoCallDriver, so a filter attached above the
 * function driver actually sees the request.
 *
 * dobj->StackSize is what makes that work: it is 1 for a bare device and
 * grows with every attach, so the IRP is exactly deep enough for the stack
 * that exists right now rather than a guessed maximum.
 *
 * Note IoGetNextIrpStackLocation and not IoGetCurrentIrpStackLocation. A
 * freshly allocated IRP has not been given to anybody, so there is no
 * current location; the first IoCallDriver steps down onto the one filled
 * in here. */
static PIRP wdm_begin(PDEVICE_OBJECT dobj, uint8 major, void *buffer,
                      uint32 in_len, uint32 out_len, uint32 code,
                      uint8 minor) {
    PIRP irp;
    PIO_STACK_LOCATION loc;

    if (dobj == NULL) {
        return NULL;
    }
    irp = IoAllocateIrp(dobj->StackSize, 0);
    if (irp == NULL) {
        return NULL;
    }
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    irp->UserBuffer           = buffer;

    loc = IoGetNextIrpStackLocation(irp);
    loc->MajorFunction = major;
    loc->MinorFunction = minor;
    loc->Parameters_DeviceIoControl.InputBufferLength  = in_len;
    loc->Parameters_DeviceIoControl.OutputBufferLength = out_len;
    loc->Parameters_DeviceIoControl.IoControlCode      = code;
    return irp;
}

/* The device a request must ENTER the stack at - the topmost attached
 * device, not the one device.c happens to hold a pointer to. Entering below
 * a filter would be indistinguishable from having no filter at all, which
 * is exactly the bug this pass exists to make impossible. */
static PDEVICE_OBJECT wdm_stack_top(PDEVICE_OBJECT dobj) {
    int guard;

    for (guard = 0; dobj != NULL && dobj->AttachedDevice != NULL; guard++) {
        if (guard > IRP_MAX_STACK_LOCATIONS) {
            break;
        }
        dobj = dobj->AttachedDevice;
    }
    return dobj;
}

static int64 wdm_dev_read(device_t *gdev, uint64 offset, void *buf, uint64 n) {
    PDEVICE_OBJECT dobj = wdm_stack_top((PDEVICE_OBJECT)gdev->body);
    PIRP irp;
    NTSTATUS st;
    int64 rc;

    (void)offset; /* Parameters models DeviceIoControl's shape only - see
                    * wdm.h; a driver needing the byte offset is what grows
                    * this the same way IRP_MJ_* itself grows. */
    irp = wdm_begin(dobj, IRP_MJ_READ, buf, 0, (uint32)n, 0, 0);
    if (irp == NULL) {
        return -1;
    }
    st = IoCallDriver(dobj, irp);
    rc = NT_SUCCESS(st) ? (int64)irp->IoStatus.Information : -1;
    IoFreeIrp(irp);
    return rc;
}

static int64 wdm_dev_write(device_t *gdev, uint64 offset, const void *buf, uint64 n) {
    PDEVICE_OBJECT dobj = wdm_stack_top((PDEVICE_OBJECT)gdev->body);
    PIRP irp;
    NTSTATUS st;
    int64 rc;

    (void)offset;
    irp = wdm_begin(dobj, IRP_MJ_WRITE, (void *)buf, (uint32)n, 0, 0, 0);
    if (irp == NULL) {
        return -1;
    }
    st = IoCallDriver(dobj, irp);
    rc = NT_SUCCESS(st) ? (int64)irp->IoStatus.Information : -1;
    IoFreeIrp(irp);
    return rc;
}

static int wdm_dev_control(device_t *gdev, uint32 code, void *arg, uint64 arg_size) {
    PDEVICE_OBJECT dobj = wdm_stack_top((PDEVICE_OBJECT)gdev->body);
    PIRP irp;
    NTSTATUS st;

    irp = wdm_begin(dobj, IRP_MJ_DEVICE_CONTROL, arg, (uint32)arg_size,
                    (uint32)arg_size, code, 0);
    if (irp == NULL) {
        return -1;
    }
    st = IoCallDriver(dobj, irp);
    IoFreeIrp(irp);
    return NT_SUCCESS(st) ? 0 : -1;
}

static void wdm_dev_detach(device_t *gdev) {
    PDEVICE_OBJECT dobj = wdm_stack_top((PDEVICE_OBJECT)gdev->body);
    PIRP irp;

    irp = wdm_begin(dobj, IRP_MJ_PNP, NULL, 0, 0, 0,
                    IRP_MN_SURPRISE_REMOVAL);
    if (irp == NULL) {
        return;
    }
    IoCallDriver(dobj, irp);
    IoFreeIrp(irp);
}

void WDM_ABI DbgPrint(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    kvprintf(0x0F, fmt, ap);
    va_end(ap);
}

const device_ops_t wdm_device_ops = {
    .name    = "wdm",
    .parse   = NULL,
    .read    = wdm_dev_read,
    .write   = wdm_dev_write,
    .control = wdm_dev_control,
    .detach  = wdm_dev_detach,
};
