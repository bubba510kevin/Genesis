#ifndef PCI_H
#define PCI_H

#include "bus.h"
#include "typesk.h"

/* PCI configuration-space access and enumeration, plus the bus this
 * discovers devices onto (see bus.h for the probe/attach machinery that
 * runs on top of what this file finds).
 *
 * Uses the legacy CONFIG_ADDRESS/CONFIG_DATA mechanism (ports 0xCF8/0xCFC) -
 * every x86 chipset since the original PCI spec supports it, and it needs
 * nothing paging cares about (I/O ports, not memory-mapped ECAM). ECAM
 * (memory-mapped extended config space, needed past the first 256 bytes per
 * function) is a documented later step, not a first-cut requirement:
 * nothing this kernel talks to yet has a capability living past 0xFF.
 *
 * Scans bus 0 only. A found PCI-to-PCI bridge would have a secondary bus
 * number to recurse into; QEMU's default machines put everything this
 * kernel currently cares about on bus 0, so that recursion is left for the
 * day a bridge is actually found (see pci_init in pci.c).
 */

#define PCI_CONFIG_ADDRESS    0xCF8
#define PCI_CONFIG_DATA       0xCFC

#define PCI_VENDOR_NONE       0xFFFF  /* what an empty slot reads back as */
#define PCI_HDRTYPE_MULTIFUNC 0x80    /* bit in the header-type byte      */

#define PCI_MAX_FUNCS         64

/* One found PCI function - a full device, or one function of a
 * multi-function device. Doubles as the ivars a bus_dev_t child carries
 * (see bus_add_child in bus.h): the driver that matches it reads these
 * through bus_get_ivars, not by re-reading config space itself. */
typedef struct {
    uint8  bus, slot, func;
    uint16 vendor_id, device_id;
    uint8  class_code, subclass, prog_if, revision_id;
    uint8  hdr_type;
    uint8  int_pin, int_line;
    uint32 bar_raw[6];    /* as read at scan time; see pci_bar_size */

    /* Config-space offset of the MSI capability, or 0 if the function has
     * none. Found once during the scan rather than re-walked per query: the
     * capability list is a linked list in config space, and walking it costs
     * an I/O port round trip per link. */
    uint8  msi_cap;
    /* The IDT vector pci_msi_alloc assigned, or 0 if MSI is not enabled on
     * this function. */
    uint8  msi_vector;
} pci_ivars_t;

/* Raw config-space access, in case a driver needs a register this struct
 * does not carry (capability pointers, vendor-specific registers, ...).
 * bus/slot/func address one function; reg is the byte offset, dword-
 * aligned internally regardless of access width. */
uint32 pci_cfg_read32(uint8 bus, uint8 slot, uint8 func, uint8 reg);
uint16 pci_cfg_read16(uint8 bus, uint8 slot, uint8 func, uint8 reg);
uint8  pci_cfg_read8(uint8 bus, uint8 slot, uint8 func, uint8 reg);
void   pci_cfg_write32(uint8 bus, uint8 slot, uint8 func, uint8 reg, uint32 value);

/* Scan bus 0, slots 0-31, and functions 0-7 of any slot whose function 0
 * sets the multi-function bit. Fills the internal function table and, for
 * each function found, adds it as a child of pci_root() - discovery only,
 * no driver runs yet. Call bus_attach_children(pci_root()) afterwards to
 * match and attach drivers. Safe to call more than once; re-scanning
 * resets the table (PCI devices do not appear/disappear at runtime in
 * this pass, so this is a boot-time-only operation in practice). */
void pci_init(void);

/* One line per function found, independent of whether any driver matched
 * it - the same "report what was found" posture as e820_report/
 * ata_report. */
void pci_report(uint8 color);

/* Read/size BAR number `bar` (0-5) of `f`. On return, *base holds the raw
 * address (I/O port or physical memory address, masked of its type bits),
 * *size holds the region's size in bytes, and *is_mem is non-zero for a
 * memory BAR, zero for I/O. Returns 0 on success, -1 if `bar` names the
 * high dword of a 64-bit BAR (read the pair through the low-numbered bar
 * instead) or is out of range. Sizing probes hardware - writes all-ones,
 * reads back which low bits the device ignored, restores the original
 * value - so do not call this where that could race a concurrent access
 * to the same function; not a concern yet with one CPU and no driver
 * touching another driver's function. */
int pci_bar_size(const pci_ivars_t *f, int bar, uint64 *base, uint64 *size,
                 int *is_mem);

/* --- capabilities and MSI ------------------------------------------------
 * ROADMAP item 11 names "no MSI/MSI-X, legacy INTx only" as deferred. This
 * closes the MSI half. MSI-X stays deferred on purpose: its vector table
 * lives in a BAR-mapped region rather than in config space, which is
 * materially more machinery than a first cut needs, and nothing in this tree
 * needs more than one vector per function yet.
 */

/* Byte offset of capability `cap_id` in `f`'s config space, or 0 if the
 * function has no capability list or does not implement that capability.
 * Zero is unambiguous as "not found": the capability list can never start
 * before offset 0x40, since everything below that is the standard header. */
uint8 pci_find_capability(const pci_ivars_t *f, uint8 cap_id);

#define PCI_CAP_ID_PM     0x01   /* power management */
#define PCI_CAP_ID_MSI    0x05
#define PCI_CAP_ID_VENDOR 0x09
#define PCI_CAP_ID_PCIE   0x10
#define PCI_CAP_ID_MSIX   0x11

/* Allocate an interrupt vector, program `f`'s MSI capability to raise it,
 * and enable MSI. Returns the vector (48-254), or -1 on failure - no MSI
 * capability, no free vector, or no Local APIC to deliver through.
 *
 * Also sets the command register's INTx-disable bit. That is not optional
 * tidiness: a function left able to assert its legacy line while MSI is on
 * can deliver the same event twice, once through each path, and the legacy
 * one lands on a shared line whose handler knows nothing about it.
 *
 * `handler` is called from interrupt context with `ctx`, through
 * kernel/idt_alloc.c. It must NOT send an EOI - interrupt.c does that, and
 * doing it twice acknowledges the next interrupt as well as this one. */
int pci_msi_alloc(pci_ivars_t *f, void (*handler)(void *ctx), void *ctx);

/* Disable MSI on `f`, release its vector, and re-enable INTx. Safe on a
 * function that never had MSI enabled. */
void pci_msi_release(pci_ivars_t *f);

/* One line per function that has an MSI capability, whether or not it is
 * enabled - the same "report what was found" posture as pci_report. */
void pci_msi_report(uint8 color);

/* Program the first MSI-capable function found, read every register back
 * off the device to confirm it latched what was written, then release it
 * and confirm that took too. Returns the number of failures; a machine with
 * no MSI-capable function is zero failures and a printed note, not a
 * failure. Does NOT prove an MSI is ever delivered - see the comment on the
 * function in pci.c for exactly which half is tested where. */
int pci_msi_selftest(void);

/* The (only, for now - see the bus-0-only note above) PCI bus as a
 * bus_dev_t. Every function pci_init finds is added as this node's child;
 * pass it to bus_attach_children to run driver matching. NULL until
 * pci_init has run once. */
bus_dev_t *pci_root(void);

#endif
