/* A PCI driver written in FreeBSD Newbus idiom, loaded from /boot/kernel at
 * runtime.
 *
 * The companion to src/kmod/lkpi_ahci.c, and the same claim: nothing in this
 * file is Genesis-shaped. It is ordinary FreeBSD driver source - <sys/bus.h>
 * and <dev/pci/pcivar.h>, a device_method_t table built with DEVMETHOD,
 * device_t/device_get_softc, bus_alloc_resource_any with PCIR_BAR(n) as the
 * rid, rman_get_start, device_printf, and DRIVER_MODULE at the bottom - and
 * it is compiled with `gcc -c` into a relocatable object that
 * kernel/driver/kldload.c links against the kernel's own symbol table.
 *
 * It talks to QEMU's RTL8139 (10ec:8139) and really does talk to it: BAR 0 is
 * an I/O-port range, allocated through the bus with real conflict detection,
 * and the MAC address is read out of the chip's ID registers a byte at a time
 * through bus_read_1. A MAC that reads back as all 0xFF or all 0x00 means the
 * resource is pointing at nothing; printing the real one is the evidence.
 *
 * That I/O-port BAR is deliberate. lkpi_ahci exercises the MEMORY half of the
 * register abstraction, through ioremap; this exercises the PORT half, where
 * there is no mapping at all and the resource's start IS the port number.
 * bus_read_1 dispatches on which kind the resource is, and a driver written
 * once works against either - which is the whole reason that abstraction
 * exists, and it is not tested by a driver that only ever sees one kind.
 *
 * What it deliberately does NOT do is drive the NIC. There is no ifnet in
 * this kernel (ROADMAP item 6), nothing to attach a network interface to, and
 * a half-initialised NIC with its receive ring pointed at nothing would be a
 * way to corrupt memory by DMA. It probes, reports, and stops.
 */

#include <dev/pci/pcivar.h>
#include <sys/bus.h>

/* RTL8139 registers, from the datasheet. IDR0-5 hold the MAC address and are
 * readable immediately after reset with no initialisation at all - which is
 * what makes them the right thing to prove a mapping with. */
#define RL_IDR0     0x00
#define RL_TXSTAT0  0x10
#define RL_CMD      0x37
#define RL_CONFIG1  0x52

struct nb_rtl_softc {
    struct resource *port;
    int              rid;
    unsigned char    mac[6];
};

static int nb_rtl_probe(device_t dev)
{
    if (pci_get_vendor(dev) != 0x10EC || pci_get_device(dev) != 0x8139)
        return 1;   /* not mine */

    device_set_desc(dev, "RealTek 8139 10/100BaseTX");
    return BUS_PROBE_DEFAULT;
}

static int nb_rtl_attach(device_t dev)
{
    struct nb_rtl_softc *sc = device_get_softc(dev);
    int i;

    /* The real bus_alloc_resource_any idiom, exactly as FreeBSD driver source
     * writes it. This goes through bus.c's conflict detection, so two drivers
     * cannot both claim this range - which is the gap ROADMAP item 11 named
     * ("a driver reads pci_bar_size straight off the raw pci_ivars_t with no
     * bus-mediated conflict detection at all"). */
    sc->rid  = PCIR_BAR(0);
    sc->port = bus_alloc_resource_any(dev, SYS_RES_IOPORT, &sc->rid,
                                      RF_ACTIVE);
    if (sc->port == NULL) {
        device_printf(dev, "could not allocate the I/O port range\n");
        return 1;
    }

    /* Enable port decoding before touching a port. The device answers
     * nothing with this bit clear, and the reads below would come back as
     * 0xFF with no indication why. */
    pci_enable_io(dev, SYS_RES_IOPORT);

    for (i = 0; i < 6; i++)
        sc->mac[i] = bus_read_1(sc->port, RL_IDR0 + i);

    device_printf(dev, "%s rev %d\n", device_get_desc(dev),
                  pci_get_revid(dev));
    device_printf(dev, "io port %lx size %lx, irq %d\n",
                  rman_get_start(sc->port), rman_get_size(sc->port),
                  pci_read_config(dev, PCIR_INTLINE, 1));
    /* Printed byte by byte rather than with %pM: Genesis's kprintf does not
     * implement Linux's extended pointer formats, and this is FreeBSD source
     * anyway - upstream spells it %6D, which kprintf does not have either. */
    device_printf(dev, "mac %x:%x:%x:%x:%x:%x  cmd %x  config1 %x\n",
                  sc->mac[0], sc->mac[1], sc->mac[2],
                  sc->mac[3], sc->mac[4], sc->mac[5],
                  bus_read_1(sc->port, RL_CMD),
                  bus_read_1(sc->port, RL_CONFIG1));

    /* No bus_setup_intr and no ifnet - see the file comment. The interrupt
     * would have nowhere to deliver a packet to. */
    return 0;
}

static void nb_rtl_detach(device_t dev)
{
    struct nb_rtl_softc *sc = device_get_softc(dev);

    if (sc != NULL && sc->port != NULL) {
        bus_release_resource(dev, SYS_RES_IOPORT, sc->rid, sc->port);
        sc->port = NULL;
    }
}

static device_method_t nb_rtl_methods[] = {
    DEVMETHOD(device_probe,  nb_rtl_probe),
    DEVMETHOD(device_attach, nb_rtl_attach),
    DEVMETHOD(device_detach, nb_rtl_detach),
    DEVMETHOD_END,
};

static driver_t nb_rtl_driver = {
    .name    = "nb_rtl",
    .methods = nb_rtl_methods,
    .size    = sizeof(struct nb_rtl_softc),
    .pass    = BUS_PASS_DEFAULT,
};

DRIVER_MODULE(nb_rtl, pci, nb_rtl_driver, 0, 0);
