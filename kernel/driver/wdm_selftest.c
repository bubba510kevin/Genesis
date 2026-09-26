#include "kprintf.h"
#include "typesk.h"
#include "wdm.h"

/* Part 9's check: a REAL two-deep device stack, an IRP travelling down it,
 * and completion routines firing on the way back up.
 *
 * Two drivers are built here rather than one, because almost everything
 * interesting about the device-stack model is invisible with one. A single
 * driver plus a single device object behaves identically whether
 * IoCallDriver walks a stack or calls MajorFunction[] directly - which is
 * exactly what wdm.c used to do, and why nothing caught it.
 *
 * The layout, top to bottom:
 *
 *     filter FDO   (filter_driver)  <- requests enter here
 *     function FDO (func_driver)    <- does the work
 *
 * The filter forwards down, keeps its own stack location, and registers a
 * completion routine. So one read exercises: allocation sized from
 * StackSize, entry at the top, IoCopyCurrentIrpStackLocationToNext,
 * IoCallDriver's step down, the lower driver's dispatch, IoCompleteRequest's
 * walk back up, and the completion routine seeing the result the lower
 * driver produced.
 */

static DRIVER_OBJECT func_driver;
static DRIVER_OBJECT filter_driver;

/* Ordering witness. Each stage appends a letter, so the final string is a
 * record of the path the IRP actually took rather than a set of booleans
 * that cannot tell "both ran" from "both ran in the right order". */
static char trace[16];
static int  trace_len;

static void note(char c) {
    if (trace_len < (int)sizeof(trace) - 1) {
        trace[trace_len++] = c;
        trace[trace_len]   = '\0';
    }
}

/* --- the function driver: the bottom of the stack ---------------------- */

static NTSTATUS WDM_ABI func_read(PDEVICE_OBJECT dev, PIRP irp) {
    PIO_STACK_LOCATION loc = IoGetCurrentIrpStackLocation(irp);

    note('f');
    /* The location the bottom driver sees must be the one the filter
     * copied down, carrying the original request - if the length arrives
     * as zero the copy did not happen. */
    if (loc->Parameters_DeviceIoControl.OutputBufferLength != 64) {
        note('!');
    }
    if (loc->DeviceObject != dev) {
        /* IoCallDriver stamps the device into the location it steps onto.
         * A driver that trusted loc->DeviceObject and got somebody else's
         * would corrupt the wrong extension. */
        note('?');
    }
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 64;
    IoCompleteRequest(irp, 0);
    return STATUS_SUCCESS;
}

/* What func_write got back when it tried to forward with nothing below it.
 * A file-scope witness rather than a return value, because the status that
 * matters is the one IoCallDriver handed the DRIVER, not the one the
 * driver's own dispatch returned to the caller. */
static NTSTATUS forward_status;

static NTSTATUS WDM_ABI func_write(PDEVICE_OBJECT dev, PIRP irp) {
    /* Deliberately forwards to itself while holding the bottom location.
     * There is no location below, and IoCallDriver must say so rather than
     * step to Stack[0] and call whatever a zeroed MajorFunction resolves
     * to. */
    forward_status = IoCallDriver(dev, irp);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, 0);
    return STATUS_SUCCESS;
}

/* --- the filter: forwards down and watches the result ------------------ */

static NTSTATUS WDM_ABI filter_completion(PDEVICE_OBJECT dev, PIRP irp,
                                          void *ctx) {
    (void)dev;
    note('c');
    if (ctx != (void *)&filter_driver) {
        note('x');       /* the context did not survive */
    }
    if (irp->IoStatus.Information != 64) {
        note('y');       /* the lower driver's result did not survive */
    }
    return STATUS_SUCCESS;
}

/* The lower device, as a real filter keeps it: in its own storage, because
 * DEVICE_OBJECT has no downward pointer (NT's does not either). */
static PDEVICE_OBJECT filter_lower;

static NTSTATUS WDM_ABI filter_read(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    note('F');

    /* Copy rather than skip, precisely so there is still a location of our
     * own to hang a completion routine on. */
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, filter_completion, &filter_driver, 1, 1, 0);
    return IoCallDriver(filter_lower, irp);
}

int wdm_selftest(void) {
    int failures = 0;
    PDEVICE_OBJECT func_dev = NULL;
    PDEVICE_OBJECT filter_dev = NULL;
    PDEVICE_OBJECT attached_to;
    PIRP irp;
    PIO_STACK_LOCATION loc;
    NTSTATUS st;
    int i;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        func_driver.MajorFunction[i]   = NULL;
        filter_driver.MajorFunction[i] = NULL;
    }
    func_driver.MajorFunction[IRP_MJ_READ]   = func_read;
    func_driver.MajorFunction[IRP_MJ_WRITE]  = func_write;
    filter_driver.MajorFunction[IRP_MJ_READ] = filter_read;
    func_driver.DriverName   = "wdm_selftest_func";
    filter_driver.DriverName = "wdm_selftest_filter";

    if (!NT_SUCCESS(IoCreateDevice(&func_driver, 0, &func_dev)) ||
        !NT_SUCCESS(IoCreateDevice(&filter_driver, 0, &filter_dev))) {
        kprintf_c(0x0C, "wdm selftest: IoCreateDevice failed\n");
        return 1;
    }

    /* --- 1. stacking ---------------------------------------------------- */
    if (func_dev->StackSize != 1) {
        kprintf_c(0x0C, "wdm selftest: a fresh device has StackSize %d\n",
                  func_dev->StackSize);
        failures++;
    }
    attached_to = IoAttachDeviceToDeviceStack(filter_dev, func_dev);
    if (attached_to != func_dev) {
        kprintf_c(0x0C, "wdm selftest: attach returned the wrong device\n");
        failures++;
    }
    filter_lower = attached_to;

    /* The stack must have got DEEPER, and the lower device must know
     * something is above it. Both halves: a StackSize that grows without
     * AttachedDevice being set gives an IRP with a spare location nobody
     * uses, and the reverse gives a stack an IRP is too short for. */
    if (filter_dev->StackSize != 2) {
        kprintf_c(0x0C, "wdm selftest: filter StackSize %d, wanted 2\n",
                  filter_dev->StackSize);
        failures++;
    }
    if (func_dev->AttachedDevice != filter_dev) {
        kprintf_c(0x0C, "wdm selftest: AttachedDevice not set\n");
        failures++;
    }

    /* --- 2. one request down the whole stack ---------------------------- */
    trace_len = 0;
    trace[0]  = '\0';

    irp = IoAllocateIrp(filter_dev->StackSize, 0);
    if (irp == NULL) {
        kprintf_c(0x0C, "wdm selftest: IoAllocateIrp failed\n");
        IoDeleteDevice(filter_dev);
        IoDeleteDevice(func_dev);
        return failures + 1;
    }
    loc = IoGetNextIrpStackLocation(irp);
    loc->MajorFunction = IRP_MJ_READ;
    loc->Parameters_DeviceIoControl.OutputBufferLength = 64;

    st = IoCallDriver(filter_dev, irp);
    if (!NT_SUCCESS(st)) {
        kprintf_c(0x0C, "wdm selftest: IoCallDriver returned %x\n", st);
        failures++;
    }

    /* "Ffc": filter dispatch, function dispatch, filter completion. Every
     * letter matters and so does the order - 'f' alone is what the old
     * direct-call dispatch would have produced, and "Ff" without 'c' is a
     * stack that layers but never completes back up. */
    if (trace[0] != 'F' || trace[1] != 'f' || trace[2] != 'c' ||
        trace[3] != '\0') {
        kprintf_c(0x0C, "wdm selftest: path was \"%s\", wanted \"Ffc\"\n",
                  trace);
        failures++;
    }
    if (irp->IoStatus.Information != 64) {
        kprintf_c(0x0C, "wdm selftest: Information %d survived as %d\n",
                  64, (int)irp->IoStatus.Information);
        failures++;
    }
    IoFreeIrp(irp);

    /* --- 3. the pool is not leaking ------------------------------------- */
    irp = IoAllocateIrp(1, 0);
    if (irp == NULL) {
        kprintf_c(0x0C, "wdm selftest: pool exhausted after one request\n");
        failures++;
    } else {
        /* Out of range in both directions must be refused, not clamped -
         * a clamped StackSize gives an IRP too short for its stack and the
         * overrun happens later, inside IoCallDriver. */
        IoFreeIrp(irp);
        if (IoAllocateIrp(0, 0) != NULL ||
            IoAllocateIrp(IRP_MAX_STACK_LOCATIONS + 1, 0) != NULL) {
            kprintf_c(0x0C, "wdm selftest: allocated an out-of-range "
                            "stack size\n");
            failures++;
        }
    }

    /* --- 4. forwarding past the bottom is refused ------------------------
     *
     * The obvious way to write this - call IoCallDriver twice on a
     * one-location IRP - does NOT test it, and getting that wrong once is
     * worth recording. IoCompleteRequest walks CurrentLocation back up to
     * the top, so after the first request completes the IRP is legitimately
     * re-sendable and the second call is correct, not an overrun.
     *
     * The real case is a driver holding the BOTTOM location and forwarding
     * anyway - it has not completed, so there is genuinely nowhere below.
     * func_write does exactly that and records what it got back. */
    forward_status = STATUS_SUCCESS;
    irp = IoAllocateIrp(1, 0);
    if (irp != NULL) {
        IoGetNextIrpStackLocation(irp)->MajorFunction = IRP_MJ_WRITE;
        IoCallDriver(func_dev, irp);
        if (forward_status != STATUS_INVALID_DEVICE_REQUEST) {
            kprintf_c(0x0C, "wdm selftest: forwarding past the bottom of "
                            "the stack was allowed (%x)\n", forward_status);
            failures++;
        }
        IoFreeIrp(irp);
    }

    IoDetachDevice(func_dev);
    if (func_dev->AttachedDevice != NULL) {
        kprintf_c(0x0C, "wdm selftest: detach left the stack linked\n");
        failures++;
    }
    IoDeleteDevice(filter_dev);
    IoDeleteDevice(func_dev);

    if (failures == 0) {
        kprintf("wdm: selftest passed - IRP went filter -> function -> "
                "completion\n");
    } else {
        kprintf_c(0x0C, "wdm: selftest FAILED (%d)\n", failures);
    }
    return failures;
}
