#ifndef ACPI_H
#define ACPI_H

#include "typesk.h"

/* Just enough ACPI to answer one question: which Local APIC IDs exist?
 *
 * Deliberately NOT a general ACPI implementation. There is no AML
 * interpreter here and there is not going to be one in this pass - AML is a
 * bytecode language with its own object model, and everything this kernel
 * needs from ACPI so far lives in the fixed, statically-parseable tables.
 * The MADT is one of those: a flat array of variable-length entries, no
 * interpretation required.
 *
 * The alternative was the Intel MP table (the "_MP_" floating pointer),
 * which is smaller to parse. ACPI is used instead because MP tables are
 * deprecated, absent on q35 and on real modern hardware, and QEMU only still
 * emits them for i440fx. Parsing the table that will still be there is worth
 * the extra thirty lines.
 */

/* Find the MADT and record every enabled Local APIC, every IOAPIC, and every
 * interrupt source override. Safe to call more than once. Returns the number
 * of CPUs found, or 0 if there is no usable ACPI - in which case the caller
 * should assume the boot CPU is alone, which is what Genesis did
 * unconditionally before this existed.
 *
 * The name is now narrower than what it does: one walk of the MADT collects
 * all three. Kept rather than renamed because every caller asks the CPU
 * question and the IOAPIC accessors below are reached from ioapic_init, which
 * calls this for its side effect and says so. */
int acpi_enumerate_cpus(void);

/* How many CPUs acpi_enumerate_cpus found, and their APIC IDs. Index 0 is
 * not guaranteed to be the boot CPU - compare against lapic_id(). */
int    acpi_cpu_count(void);
uint32 acpi_cpu_apic_id(int index);

/* The MADT's Local APIC MMIO address, or 0 if unknown. Usually 0xFEE00000,
 * but the MADT is allowed to relocate it and the field exists precisely
 * because firmware sometimes does. */
uint64 acpi_lapic_address(void);

/* --- the IOAPIC half -----------------------------------------------------
 *
 * A machine may have more than one IOAPIC, each owning a contiguous run of
 * Global System Interrupts starting at its own gsi_base. "GSI" is the flat
 * numbering across all of them: IOAPIC 0 typically owns GSI 0-23, a second
 * one continues at 24. An ISA IRQ number is NOT a GSI, which is what the
 * overrides below exist to say.
 */
int acpi_ioapic_count(void);
int acpi_ioapic(int index, uint32 *out_id, uint64 *out_addr,
                uint32 *out_gsi_base);

/* Interrupt source overrides. Firmware uses these to say "ISA IRQ `source`
 * is not GSI `source`, it is GSI `gsi`" - and on essentially every machine
 * with an IOAPIC, including QEMU, the timer is one of them: ISA IRQ 0 comes
 * out on GSI 2. Programming GSI 0 for the timer instead gets a redirection
 * entry that is correct-looking and wired to nothing, so the scheduler never
 * ticks. This is the single trap in IOAPIC bring-up and the reason these are
 * parsed rather than assumed.
 *
 * `flags` is the MPS INTI field: bits 0-1 polarity (1 = active high, 3 =
 * active low), bits 2-3 trigger mode (1 = edge, 3 = level). Zero in either
 * pair means "conforms to the bus", which for ISA is active-high edge. */
int acpi_override_count(void);
int acpi_override(int index, uint8 *out_source, uint32 *out_gsi,
                  uint16 *out_flags);

/* One line saying what was found, or why nothing was. */
void acpi_report(uint8 color);

#endif
