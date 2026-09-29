/* A PCI driver written in Linux idiom, loaded from /lib/modules at runtime.
 *
 * The point of this file is that nothing in it is Genesis-shaped. It is
 * ordinary Linux driver source - the same includes, the same struct
 * pci_driver, the same PCI_DEVICE() id table, the same module_pci_driver()
 * at the bottom, the same pr_info/kzalloc/ioremap/readl - and it is compiled
 * with `gcc -c` into a relocatable object that kernel/driver/kldload.c links
 * against the kernel's own symbol table at boot.
 *
 * It talks to QEMU's ich9-ahci controller (8086:2922) and it really does talk
 * to it: BAR 5 is mapped through ioremap and the AHCI capability registers
 * are read back off the hardware. Reading a plausible port count and version
 * out of a device is evidence that the mapping is real; printing a string
 * would not have been.
 *
 * What it deliberately does NOT do is drive the controller. This is not a
 * storage driver and there is no attempt to reset it, enable AHCI mode, or
 * touch a port - Genesis already has a working disk path (kernel/dev/ata.c)
 * and a half-initialised AHCI controller beside it would be a way to lose
 * data, not a feature. It probes, reports, and stops.
 */

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/* AHCI's HBA memory registers, from the spec. Only the three that can be
 * read without disturbing anything. */
#define AHCI_HBA_CAP  0x00   /* capabilities: port count, command slots  */
#define AHCI_HBA_GHC  0x04   /* global host control                      */
#define AHCI_HBA_VS   0x10   /* version, as BCD-ish major/minor          */

#define AHCI_CAP_NP   0x1F        /* bits 0-4: number of ports, minus one */
#define AHCI_CAP_NCS  0x1F00      /* bits 8-12: command slots, minus one  */

struct ahci_probe {
    void __iomem *mmio;
    spinlock_t    lock;
    u32           cap;
    u32           version;
};

static const struct pci_device_id ahci_probe_ids[] = {
    { PCI_DEVICE(0x8086, 0x2922) },
    /* The all-zero terminator Linux's id_table convention requires. Not
     * decoration - lkpi.c walks until it sees it. */
    { 0 }
};
MODULE_DEVICE_TABLE(pci, ahci_probe_ids);

static int ahci_probe_pci(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct ahci_probe *sc;
    unsigned long bar_start, bar_len;
    u16 command = 0;
    int ports, slots;

    (void)id;

    sc = kzalloc(sizeof(*sc), GFP_KERNEL);
    if (!sc)
        return -ENOMEM;

    spin_lock_init(&sc->lock);

    if (pci_enable_device(pdev)) {
        pr_err("lkpi_ahci: pci_enable_device failed\n");
        kfree(sc);
        return -ENODEV;
    }

    /* BAR 5 is where AHCI puts its HBA registers - the spec calls it ABAR.
     * Checked for being memory rather than assumed: a port BAR here would
     * mean this is not the device we think it is. */
    bar_start = pci_resource_start(pdev, 5);
    bar_len   = pci_resource_len(pdev, 5);
    if (!bar_start || !bar_len ||
        !(pci_resource_flags(pdev, 5) & IORESOURCE_MEM)) {
        pr_err("lkpi_ahci: BAR 5 is not a memory resource\n");
        kfree(sc);
        return -ENODEV;
    }

    sc->mmio = pci_iomap(pdev, 5, 0);
    if (!sc->mmio) {
        pr_err("lkpi_ahci: could not map BAR 5\n");
        kfree(sc);
        return -ENOMEM;
    }

    /* The actual hardware reads. If ioremap handed back a mapping of the
     * wrong physical page these come back as 0x00000000 or 0xFFFFFFFF, which
     * is why the numbers are printed rather than a success message. */
    sc->cap     = readl((char *)sc->mmio + AHCI_HBA_CAP);
    sc->version = readl((char *)sc->mmio + AHCI_HBA_VS);

    ports = (int)(sc->cap & AHCI_CAP_NP) + 1;
    slots = (int)((sc->cap & AHCI_CAP_NCS) >> 8) + 1;

    pci_read_config_word(pdev, PCI_COMMAND, &command);

    pr_info("lkpi_ahci: %x:%x at bar5 %lx len %lx irq %d\n",
            pdev->vendor, pdev->device, bar_start, bar_len,
            pci_irq_line(pdev));
    pr_info("lkpi_ahci: HBA cap %x version %x - %d ports, %d command slots\n",
            sc->cap, sc->version, ports, slots);
    pr_info("lkpi_ahci: pci command register now %x\n", command);

    pci_set_drvdata(pdev, sc);

    /* Deliberately NOT taking the controller over - see the file comment.
     * Genesis boots off kernel/dev/ata.c and a partly-configured AHCI
     * controller alongside it is a way to lose a disk. */
    return 0;
}

static void ahci_remove_pci(struct pci_dev *pdev)
{
    struct ahci_probe *sc = pci_get_drvdata(pdev);

    if (!sc)
        return;
    if (sc->mmio)
        pci_iounmap(pdev, sc->mmio);
    kfree(sc);
}

static struct pci_driver ahci_probe_driver = {
    .name     = "lkpi_ahci",
    .id_table = ahci_probe_ids,
    .probe    = ahci_probe_pci,
    .remove   = ahci_remove_pci,
};

module_pci_driver(ahci_probe_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Genesis");
MODULE_DESCRIPTION("LinuxKPI source-compat proof: probes an AHCI controller");
