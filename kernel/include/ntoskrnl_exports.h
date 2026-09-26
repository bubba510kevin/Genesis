#ifndef NTOSKRNL_EXPORTS_H
#define NTOSKRNL_EXPORTS_H

#include "typesk.h"

/* A synthetic ntoskrnl.exe/hal.dll export surface for kernel/pe.c's
 * pe_load_driver import resolution - see pe.h's pe_load_driver comment
 * and kernel/ntoskrnl_exports.c. Genesis never maps a real ntoskrnl.exe
 * image (there isn't one), so find_export's "read a mapped PE image's own
 * IMAGE_EXPORT_DIRECTORY" approach cannot apply here - this is a plain
 * name/address table over wdm.c's already-working native functions
 * instead, resolved by resolve_imports before it would otherwise call
 * load_dependency for these two names. */

/* Non-zero if `dll_name` (case-insensitive) is one of the synthetic
 * kernel export providers this table answers for, rather than a real
 * on-disk dependency. */
int nt_is_synthetic_dll(const char *dll_name);

/* Look up `symbol_name` (case-sensitive, matching real PE export-name
 * convention) in the synthetic table for `dll_name`. Returns 1 and fills
 * *addr on success, 0 if unresolved - same "refuse rather than guess"
 * posture pe.c's own find_export already takes for a real missing
 * export. */
int nt_resolve_import(const char *dll_name, const char *symbol_name,
                      uint64 *addr);

#endif
