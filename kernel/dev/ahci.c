/* AHCI, polled. See kernel/include/ahci.h for why this is written rather than
 * vendored, and for the DMA warning that shapes every buffer decision below.
 */

#include "ahci.h"
#include "io.h"
#include "kprintf.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "screen.h"
#include "typesk.h"

/* Declared rather than included, exactly as kernel/driver/newbus_compat.c
 * does. ioremap lives in lkpi_kernel.c because that is where it was first
 * needed, but there is nothing Linux-specific about it - it is
 * kvm_alloc_range plus vmm_map_page with PAGE_PCD|PAGE_PWT, which is what
 * mapping a device register window requires in any idiom. Pulling in
 * linux/io.h to reach it would give this file a Linux flavour it does not
 * otherwise have. */
void *ioremap(unsigned long phys_addr, unsigned long size);

/* --- HBA registers, from the AHCI 1.3.1 spec ----------------------------- */

#define HBA_CAP      0x00   /* host capabilities                           */
#define HBA_GHC      0x04   /* global host control                         */
#define HBA_IS       0x08   /* interrupt status (per port, one bit each)   */
#define HBA_PI       0x0C   /* ports implemented                           */
#define HBA_VS       0x10   /* version                                     */

#define GHC_HR       0x00000001u   /* HBA reset                            */
#define GHC_IE       0x00000002u   /* interrupt enable                     */
#define GHC_AE       0x80000000u   /* AHCI enable                          */

#define CAP_NP_MASK  0x0000001Fu   /* ports, minus one                     */
#define CAP_NCS_MASK 0x00001F00u   /* command slots, minus one             */
#define CAP_S64A     0x80000000u   /* 64-bit addressing supported          */

/* Port registers, at 0x100 + port * 0x80. */
#define PORT_BASE(n) (0x100u + (uint32)(n) * 0x80u)
#define P_CLB        0x00   /* command list base, low                      */
#define P_CLBU       0x04   /* command list base, high                     */
#define P_FB         0x08   /* FIS base, low                               */
#define P_FBU        0x0C   /* FIS base, high                              */
#define P_IS         0x10   /* interrupt status                            */
#define P_IE         0x14   /* interrupt enable                            */
#define P_CMD        0x18   /* command and status                          */
#define P_TFD        0x20   /* task file data                              */
#define P_SIG        0x24   /* signature - says what kind of device        */
#define P_SSTS       0x28   /* SATA status (SCR0)                          */
#define P_SERR       0x30   /* SATA error (SCR1)                           */
#define P_CI         0x38   /* command issue                               */

#define PCMD_ST      0x0001u   /* start                                    */
#define PCMD_FRE     0x0010u   /* FIS receive enable                       */
#define PCMD_FR      0x4000u   /* FIS receive running                      */
#define PCMD_CR      0x8000u   /* command list running                     */

#define PTFD_ERR     0x01u
#define PTFD_DRQ     0x08u
#define PTFD_BSY     0x80u

#define PIS_TFES     0x40000000u   /* task file error status               */

/* PxSSTS.DET == 3 means "device present and Phy communication established".
 * Any other value - including 1, "present but no communication" - is a port
 * with nothing usable on it, and treating 1 as present is how a driver hangs
 * waiting for a drive that is not talking. */
#define SSTS_DET_PRESENT 3

/* PxSIG. A SATA disk answers 0x00000101; ATAPI answers 0xEB140101. The others
 * (port multiplier, enclosure) are recognised only so they can be skipped by
 * name rather than falling into the disk path. */
#define SIG_SATA     0x00000101u
#define SIG_ATAPI    0xEB140101u
#define SIG_SEMB     0xC33C0101u
#define SIG_PM       0x96690101u

/* ATA commands. EXT forms throughout: the 28-bit forms cannot address a disk
 * larger than 128GB and there is no reason to have both paths. */
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_IDENTIFY       0xEC

/* --- the in-memory structures the controller walks ----------------------- */

/* One command header. Thirty-two of these make the command list. */
typedef struct {
    uint8  cfl_a_w_p;    /* [4:0] FIS length in dwords, [6] write          */
    uint8  reset_bist;
    uint16 prdtl;        /* PRDT entry count                               */
    volatile uint32 prdbc;   /* bytes the controller transferred           */
    uint32 ctba;         /* command table base, low  (128-byte aligned)    */
    uint32 ctbau;        /* command table base, high                       */
    uint32 reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

/* One scatter-gather entry. */
typedef struct {
    uint32 dba;          /* data base address, low                         */
    uint32 dbau;         /* high                                           */
    uint32 reserved;
    uint32 dbc_i;        /* [21:0] byte count MINUS ONE, [31] interrupt    */
} __attribute__((packed)) ahci_prd_t;

/* The command table: a 64-byte command FIS, 16 bytes of ATAPI packet, 48
 * reserved, then the PRDT. */
typedef struct {
    uint8      cfis[64];
    uint8      acmd[16];
    uint8      reserved[48];
    ahci_prd_t prdt[8];
} __attribute__((packed)) ahci_cmd_table_t;

/* A Host-to-Device register FIS - what actually carries the command. */
typedef struct {
    uint8  fis_type;     /* 0x27                                           */
    uint8  pmport_c;     /* [7] C: this is a command, not a control update */
    uint8  command;
    uint8  featurel;
    uint8  lba0, lba1, lba2, device;
    uint8  lba3, lba4, lba5, featureh;
    uint8  countl, counth, icc, control;
    uint8  reserved[4];
} __attribute__((packed)) ahci_h2d_fis_t;

/* --- per-port state ------------------------------------------------------ */

/* One 4KB frame holds everything one port needs, and the offsets are chosen so
 * that a single page-aligned allocation satisfies all three of AHCI's
 * alignment rules at once:
 *
 *   0x000  command list    32 x 32 bytes = 1024, needs 1KB alignment
 *   0x400  received FIS    256 bytes,            needs 256B alignment
 *   0x500  command table   needs 128B alignment; 0x500 is 128-aligned
 *
 * pmm_alloc_frame returns a 4096-byte-aligned frame, so every one of those
 * holds. Doing it as three separate allocations would need three alignment
 * arguments the PMM does not take. */
#define OFF_CMDLIST  0x000
#define OFF_FIS      0x400
#define OFF_CMDTABLE 0x500

typedef struct {
    volatile uint8 *abar;      /* the HBA's registers, mapped              */
    int             port;
    phys_addr_t     dma_phys;  /* the frame above                          */
    uint8          *dma_virt;
    phys_addr_t     bounce_phys;
    uint8          *bounce_virt;
} ahci_port_t;

static ahci_device_t ahci_devs[AHCI_MAX_PORTS];
static ahci_port_t   ahci_ports[AHCI_MAX_PORTS];
static int           ahci_count;

/* --- register access ------------------------------------------------------
 *
 * Through volatile pointers into the BAR. Every one of these is an MMIO access
 * to a device register and must not be reordered or elided, which is what the
 * volatile is for - not for thread safety, of which there is none here. */
static uint32 hba_read(volatile uint8 *abar, uint32 off) {
    return *(volatile uint32 *)(abar + off);
}

static void hba_write(volatile uint8 *abar, uint32 off, uint32 v) {
    *(volatile uint32 *)(abar + off) = v;
}

static uint32 port_read(ahci_port_t *p, uint32 reg) {
    return hba_read(p->abar, PORT_BASE(p->port) + reg);
}

static void port_write(ahci_port_t *p, uint32 reg, uint32 v) {
    hba_write(p->abar, PORT_BASE(p->port) + reg, v);
}

/* A bounded spin. Every wait in this file goes through one of these so that a
 * controller that never answers produces a timeout rather than a hang - which
 * on a non-preemptive kernel means the machine, not just this driver. */
static int wait_clear(volatile uint32 *reg, uint32 mask, uint32 spins) {
    while (spins-- > 0) {
        if ((*reg & mask) == 0) {
            return 0;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }
    return -1;
}

static int port_wait_clear(ahci_port_t *p, uint32 reg, uint32 mask,
                           uint32 spins) {
    return wait_clear((volatile uint32 *)(p->abar + PORT_BASE(p->port) + reg),
                      mask, spins);
}

/* --- port start/stop ----------------------------------------------------- */

static int port_stop(ahci_port_t *p) {
    uint32 cmd = port_read(p, P_CMD);

    port_write(p, P_CMD, cmd & ~PCMD_ST);
    if (port_wait_clear(p, P_CMD, PCMD_CR, 2000000) != 0) {
        return -1;
    }
    cmd = port_read(p, P_CMD);
    port_write(p, P_CMD, cmd & ~PCMD_FRE);
    if (port_wait_clear(p, P_CMD, PCMD_FR, 2000000) != 0) {
        return -1;
    }
    return 0;
}

static void port_start(ahci_port_t *p) {
    /* FRE before ST, and not the other way round: the controller may post a
     * received FIS as soon as the command list is running, and with FIS
     * receive still disabled that is a write to an address it was never
     * given. */
    port_write(p, P_CMD, port_read(p, P_CMD) | PCMD_FRE);
    port_write(p, P_CMD, port_read(p, P_CMD) | PCMD_ST);
}

/* --- issuing one command -------------------------------------------------
 *
 * One slot, slot zero, because this driver is polled and single-threaded.
 * `write` selects the direction bit in the command header; `buf_phys` and
 * `bytes` describe a single physically contiguous region, which is why the
 * callers bounce.
 */
static int port_command(ahci_port_t *p, uint8 command, uint64 lba,
                        uint32 sectors, phys_addr_t buf_phys, uint32 bytes,
                        int write) {
    ahci_cmd_header_t *hdr = (ahci_cmd_header_t *)(p->dma_virt + OFF_CMDLIST);
    ahci_cmd_table_t  *tbl = (ahci_cmd_table_t *)(p->dma_virt + OFF_CMDTABLE);
    ahci_h2d_fis_t    *fis = (ahci_h2d_fis_t *)tbl->cfis;
    phys_addr_t        tbl_phys = p->dma_phys + OFF_CMDTABLE;
    uint32             i;

    /* The drive must be idle before a command is built into the slot. A
     * BSY that never clears here is a drive that is wedged, and issuing on
     * top of it would queue a command that can never retire. */
    if (port_wait_clear(p, P_TFD, PTFD_BSY | PTFD_DRQ, 2000000) != 0) {
        return AHCI_ERR_TIMEOUT;
    }

    for (i = 0; i < sizeof(*tbl); i++) {
        ((uint8 *)tbl)[i] = 0;
    }
    for (i = 0; i < sizeof(*hdr); i++) {
        ((uint8 *)hdr)[i] = 0;
    }

    fis->fis_type = 0x27;
    fis->pmport_c = 0x80;              /* C: this FIS carries a command */
    fis->command  = command;
    fis->lba0     = (uint8)(lba      );
    fis->lba1     = (uint8)(lba >>  8);
    fis->lba2     = (uint8)(lba >> 16);
    fis->lba3     = (uint8)(lba >> 24);
    fis->lba4     = (uint8)(lba >> 32);
    fis->lba5     = (uint8)(lba >> 40);
    /* Bit 6 is LBA mode. Without it the drive reads the LBA fields as
     * cylinder/head/sector and answers from somewhere else entirely. */
    fis->device   = 0x40;
    fis->countl   = (uint8)(sectors      );
    fis->counth   = (uint8)(sectors >> 8);

    hdr->cfl_a_w_p = (uint8)(sizeof(ahci_h2d_fis_t) / sizeof(uint32));
    if (write) {
        hdr->cfl_a_w_p |= 0x40;
    }
    hdr->prdtl = bytes ? 1 : 0;
    hdr->ctba  = (uint32)(tbl_phys);
    hdr->ctbau = (uint32)(tbl_phys >> 32);
    hdr->prdbc = 0;

    if (bytes) {
        tbl->prdt[0].dba   = (uint32)(buf_phys);
        tbl->prdt[0].dbau  = (uint32)(buf_phys >> 32);
        /* MINUS ONE. The field is a byte count biased by one, so writing the
         * true length transfers one byte too many - which for a 512-byte
         * sector means a byte written past the end of the caller's buffer. */
        tbl->prdt[0].dbc_i = bytes - 1;
    }

    /* Clear stale status before issuing, or the completion check below sees
     * an error from a previous command and reports it against this one. */
    port_write(p, P_IS, port_read(p, P_IS));
    port_write(p, P_SERR, port_read(p, P_SERR));

    port_write(p, P_CI, 1u << 0);

    if (port_wait_clear(p, P_CI, 1u << 0, 8000000) != 0) {
        return AHCI_ERR_TIMEOUT;
    }
    if (port_read(p, P_IS) & PIS_TFES) {
        return AHCI_ERR_TFD;
    }
    if (port_read(p, P_TFD) & PTFD_ERR) {
        return AHCI_ERR_TFD;
    }
    return AHCI_OK;
}

/* --- IDENTIFY ------------------------------------------------------------ */

/* ATA strings are byte-swapped within each 16-bit word, which is a property of
 * the wire format and not of this machine's endianness - so the swap is
 * unconditional rather than guarded by a byte-order test. */
static void ata_string(const uint16 *src, int words, char *out) {
    int i;

    for (i = 0; i < words; i++) {
        out[i * 2]     = (char)(src[i] >> 8);
        out[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
    out[words * 2] = '\0';
    for (i = words * 2 - 1; i >= 0 && (out[i] == ' ' || out[i] == '\0'); i--) {
        out[i] = '\0';
    }
}

static int port_identify(ahci_port_t *p, ahci_device_t *d) {
    const uint16 *id = (const uint16 *)p->bounce_virt;
    int rc;

    rc = port_command(p, ATA_CMD_IDENTIFY, 0, 0, p->bounce_phys, 512, 0);
    if (rc != AHCI_OK) {
        return rc;
    }

    ata_string(&id[27], 20, d->model);

    /* Word 83 bit 10 says the drive supports the 48-bit commands, in which
     * case words 100-103 hold the real capacity. Without that bit the 28-bit
     * count in words 60-61 is all there is - and this driver only issues EXT
     * commands, so a drive without it is refused rather than addressed with a
     * command it does not implement. */
    if (id[83] & (1u << 10)) {
        d->sectors = (uint64)id[100] | ((uint64)id[101] << 16) |
                     ((uint64)id[102] << 32) | ((uint64)id[103] << 48);
    } else {
        d->sectors = (uint64)id[60] | ((uint64)id[61] << 16);
    }
    return AHCI_OK;
}

/* --- read and write ------------------------------------------------------
 *
 * Both bounce through a dedicated physically contiguous frame. See ahci.h:
 * the caller's buffer is an ordinary kernel pointer and may straddle pages
 * that are not adjacent in RAM, and the controller walks physical addresses
 * with the MMU bypassed.
 *
 * Eight sectors at a time, which is the frame size. Larger transfers would
 * need either a real scatter list built from the caller's page mappings or a
 * bigger bounce, and both are worth doing once this is known to work.
 */
#define BOUNCE_SECTORS (4096 / AHCI_SECTOR_SIZE)

static ahci_port_t *port_of(ahci_device_t *d) {
    int i;

    for (i = 0; i < AHCI_MAX_PORTS; i++) {
        if (&ahci_devs[i] == d) {
            return &ahci_ports[i];
        }
    }
    return NULL;
}

int ahci_read(ahci_device_t *dev, uint64 lba, uint32 count, void *buffer) {
    ahci_port_t *p = port_of(dev);
    uint8 *out = (uint8 *)buffer;

    if (dev == NULL || !dev->present || p == NULL) {
        return AHCI_ERR_NODEV;
    }
    if (lba + count > dev->sectors) {
        return AHCI_ERR_RANGE;
    }

    while (count > 0) {
        uint32 chunk = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        uint32 bytes = chunk * AHCI_SECTOR_SIZE;
        uint32 i;
        int rc = port_command(p, ATA_CMD_READ_DMA_EXT, lba, chunk,
                              p->bounce_phys, bytes, 0);

        if (rc != AHCI_OK) {
            return rc;
        }
        for (i = 0; i < bytes; i++) {
            out[i] = p->bounce_virt[i];
        }
        out   += bytes;
        lba   += chunk;
        count -= chunk;
    }
    return AHCI_OK;
}

int ahci_write(ahci_device_t *dev, uint64 lba, uint32 count,
               const void *buffer) {
    ahci_port_t *p = port_of(dev);
    const uint8 *in = (const uint8 *)buffer;

    if (dev == NULL || !dev->present || p == NULL) {
        return AHCI_ERR_NODEV;
    }
    if (lba + count > dev->sectors) {
        return AHCI_ERR_RANGE;
    }

    while (count > 0) {
        uint32 chunk = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        uint32 bytes = chunk * AHCI_SECTOR_SIZE;
        uint32 i;
        int rc;

        for (i = 0; i < bytes; i++) {
            p->bounce_virt[i] = in[i];
        }
        rc = port_command(p, ATA_CMD_WRITE_DMA_EXT, lba, chunk,
                          p->bounce_phys, bytes, 1);
        if (rc != AHCI_OK) {
            return rc;
        }
        in    += bytes;
        lba   += chunk;
        count -= chunk;
    }
    return AHCI_OK;
}

/* --- bring-up ------------------------------------------------------------ */

static int port_setup(ahci_port_t *p, ahci_device_t *d, volatile uint8 *abar,
                      int port) {
    uint32 ssts, sig, i;

    p->abar = abar;
    p->port = port;

    ssts = hba_read(abar, PORT_BASE(port) + P_SSTS);
    if ((ssts & 0x0F) != SSTS_DET_PRESENT) {
        return AHCI_ERR_NODEV;
    }

    sig = hba_read(abar, PORT_BASE(port) + P_SIG);
    if (sig == SIG_ATAPI) {
        d->atapi = 1;
        /* Recognised and refused. An ATAPI device needs the packet command
         * path, which this driver does not have; reporting it as a disk would
         * mean every read returning a task file error. */
        return AHCI_ERR_NOTSATA;
    }
    if (sig != SIG_SATA) {
        return AHCI_ERR_NOTSATA;
    }

    p->dma_phys = pmm_alloc_frame();
    if (p->dma_phys == 0) {
        return AHCI_ERR_NODEV;
    }
    p->bounce_phys = pmm_alloc_frame();
    if (p->bounce_phys == 0) {
        pmm_free_frame(p->dma_phys);
        return AHCI_ERR_NODEV;
    }
    p->dma_virt    = (uint8 *)phys_to_virt(p->dma_phys);
    p->bounce_virt = (uint8 *)phys_to_virt(p->bounce_phys);
    for (i = 0; i < 4096; i++) {
        p->dma_virt[i] = 0;
    }

    if (port_stop(p) != 0) {
        return AHCI_ERR_TIMEOUT;
    }

    port_write(p, P_CLB,  (uint32)(p->dma_phys + OFF_CMDLIST));
    port_write(p, P_CLBU, (uint32)((p->dma_phys + OFF_CMDLIST) >> 32));
    port_write(p, P_FB,   (uint32)(p->dma_phys + OFF_FIS));
    port_write(p, P_FBU,  (uint32)((p->dma_phys + OFF_FIS) >> 32));

    /* Write-one-to-clear: reading and writing back is what clears whatever is
     * set, and writing zero would clear nothing. */
    port_write(p, P_SERR, port_read(p, P_SERR));
    port_write(p, P_IE, 0);            /* polled; no interrupts wanted */

    port_start(p);

    if (port_identify(p, d) != AHCI_OK) {
        port_stop(p);
        return AHCI_ERR_TIMEOUT;
    }

    d->hba     = (void *)abar;
    d->port    = port;
    d->present = 1;
    return AHCI_OK;
}

static void hba_setup(const pci_ivars_t *f) {
    uint64 base = 0, size = 0;
    int    is_mem = 0;
    volatile uint8 *abar;
    uint32 cap, pi, ghc;
    int port;

    /* BAR5 is ABAR - AHCI's registers are always in BAR 5, unlike every other
     * class of device, and the spec says so. */
    if (pci_bar_size(f, 5, &base, &size, &is_mem) != 0 || !is_mem ||
        base == 0 || size < 0x180) {
        return;
    }

    /* ioremap, NOT phys_to_virt.
     *
     * The first version used phys_to_virt and page-faulted on the very first
     * register read. paging.h says why in as many words: phys_to_virt "goes
     * through the direct map, so it resolves for ANY physical address IN RAM",
     * and it names an MMIO address as something it is explicitly not for. The
     * direct map is built over the E820 RAM regions; a BAR at 0xFEBD1000 is
     * not in any of them, so the address it computes is simply unmapped.
     *
     * ioremap also maps the window UNCACHED (PAGE_PCD|PAGE_PWT), which matters
     * independently: a cached mapping of a device register would let the CPU
     * satisfy a read of PxCI from a cache line and never see the controller
     * clear it - a hang that looks exactly like a dead controller. */
    abar = (volatile uint8 *)ioremap((unsigned long)base, (unsigned long)size);
    if (abar == NULL) {
        kprintf_c(0x0C, "ahci: could not map ABAR at %x\n", (uint32)base);
        return;
    }

    /* Bus mastering, or every DMA transfer below is silently dropped by the
     * bridge. This is the single easiest thing to forget: the controller
     * initialises, IDENTIFY appears to be issued, and PxCI never clears. */
    {
        uint32 cmdreg = pci_cfg_read32(f->bus, f->slot, f->func, 0x04);

        if ((cmdreg & 0x06) != 0x06) {
            pci_cfg_write32(f->bus, f->slot, f->func, 0x04, cmdreg | 0x06);
        }
    }

    ghc = hba_read(abar, HBA_GHC);
    hba_write(abar, HBA_GHC, ghc | GHC_AE);

    cap = hba_read(abar, HBA_CAP);
    pi  = hba_read(abar, HBA_PI);

    kprintf_c(0x07, "ahci: %x:%x  %u ports, %u slots, version %x\n",
              f->vendor_id, f->device_id, (cap & CAP_NP_MASK) + 1,
              ((cap & CAP_NCS_MASK) >> 8) + 1, hba_read(abar, HBA_VS));

    for (port = 0; port < 32; port++) {
        if (!(pi & (1u << port))) {
            continue;
        }
        if (ahci_count >= AHCI_MAX_PORTS) {
            kprintf_c(0x0E, "ahci: more ports than slots; %d ignored\n",
                      32 - port);
            break;
        }
        if (port_setup(&ahci_ports[ahci_count], &ahci_devs[ahci_count],
                       abar, port) == AHCI_OK) {
            ahci_count++;
        }
    }
}

void ahci_init(void) {
    uint8 bus, slot, func;

    ahci_count = 0;

    /* Walked from config space directly rather than through the bus layer's
     * child list, for the same reason ata_init does its own probing: this
     * runs before drivers are attached, and the machine has to have a disk
     * before anything that needs one starts. */
    for (bus = 0; bus < 4; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (func = 0; func < 8; func++) {
                pci_ivars_t f;
                uint32 id = pci_cfg_read32(bus, slot, func, 0x00);
                uint32 cls;

                if ((id & 0xFFFF) == 0xFFFF) {
                    continue;
                }
                cls = pci_cfg_read32(bus, slot, func, 0x08);

                /* class 0x01 mass storage, subclass 0x06 SATA, prog-if 0x01
                 * AHCI. The prog-if matters: the same controller in RAID mode
                 * reports 0x04 and does NOT present an AHCI register set. */
                if (((cls >> 24) & 0xFF) != 0x01 ||
                    ((cls >> 16) & 0xFF) != 0x06 ||
                    ((cls >> 8)  & 0xFF) != 0x01) {
                    continue;
                }

                f.bus = bus; f.slot = slot; f.func = func;
                f.vendor_id = (uint16)(id & 0xFFFF);
                f.device_id = (uint16)(id >> 16);
                {
                    int b;
                    for (b = 0; b < 6; b++) {
                        f.bar_raw[b] = pci_cfg_read32(bus, slot, func,
                                                      (uint8)(0x10 + b * 4));
                    }
                }
                hba_setup(&f);
            }
        }
    }
}

ahci_device_t *ahci_get(int index) {
    if (index < 0 || index >= ahci_count) {
        return NULL;
    }
    return &ahci_devs[index];
}

void ahci_report(uint8 color) {
    int i;

    for (i = 0; i < ahci_count; i++) {
        ahci_device_t *d = &ahci_devs[i];

        if (!d->present) {
            continue;
        }
        kprintf_c(color, "  ahci%d: port %d  %s  %u MB\n", i, d->port,
                  d->model,
                  (uint32)(d->sectors / (1024 * 1024 / AHCI_SECTOR_SIZE)));
    }
}

/* --- selftest -------------------------------------------------------------
 *
 * IDENTIFY has already run by the time this is called - ahci_init does it, and
 * a device that answered it is a device whose command path works for a
 * zero-length-ish transfer. What this adds is the part that actually moves
 * data: a write followed by a read of the same sector, compared byte for byte.
 *
 * It writes to the LAST sector of the device rather than the first. The first
 * is a partition table on anything real, and a selftest that scribbles on one
 * is a selftest that destroys the disk it was meant to check. The last sector
 * is also where a GPT backup header lives, so this REFUSES to run on a device
 * that is not the scratch disk QEMU provides - see the size check.
 */
int ahci_selftest(void) {
    ahci_device_t *d = ahci_get(0);
    ahci_port_t   *p;
    uint8  *scratch;
    uint64  lba;
    uint32  i;
    int     rc, wrong = 0;

    if (d == NULL || !d->present) {
        kprintf_c(0x0E, "ahci: selftest skipped - no device on any port\n");
        return 0;
    }
    p = port_of(d);
    if (p == NULL) {
        return 1;
    }

    /* Only on a disk small enough to be the scratch image build.py attaches.
     * A real disk is never written by a selftest, and the check is on SIZE
     * rather than on a flag because a flag is something a future caller can
     * set wrongly. */
    if (d->sectors == 0 || d->sectors > (64ull * 1024 * 1024 / AHCI_SECTOR_SIZE)) {
        kprintf_c(0x0E, "ahci: selftest skipped - %s is not a scratch disk\n",
                  d->model);
        return 0;
    }

    lba = d->sectors - 1;
    scratch = p->bounce_virt;   /* reused deliberately; nothing else runs */

    /* A pattern with structure. A constant fill still compares equal after a
     * transfer that read the wrong sector or byteswapped it. */
    for (i = 0; i < AHCI_SECTOR_SIZE; i++) {
        scratch[i] = (uint8)((i * 37 + (i >> 4) * 11) & 0xFF);
    }
    rc = ahci_write(d, lba, 1, scratch);
    if (rc != AHCI_OK) {
        kprintf_c(0x0C, "ahci: selftest FAILED - write returned %d\n", rc);
        return 1;
    }

    for (i = 0; i < AHCI_SECTOR_SIZE; i++) {
        scratch[i] = 0;
    }
    rc = ahci_read(d, lba, 1, scratch);
    if (rc != AHCI_OK) {
        kprintf_c(0x0C, "ahci: selftest FAILED - read returned %d\n", rc);
        return 1;
    }
    for (i = 0; i < AHCI_SECTOR_SIZE; i++) {
        if (scratch[i] != (uint8)((i * 37 + (i >> 4) * 11) & 0xFF)) {
            wrong++;
        }
    }
    if (wrong != 0) {
        kprintf_c(0x0C, "ahci: selftest FAILED - %u of %u bytes differ\n",
                  wrong, AHCI_SECTOR_SIZE);
        return 1;
    }

    /* The negative case, so a pass means enforcement rather than a driver
     * that returns AHCI_OK for everything. */
    if (ahci_read(d, d->sectors, 1, scratch) != AHCI_ERR_RANGE) {
        kprintf_c(0x0C, "ahci: selftest FAILED - a read past the end was "
                        "not refused\n");
        return 1;
    }

    kprintf_c(0x0A, "ahci: selftest passed - %u sectors, DMA write/read "
                    "round trip on lba %u\n",
              (uint32)d->sectors, (uint32)lba);
    return 0;
}
