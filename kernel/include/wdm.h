#ifndef WDM_H
#define WDM_H

#include "bus.h"
#include "device.h"
#include "typesk.h"

/* A from-scratch, Genesis-native reduction of the WDM shape - DRIVER_OBJECT/
 * MajorFunction[]/AddDevice/IRP - confirmed against Microsoft's own WDM
 * documentation, not a reuse of src/ntdll/ntdll.h's userspace NTSTATUS/
 * UNICODE_STRING (different compilation unit and target entirely: this is
 * -ffreestanding/-mcmodel=kernel, that is userspace-linked), but deliberately
 * matching that header's field names/order where both define the same NT
 * concept, so the two stay conventionally consistent rather than drifting
 * into two different ideas of what a UNICODE_STRING is.
 *
 * This is the direct payoff of device.h's own design comment: "every slot
 * below is one IRP major function, deliberately... the dispatch table it
 * registers is translated into one of these five." wdm.c's adapter is that
 * translation - a matched DRIVER_OBJECT's MajorFunction[] entries get called
 * through device_ops_t's read/write/control/parse/detach slots, not through
 * a second, parallel I/O-stack engine.
 *
 * See the plan's Non-goals: no PnP device-stacking (one driver, one device
 * object - the same assumption device.h already makes kernel-wide), no real
 * IRQL preemption (KeAcquireSpinLock/KeRaiseIrql are honest uniprocessor
 * stubs), no binary .sys loading (DriverEntry is called by hand from flk.c,
 * the same explicit-registration posture module_pci_driver already uses for
 * the Linux-shaped side).
 */

/* Real NT's x86-64 kernel-mode ABI is Win64 (args in RCX/RDX/R8/R9,
 * caller-cleaned shadow space) - the ACTUAL binary contract every real
 * .sys is compiled to, unconditionally, on every real Windows x86-64
 * system. Genesis's own kernel is plain SysV (GCC's default for
 * -ffreestanding ELF), so anything called ACROSS that boundary - a loaded
 * driver's DriverEntry/AddDevice/MajorFunction[] entries, and every native
 * function exposed through the synthetic ntoskrnl.exe export table
 * (ntoskrnl_exports.c) a loaded driver's machine code calls back into -
 * needs this attribute explicitly, on both the function-pointer TYPE and
 * the actual DEFINITION assigned to it, or the two sides disagree about
 * which registers the arguments are in. Without it, a real (or this pass's
 * synthetic, hand-assembled) .sys reading its arguments from RCX/RDX would
 * find whatever SysV-convention garbage was left in those registers, not
 * the values Genesis's C caller actually passed in RDI/RSI - silent
 * argument corruption, not a compile error. GCC/Clang support this
 * attribute purely at the compiler level (no header, no runtime
 * dependency) - the same mechanism Linux's own kernel uses to call UEFI
 * runtime services, a different but structurally identical cross-ABI
 * boundary. Calls made FROM inside a WDM_ABI function to an ordinary
 * (non-WDM_ABI) Genesis function - kprintf_c, kcalloc, and so on - are
 * unaffected: GCC picks the convention per callee, not per caller, so
 * mixing ABIs within one function body is fine and already happens
 * throughout this file. */
#define WDM_ABI __attribute__((ms_abi))

typedef int32 NTSTATUS;

#define STATUS_SUCCESS                 ((NTSTATUS)0x00000000)
#define STATUS_UNSUCCESSFUL            ((NTSTATUS)0xC0000001)
#define STATUS_NOT_SUPPORTED           ((NTSTATUS)0xC00000BBu)
#define STATUS_NO_SUCH_DEVICE          ((NTSTATUS)0xC000000Eu)
#define STATUS_INSUFFICIENT_RESOURCES  ((NTSTATUS)0xC000009Au)
/* Not an error despite the 0xC0 prefix - it is what a completion routine
 * returns to STOP IoCompleteRequest's walk up the stack and keep the IRP.
 * Real NT's value. */
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xC0000016u)
#define STATUS_PENDING                 ((NTSTATUS)0x00000103u)
#define STATUS_INVALID_DEVICE_REQUEST  ((NTSTATUS)0xC0000010u)

/* Top bit clear (interpreted as signed) is success or informational, same
 * rule ntdll.h's NT_SUCCESS uses, restated for a signed NTSTATUS. */
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

typedef struct _UNICODE_STRING {
    uint16  Length;
    uint16  MaximumLength;
    uint16 *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef uint8 KIRQL;
#define PASSIVE_LEVEL   0
#define DISPATCH_LEVEL  2

typedef struct _KSPIN_LOCK {
    /* The lock word itself, contended by real atomic exchange - not a byte
     * set beside a lock. See KeAcquireSpinLock in kernel/wdm.c. */
    volatile uint32 locked;
} KSPIN_LOCK, *PKSPIN_LOCK;

/* --- IRPs, reduced -------------------------------------------------------
 *
 * IRP_MJ_* values match real NT's numbering exactly - a driver written
 * against this switches on the same constants either way. Only the six
 * this kernel's own device_ops_t (device.h) has a slot for exist; a driver
 * touching IRP_MJ_FLUSH_BUFFERS or another one is a sign this table grows,
 * not a reason to add all ~28 speculatively. */
#define IRP_MJ_CREATE           0x00
#define IRP_MJ_CLOSE            0x02
#define IRP_MJ_READ             0x03
#define IRP_MJ_WRITE            0x04
#define IRP_MJ_DEVICE_CONTROL   0x0E
#define IRP_MJ_PNP              0x1B
#define IRP_MJ_MAXIMUM_FUNCTION 0x1B

#define IRP_MN_SURPRISE_REMOVAL 0x17

typedef struct _IO_STATUS_BLOCK {
    NTSTATUS Status;
    uint64   Information;  /* bytes transferred, or a request-specific value */
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

struct _DEVICE_OBJECT;

/* Parameters covers only DeviceIoControl's shape - IoControlCode plus the
 * two buffer lengths - the minimal case a first driver needs (see the
 * plan). Real WDM's Parameters is a union over all ~28 major functions
 * (Read, Write, Create, ...); this one grows the same way IRP_MJ_* above
 * does, when a driver actually needs Parameters.Read's byte offset rather
 * than before. */
struct _IRP;

/* Called on the way back UP the device stack by IoCompleteRequest. Returning
 * STATUS_MORE_PROCESSING_REQUIRED stops the walk and leaves the IRP alive -
 * which is how a driver that wants to look at a completed request before
 * anyone above it does keeps ownership. */
typedef NTSTATUS (WDM_ABI *PIO_COMPLETION_ROUTINE)(struct _DEVICE_OBJECT *DeviceObject,
                                                   struct _IRP *Irp,
                                                   void *Context);

typedef struct _IO_STACK_LOCATION {
    uint8  MajorFunction;
    uint8  MinorFunction;
    struct {
        uint32 OutputBufferLength;
        uint32 InputBufferLength;
        uint32 IoControlCode;
    } Parameters_DeviceIoControl;
    struct _DEVICE_OBJECT *DeviceObject;

    /* Set by IoSetCompletionRoutine on the NEXT location, not this one -
     * a completion routine belongs to the driver that is passing the IRP
     * down, and it runs when the level below finishes. */
    PIO_COMPLETION_ROUTINE CompletionRoutine;
    void                  *Context;
    uint8                  InvokeOnSuccess;
    uint8                  InvokeOnError;
    uint8                  InvokeOnCancel;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

/* How deep a device stack this build supports. Eight is far more than the
 * two this tree actually builds; the cost is per-IRP and IRPs come from a
 * fixed pool, so it is bounded either way. */
#define IRP_MAX_STACK_LOCATIONS 8

/* A REAL stack of locations, one per device in the stack, replacing the
 * single embedded CurrentStackLocation this had before.
 *
 * Indexed 1..StackCount, and CurrentLocation counts DOWN as the IRP travels
 * toward the hardware - which is NT's own direction, not an arbitrary
 * choice. It is worth matching: IoSkipCurrentIrpStackLocation and
 * IoCopyCurrentIrpStackLocationToNext are meaningless unless "next" means
 * the same direction a driver author expects, and the whole point of this
 * subsystem is that a driver written against real WDM documentation
 * behaves. CurrentLocation == StackCount + 1 means the IRP has not been
 * passed to anybody yet.
 *
 * Index 0 is left unused so the arithmetic reads the way NT's does rather
 * than carrying an off-by-one everywhere. */
typedef struct _IRP {
    IO_STATUS_BLOCK    IoStatus;
    void               *UserBuffer;
    int8               StackCount;
    int8               CurrentLocation;
    uint8              Pending;
    uint8              InUse;        /* pool bookkeeping, not NT's */
    IO_STACK_LOCATION  Stack[IRP_MAX_STACK_LOCATIONS + 1];
} IRP, *PIRP;

typedef struct _DEVICE_OBJECT {
    struct _DRIVER_OBJECT *DriverObject;
    void                  *DeviceExtension;
    bus_dev_t             *BusDev;  /* the matched PCI function, if any -
                                      * what AddDevice's PhysicalDeviceObject
                                      * argument carries here */

    /* The device attached ON TOP of this one - a filter, or the FDO above a
     * PDO. NT's direction exactly: AttachedDevice points UP. A driver that
     * needs to reach DOWN keeps the pointer IoAttachDeviceToDeviceStack
     * returned, in its own extension, which is what real drivers do and why
     * there is no matching AttachedTo field here. */
    struct _DEVICE_OBJECT *AttachedDevice;

    /* One more than the device below, so a caller allocating an IRP knows
     * how many locations the whole stack needs. Set by IoCreateDevice to 1
     * and raised by IoAttachDeviceToDeviceStack. */
    int8                   StackSize;
} DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef NTSTATUS (WDM_ABI *PDRIVER_DISPATCH)(PDEVICE_OBJECT DeviceObject, PIRP Irp);
typedef NTSTATUS (WDM_ABI *PDRIVER_ADD_DEVICE)(struct _DRIVER_OBJECT *DriverObject,
                                        PDEVICE_OBJECT PhysicalDeviceObject);

typedef struct _DRIVER_EXTENSION {
    PDRIVER_ADD_DEVICE AddDevice;
} DRIVER_EXTENSION, *PDRIVER_EXTENSION;

/* Genesis-only, not real WDM: real PnP matching is INF-file based, entirely
 * outside what this pass models (see the plan - WDM's payoff is binary
 * compatibility with a .sys, not a from-scratch PnP database). A driver
 * that wants to match specific hardware fills this in before flk.c calls
 * DriverEntry; NULL means match every PCI function unconditionally, the
 * same BUS_PROBE_GENERIC catch-all role pci_generic.c plays for Newbus.
 * Terminated by a {0, 0} entry, the same convention linux/pci.h's
 * pci_device_id table uses. */
typedef struct _WDM_HARDWARE_ID {
    uint16 VendorId;
    uint16 DeviceId;
} WDM_HARDWARE_ID;

typedef struct _DRIVER_OBJECT {
    PDRIVER_DISPATCH        MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
    DRIVER_EXTENSION        ExtensionStorage;  /* real NT allocates this
                                                 * separately; embedding it
                                                 * costs nothing when the
                                                 * driver object never
                                                 * outlives the kernel */
    PDRIVER_EXTENSION       DriverExtension;   /* points at ExtensionStorage -
                                                 * set before DriverEntry
                                                 * runs, so DriverObject->
                                                 * DriverExtension->AddDevice
                                                 * = X works exactly like
                                                 * real driver source */
    const WDM_HARDWARE_ID  *HardwareIds;
    const char              *DriverName;
} DRIVER_OBJECT, *PDRIVER_OBJECT;

typedef NTSTATUS (WDM_ABI *PDRIVER_INITIALIZE)(PDRIVER_OBJECT DriverObject,
                                        PUNICODE_STRING RegistryPath);

/* Real NT's IoCreateDriver: allocates the DRIVER_OBJECT (from a fixed pool
 * here, see wdm.c), wires DriverExtension to point at its own embedded
 * storage so `DriverObject->DriverExtension->AddDevice = X` inside
 * InitializationFunction works exactly like real driver source, calls
 * InitializationFunction (i.e. DriverEntry) on it, and - unlike real NT,
 * which stops there - also registers the resulting driver_t (bus.h) on the
 * "pci" devclass, the same role linux_pci_register_driver plays for the
 * Linux-shaped side. Not called automatically - see bus.h's "no magic, no
 * hidden constructors" rule; flk.c calls this once per WDM driver. */
NTSTATUS IoCreateDriver(const char *DriverName,
                        PDRIVER_INITIALIZE InitializationFunction);

/* The device_ops_t (device.h) translation this whole file exists to prove:
 * a device_t attached with these ops and a PDEVICE_OBJECT as its body
 * reaches a WDM driver's MajorFunction[] dispatch table for every read/
 * write/control/detach, through one synthesized IRP per call - the literal
 * payoff of device.h's "the dispatch table it registers is translated into
 * one of these five" comment. Not wired to any device by this pass's demo
 * driver (see wdm_demo.c) - available for the day a real WDM driver's
 * AddDevice calls dev_alloc()+dev_attach() with it, the same two-step
 * disk.c's disk_register already takes for Newbus. */
extern const device_ops_t wdm_device_ops;

/* IoCreateDevice/IoDeleteDevice - reduced. Real WDM's version takes a
 * DEVICE_TYPE, characteristics, exclusivity, and an optional name; this
 * pass keeps only what AddDevice actually needs to build a device object -
 * a DeviceExtension of the requested size, kcalloc'd and zeroed exactly
 * like ExAllocatePool2 below. */
/* WDM_ABI on every declaration below: each of these is a real entry in
 * the synthetic ntoskrnl.exe export table (ntoskrnl_exports.c) that a
 * loaded driver's machine code calls directly through its IAT, using real
 * Win64 argument registers - see WDM_ABI's own comment above. */
NTSTATUS WDM_ABI IoCreateDevice(PDRIVER_OBJECT DriverObject, uint32 DeviceExtensionSize,
                        PDEVICE_OBJECT *DeviceObject);
void     WDM_ABI IoDeleteDevice(PDEVICE_OBJECT DeviceObject);

/* --- IRPs, device stacks, and completion (ROADMAP item 11) ---------------
 *
 * All of this was previously either absent or a no-op. The IRP was a
 * STACK-ALLOCATED local in wdm.c with one embedded stack location; there
 * was no IoCallDriver, no stacking, and IoCompleteRequest did nothing at
 * all. A filter driver - the entire reason WDM has this shape - could not
 * be written against it.
 */

/* Allocate an IRP with `StackSize` locations from a fixed pool. Returns
 * NULL if the pool is empty or StackSize exceeds IRP_MAX_STACK_LOCATIONS.
 * CurrentLocation starts one ABOVE the top, so the first IoCallDriver lands
 * on location StackSize - which is why a caller fills in
 * IoGetNextIrpStackLocation and not IoGetCurrentIrpStackLocation before
 * that first call. */
PIRP WDM_ABI IoAllocateIrp(int8 StackSize, uint8 ChargeQuota);
void WDM_ABI IoFreeIrp(PIRP Irp);

/* Hand `Irp` to `DeviceObject`'s driver. Steps DOWN one stack location,
 * stamps this device into it, and calls the MajorFunction the location
 * names. Returns whatever the dispatch routine returned.
 *
 * Returns STATUS_NO_SUCH_DEVICE rather than faulting if the device has no
 * driver or no routine for that major function - a driver that forwards to
 * a device it never attached to would otherwise call through NULL. */
NTSTATUS WDM_ABI IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* The location belonging to the driver currently holding the IRP. */
PIO_STACK_LOCATION WDM_ABI IoGetCurrentIrpStackLocation(PIRP Irp);

/* The location the NEXT driver down will see. What a driver fills in
 * before calling IoCallDriver. */
PIO_STACK_LOCATION WDM_ABI IoGetNextIrpStackLocation(PIRP Irp);

/* Pass the request down UNCHANGED and give up the right to see it come
 * back: it steps CurrentLocation back up by one so IoCallDriver's step down
 * lands on the same location the caller was given. Cheaper than copying and
 * the usual choice for a filter with nothing to add - but a driver that
 * skips CANNOT set a completion routine, because it no longer owns a
 * location to hang one on. */
void WDM_ABI IoSkipCurrentIrpStackLocation(PIRP Irp);

/* Copy this location down to the next one, so the level below sees the same
 * request and this driver keeps its own location - which is what makes a
 * completion routine possible. Clears the copied completion fields:
 * inheriting the caller's routine would call it twice. */
void WDM_ABI IoCopyCurrentIrpStackLocationToNext(PIRP Irp);

/* Register `Routine` to run when the levels below finish with the IRP. Set
 * on the NEXT location, per NT - a completion routine belongs to the driver
 * that passed the request down. */
void WDM_ABI IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE Routine,
                                    void *Context, uint8 InvokeOnSuccess,
                                    uint8 InvokeOnError, uint8 InvokeOnCancel);

/* Finish the request and walk back UP the stack running completion
 * routines, innermost first. A routine returning
 * STATUS_MORE_PROCESSING_REQUIRED stops the walk immediately and leaves the
 * IRP intact and owned by that driver.
 *
 * This used to be an empty function. It is the half of the device-stack
 * model that makes layering observable rather than merely structural. */
void WDM_ABI IoCompleteRequest(PIRP Irp, int8 PriorityBoost);

/* Attach `SourceDevice` on top of `TargetDevice`'s stack and return the
 * device it actually landed on - the previous TOP of that stack, which is
 * not necessarily TargetDevice itself if something else attached first.
 * The caller must keep that pointer: it is the only way back down, and NT
 * has no field for it because a driver stores it in its own extension.
 *
 * Returns NULL if either argument is NULL or the stack is already
 * IRP_MAX_STACK_LOCATIONS deep - refusing rather than building a stack
 * deeper than any IRP can carry, which would fault at the first
 * IoCallDriver instead of here. */
PDEVICE_OBJECT WDM_ABI IoAttachDeviceToDeviceStack(PDEVICE_OBJECT SourceDevice,
                                                   PDEVICE_OBJECT TargetDevice);

/* Undo the attach. Safe on a device that was never attached. */
void WDM_ABI IoDetachDevice(PDEVICE_OBJECT TargetDevice);

/* Exercise the whole thing: a two-deep stack, an IRP travelling down
 * through both, a filter's completion routine firing on the way back up in
 * the right order, skip-vs-copy, and pool accounting. Returns the number of
 * failures. */
int wdm_selftest(void);

/* Honest uniprocessor stubs (see Non-goals): raise/lower a tracked IRQL and
 * treat "acquire" as "nothing else can preempt a uniprocessor kernel
 * between here and release" - true today, and exactly the same posture
 * ROADMAP item 6 already states for BSD's mtx/rwlock/sx. */
void  WDM_ABI KeAcquireSpinLock(PKSPIN_LOCK SpinLock, KIRQL *OldIrql);
void  WDM_ABI KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql);
void  WDM_ABI KeRaiseIrql(KIRQL NewIrql, KIRQL *OldIrql);
void  WDM_ABI KeLowerIrql(KIRQL NewIrql);
KIRQL WDM_ABI KeGetCurrentIrql(void);

/* ExAllocatePool2 zeroes by default (matching real NT unless
 * POOL_FLAG_UNINITIALIZED is set, which this pass does not model) - the
 * same reason kcalloc, not kmalloc, sits underneath. Flags and Tag are
 * accepted and ignored; Genesis's heap has no pool-tag accounting. */
void *WDM_ABI ExAllocatePool2(uint32 Flags, uint64 NumberOfBytes, uint32 Tag);
void  WDM_ABI ExFreePool(void *P);

/* Real driver source logs through DbgPrint, not a kernel-internal
 * function directly - wraps kprintf (kprintf.h), same default color every
 * other native WDM print in this file uses. `%d %u %x %lx %s %%` only,
 * see kprintf.h - a real DbgPrint's full printf surface is not this
 * pass's problem, same posture as everywhere else in this file. Variadic
 * + ms_abi together is unusual but well-defined for GCC/Clang on x86-64 -
 * the shadow-space/register-args half of Win64 varargs, which is all a
 * fixed-format-then-va_list callee like this one needs. */
void WDM_ABI DbgPrint(const char *fmt, ...);

#endif
