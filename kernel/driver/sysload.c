#include "fs.h"
#include "kheap.h"
#include "paging.h"
#include "pe.h"
#include "screen.h"
#include "sysload.h"
#include "wdm.h"

/* Loads a real kernel-mode driver image off disk and hands it to wdm.c -
 * see the plan's Part B. Everything below load_image/resolve_imports
 * (kernel/pe.c) and IoCreateDriver (kernel/wdm.c) is already built and
 * unmodified by this file; this is only the file-read + wiring glue,
 * the same role exec_read_file/exec_release_file (kernel/syscall.c) play
 * for execve, applied to a driver instead of a process image. */

/* How pe_load_driver reads a REAL (non-synthetic - see ntoskrnl_exports.h)
 * dependency DLL: from the same driver-load path a real .sys's own
 * dependency would sit next to, /wsr/Windows/System32/Drivers - the path
 * ROADMAP item 4 already names. Not PE_SYSTEM_DIR (pe.h): that constant
 * is userspace ntdll-loading's own directory and unrelated to where a
 * kernel-mode driver's dependencies live. */
#define SYS_DRIVER_DIR "/wsr/Windows/System32/Drivers/"

static int sys_dep_reader(const char *path, uint8 **out, uint32 *size) {
    char full[128];
    uint64 at = 0;
    uint64 i;

    for (i = 0; SYS_DRIVER_DIR[i] != '\0' && at + 1 < sizeof(full); i++) {
        full[at++] = SYS_DRIVER_DIR[i];
    }
    for (i = 0; path[i] != '\0' && at + 1 < sizeof(full); i++) {
        full[at++] = path[i];
    }
    full[at] = '\0';

    return fs_read_whole(full, out, size) == 0 ? 0 : -1;
}

static void sys_dep_release(uint8 *buf) {
    fs_free_file(buf);
}

/* Reads `path` (an absolute namespace path, e.g. under
 * /wsr/Windows/System32/Drivers/), maps and relocates it into kernel
 * address space, resolves its imports (synthetic ntoskrnl.exe/hal.dll
 * table first, real on-disk dependencies second - see kernel/pe.c), and
 * hands its DriverEntry to IoCreateDriver (wdm.h). Returns 0, or a
 * negative value on any failure - a driver that fails to load is logged
 * and skipped, the same "logged, not fatal" posture bus_probe_and_attach
 * already takes for a device nothing matched. */
int sys_load_driver(const char *path) {
    uint8 *image = NULL;
    uint32 size = 0;
    pe_info_t info;
    NTSTATUS st;
    int rc;

    rc = fs_read_whole(path, &image, &size);
    if (rc != 0) {
        print_string("sysload: cannot read ", 0x0C);
        print_string(path, 0x0C);
        print_string("\n", 0x0C);
        return -1;
    }

    rc = pe_load_driver(vmm_kernel_space(), image, size, &info,
                        sys_dep_reader, sys_dep_release);
    /* The image bytes are already copied into the mapped destination by
     * load_image's copy_in - nothing below needs the source buffer, the
     * same point execve's own image-loading path is freed at. */
    fs_free_file(image);
    if (rc != PE_OK) {
        print_string("sysload: ", 0x0C);
        print_string(path, 0x0C);
        print_string(": ", 0x0C);
        print_string(pe_strerror(rc), 0x0C);
        print_string("\n", 0x0C);
        return -1;
    }

    st = IoCreateDriver(path, (PDRIVER_INITIALIZE)info.entry);
    if (!NT_SUCCESS(st)) {
        print_string("sysload: ", 0x0C);
        print_string(path, 0x0C);
        print_string(": DriverEntry failed\n", 0x0C);
        return -1;
    }
    return 0;
}
