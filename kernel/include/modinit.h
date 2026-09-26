#ifndef MODINIT_H
#define MODINIT_H

/* How a loadable module says "call this when I am loaded".
 *
 * --- the problem this closes ---------------------------------------------
 * Both compat layers already generated an entry function. LinuxKPI's
 * module_pci_driver(x) generated x_lkpi_module_init and Newbus's
 * DRIVER_MODULE(name, ...) generated name_newbus_module_init - and
 * kernel/kldload.c searched for exactly three hardcoded names, none of which
 * was either of those. A real driver dropped in /boot/kernel therefore
 * loaded, relocated correctly, and then failed KLD_ERR_NOENTRY with its
 * entry point sitting right there in its own symbol table. The two halves
 * were built in different passes and never met.
 *
 * Adding the generated names to the search list would have fixed the two
 * cases that exist and broken again on the third, because the name depends
 * on a macro argument the loader cannot know.
 *
 * --- what it does instead -------------------------------------------------
 * A dedicated section holding function pointers. Both upstreams solve this
 * the same way and for the same reason: FreeBSD's DRIVER_MODULE builds a
 * linker set (which IS a section), and Linux's module_init puts a pointer in
 * .initcall. A section is the one piece of metadata a relocatable object can
 * carry that names something without the loader having to guess a symbol.
 *
 * The pointers need R_X86_64_64 relocations to be meaningful, which kldload
 * already applies - the section is SHF_ALLOC, so it gets placed, and its
 * RELA section patches each pointer to wherever the function landed.
 *
 * Return value: 0 for success, non-zero to fail the load. This is Linux's
 * convention (init_module returns int) and it is honoured - kldload unwinds
 * a module whose init declines, rather than the previous behaviour of
 * calling through a void(*)(void) and discarding the answer, which recorded
 * a driver that had explicitly failed as loaded.
 */

#define GENESIS_MODULE_INIT(fn)                                            \
    static int (*const __genesis_modinit_##fn)(void)                       \
        __attribute__((used, section(".genesis_modinit"))) = (fn)

/* The section name, for kldload.c. Spelled once. */
#define GENESIS_MODINIT_SECTION ".genesis_modinit"

/* --- and the way back out ------------------------------------------------
 *
 * The mirror image, and it exists for the same reason: the loader cannot
 * guess a symbol name, so the exit pointer goes in a section too.
 *
 * This is NOT symmetrical with init in one respect worth stating, because it
 * is the difference between the two that decides what a caller may do here.
 * An init function runs with the module fully placed and relocated and
 * nothing yet depending on it. An exit function runs with the module still
 * mapped and executable, but with the kernel about to unmap it - so it must
 * leave BEHIND no pointer into its own image. Every driver it registered,
 * every callout it armed, every interrupt handler it installed, every task
 * it queued: gone before this returns, or the unload is refused and the
 * module is left in a stopped-but-loaded state. kldload.c checks, because a
 * stale function pointer into unmapped memory is a fault with no name on it.
 *
 * Return type is void, unlike init's int. There is no useful answer to
 * "unloading failed": the kernel has already decided, and a module that
 * cannot be stopped has to say so by leaving its registrations in place,
 * which is the thing that is actually checked. */
#define GENESIS_MODULE_EXIT(fn)                                            \
    static void (*const __genesis_modexit_##fn)(void)                      \
        __attribute__((used, section(".genesis_modexit"))) = (fn)

#define GENESIS_MODEXIT_SECTION ".genesis_modexit"

#endif
