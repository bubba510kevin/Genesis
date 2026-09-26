#include "kprintf.h"
#include "pci.h"
#include "wdm.h"
#include "wdm_demo.h"

/* Proof-of-concept WDM-shaped driver - the same role pci_generic.c and
 * lkpi_demo.c play for their models, proving the whole chain end to end:
 * IoCreateDriver -> DriverEntry -> driver_t -> bus_probe_and_attach ->
 * AddDevice -> kprintf.
 *
 * Matches QEMU's std VGA (1234:1111) specifically, not every function -
 * see lkpi_demo.c's comment: pci_generic, lkpi_demo, and this are all on
 * the same "pci" devclass, and only the highest-priority match per device
 * ever attaches, so three simultaneous catch-alls would mean two of the
 * three demos never print. A real WDM driver replaces this table with
 * whatever hardware it actually targets. */
static const WDM_HARDWARE_ID wdm_demo_ids[] = {
    { 0x1234, 0x1111 }, /* QEMU emulated std VGA */
    { 0, 0 },
};

/* What a real AddDevice keeps per device. LowerDevice is the pointer
 * IoAttachDeviceToDeviceStack returned - the only way back down, because
 * DEVICE_OBJECT deliberately has no downward field (neither does NT's). */
typedef struct {
    PDEVICE_OBJECT LowerDevice;
} wdm_demo_ext_t;

/* IRP_MJ_READ, so the demo's device object is a real target rather than an
 * object that only ever gets created. Reports through the IRP the way a
 * driver has to - IoStatus, not the return value, which is the shape
 * difference wdm.h's IO_STATUS_BLOCK comment already names. */
static NTSTATUS WDM_ABI wdm_demo_read(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PIO_STACK_LOCATION loc = IoGetCurrentIrpStackLocation(Irp);

    (void)DeviceObject;
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = loc->Parameters_DeviceIoControl.OutputBufferLength;
    IoCompleteRequest(Irp, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS WDM_ABI wdm_demo_add_device(PDRIVER_OBJECT DriverObject,
                                     PDEVICE_OBJECT PhysicalDeviceObject) {
    const pci_ivars_t *f;
    PDEVICE_OBJECT fdo = NULL;
    wdm_demo_ext_t *ext;

    if (PhysicalDeviceObject->BusDev == NULL) {
        return STATUS_NO_SUCH_DEVICE;
    }
    f = (const pci_ivars_t *)bus_get_ivars(PhysicalDeviceObject->BusDev);

    /* A real AddDevice: create the functional device object, attach it on
     * top of the PDO, and remember what it attached to. This used to just
     * print - there was no stack to attach to and IoAttachDeviceToDevice-
     * Stack did not exist. */
    if (!NT_SUCCESS(IoCreateDevice(DriverObject, sizeof(wdm_demo_ext_t),
                                   &fdo))) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ext = (wdm_demo_ext_t *)fdo->DeviceExtension;
    ext->LowerDevice = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (ext->LowerDevice == NULL) {
        IoDeleteDevice(fdo);
        return STATUS_UNSUCCESSFUL;
    }

    kprintf_c(0x0D, "wdm_demo: matched %x:%x  stack depth %d\n",
              f->vendor_id, f->device_id, fdo->StackSize);
    return STATUS_SUCCESS;
}

static NTSTATUS WDM_ABI wdm_demo_driver_entry(PDRIVER_OBJECT DriverObject,
                                       PUNICODE_STRING RegistryPath) {
    (void)RegistryPath;
    DriverObject->DriverExtension->AddDevice = wdm_demo_add_device;
    DriverObject->MajorFunction[IRP_MJ_READ] = wdm_demo_read;
    DriverObject->HardwareIds = wdm_demo_ids;
    return STATUS_SUCCESS;
}

int wdm_demo_init(void) {
    NTSTATUS st = IoCreateDriver("wdm_demo", wdm_demo_driver_entry);
    return NT_SUCCESS(st) ? 0 : -1;
}
