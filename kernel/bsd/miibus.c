/* The MII PHY framework's MAC-facing half.
 *
 * See kernel/bsd/compat/dev/mii/miivar.h for what MII is and why FreeBSD
 * splits a NIC into a MAC driver and a separate PHY driver. This file is the
 * part between them.
 *
 * --- what is real ---------------------------------------------------------
 * The KOBJ interface. A MAC driver's DEVMETHOD(miibus_readreg, ...) entries
 * are dispatchable, which is what kernel/bsd/compat/dev/mii/miibus_if.h
 * builds on and what makes the whole split work at all. mii_attach really
 * does probe the MII address space for a PHY, using the driver's own
 * readreg method, and really does read back the PHY's identity registers.
 *
 * --- what is not ----------------------------------------------------------
 * There are no PHY DRIVERS. Upstream ships about forty (dev/mii/*phy.c), one
 * per PHY family, and each is a real driver with its own reset sequence and
 * quirks. Without one there is no autonegotiation state machine, so:
 *
 *   - mii_attach finds the PHY, reports what it is, and attaches nothing to
 *     it. It returns 0 - success - because the MAC driver's attach should
 *     continue; a MAC with an unknown PHY is a working MAC.
 *   - mii_mediachg cannot push a media selection down, and says so.
 *   - mii_pollstat and mii_tick read the PHY's basic status register
 *     DIRECTLY, which is standard (clause 22) and tells the truth about link
 *     up/down and speed, without any PHY-specific knowledge.
 *
 * So link state is real and negotiation is not. That is the honest boundary
 * and it is drawn where the vendoring boundary naturally falls: porting a
 * PHY driver is its own job and needs this framework to exist first.
 */

#include <sys/param.h>
/* Before anything that reaches net/if.h: struct ifreq embeds a sockaddr by
 * value, and the error when this is missing names net/if.h rather than the
 * file that forgot the include. */
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <net/if.h>
#include <net/if_media.h>
#include <dev/mii/mii.h>
#include <dev/mii/mii_bitbang.h>
#include <dev/mii/miivar.h>
#include <dev/mii/miibus_if.h>

void if_link_state_change(if_t ifp, int link_state);
device_t newbus_add_miibus_child(device_t parent);

#include "kheap.h"
#include "kprintf.h"

/* The descriptor tokens miibus_if.h declares. Real, distinct addresses -
 * only their identity matters, never their contents, exactly as
 * kernel/include/newbus_compat.h documents for the six built-in device_*
 * tokens. */
const int miibus_readreg_desc;
const int miibus_writereg_desc;
const int miibus_statchg_desc;
const int miibus_linkchg_desc;
const int miibus_mediainit_desc;

/* One mii_data per attached MAC. A fixed pool, this tree's convention. */
#define MIIBUS_MAX 4
static struct mii_data mii_pool[MIIBUS_MAX];
static int             mii_pool_used;

/* miibus as a driver_t, because a MAC driver's DRIVER_MODULE(miibus, rl,
 * miibus_driver, 0, 0) registers it by name. It never probes anything -
 * mii_attach creates the association directly - so its methods are empty.
 * It exists so that line of real driver source resolves. */
static device_method_t miibus_methods[] = {
    DEVMETHOD_END,
};

driver_t miibus_driver = {
    "miibus",
    miibus_methods,
    sizeof(struct mii_data),
    0,
};

/* Is there a PHY at this address? A read of BMSR that comes back as all-ones
 * or all-zeros means nothing answered - the MDIO bus floats to one or the
 * other depending on the MAC's pull-ups, so BOTH have to be treated as
 * absent. Checking only 0xFFFF finds a "PHY" at every address on a MAC that
 * pulls low. */
static void mii_update(struct mii_data *mii, device_t dev);

/* --- the miibus handle's softc ------------------------------------------
 *
 * A MAC driver reaches its mii_data with device_get_softc(sc->rl_miibus).
 * Upstream sc->rl_miibus is a real CHILD device whose softc IS the mii_data,
 * created by device_add_child. Genesis's newbus compat has no child-device
 * creation (see device_add_child in kernel/driver/newbus_compat.c), so the
 * handle is the MAC device itself - and device_get_softc on it would return
 * the MAC's own softc, which is the wrong structure entirely.
 *
 * So this table records which mii_data a handle means, and
 * kernel/driver/newbus_compat.c's device_get_softc consults it first. That
 * is a real wart and it is the one place the missing child-device machinery
 * shows through; it is here rather than hidden because the alternative -
 * a driver silently reading its own softc as if it were a mii_data - is
 * unrecoverable. */
static struct { device_t dev; struct mii_data *mii; } mii_softc_map[MIIBUS_MAX];

static void mii_register_softc(device_t dev, struct mii_data *mii) {
    int i;

    for (i = 0; i < MIIBUS_MAX; i++) {
        if (mii_softc_map[i].dev == 0 || mii_softc_map[i].dev == dev) {
            mii_softc_map[i].dev = dev;
            mii_softc_map[i].mii = mii;
            return;
        }
    }
}

struct mii_data *mii_softc_for(device_t dev) {
    int i;

    for (i = 0; i < MIIBUS_MAX; i++) {
        if (mii_softc_map[i].dev == dev) {
            return mii_softc_map[i].mii;
        }
    }
    return 0;
}

static int phy_present(device_t dev, int phy) {
    int bmsr = MIIBUS_READREG(dev, phy, MII_BMSR);

    if (bmsr == 0 || bmsr == 0xFFFF) {
        return 0;
    }
    return 1;
}

int mii_attach(device_t dev, device_t *miibus, if_t ifp,
               ifm_change_cb_t ifmedia_upd, ifm_stat_cb_t ifmedia_sts,
               int capmask, int phyloc, int offloc, int flags) {
    struct mii_data *mii;
    int phy, found = -1;

    (void)capmask; (void)offloc; (void)flags;

    if (mii_pool_used >= MIIBUS_MAX) {
        device_printf(dev, "miibus: pool exhausted\n");
        return ENOMEM;
    }
    mii = &mii_pool[mii_pool_used++];
    mii->mii_ifp = ifp;
    mii->mii_phy = 0;
    mii->mii_media_status = 0;
    mii->mii_media_active = 0;

    /* The media list, owned by the PHY layer. Initialised with the MAC's own
     * change/status callbacks, which is how a media selection reaches the
     * driver. */
    ifmedia_init(&mii->mii_media, IFM_IMASK, ifmedia_upd, ifmedia_sts);

    /* Find the PHY. MII_PHY_ANY means scan; a specific address means use it.
     * Scanning from 0 upward is what upstream does. */
    if (phyloc == MII_PHY_ANY) {
        for (phy = 0; phy < 32; phy++) {
            if (phy_present(dev, phy)) {
                found = phy;
                break;
            }
        }
    } else if (phy_present(dev, phyloc)) {
        found = phyloc;
    }

    if (found < 0) {
        device_printf(dev, "miibus: no PHY found\n");
        /* ENXIO, so the MAC driver's own error path runs. A MAC with no PHY
         * at all cannot bring a link up, and reporting success here would
         * hide that behind an interface that simply never carries traffic. */
        return ENXIO;
    }

    mii->mii_instance = found;
    /* Remember WHICH device this mii_data belongs to.
     *
     * Without it mii_tick and mii_pollstat had no way to reach the MAC's
     * register methods and passed NULL, so every PHY read returned 0, the
     * link read DOWN, and re(4) - which refuses to transmit on a down
     * link - silently queued and dropped everything. The interface
     * attached, reported the right MAC, registered its interrupt, and sent
     * not one frame. */
    mii->mii_dev = dev;
    {
        int id1 = MIIBUS_READREG(dev, found, MII_PHYIDR1);
        int id2 = MIIBUS_READREG(dev, found, MII_PHYIDR2);

        device_printf(dev, "miibus: PHY at address %d, id %x:%x\n",
                      found, id1, id2);
    }

    /* Advertise what the PHY says it can do, read straight out of its BMSR.
     * Clause 22 makes these bits mean the same thing on every PHY, which is
     * why this needs no PHY-specific driver. */
    {
        int bmsr = MIIBUS_READREG(dev, found, MII_BMSR);

        ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_AUTO, 0, 0);
        if (bmsr & BMSR_10THDX) {
            ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_10_T, 0, 0);
        }
        if (bmsr & BMSR_10TFDX) {
            ifmedia_add(&mii->mii_media,
                        IFM_ETHER | IFM_10_T | IFM_FDX, 0, 0);
        }
        if (bmsr & BMSR_100TXHDX) {
            ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_100_TX, 0, 0);
        }
        if (bmsr & BMSR_100TXFDX) {
            ifmedia_add(&mii->mii_media,
                        IFM_ETHER | IFM_100_TX | IFM_FDX, 0, 0);
        }
        ifmedia_set(&mii->mii_media, IFM_ETHER | IFM_AUTO);
    }

    /* The MAC driver reaches its mii_data through device_get_softc on this
     * handle. Genesis has no child-device creation for it, so the handle IS
     * the parent device and the softc lookup is intercepted - see
     * mii_softc_of below. That is a divergence from upstream's real child
     * device, and it is why *miibus is set to dev itself. */
    /* A DISTINCT handle for the miibus.
     *
     * It cannot be the MAC device: a MAC driver does device_get_softc(dev)
     * in its own methods expecting its own softc, and
     * device_get_softc(sc->rl_miibus) expecting the mii_data. One device_t
     * cannot answer both, and making it try returned a mii_data to
     * re_miibus_statchg, which read an interface pointer out of it and
     * faulted.
     *
     * So a real child bus_dev_t is created - which is what upstream's
     * device_add_child does - and the mii_data is registered against THAT.
     * The MAC device is untouched and keeps answering with its own softc. */
    {
        device_t child = newbus_add_miibus_child(dev);

        if (child == 0) {
            device_printf(dev, "miibus: could not create the child device\n");
            return ENOMEM;
        }
        *miibus = child;
        mii_register_softc(child, mii);
    }

    /* Publish the handle BEFORE reading the link.
     *
     * re(4)'s statchg method does device_get_softc(sc->rl_miibus), so the
     * driver must already have the handle when the first statchg fires -
     * otherwise it calls device_get_softc(NULL) and faults reading offset
     * 0x20. Which is exactly what happened. */
    /* Read the link once now, so the MAC is told before anything tries to
     * transmit rather than only at the first one-second tick. */
    mii_update(mii, dev);
    kprintf_c(0x0A, "miibus: link %s, media %x\n",
              (mii->mii_media_status & IFM_ACTIVE) ? "UP" : "DOWN",
              mii->mii_media_active);
    /* `mii` already points INTO the pool - the copy that used to be here
     * was copying a slot onto itself, which was harmless and misleading. */
    return 0;
}

/* Which mii_data belongs to this device. Linear over a four-entry pool, and
 * matched on the DEVICE rather than on "the first one in use" - which is
 * what it did first and which is only correct while there is one NIC. */
static struct mii_data *mii_of(device_t dev) {
    int i;

    for (i = 0; i < mii_pool_used; i++) {
        if (mii_pool[i].mii_dev == dev) {
            return &mii_pool[i];
        }
    }
    return 0;
}

void mii_detach(device_t miibus) {
    struct mii_data *mii = mii_of(miibus);

    if (mii != 0) {
        ifmedia_removeall(&mii->mii_media);
        mii->mii_ifp = 0;
    }
}

/* Read link state straight out of the PHY's basic status register.
 *
 * BMSR_LINK is LATCHING LOW: it reports link-down if the link has been down
 * at any point since the last read, so a single read after a transient can
 * say "down" while the link is up. Upstream reads it twice for this reason
 * and so does this. Getting it wrong produces an interface that reports
 * itself down until something else happens to poll it. */
static void mii_update(struct mii_data *mii, device_t dev) {
    int bmsr, bmcr;
    int old_status, old_active;

    if (mii == 0 || dev == 0) {
        return;
    }
    old_status = mii->mii_media_status;
    old_active = mii->mii_media_active;
    (void)MIIBUS_READREG(dev, mii->mii_instance, MII_BMSR);
    bmsr = MIIBUS_READREG(dev, mii->mii_instance, MII_BMSR);
    bmcr = MIIBUS_READREG(dev, mii->mii_instance, MII_BMCR);

    mii->mii_media_status = IFM_AVALID;
    if (bmsr & BMSR_LINK) {
        mii->mii_media_status |= IFM_ACTIVE;
    }

    /* Speed and duplex from the control register. With no PHY driver there
     * is no autonegotiation RESULT to read - the ANLPAR decode is
     * PHY-specific - so this reports what the PHY is CONFIGURED at, which is
     * accurate for a forced setting and is the PHY's power-on default
     * otherwise. */
    mii->mii_media_active = IFM_ETHER;
    mii->mii_media_active |= (bmcr & BMCR_S100) ? IFM_100_TX : IFM_10_T;
    if (bmcr & BMCR_FDX) {
        mii->mii_media_active |= IFM_FDX;
    }

    /* TELL THE MAC. This is the whole point of the PHY layer from the
     * driver's side: re(4) keeps an RL_FLAG_LINK and refuses to transmit
     * without it, and the only thing that sets it is its own statchg
     * method being called.
     *
     * Its absence was invisible in every other way - the interface
     * attached, reported the right MAC, registered its interrupt, and
     * silently dropped every packet handed to it. */
    if (mii->mii_media_status != old_status ||
        mii->mii_media_active != old_active) {
        MIIBUS_STATCHG(dev);
        if (mii->mii_ifp != 0) {
            if_link_state_change(mii->mii_ifp,
                (mii->mii_media_status & IFM_ACTIVE) ? LINK_STATE_UP
                                                     : LINK_STATE_DOWN);
        }
    }
}

void mii_tick(struct mii_data *mii) {
    if (mii != 0 && mii->mii_ifp != 0) {
        mii_update(mii, mii->mii_dev);
    }
}

void mii_pollstat(struct mii_data *mii) {
    if (mii != 0 && mii->mii_ifp != 0) {
        mii_update(mii, mii->mii_dev);
    }
}

/* mii_mediachg - apply a media selection, and REFRESH THE LINK STATE.
 *
 * The second half is what matters here and it is not optional. A MAC driver
 * calls mii_mediachg from its init routine, and upstream that walks down to
 * the PHY driver and back up through statchg - which is how the MAC learns
 * the link is usable AFTER it has set IFF_DRV_RUNNING.
 *
 * That ordering is the whole problem this solves. re_miibus_statchg returns
 * immediately unless IFF_DRV_RUNNING is set, and mii_attach runs during
 * ATTACH, long before the interface is up. So the statchg fired at attach
 * time was discarded, re never set RL_FLAG_LINK, and re_start_locked
 * returned without transmitting anything - with no error, because refusing
 * to transmit on a down link is correct behaviour. The interface reported
 * link UP and sent nothing.
 *
 * What is still NOT done is pushing a FORCED media selection down: which
 * BMCR bits to write for a given IFM_ word is PHY-specific past speed and
 * duplex, and that is what a PHY driver knows. Autonegotiated media is read
 * back correctly; asking for a specific one is accepted and ignored. */
int mii_mediachg(struct mii_data *mii) {
    if (mii == 0 || mii->mii_dev == 0) {
        return ENXIO;
    }
    mii_update(mii, mii->mii_dev);
    /* Unconditionally, not only on change: the caller has just changed
     * something about the interface (that is why it called), and the
     * state it needs refreshed may be its own rather than the PHY's. */
    MIIBUS_STATCHG(mii->mii_dev);
    return 0;
}

/* --- mii_bitbang ---------------------------------------------------------
 *
 * Some MACs have no MDIO controller and drive the two PHY-management wires
 * by hand, one bit at a time. if_rl.c is one of them. Upstream factors the
 * bit-banging out here so each such driver only supplies a table saying
 * which register bits are MDC/MDIO and how to read and write them.
 *
 * This is the clause-22 frame format, written out: 32 preamble ones, a
 * start-of-frame, an opcode, the PHY and register addresses, a turnaround,
 * and the 16 data bits. It is fully implemented because it is entirely
 * generic - there is nothing PHY-specific in it.
 */

static void bb_send(device_t dev, mii_bitbang_ops_t ops, uint32 data,
                    int nbits) {
    uint32 mask = 1u << (nbits - 1);
    uint32 val;

    val = ops->mbo_read(dev);
    val &= ~(ops->mbo_bits[MII_BIT_MDO] | ops->mbo_bits[MII_BIT_MDC]);
    val |= ops->mbo_bits[MII_BIT_DIR_HOST_PHY];

    while (mask != 0) {
        if (data & mask) {
            val |= ops->mbo_bits[MII_BIT_MDO];
        } else {
            val &= ~ops->mbo_bits[MII_BIT_MDO];
        }
        /* The data bit must be stable BEFORE the clock rises - that is what
         * a clocked bus means, and doing it the other way round latches the
         * previous bit. */
        ops->mbo_write(dev, val);
        DELAY(1);
        ops->mbo_write(dev, val | ops->mbo_bits[MII_BIT_MDC]);
        DELAY(1);
        ops->mbo_write(dev, val);
        DELAY(1);
        mask >>= 1;
    }
}

int mii_bitbang_readreg(device_t dev, mii_bitbang_ops_t ops, int phy,
                        int reg) {
    uint32 val;
    int i, data = 0;

    /* 32 ones of preamble, then the read frame. */
    bb_send(dev, ops, 0xFFFFFFFFu, 32);
    bb_send(dev, ops, 0x6u, 4);              /* start 01, opcode 10 (read) */
    bb_send(dev, ops, (uint32)phy, 5);
    bb_send(dev, ops, (uint32)reg, 5);

    /* Turnaround: the host releases the line and the PHY drives it. */
    val = ops->mbo_read(dev);
    val &= ~(ops->mbo_bits[MII_BIT_MDO] | ops->mbo_bits[MII_BIT_MDC]);
    val |= ops->mbo_bits[MII_BIT_DIR_PHY_HOST];
    ops->mbo_write(dev, val);
    DELAY(1);
    ops->mbo_write(dev, val | ops->mbo_bits[MII_BIT_MDC]);
    DELAY(1);
    ops->mbo_write(dev, val);
    DELAY(1);

    for (i = 0; i < 16; i++) {
        data <<= 1;
        ops->mbo_write(dev, val | ops->mbo_bits[MII_BIT_MDC]);
        DELAY(1);
        if (ops->mbo_read(dev) & ops->mbo_bits[MII_BIT_MDI]) {
            data |= 1;
        }
        ops->mbo_write(dev, val);
        DELAY(1);
    }
    return data;
}

void mii_bitbang_writereg(device_t dev, mii_bitbang_ops_t ops, int phy,
                          int reg, int val) {
    bb_send(dev, ops, 0xFFFFFFFFu, 32);
    bb_send(dev, ops, 0x5u, 4);              /* start 01, opcode 01 (write) */
    bb_send(dev, ops, (uint32)phy, 5);
    bb_send(dev, ops, (uint32)reg, 5);
    bb_send(dev, ops, 0x2u, 2);              /* turnaround 10, host-driven  */
    bb_send(dev, ops, (uint32)val, 16);
}

/* Reset one PHY. A KOBJ method on the PHY driver upstream; with no PHY
 * drivers here it writes BMCR_RESET directly, which is clause 22 and
 * therefore correct on any PHY. The self-clearing wait matters: the PHY
 * ignores writes while it is resetting, so a driver that programs it
 * immediately afterwards programs nothing. */
void PHY_RESET(struct mii_softc *sc) {
    int i;

    if (sc == 0) {
        return;
    }
    MIIBUS_WRITEREG(sc->mii_dev, sc->mii_phy, MII_BMCR, BMCR_RESET);
    for (i = 0; i < 100; i++) {
        if (!(MIIBUS_READREG(sc->mii_dev, sc->mii_phy, MII_BMCR) &
              BMCR_RESET)) {
            return;
        }
        DELAY(1000);
    }
}
