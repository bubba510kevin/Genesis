/* A real loadable kernel module - ROADMAP item 4's /boot/kernel path.
 *
 * Compiled with `gcc -c` to a RELOCATABLE object (ET_REL), not linked. It
 * has undefined symbols - kprintf_c is the kernel's - and relocation entries
 * saying where to patch them in, which is exactly what kernel/kldload.c
 * exists to resolve. Nothing about this file is Genesis-specific except the
 * two symbols it calls and the name of its init function.
 *
 * The evidence standard is test.sys's: the module's own log line, printed
 * from code that was not in the kernel image, at an address chosen at load
 * time.
 */

/* Declared, not included: the module is built against the kernel's ABI, not
 * against its headers. Matching the real prototype matters - a wrong one
 * here is a wrong call at run time, and there is no link step to catch it. */
extern void kprintf_c(unsigned char color, const char *fmt, ...);

/* A string in .rodata and a global in .data, so the load exercises more
 * than one allocatable section - a loader that only placed .text would
 * still print something for a module with no data at all. */
static const char banner[] = "hello_kmod: loaded from /boot/kernel";
static int load_count;

/* A function called from init, so there is at least one intra-module
 * relocation (a PC32 call) as well as the cross-module ones to the kernel. */
static void say(const char *what) {
    kprintf_c(0x0D, "%s\n", what);
}

/* The name kldload.c looks for. Three spellings are accepted - this one,
 * Linux's init_module, and module_init - so a module written for either
 * convention loads unmodified. */
void kld_module_init(void);

void kld_module_init(void) {
    load_count++;
    say(banner);
    kprintf_c(0x0D, "hello_kmod: .data survived relocation (count %d)\n",
              load_count);
}

/* --- and the way back out ------------------------------------------------
 *
 * The counterpart to kld_module_init, reached through a pointer in the
 * .genesis_modexit section - the mechanism kernel/include/modinit.h's
 * GENESIS_MODULE_EXIT emits and kernel/driver/kldload.c's kld_unload calls.
 *
 * Written out by hand rather than by including modinit.h, because the whole
 * point of this module is that it is built against NOTHING: it declares the
 * kernel functions it calls and it declares the loader contract it takes
 * part in, and if either of those is wrong the failure is at run time. The
 * attribute below is exactly what GENESIS_MODULE_EXIT expands to, and a
 * change to that macro that this file did not follow would show up as a
 * module whose exit function is never called - which the unload selftest
 * asserts, so it would show up loudly.
 *
 * `used` is load-bearing: this pointer has no reader inside the module, so
 * without it the compiler discards the object and the section never appears
 * in the .ko at all.
 *
 * There is nothing here to undo - this module registers no driver, arms no
 * callout and installs no interrupt handler, which is exactly why it is the
 * right subject for the unload test: it isolates the image lifecycle from
 * the deregistration logic. So it prints, and printing is the proof that
 * code inside a module that is about to be unmapped ran first. */
static void kld_module_exit(void) {
    kprintf_c(0x0D, "hello_kmod: exit called, %d load%s this boot\n",
              load_count, load_count == 1 ? "" : "s");
}

static void (*const __genesis_modexit_hello)(void)
    __attribute__((used, section(".genesis_modexit"))) = kld_module_exit;
