#include "ahci.h"
#include "ata.h"
#include "bcache.h"
#include "bus.h"
#include "cpu.h"
#include "devices.h"
#include "dispatch.h"
#include "disk.h"
#include "e820.h"
#include "elf.h"
#include "device.h"
#include "fat.h"
#include "fatfs.h"
#include "fs.h"
#include "volume.h"
#include "fatfs.h"
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "keyboard.h"
#include "kheap.h"
#include "kstack.h"
#include "kthread.h"
#include "ns.h"
#include "object.h"
#include "paging.h"
#include "pe.h"
#include "tty.h"
#include "lkpi_demo.h"
#include "newbus_compat_demo.h"
#include "pci.h"
#include "hints.h"
#include "kldload.h"
/* kernel/bsd/kern_devsysctl.c. Declared here rather than through
 * <sys/bus.h>, which flk.c must not include: that header and
 * kernel/include/bus.h define two different driver_t and are deliberately
 * never in one translation unit. */
int devsysctl_selftest(void);
/* kernel/bsd/kern_lowmem.c, declared here for the same reason. */
void genesis_lowmem_init(void);
int  genesis_lowmem_selftest(void);
void genesis_lowmem_report(unsigned char color);
int  pcache_selftest(void);
void pcache_report(uint8 color);
#include "sysload.h"
#include "pci_generic.h"
#include "wdm.h"
#include "wdm_demo.h"
#include "pic.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "rtc.h"
#include "timer.h"
#include "screen.h"
#include "serial.h"
#include "syscall.h"
#include "typesk.h"
#include "vmalloc.h"
#include "bsd.h"
#include "idt.h"
#include "interrupt.h"
#include "acpi.h"
#include "ioapic.h"
#include "netstack.h"
#include "lapic.h"
#include "ksmp.h"
#include "klock.h"
#include "gnfs.h"

/* Physical end of the kernel image, from linker.ld. `end` is a 64-bit VIRTUAL
 * address now, so the PMM - which works in physical frames - must use this or
 * it will try to mark most of a 64-bit address space as used. */
extern uint64 kernel_phys_end;
extern uint64 kernel_stack_top;
extern uint64 ist1_stack_top;

/* How much physical memory the frame bitmap can describe. NOT a claim about
 * how much RAM exists - E820 answers that now, and pmm_init manages whichever
 * is smaller. This is purely the size of a .bss buffer: 4GB of frames costs
 * 128KB, and .bss is NOBITS so it costs nothing in the boot image.
 *
 * Must stay under KERNEL_MAP_SIZE minus the kernel image, since the bitmap
 * lives in .bss and the kernel window has to cover it. */
#define PMM_MANAGED_LIMIT 0x100000000ULL         /* 4GB */
#define PMM_BITMAP_BYTES  (PMM_MANAGED_LIMIT / PMM_PAGE_SIZE / 8)
#define PMM_FRAME_COUNT   (PMM_MANAGED_LIMIT / PMM_PAGE_SIZE)

static uint8 pmm_bitmap[PMM_BITMAP_BYTES];

/* One byte per frame for reference counts, eight times the bitmap: 1MB to
 * describe 4GB. Same reasoning as the bitmap - .bss is NOBITS, so it costs
 * nothing in the boot image and only address space in the kernel window,
 * which has 4MB and is currently using under 400KB of it. Both arrays are
 * covered by the kernel_phys_end reservation below, since linker.ld puts
 * .bss inside it. */
static uint8 pmm_refcounts[PMM_FRAME_COUNT];

/* One page below 1GB. Any canonical address under ELF_USER_LIMIT that does
 * not collide with the loaded segments will do; this is just conventional and
 * far from the 0x400000 the test binary loads at. */
/* What the first process is invoked as.
 *
 * For busybox, argv[0] is the binary and argv[1] selects the applet - which is
 * why `busybox sh -c '...'` works and is exactly the command your strace list
 * came from. `sh -c` reads its command from argv and never touches stdin, so
 * it needs no keyboard driver, no line discipline and no read().
 *
 *   static const char *const user_argv[] = {
 *       "/BUSYBOX", "sh", "-c", "echo hi from ash", NULL };
 */
/* The file to load, as an 8.3 name on the FAT volume. Kept separate from
 * argv[0] because busybox picks its applet from the BASENAME of argv[0] -
 * pass "/BUSYBOX" and it looks for an applet called BUSYBOX, does not find
 * one, and exits 127. */
/* A path now, not a bare name in the root.
 *
 * Small change, and the point of the whole filesystem layer: the kernel loads
 * this because that is where it is, not because a #define and a flat root
 * directory happened to agree. It is also the shape execve needs - "resolve a
 * path, load what it points at" is the same operation whether the caller is
 * the boot path or a syscall. */
#define USER_BINARY  "/bin/busybox"

/* An interactive shell rather than a one-shot command.
 *
 * argv[0] selects the applet by basename, so "sh" reaches ash directly with
 * no "busybox" indirection. -i is explicit rather than inferred: ash decides
 * it is interactive from isatty(0), and while the TCGETS answer in syscall.c
 * now makes that come out true, saying so outright means the shell still
 * prompts if the ioctl story ever changes.
 *
 * The one-shot form is worth keeping around for bisecting:
 *   { "sh", "-c", "echo hi from ash", NULL }
 */
static const char *const user_argv[] = { "sh", "-i", NULL };
/* PATH is searched left to right and the first hit wins, so this order is a
 * policy decision, not a formality. Note that stat() still answers -ENOENT for
 * every path, so ash's search fails at the first directory - the string is in
 * place ahead of the syscall layer that will make it mean something. */
static const char *const user_envp[] = {
    "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/sbin:/usr/local/bin:/wsr/System32",
    "HOME=/",
    "TERM=linux",
    NULL
};

/* USER_STACK_TOP / USER_STACK_SIZE moved to syscall.c, which is where
 * user_stack_create lives and where execve now builds one too. */
/* The placeholder \Device\HarddiskVolume1 object that used to be declared here
 * is gone, and so is the static fat_volume_t behind it.
 *
 * It was an object with no read, no write and no getdents, whose whole job was
 * to hold a name until something could parse a remainder against it. That
 * something exists now: volume.c creates a real volume device per partition,
 * names it, and its parse op turns "\bin\sh" into a file object. Keeping the
 * placeholder beside it would mean two things claiming
 * \Device\HarddiskVolume1, and whichever registered second would fail.
 *
 * The static fat_volume_t went with it, for the same reason fatfs.c grew a
 * pool: one static volume is one mountable filesystem, and a partition table
 * means there can be several. flk.c does not name a filesystem type any more
 * at all - it names an ORDER, which is all a boot path should be. */

static void namespace_init(void) {
    ns_init();

    if (ns_insert("\\Device\\Console", tty_console()) != 0) {
        print_string("ns: could not name the console\n", 0x0C);
    }

    /* Pseudo-devices - null today - registered the same way the console was:
     * an object, a name in \Device\, links in \??\. Nothing about open(2)
     * or /dev knows any of them individually. */
    devices_register();

    /* Raw disks: \Device\Harddisk<N>\DR<N>, linked as \??\sda and friends.
     *
     * Separate objects from \Device\HarddiskVolume1 above, and deliberately
     * so - see disk.h. The volume parses a path into a file; the disk hands
     * back bytes at an offset. Collapse them and you can either image the
     * disk or open a file on it, not both. */
    disk_register();

    /* DOS-device names. C: is NOT here any more.
     *
     * It is assigned by volume.c to whichever volume becomes the root, which
     * is what a drive letter has to be if it is going to mean anything. A
     * hardcoded link to \Device\HarddiskVolume1 was correct for exactly as
     * long as there was one volume and it was always the root - and it would
     * have gone on resolving, to the WRONG volume, the moment that stopped
     * being true. A link rather than an object, so the letter can be
     * repointed without anything that uses it knowing: that part was right,
     * and it is what volume.c's assign_letter creates.
     *
     * CON stays, because there is one console and no policy to decide. */
    ns_link("\\??\\CON",     "\\Device\\Console");

    /* What /dev/<name> resolves through. sys_openat rewrites the POSIX form
     * into \??\<name> rather than keeping a second table of device names. */
    ns_link("\\??\\console", "\\Device\\Console");
    ns_link("\\??\\tty",     "\\Device\\Console");
    ns_link("\\??\\stdin",   "\\Device\\Console");
    ns_link("\\??\\stdout",  "\\Device\\Console");
    ns_link("\\??\\stderr",  "\\Device\\Console");
}

/* fs_root moved to vfs.c, which is the generic half of the filesystem layer.
 * It used to live here because flk.c owned the one fat_volume_t; now flk.c
 * owns a MOUNT, hands the result to fs_set_root, and never mentions the
 * filesystem's type again. */
/* Read HELLO off the FAT volume and report its ELF headers.
 *
 * Nothing is mapped and nothing is executed - the point is that every value
 * printed has an answer on the host. `readelf -l /tmp/hello` should agree
 * line for line. A parse verified against readelf is one you can then build
 * the mapping stage on top of without wondering which layer is wrong. */
/* fs_root moved to vfs.c, which is the generic half of the filesystem layer.
 * It used to live here because flk.c owned the one fat_volume_t; now flk.c
 * owns a MOUNT, hands the result to fs_set_root, and never mentions the
 * filesystem's type again. */
/* Read HELLO off the FAT volume and report its ELF headers.
 *
 * Nothing is mapped and nothing is executed - the point is that every value
 * printed has an answer on the host. `readelf -l /tmp/hello` should agree
 * line for line. A parse verified against readelf is one you can then build
 * the mapping stage on top of without wondering which layer is wrong. */
static void start_init_process(void) {
    uint32 size;
    uint8 *image = NULL;
    int rc;

    /* Through fs_read_whole, not fat_read_path.
     *
     * Three calls became one and the file no longer names a filesystem, but
     * that is the smaller half. The larger half is that this now goes through
     * the MOUNT TABLE - so process 1 is loaded from whatever volume is
     * mounted at "/", which is what makes "then flip root" in the ZFS plan a
     * change to one mount rather than a change here.
     *
     * The size probe went with it. fs_read_whole sizes from the directory
     * entry and treats a short read as -EIO, which is the check the two-call
     * version could not make: a read that came back short after a successful
     * size probe was indistinguishable from a small file. */
    rc = fs_read_whole(USER_BINARY, &image, &size);
    if (rc == -2) {                          /* -ENOENT */
        print_string("  no ", 0x0E);
        print_string(USER_BINARY, 0x0E);
        print_string(" on the volume\n", 0x0E);
        return;
    }
    if (rc != 0) {
        print_string("  init: read failed\n", 0x0C);
        return;
    }

    elf_report(image, size, 0x0F);

    /* Process 1 gets its own address space BEFORE the loader runs.
     *
     * It used to be loaded straight into the kernel's own PML4, on the
     * reasoning that the boot path had already put a binary there by the time
     * a process table existed - and that was survivable only because the one
     * binary that ever ran here was busybox, which execve's immediately and
     * acquires a private space on the way. Anything that does NOT exec is
     * left running in the kernel space, and fork(2) refuses it: there is no
     * meaning to copy-on-writing the kernel's own mappings, so sys_fork
     * returns -EINVAL and every child-related call after it fails.
     *
     * Creating the space here costs one frame and makes process 1 an ordinary
     * process. */
    {
        process_t       *p  = proc_current();
        address_space_t *as = vmm_space_create();

        if (as == NULL) {
            print_string("  no memory for the init address space\n", 0x0C);
            kfree(image);
            return;
        }
        vmm_switch_to(as);
        p->space        = as;
        p->shares_space = 0;
    }

    {
        elf_info_t info;
        int lrc = elf_load(image, size, &info);

        if (lrc != ELF_OK) {
            print_string("  load failed: ", 0x0C);
            print_string(elf_strerror(lrc), 0x0C);
            print_string("\n", 0x0C);
            kfree(image);
            return;
        }

        /* The image buffer is finished with the moment the segments are
         * copied - the binary now lives at its own vaddrs, not here. */
        kfree(image);

        {
            /* interp_base 0: process 1 is loaded by elf_load_into, which
             * refuses a PT_INTERP image outright here rather than growing a
             * second copy of execve's interpreter path. Boot loads exactly
             * one binary and it is the one this tree builds static; when that
             * stops being true, the fix is for boot to execve rather than for
             * this call site to learn about dynamic linking. */
            uint64 stack = user_stack_create(USER_STACK_TOP, USER_STACK_SIZE,
                                             user_argv, user_envp,
                                             info.phdr_vaddr, info.phnum,
                                             info.phentsize, info.entry, 0);

            if (stack == 0) {
                print_string("  no memory for a user stack\n", 0x0C);
                return;
            }

            /* The heap starts above everything the loader mapped, so brk()
             * grows into space no segment claimed. */
            user_set_brk(info.highest_vaddr);

            print_string("  entering ring 3 at ", 0x0A);
            print_hex64(info.entry, 0x0A);
            print_string("  rsp ", 0x0A);
            print_hex64(stack, 0x0A);
            print_string("\n", 0x0A);

            /* Does not return. From here the CPU stops trusting the code it
             * runs: the loaded binary can no longer touch the VGA buffer, or
             * anything else in the kernel half, and has to ask instead. */
            enter_user_mode(info.entry, stack);
        }
    }
}

/* fs_root moved to vfs.c, which is the generic half of the filesystem layer.
 * It used to live here because flk.c owned the one fat_volume_t; now flk.c
 * owns an ORDER, and never mentions the filesystem's type at all. */

/* Bring the storage stack up, and prove it did.
 *
 * This replaces mount_data_disk, which mounted ata1 by hand with fat_mount,
 * then mounted the SAME disk a second time through the vtable, then listed
 * the first copy. Every part of that now belongs to somebody else:
 *
 *   which partitions exist        part.c
 *   what a partition is           volume.c
 *   which filesystem it holds     the probe table
 *   where it gets mounted         the mount table in vfs.c
 *
 * What is left is the ORDER, and one report. The report drives off fs_root()
 * through the vtable rather than off a fat_volume_t, and that is the change
 * that makes it worth printing: the old listing walked the copy the rest of
 * the kernel did NOT use, so it would have gone on printing a correct
 * directory after the vtable path broke completely. */

static int report_entry(const fs_dirent_t *ent, void *ctx) {
    (void)ctx;
    print_string("    ", 0x0F);
    print_string(ent->name, 0x0F);
    print_string(ent->is_dir ? "  <DIR>" : "", 0x07);
    print_string("\n", 0x0F);
    return 0;
}

static void storage_init(void) {
    fs_node_t root_node;
    int rc;

    /* Filesystems register BEFORE anything is scanned. A prober registered
     * after the scan is one nothing will ever be probed against, and the
     * symptom is a volume reported as unrecognised - which reads like a bad
     * disk rather than a boot-order mistake. */
    volume_register_fs("fat16", fatfs_probe);

    /* gnfs, the native COW filesystem: kernel/include/gnfs.h is its one-call
     * opaque entry point - no types, no headers beyond that one. Registered
     * after FAT, which matters only for speed: probe order is a speed
     * question, not a correctness one.
     *
     * The vendored ZFS reader used to register here too, through the same
     * one-call shape (zfs_init), and was removed 2026-09-26; gnfs carries the
     * copy-on-write filesystem and the ACL fixtures it used to. That shape
     * is why removing it was one line here and a directory. */
    gnfs_init();

    volume_init();
    volume_report();

    if (fs_root() == NULL) {
        print_string("fs: no root volume mounted\n", 0x0C);
        return;
    }

    print_string("  root directory:\n", 0x0F);
    rc = fs_lookup("/", &root_node);
    if (rc != 0) {
        print_string("  (root lookup failed)\n", 0x0C);
        return;
    }
    fs_iterate(&root_node, report_entry, NULL);

    /* One file read back, through the same path a process would take.
     *
     * /etc/motd rather than the old TEST.TXT, and the directory matters more
     * than the name: a file in the root exercises the root-directory special
     * case and nothing else, while a file one level down exercises the
     * directory walk - which is exactly where a filesystem mounted on a
     * PARTITION would first show a wrong base LBA. Reading it at boot means
     * that mistake is visible before init runs, not after. */
    {
        uint8 *buf = NULL;
        uint32 size = 0;

        rc = fs_read_whole("/etc/motd", &buf, &size);
        if (rc == 0) {
            uint32 i;

            print_string("  /etc/motd: ", 0x0A);
            for (i = 0; i < size && i < 64; i++) {
                char c[2];
                c[0] = (buf[i] == '\n') ? ' ' : (char)buf[i];
                c[1] = '\0';
                print_string(c, 0x0A);
            }
            print_string("\n", 0x0A);
            fs_free_file(buf);
        } else {
            print_string("  (no /etc/motd on the volume)\n", 0x0E);
        }
    }
}


void flk(void) {
    const e820_map_t *memory_map;

    /* First, before anything prints. Every print_string from here on is
     * mirrored to COM1, so `-serial stdio` captures the whole boot rather
     * than whatever survived the last scroll of an 80x25 screen. It needs no
     * memory, no IDT and no paging - just two I/O ports - which is why it can
     * come this early, and why it still works from a fault handler later. */
    serial_init();

    clear_screen(0x07);

    print_string("Genesis\n", 0x0F);
    if (serial_present()) {
        print_string("COM1 ready (this log is mirrored to serial)\n", 0x0F);
    }

    /* Before anything that can fault: the boot trampoline's GDT has no TSS,
     * and the double fault gate needs an IST stack to be worth having. */
    gdt_init();
    gdt_set_kernel_stack((uint64)&kernel_stack_top);
    gdt_set_ist(1, (uint64)&ist1_stack_top);
    print_string("GDT + TSS loaded\n", 0x0F);

    idt_init();
    print_string("IDT loaded\n", 0x0F);

    pic_remap(PIC1_OFFSET, PIC2_OFFSET);
    print_string("PIC remapped\n", 0x0F);

    /* 100Hz instead of the BIOS's 18.2Hz. Programmed before interrupts are
     * enabled so the first tick arrives at the rate the scheduler assumes. */
    timer_init(TIMER_HZ);
    print_string("Timer at 100Hz\n", 0x0F);

    /* The callout wheel. After timer_init because it reads the rate the PIT
     * was ACTUALLY programmed at (99.998Hz, not 100) and derives its
     * sbintime tick from it, and before sti because timer_tick() calls into
     * it - it is a no-op until this runs, but there is no reason to leave a
     * window where ticks are dropped on the floor. */
    net_callout_init();

    /* The wall clock, read once and then derived from the tick. After
     * timer_init because the epoch is recorded relative to uptime, and the
     * uptime it is relative to has to be running.
     *
     * A machine whose RTC cannot be read reports zero and the clock is uptime
     * alone - 1970, monotonic and self-consistent, which is what every Unix
     * had before its clock was set. Said out loud at boot, because a wrong
     * date that nothing mentions is discovered later by a build system. */
    {
        uint64 epoch = rtc_read_epoch();

        if (epoch != 0) {
            timer_set_boot_epoch(epoch);
            print_string("Wall clock set from CMOS RTC\n", 0x0F);
        } else {
            print_string("No usable RTC - wall clock is uptime\n", 0x0E);
        }
    }

    /* Before sti, so the first keystroke cannot arrive while the ring indices
     * are still whatever .bss held. */
    kbd_init();
    print_string("Keyboard ready\n", 0x0F);

    memory_map = e820_load();
    print_string("e820 memory map:\n", 0x0F);
    e820_report(memory_map, 0x07);

    /* Nothing reserves the VGA aperture or the ROM area by hand any more -
     * E820 reports them as non-usable and pmm_init only releases what a
     * type-1 entry vouches for. What the firmware cannot know is where this
     * kernel was loaded, so that reservation stays. It runs from 0 rather
     * than from 0x7E00 to cover the IVT, the BDA and the E820 buffer the
     * bootloader left at 0x5000 along with the image itself. */
    pmm_init(memory_map, pmm_bitmap, PMM_BITMAP_BYTES,
             pmm_refcounts, PMM_FRAME_COUNT);
    pmm_mark_region_used(0, (phys_addr_t)&kernel_phys_end);

    print_string("PMM: ", 0x0F);
    print_hex64(pmm_free_frames() * PMM_PAGE_SIZE, 0x0A);
    print_string(" bytes free in ", 0x0F);
    print_hex64(pmm_total_frames(), 0x0A);
    print_string(" frames\n", 0x0F);

    /* Replaces the boot trampoline's 2MB-page tables with the kernel window
     * plus a direct map of all RAM, and drops the identity map along the
     * way. Must come after pmm_init: the direct map's own tables are frames. */
    paging_init(memory_map);
    if (!paging_physmap_ready()) {
        print_string("paging: DIRECT MAP FAILED - halting\n", 0x4F);
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    print_string("Paging rebuilt (4-level, direct map)\n", 0x0F);

    /* Kernel VA range allocator (ROADMAP item 11): reserves ranges for the
     * heap, kernel stacks and driver-image window below, instead of each
     * trusting its own hand-picked base constant not to collide with the
     * others by comment alone. Must run before any of those three. */
    kvm_init();

    /* No-execute, before anything maps a page that asks for it. Nothing here
     * depends on the answer - PAGE_NX is stripped rather than refused when
     * the CPU has no NX - so this reports and continues either way. It is
     * above start_init_process() because the first user image is loaded from
     * there, and a loader that asked for NX before this ran would have made
     * an entry with a reserved bit set. */
    if (paging_enable_nx()) {
        print_string("NX enabled (EFER.NXE)\n", 0x0F);
    } else {
        print_string("NX unavailable - W^X will not be enforced\n", 0x0E);
    }

    /* After paging, because process 1 records the address space it runs in. */
    proc_init((uint64)&kernel_stack_top);
    sched_init(proc_current());

    /* ULE in place of round-robin. Installed right after sched_init, which
     * is the only safe moment: sched_set_policy does not migrate state, and
     * neither policy holds any that matters (readiness lives in the process,
     * not in a queue) - but that is only true before anything has run.
     *
     * Round-robin stays compiled in and reachable. Deleting it would remove
     * the only thing a ULE hang could be bisected against, and sched.h's
     * whole reason for having an interface is that a failure should be
     * attributable to the policy or the switcher but not both.  */
    sched_set_policy(sched_ule_policy());

    kheap_init();
    print_string("Kernel heap ready\n", 0x0F);

    kstack_init();

    /* Kernel threads. After kstack_init, because kthread_create takes a
     * kernel stack out of the same slot table proc_alloc does, and after
     * sched_init because a kernel thread is a schedulable process_t and
     * there has to be a policy installed to schedule it.
     *
     * Nothing creates one at boot. This only empties the wait-channel queues
     * sleep(9) blocks on (kernel/proc/ksleep.c) and resets the live count -
     * the first real thread is ROADMAP item 7's txg syncer, which does not
     * exist yet. It is here rather than lazily on first use so that
     * ksleep_wake(), which runs from interrupt handlers, never finds an
     * uninitialised queue. */
    kthread_init();

    /* The taskqueue and its servicing kernel thread. Directly after
     * kthread_init, and before anything can enqueue - the network stack's
     * SYSINITs and a driver attaching both do.
     *
     * Enqueuing before this point is not an error: taskqueue_enqueue runs the
     * task inline when there is no thread to hand it to, which is exactly
     * what the whole subsystem used to do. So this line decides when work
     * starts being DEFERRED, not whether it runs. */
    genesis_taskqueue_init();

    pe_driver_window_init();
    /* And the same for loadable ELF modules, which used to load onto the
     * heap - see kldload.c on why W^X forced them off it. */
    kld_image_window_init();

    /* The vendored FreeBSD mbuf(9) subsystem (kernel/bsd/). After
     * kheap_init because every UMA slab is a kmalloc_a page, and before
     * anything that might want a packet buffer - which today is nothing, so
     * the position here is about the heap dependency only.
     *
     * The selftest runs at boot rather than from userspace on purpose: this
     * code is unreachable from ring 3 (there is no socket layer yet, by
     * design - see the plan's non-goals), so a boot-time check is the only
     * place it gets exercised at all. See src/verif.c. */
    /* The Local APIC.
     *
     * Position is load-bearing and was got wrong once: this was first placed
     * next to net_callout_init(), right after timer_init(), which is BEFORE
     * paging_init() and pmm_init() in this function. lapic_init maps an MMIO
     * page - it needs the direct map to exist and the PMM to be able to hand
     * out a page-table frame - so it page-faulted inside ensure_table on a
     * direct-map read. (The fault report named vmm_map_page and lapic_init in
     * the backtrace, which is Part 2 of this pass paying for itself.)
     *
     * So: after paging, the PMM and kvm_init, and before anything allocates a
     * dynamic vector - a vector with no LAPIC to deliver it is a vector
     * nothing can raise. See kernel/lapic.h for why the LAPIC is here at all
     * rather than in the SMP part of the plan: an MSI is a memory write the
     * LAPIC decodes, so there is no MSI without it. */
    lapic_init();
    lapic_report(0x0F);

    /* The IOAPIC, taking over the legacy lines from the 8259 pair.
     *
     * Directly after lapic_init and for a hard reason: an IOAPIC delivers
     * INTO a Local APIC, so bringing it up first would point every device
     * line at a controller that is not accepting yet - which does not fail
     * loudly, it just silently stops the timer.
     *
     * Before sti, so there is no window in which both controllers are live.
     * ioapic_init masks the 8259s as part of taking over, and between "route
     * the line at the IOAPIC" and "mask it at the PIC" the same device
     * interrupt has two paths to the CPU and only one of them gets an EOI.
     * With interrupts still off, that window cannot be observed.
     *
     * A machine with no IOAPIC changes nothing: this returns 0 and every
     * legacy line stays exactly where it was. */
    ioapic_init();
    ioapic_report(0x0F);

    /* UMA before the first zone is created. genesis_uma_init runs what
     * upstream's SYSINIT would have - see kernel/bsd/uma_vendor.c. */
    genesis_uma_init();
    net_mbuf_init();
    net_mbuf_selftest();

    /* The block cache gets its arena from the heap, here, because this is the
     * first point where there IS a heap and the last point before anything
     * reads a disk. 256KB of it: 64 blocks of 4KB.
     *
     * A failed kmalloc is not a failed boot. bcache_init(NULL, 0) leaves the
     * cache disabled and every dev_read goes straight to the driver - slower,
     * never wrong - which is the behaviour a kernel that cannot allocate
     * 256KB should have, rather than refusing to mount its own root. */
    {
        const uint64 arena_bytes = BCACHE_MAX_BLOCKS * (uint64)BCACHE_BLOCK_SIZE;
        void *arena = kmalloc(arena_bytes + BCACHE_BLOCK_SIZE);

        /* One block of slack on the request: bcache_init aligns the arena up
         * to a block boundary and would otherwise lose the last block to the
         * rounding, so the cache would quietly be 63 blocks. Asking for the
         * slack is cheaper than a cache whose size does not match the constant
         * that names it. */
        bcache_init(arena, arena == NULL ? 0 : arena_bytes + BCACHE_BLOCK_SIZE);
        if (!bcache_enabled()) {
            print_string("bcache: disabled (no memory)\n", 0x0E);
        }
    }

    /* After gdt_init, because MSR_STAR encodes selectors that must exist. */
    syscall_init((uint64)&kernel_stack_top);
    print_string("SYSCALL enabled\n", 0x0F);

    fpu_init();
    print_string("FPU + SSE enabled\n", 0x0F);

    cpu_report(0x0F);

    {
        uint64 *a = (uint64 *)kmalloc(64);
        uint64 *b = (uint64 *)kmalloc(4096);
        void   *page = kmalloc_a(128);

        if (a && b && page) {
            a[0] = 0xDEADBEEFCAFEBABEULL;
            b[511] = 0x1BADB0021BADB002ULL;

            if (a[0] == 0xDEADBEEFCAFEBABEULL &&
                b[511] == 0x1BADB0021BADB002ULL &&
                ((uint64)page & 0xFFF) == 0) {
                print_string("kmalloc OK\n", 0x0A);
            } else {
                print_string("kmalloc readback FAILED\n", 0x0C);
            }

            kfree(a);
            kfree(b);
            kfree(page);

            print_string(kheap_check() == 0 ? "kheap consistent after free\n"
                                            : "kheap CORRUPT after free\n",
                         kheap_check() == 0 ? 0x0A : 0x0C);
        } else {
            print_string("kmalloc FAILED\n", 0x0C);
        }
    }

    ata_init();
    ata_report(0x0F);

    /* AHCI, after ATA and not instead of it.
     *
     * The target hardware (a Dell OptiPlex 3040, H110 chipset) presents its
     * SATA controller in AHCI mode, where ata.c's PIO ports at 0x1F0/0x170
     * answer nothing at all - the CSM reads the disk via INT 13h long enough
     * to load this kernel and then the ATA probe finds no drives. Both probes
     * run so that a machine with either kind of controller boots, and so that
     * a machine with both proves the two paths coexist.
     *
     * Before PCI bus discovery below, deliberately: this walks config space
     * itself, for the same reason ata_init does its own probing - the machine
     * needs a disk before anything that wants one starts. */
    ahci_init();
    ahci_report(0x0F);

    /* Bus discovery, then driver matching. pci_report is a raw dump -
     * every function found, independent of whether anything claimed it -
     * the same posture e820_report/ata_report already take. All three
     * driver models named in ROADMAP item 4 are registered on the same
     * "pci" devclass before the one bus_attach_children call below runs
     * their combined probe/attach: pci_generic (Newbus) is the catch-all,
     * lowest priority; lkpi_demo (Linux-shaped) claims the emulated e1000
     * by vendor/device ID; wdm_demo (WDM-shaped) claims the emulated std
     * VGA the same way. Each one's own attach()/probe()/AddDevice log line
     * is the proof its whole chain - registration through bus_probe_and_
     * attach through that model's own driver-source idiom - actually
     * works, not just that it compiles. See ROADMAP item 4/11 and
     * kernel/include/bus.h for what this deliberately does not do yet (no
     * bridge recursion, no real hardware driver, no PnP device-stacking). */
    bus_selftest();

    pci_init();
    pci_report(0x0F);
    pci_generic_register();
    lkpi_demo_driver_lkpi_module_init();
    wdm_demo_init();
    newbus_compat_demo_pci_newbus_module_init();
    bus_attach_children(pci_root());

    /* What the drivers above actually reserved through bus_alloc_resource.
     * Empty until a driver asks for something, which is itself the useful
     * signal: before Part 4 there was nothing to ask, and a driver reading
     * a BAR straight off the raw ivars left no trace at all. */
    bus_resource_report(0x0F);

    /* Which functions could take an MSI. Reported unconditionally, because
     * "none" is itself the answer on QEMU's default i440fx machine and a
     * reader needs to know that rather than wonder why nothing appeared. */
    pci_msi_report(0x0F);

    /* And whether programming one actually takes. Here rather than after
     * sti, unlike the idt selftest: this test never waits for an interrupt,
     * it only writes config space and reads it back, so it has no reason to
     * run with interrupts on - and running it before they are enabled means
     * a device left half-programmed by a failure cannot raise anything. */
    pci_msi_selftest();

    /* Legacy IRQ sharing. Before sti, because the selftest registers on a
     * line, drives irq_dispatch by hand and unregisters again - and
     * irq_register UNMASKS the line it is given, so doing this with
     * interrupts live would briefly arm IRQ 11 with test handlers on it. */
    /* The serial console's receive half, once the IRQ layer exists.
     *
     * Transmit has been up since early boot (serial_init, far above) because
     * a boot log is worth having before anything else works. Receive has to
     * wait for irq_register, and it is registered here rather than beside
     * serial_init so the ordering dependency is visible instead of implied.
     *
     * The point of it is the bare-metal target: that machine has a USB
     * keyboard and no PS/2 device, and keyboard.c is PS/2-only, so without
     * this the shell comes up and cannot be typed at. */
    serial_rx_init();

    irq_selftest();
    irq_report(0x0F);
    serial_rx_report(0x0F);

    /* WDM's IRP/device-stack machinery. Independent of any real device -
     * it builds its own two-driver stack - so it runs here with the other
     * subsystem selftests rather than waiting on a match. */
    wdm_selftest();

    /* SMP bring-up. After the LAPIC (it sends the IPIs), after paging and
     * kheap (it maps a page and allocates AP stacks), after the IDT (an AP
     * loads it), and with interrupts still OFF - the BSP spins waiting for
     * each AP and does not need any. */
    acpi_enumerate_cpus();
    acpi_report(0x0F);
    smp_init();
    smp_report(0x0F);

    /* The disk we booted from is the disk we are reading, so LBA 0 is our own
     * boot sector and its last two bytes must be 0x55 0xAA. Verifying against
     * data whose contents are already known separates "the driver works" from
     * "the driver returned something" - a read that silently produced zeroes
     * would pass any check that only looked at the return code. */
    {
        ata_device_t *disk = ata_get(0);
        uint8 *sector = (uint8 *)kmalloc(ATA_SECTOR_SIZE);

        if (disk && sector) {
            int rc = ata_read(disk, 0, 1, sector);

            if (rc != ATA_OK) {
                print_string("ata: read of LBA 0 failed\n", 0x0C);
            } else if (sector[510] == 0x55 && sector[511] == 0xAA) {
                print_string("ata: LBA 0 boot signature OK\n", 0x0A);
            } else {
                print_string("ata: LBA 0 read but signature WRONG\n", 0x0C);
            }
            kfree(sector);
        }
    }

    /* Namespace FIRST, then storage. The order is reversed from what it was,
     * and the reversal is the change.
     *
     * It used to be mount-then-name, because the namespace held a placeholder
     * object for a volume that had already been mounted by hand. Volumes name
     * THEMSELVES now as they are discovered - so \Device\ has to exist
     * before volume_init runs, and disk_register (inside namespace_init) has
     * to have created the disk devices whose partition tables get scanned. */
    namespace_init();

    /* The NT object manager's standard directories and the type list
     * (ROADMAP item 14). After namespace_init, because it creates \ and
     * \Device; before ns_report, so the report shows what it added. Nothing
     * is named under \BaseNamedObjects at boot - that is userspace's to
     * fill - but the directory has to exist for the first CreateMutexW, and
     * \ObjectTypes is populated here from every type registered so far. */
    dispatch_init();
    storage_init();

    dev_report();
    ns_report();

    /* After storage_init, so the numbers are the boot's own: mounting a root
     * and reading /etc/motd is several hundred sector reads through the FAT,
     * and a cache that is installed but not being consulted reports zero hits
     * here. That is the whole reason this line exists rather than a comment
     * saying the cache is enabled. */
    bcache_report();

    /* The kernel-mode .sys loader (kernel/sysload.c, ROADMAP item 4/11) -
     * after storage_init, since it reads through the same mounted root
     * volume namespace_init/storage_init just built (/wsr/Windows/
     * System32/Drivers/, the path ROADMAP item 4 names), and before
     * start_init_process's point of no return. test.sys is Genesis's own
     * synthetic mkpe.py-built image (src/mkpe.py --sys), not a real
     * third-party driver - see the plan's Non-goals - but it is a REAL
     * PE32+ file on REAL disk, mapped into REAL kernel address space,
     * relocated, and import-resolved against the synthetic ntoskrnl.exe
     * table (kernel/ntoskrnl_exports.c) exactly like a genuine one would
     * be. Its own DriverEntry logs proof of each step through DbgPrint. */
    sys_load_driver("/wsr/Windows/System32/Drivers/test.sys");

    /* And the other three directories ROADMAP item 4 names - /boot/kernel,
     * /boot/modules and /lib/modules - which until Part 15 were aspirational
     * and item 11's text said so. Same position and the same reason as the
     * .sys load above: after the root volume is mounted, before the point of
     * no return. A relocatable module links against the kernel's own symbol
     * table (Part 2), which is why item 11 listed that as a prerequisite. */
    /* --- the vendored network stack ------------------------------------
     *
     * Two steps here, both BEFORE the NIC driver module loads, and one more
     * (configuration) after it.
     *
     * 1. The sysctl tree. Every SYSCTL_ macro in the vendored tree emits a
     *    static OID into a link set; this is what links them into a tree. It
     *    is first because a SYSINIT may add a dynamic OID under a static
     *    parent, and the parent has to be there.
     *
     * 2. The SYSINITs. Around forty of them: ether_init registering the
     *    NETISR_ETHER handler, the domain registration that makes
     *    pffindproto work, if_init allocating the interface index table,
     *    ip_init, icmp_init, arp_init, and the per-protocol statistics
     *    counters. This is upstream's own mechanism (see kernel/bsd/
     *    sysinit.c for why it is real now rather than compiled away), and it
     *    replaces a growing list of explicit calls that had to be discovered
     *    one bug at a time.
     *
     * BEFORE kld_load_directories() just below, which is where a NIC driver
     * module is loaded and attaches. That ordering is load-bearing and was
     * found the hard way: net/if.c's if_alloc() reaches into the interface
     * INDEX TABLE, which if_init() - a SYSINIT at SI_SUB_INIT_IF - allocates.
     * Loading the driver first page-faulted inside if_alloc_domain reading
     * through a table that did not exist yet.
     */
    /* The one thread structure per CPU, the one process and the one
     * credential they all share. Before the SYSINITs, because several of them
     * reach curthread. */
    genesis_threads_init();

    net_sysctl_init();
    genesis_sysinit_run();
    genesis_sysinit_report(0x0F);

    kld_load_directories();
    kld_report(0x0F);

    /* AFTER the modules, not with the other bus reports further up.
     * A loadable driver is what pushed the devclass pool over its cap and
     * halted the machine here; a count taken before kld_load_directories
     * would have shown headroom that was about to be spent, which is the
     * reading that made the old cap look sufficient. */
    bus_devclass_report(0x07);

    /* W^X on the module images, read back out of the page tables rather than
     * trusted. ROADMAP item 4's oldest security remainder was that module
     * .text was writable AND executable; this is what says it is not. After
     * kld_load_directories, obviously - there is nothing to check before it. */
    ahci_selftest();
    kld_wx_selftest();

    /* And that the way back out is real. Unloads a module and reloads it,
     * and checks the four "does anything point into these bytes" sweeps
     * kld_unload refuses on. Deliberately after the W^X check rather than
     * before: this one RELOADS the module it unloads, so running it first
     * would leave W^X checking a differently-placed image and hide a seal
     * that only works on a first load. */
    kld_unload_selftest();

    /* Per-device sysctl contexts, and the fifth sweep. Here rather than with
     * the other BSD selftests because it needs a device that bus.c has
     * already created, and because device_sysctl_fini is the thing that keeps
     * the sweep above finding nothing. */
    devsysctl_selftest();

    /* Memory pressure. ROADMAP item 7's fourth blocker: UMA's reclaim
     * machinery has always been vendored and nothing ever raised vm_lowmem,
     * so a cache had no way to be told to shrink. Started here, after the
     * kernel threads and UMA are both up and before anything large is
     * allocated. */
    pcache_selftest();
    genesis_lowmem_init();
    genesis_lowmem_selftest();
    genesis_lowmem_report(0x07);
    pcache_report(0x07);
    hints_report(0x0F);

    /* Now CONFIGURE the interface the driver just attached: the address, the
     * default route, and ifconfig-up. After the module, because it needs an
     * interface to exist; before sti, because a frame arriving part-way
     * through would be matched against a half-built address list. */
    net_stack_init();



    __asm__ volatile ("sti");
    print_string("Interrupts enabled\n", 0x0F);

    /* After sti, and it has to be: the callout selftest waits on real timer
     * ticks rather than driving the wheel by hand, which is what makes it a
     * test of the wiring into timer.c and not just of the sweep. It costs
     * about 200ms of boot. */
    /* FIRST of the post-sti tests, and the ordering is load-bearing rather
     * than aesthetic.
     *
     * It waits for a real timer tick with the 8259s masked, which is the only
     * way to tell an IOAPIC that is delivering from one that is merely
     * programmed. Every other test below ALSO depends on ticks arriving -
     * the callout wheel most directly - so if the handover is broken they
     * hang, silently, before anything gets to say why. Found exactly that way:
     * an A/B run with the interrupt source override deliberately ignored
     * wedged at net_callout_selftest with no output at all, and moving this
     * ahead of it turns that same failure into one printed line.
     *
     * This test is bounded by a spin count for the same reason. */
    ioapic_selftest();

    /* After sti, and it has to be: it waits for a real reply to arrive
     * through a real interrupt. */
    net_selftest();
    net_stack_report(0x0F);
    /* The two mechanisms the protocol layer's initialisation rests on, printed
     * because both of them failed silently once. sysctl's count says the link
     * set was found; the eventhandler list says which subscriptions exist -
     * and a missing subscription is what left an interface with no IPv4 data
     * and page-faulted inside ARP. */
    net_sysctl_report(0x07);
    genesis_eventhandler_report(0x07);
    /* AFTER the network test, so it shows the interrupt the NIC driver
     * registered - the earlier irq_report runs before any module has
     * loaded and therefore before any driver has an interrupt at all. */
    irq_report(0x0F);

    net_callout_selftest();

    /* After sti, like the callout one and for the same reason: it raises a
     * real self-IPI through the Local APIC and waits for delivery, which is
     * the whole point - it tests the controller and the dispatch path, not
     * just a function pointer. */
    /* After sti, like idt_selftest and for the same reason: it waits for
     * IPIs to be delivered and answered, which cannot happen with
     * interrupts masked on the sender. */
    smp_selftest();
    lock_selftest();
    lock_report(0x0F);
    sched_ule_selftest();
    sched_ule_report(0x0F);

    /* After sti, and after the kernel-thread selftest below it in spirit:
     * the dispatcher check spawns a kernel thread and waits for it to BLOCK,
     * so it needs both a working scheduler and a running timer. Placed with
     * the other object-manager work rather than with the scheduler tests
     * because a failure here is about events and names, not about
     * switching. */

    /* After sti, and it has to be, for the same reason net_selftest and the
     * callout test are: it waits on a real sleep(9) that a real wakeup has to
     * release, and it checks that the thread was entered with interrupts
     * ENABLED - which cannot be observed on a machine where they are masked
     * anyway. It also needs the timer running, because the ceiling under
     * every sleep is counted in ticks and a frozen `ticks` turns a failed
     * wakeup into a hang instead of a reported failure. */
    kthread_selftest();

    /* After sti, and after kthread_selftest for a reason worth stating: both
     * of these are built ON kernel threads, so running them first would
     * report a condvar failure for what is really a scheduler failure. The
     * order here is the dependency order, so the first red line is the
     * lowest broken thing. */
    genesis_condvar_selftest();
    genesis_taskqueue_selftest();
    genesis_taskqueue_report(0x07);

    /* Last of the four, and the order is the dependency order: it uses a
     * kernel thread to be the second context, so a failure in kthread_selftest
     * above explains a failure here and not the other way round. */
    dispatch_selftest();

    /* No ordering dependency on anything above or below - pure logic over a
     * stack-local cred_t and acl_t, the same posture the rest of acl.c's own
     * testing takes. Placed here rather than earlier only because nothing
     * needs it to run any sooner. */
    acl_selftest();

    idt_selftest();
    interrupt_report(0x0F);

    /* Last, and it does not return.
     *
     * This used to be called from inside mount_data_disk(), which put it
     * BEFORE the namespace was built - so /dev resolved against a namespace
     * that did not exist yet and every device open came back -ENOENT. It also
     * made everything after the call in mount_data_disk() unreachable, since
     * enter_user_mode never comes back.
     *
     * Anything the first user process needs has to be initialised above this
     * line. That is the entire rule, and it is easier to keep when the call
     * is the last statement in the boot path rather than buried in a helper
     * that reads like it only mounts a disk. */
    start_init_process();

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
