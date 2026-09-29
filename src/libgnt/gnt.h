#ifndef GNT_H
#define GNT_H

/* libgnt - Windows DLLs in a Linux program on Genesis (ROADMAP item 19).
 *
 * A static library for ELF programs (musl or freestanding; it needs no libc
 * itself). The kernel does the loading - the same PE linker execve uses,
 * reached through prctl(PR_GENESIS_PE_LOAD) - and the first load gives the
 * process an NT environment: a PEB, process parameters (so GetStdHandle
 * returns this program's own stdin/stdout/stderr) and a TEB for every
 * thread, including threads created afterwards. This library does the two
 * ring-3 halves: runs each new DLL's entry point, dependencies first, and
 * looks symbols up in export directories.
 *
 *     void *dll = gnt_pe_open("mixdll.dll");       // /wsr/Windows/System32/mixdll.dll
 *     int (GNT_WINAPI *add2)(int, int) = gnt_pe_sym(dll, "add2");
 *     add2(2, 3);
 *
 * CALLING CONVENTIONS. A DLL's functions use the Win64 convention; this
 * program's use System V. They differ in which registers carry arguments,
 * where the stack arguments start and which registers a callee may clobber.
 *
 *   gnt_pe_sym       the function's real address. Call it through a
 *                    pointer declared GNT_WINAPI (GCC/clang's ms_abi), and
 *                    the compiler does the translation - every argument
 *                    type, floating point, structs, varargs. The right
 *                    choice whenever you can write the prototype.
 *
 *   gnt_pe_sym_sysv  an adapter that is callable as an ordinary System V
 *                    function, for code that cannot be annotated (a
 *                    function-pointer table, a generic dispatcher). It
 *                    moves up to 14 INTEGER or POINTER arguments into place
 *                    and passes XMM0-XMM3 through unchanged, so it is exact
 *                    for any function whose arguments are all integers and
 *                    pointers, and for one whose first N arguments are all
 *                    floating point with nothing after them. Mixed int/float
 *                    argument lists need gnt_pe_sym. Return values in RAX
 *                    or XMM0 come back as they are.
 *
 * CALLBACKS the other way - a Linux function handed to a DLL - must be
 * defined GNT_WINAPI too, so the DLL can call it.
 *
 * NOT YET: unloading (gnt_pe_close only forgets), DLLs with implicit TLS
 * (__declspec(thread); refused by the kernel), and SEH across the boundary
 * (a fault in DLL code is a Linux signal in a Linux process). */

#define GNT_WINAPI __attribute__((ms_abi))

/* Load `path` (a bare name is looked for in /wsr/Windows/System32) and whatever it
 * imports that is not loaded yet; run the new modules' entry points,
 * dependencies first. Returns the DLL's base - its HMODULE - or 0, with the
 * reason in gnt_pe_errno(). Loading a DLL again returns the same base and
 * runs nothing. */
void *gnt_pe_open(const char *path);

/* Address of export `name` ("#N" for an ordinal), forwarders followed
 * (loading the DLL they name if need be), or 0. */
void *gnt_pe_sym(void *dll, const char *name);

/* An adapter to that export, callable as a System V function - see above.
 * Adapters live as long as the process. */
void *gnt_pe_sym_sysv(void *dll, const char *name);

/* Does nothing yet but succeed: nothing is unloaded. */
int gnt_pe_close(void *dll);

/* A negated errno from the last failing call: -ENOENT for a missing export,
 * -ENOEXEC for an image the kernel would not load (the console says why),
 * -EEXIST if the process's own mappings sit where the TEB and PEB go. */
int gnt_pe_errno(void);

#endif
