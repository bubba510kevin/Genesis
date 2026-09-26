#include "acpi.h"
#include "kprintf.h"
#include "paging.h"
#include "typesk.h"

/* See acpi.h for why this is MADT-only. */

#define ACPI_MAX_CPUS 32

typedef struct __attribute__((packed)) {
    char   signature[8];      /* "RSD PTR " */
    uint8  checksum;          /* over the first 20 bytes only */
    char   oem_id[6];
    uint8  revision;          /* 0 = ACPI 1.0, 2+ = has the 64-bit fields */
    uint32 rsdt_address;
    /* ACPI 2.0+ only - do NOT read these unless revision >= 2 */
    uint32 length;
    uint64 xsdt_address;
    uint8  extended_checksum;
    uint8  reserved[3];
} acpi_rsdp_t;

typedef struct __attribute__((packed)) {
    char   signature[4];
    uint32 length;
    uint8  revision;
    uint8  checksum;
    char   oem_id[6];
    char   oem_table_id[8];
    uint32 oem_revision;
    uint32 creator_id;
    uint32 creator_revision;
} acpi_sdt_header_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32            lapic_address;
    uint32            flags;
    /* followed by variable-length entries */
} acpi_madt_t;

typedef struct __attribute__((packed)) {
    uint8 type;
    uint8 length;
} acpi_madt_entry_t;

#define MADT_TYPE_LAPIC     0
#define MADT_TYPE_IOAPIC    1
#define MADT_TYPE_OVERRIDE  2
#define MADT_TYPE_X2APIC    9

typedef struct __attribute__((packed)) {
    acpi_madt_entry_t header;
    uint8             acpi_processor_id;
    uint8             apic_id;
    uint32            flags;      /* bit 0: enabled */
} acpi_madt_lapic_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_t header;
    uint8             ioapic_id;
    uint8             reserved;
    uint32            address;    /* MMIO, always below 4GB */
    uint32            gsi_base;
} acpi_madt_ioapic_t;

typedef struct __attribute__((packed)) {
    acpi_madt_entry_t header;
    uint8             bus;        /* always 0 - ISA */
    uint8             source;     /* the ISA IRQ number */
    uint32            gsi;        /* what it actually comes out on */
    uint16            flags;      /* MPS INTI: polarity and trigger */
} acpi_madt_override_t;

#define ACPI_MAX_IOAPICS   4
#define ACPI_MAX_OVERRIDES 16

static uint32 cpu_apic_ids[ACPI_MAX_CPUS];
static int    cpu_count;
static uint64 lapic_addr;
static int    scanned;
static const char *failure;

static acpi_madt_ioapic_t   ioapics[ACPI_MAX_IOAPICS];
static int                  ioapic_count;
static acpi_madt_override_t overrides[ACPI_MAX_OVERRIDES];
static int                  override_count;

/* Checksum over `len` bytes must be zero. Every ACPI table carries one, and
 * checking it is what separates "found the signature" from "found the
 * table" - the string "RSD PTR " can occur in ROM data by coincidence. */
static int checksum_ok(const uint8 *p, uint32 len) {
    uint8 sum = 0;
    uint32 i;

    for (i = 0; i < len; i++) {
        sum = (uint8)(sum + p[i]);
    }
    return sum == 0;
}

static int sig_eq(const char *a, const char *b, int n) {
    int i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* The RSDP lives in one of two places on a PC: the first kilobyte of the
 * EBDA, or the BIOS area 0xE0000-0xFFFFF. Both are read through the direct
 * map rather than by identity-mapping low memory - paging_init dropped the
 * identity map, and phys_to_virt resolves any physical address. */
static const acpi_rsdp_t *find_rsdp(void) {
    uint64 ebda;
    uint64 addr;
    const uint8 *p;

    /* The EBDA segment word lives at physical 0x40E. */
    ebda = (uint64)(*(const uint16 *)phys_to_virt(0x40E)) << 4;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        for (addr = ebda; addr < ebda + 1024; addr += 16) {
            p = (const uint8 *)phys_to_virt(addr);
            if (sig_eq((const char *)p, "RSD PTR ", 8) &&
                checksum_ok(p, 20)) {
                return (const acpi_rsdp_t *)p;
            }
        }
    }
    for (addr = 0xE0000; addr < 0x100000; addr += 16) {
        p = (const uint8 *)phys_to_virt(addr);
        if (sig_eq((const char *)p, "RSD PTR ", 8) && checksum_ok(p, 20)) {
            return (const acpi_rsdp_t *)p;
        }
    }
    return NULL;
}

static const acpi_sdt_header_t *find_madt(const acpi_rsdp_t *rsdp) {
    const acpi_sdt_header_t *root;
    uint32 entries, i;

    /* XSDT when the firmware offers one: its pointers are 64-bit, and a
     * table above 4GB is unreachable through the RSDT's 32-bit entries.
     * QEMU keeps everything low, so this is correctness for its own sake
     * rather than something observable here. */
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        root = (const acpi_sdt_header_t *)phys_to_virt(rsdp->xsdt_address);
        if (!sig_eq(root->signature, "XSDT", 4) ||
            !checksum_ok((const uint8 *)root, root->length)) {
            return NULL;
        }
        entries = (root->length - sizeof(acpi_sdt_header_t)) / 8;
        for (i = 0; i < entries; i++) {
            uint64 phys = ((const uint64 *)((const uint8 *)root +
                            sizeof(acpi_sdt_header_t)))[i];
            const acpi_sdt_header_t *t =
                (const acpi_sdt_header_t *)phys_to_virt(phys);

            if (sig_eq(t->signature, "APIC", 4)) {
                return t;
            }
        }
        return NULL;
    }

    if (rsdp->rsdt_address == 0) {
        return NULL;
    }
    root = (const acpi_sdt_header_t *)phys_to_virt(rsdp->rsdt_address);
    if (!sig_eq(root->signature, "RSDT", 4) ||
        !checksum_ok((const uint8 *)root, root->length)) {
        return NULL;
    }
    entries = (root->length - sizeof(acpi_sdt_header_t)) / 4;
    for (i = 0; i < entries; i++) {
        uint32 phys = ((const uint32 *)((const uint8 *)root +
                        sizeof(acpi_sdt_header_t)))[i];
        const acpi_sdt_header_t *t =
            (const acpi_sdt_header_t *)phys_to_virt(phys);

        /* "APIC" is the MADT's signature. Not a typo for the table that
         * describes APICs - the signature really is APIC and the name
         * really is MADT. */
        if (sig_eq(t->signature, "APIC", 4)) {
            return t;
        }
    }
    return NULL;
}

int acpi_enumerate_cpus(void) {
    const acpi_rsdp_t *rsdp;
    const acpi_sdt_header_t *madt_hdr;
    const acpi_madt_t *madt;
    const uint8 *p, *end;

    if (scanned) {
        return cpu_count;
    }
    scanned = 1;
    cpu_count = 0;
    ioapic_count = 0;
    override_count = 0;
    failure = NULL;

    rsdp = find_rsdp();
    if (rsdp == NULL) {
        failure = "no RSDP";
        return 0;
    }
    madt_hdr = find_madt(rsdp);
    if (madt_hdr == NULL) {
        failure = "no MADT";
        return 0;
    }
    if (!checksum_ok((const uint8 *)madt_hdr, madt_hdr->length)) {
        failure = "MADT checksum";
        return 0;
    }

    madt = (const acpi_madt_t *)madt_hdr;
    lapic_addr = madt->lapic_address;

    p   = (const uint8 *)madt + sizeof(acpi_madt_t);
    end = (const uint8 *)madt + madt_hdr->length;

    while (p + sizeof(acpi_madt_entry_t) <= end) {
        const acpi_madt_entry_t *e = (const acpi_madt_entry_t *)p;

        /* A zero-length entry would loop here forever, and the length field
         * is firmware-supplied. Checked rather than trusted. */
        if (e->length < sizeof(acpi_madt_entry_t) || p + e->length > end) {
            break;
        }
        if (e->type == MADT_TYPE_LAPIC &&
            e->length >= (uint8)sizeof(acpi_madt_lapic_t)) {
            const acpi_madt_lapic_t *l = (const acpi_madt_lapic_t *)p;

            /* Bit 0 is "enabled". A disabled entry describes a socket that
             * is present but unusable; sending it INIT-SIPI-SIPI would wait
             * for a CPU that is never going to answer. */
            if ((l->flags & 1) && cpu_count < ACPI_MAX_CPUS) {
                cpu_apic_ids[cpu_count++] = l->apic_id;
            }
        } else if (e->type == MADT_TYPE_IOAPIC &&
                   e->length >= (uint8)sizeof(acpi_madt_ioapic_t)) {
            if (ioapic_count < ACPI_MAX_IOAPICS) {
                ioapics[ioapic_count++] = *(const acpi_madt_ioapic_t *)p;
            }
        } else if (e->type == MADT_TYPE_OVERRIDE &&
                   e->length >= (uint8)sizeof(acpi_madt_override_t)) {
            if (override_count < ACPI_MAX_OVERRIDES) {
                overrides[override_count++] = *(const acpi_madt_override_t *)p;
            }
        }
        p += e->length;
    }
    return cpu_count;
}

int acpi_ioapic_count(void) {
    return ioapic_count;
}

int acpi_ioapic(int index, uint32 *out_id, uint64 *out_addr,
                uint32 *out_gsi_base) {
    if (index < 0 || index >= ioapic_count) {
        return -1;
    }
    if (out_id != NULL) {
        *out_id = ioapics[index].ioapic_id;
    }
    if (out_addr != NULL) {
        *out_addr = ioapics[index].address;
    }
    if (out_gsi_base != NULL) {
        *out_gsi_base = ioapics[index].gsi_base;
    }
    return 0;
}

int acpi_override_count(void) {
    return override_count;
}

int acpi_override(int index, uint8 *out_source, uint32 *out_gsi,
                  uint16 *out_flags) {
    if (index < 0 || index >= override_count) {
        return -1;
    }
    if (out_source != NULL) {
        *out_source = overrides[index].source;
    }
    if (out_gsi != NULL) {
        *out_gsi = overrides[index].gsi;
    }
    if (out_flags != NULL) {
        *out_flags = overrides[index].flags;
    }
    return 0;
}

int acpi_cpu_count(void) {
    return cpu_count;
}

uint32 acpi_cpu_apic_id(int index) {
    if (index < 0 || index >= cpu_count) {
        return 0xFFFFFFFFu;
    }
    return cpu_apic_ids[index];
}

uint64 acpi_lapic_address(void) {
    return lapic_addr;
}

void acpi_report(uint8 color) {
    int i;

    if (cpu_count == 0) {
        kprintf_c(color, "acpi: %s - assuming one CPU\n",
                  failure != NULL ? failure : "not scanned");
        return;
    }
    kprintf_c(color, "acpi: MADT lists %d CPU%s, lapic at %lx, apic ids",
              cpu_count, cpu_count == 1 ? "" : "s", lapic_addr);
    for (i = 0; i < cpu_count; i++) {
        kprintf_c(color, " %d", cpu_apic_ids[i]);
    }
    kprintf_c(color, "\n");

    for (i = 0; i < ioapic_count; i++) {
        kprintf_c(color, "acpi: ioapic %d at %lx, gsi base %d\n",
                  ioapics[i].ioapic_id, (uint64)ioapics[i].address,
                  ioapics[i].gsi_base);
    }
    /* Printed individually rather than counted. An override is the one piece
     * of MADT data whose absence is indistinguishable from its presence until
     * something does not fire, so it is worth being able to read the actual
     * mapping out of the boot log. */
    for (i = 0; i < override_count; i++) {
        kprintf_c(color, "acpi: irq %d overridden to gsi %d, flags %x\n",
                  overrides[i].source, overrides[i].gsi,
                  (uint32)overrides[i].flags);
    }
}
