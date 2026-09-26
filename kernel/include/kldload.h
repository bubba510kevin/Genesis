#ifndef KLDLOAD_H
#define KLDLOAD_H

#include "typesk.h"

/* Loading RELOCATABLE kernel modules - ROADMAP item 4's other three
 * directories.
 *
 * Item 4 names four places a driver may come from: /lib/modules/,
 * /boot/kernel/, /boot/modules/ and /wsr/Windows/System32/Drivers. Exactly
 * one of them worked - the Windows path, through pe.c's pe_load_driver and
 * ntoskrnl_exports.c's synthetic export table, verified at boot by test.sys.
 * The other three were aspirational and item 11's text said so.
 *
 * --- why this is a different loader from elf.c ---------------------------
 *
 * elf.c loads ET_EXEC: a program with a fixed load address, PT_LOAD segments
 * and nothing left to resolve. A .ko is ET_REL - the output of `ld -r` or
 * `gcc -c`. It has no program headers at all, its sections have no addresses
 * yet, and it is full of undefined symbols and relocation entries that say
 * where to patch them in.
 *
 * So this walks SECTION headers rather than program headers, places each
 * allocatable section wherever the kernel heap put it, and then applies the
 * relocations - resolving undefined symbols through Part 2's ksyms. That is
 * why item 11 lists a kernel symbol table as a prerequisite for "a
 * Genesis-native loadable-module mechanism": without ksyms there is nothing
 * to link against.
 */

#define KLD_OK              0
#define KLD_ERR_MAGIC      -1
#define KLD_ERR_TYPE       -2   /* not ET_REL                              */
#define KLD_ERR_MACHINE    -3
#define KLD_ERR_TRUNCATED  -4
#define KLD_ERR_NOMEM      -5
#define KLD_ERR_SYMBOL     -6   /* an undefined symbol the kernel lacks    */
#define KLD_ERR_RELOC      -7   /* a relocation type this does not know    */
#define KLD_ERR_NOENTRY    -8   /* no recognisable init function           */

#define KLD_ERR_FULL       -9   /* the module table is full                */
#define KLD_ERR_INITFAIL  -10   /* the init function itself declined       */
#define KLD_ERR_NOTLOADED -11   /* kld_unload: no module by that name      */
#define KLD_ERR_BUSY      -12   /* kld_unload: something still points into
                                 * the image - see kld_unload              */

const char *kld_strerror(int rc);

/* Load and link one relocatable object, then call its init function.
 *
 * `name` is only for the report. Returns KLD_OK, or one of the errors above;
 * on failure nothing is left loaded. */
/* Reserve the VA window every module image is mapped into. Call once, after
 * kvm_init() and before the first kld_load. Safe to skip - the window falls
 * back to a literal base - but then it is not collision-checked against the
 * heap, the kernel stacks or the PE driver window.
 *
 * Module images are NOT on the kernel heap. See kldload.c: page protections
 * are per page, and a heap allocation shares its edge pages with other
 * objects, so W^X on module text is not expressible there. */
void kld_image_window_init(void);

/* Read the page protections of every loaded module back and assert W^X:
 * text present-not-writable-not-NX, data present-writable-NX. Returns the
 * number of failures. Call after kld_load_directories(). */
int kld_wx_selftest(void);

int kld_load(const char *name, const void *image, uint64 size);

/* Load every module found in the standard directories - /boot/kernel,
 * /boot/modules and /lib/modules - in that order. Returns how many loaded.
 *
 * Order matters and is upstream's: /boot/kernel holds modules shipped with
 * the kernel, /boot/modules third-party ones, and /lib/modules is Linux's
 * location. A module already loaded by name is not loaded twice. */
int kld_load_directories(void);

/* --- unloading -----------------------------------------------------------
 *
 * Call the module's exit functions, take its drivers off the bus, check that
 * nothing else still points into its image, and unmap it.
 *
 * Returns KLD_OK; KLD_ERR_NOTLOADED for a name that is not in the table; or
 * KLD_ERR_BUSY when something still holds a pointer into the image, which is
 * a REFUSAL and not a failure to try harder - unmapping under a live
 * interrupt handler or an armed callout is a fault with no name on it. See
 * kldload.c for which registries are swept and, more importantly, for what
 * that guarantee does and does not cover.
 *
 * A BUSY return can leave the module STOPPED BUT LOADED: its exit function
 * has already run by the time the residual sweep happens, and running it was
 * the only way to give it the chance to release anything. That is a bug in
 * the module and it is reported as one. */
int kld_unload(const char *name);

/* Is a module with this name in the table? The name is the FILE name as
 * kld_load was given it - "hellokm.ko", not "hellokm". */
int kld_is_loaded(const char *name);

/* How many module exit functions this kernel has called, ever. Exists for the
 * selftest: "kld_unload returned KLD_OK" is also what a stub that did nothing
 * would return, and this is what distinguishes them. */
uint64 kld_exit_call_count(void);

/* Unload and reload a module, and check the four range sweeps kld_unload
 * depends on in both directions. Returns the number of failures. Call after
 * kld_load_directories() and kld_wx_selftest(). */
int kld_unload_selftest(void);

/* One line per loaded module. */
void kld_report(uint8 color);

/* Exercise the relocation arithmetic without a file - see the
 * implementation for why that is worth doing separately. Returns the number
 * of failures. */
int kld_selftest(void);

#endif
